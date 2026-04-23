#include "gui_audio.h"
#include "gui_render.h"
#include "gui_x11.h"

#include <cairo/cairo.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sndfile.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace {

constexpr int      kProgressBarHeight = 4;
constexpr double   kTopStripRatio     = 0.10;
constexpr double   kBottomStripRatio  = 0.10;
constexpr int      kChannelGapPx      = 2;
constexpr GuiColor kWaveformColor     = {0.55, 0.75, 0.90};
constexpr GuiColor kPlayheadColor     = {0.95, 0.85, 0.35};
constexpr GuiColor kTimestampColor    = {0.80, 0.80, 0.82};

// ms-per-pixel for each numeric zoom level. Level 0 is most zoomed in.
constexpr double kZoomMsPerPixel[] = {
    1.25, 2.6, 5.2, 10.4, 20.8, 41.7, 83.3, 166.7
};
constexpr int kNumZoomLevels = static_cast<int>(
    sizeof(kZoomMsPerPixel) / sizeof(kZoomMsPerPixel[0]));

// Sentinel for the fit-file level ("whole file visible"). Computed at zoom /
// resize time, not stored as a fixed ms/pixel.
constexpr int kFitFileLevel = kNumZoomLevels;

// Timestamp text layout (bottom-left of the status strip).
constexpr int kTimestampPadX              = 8;
constexpr int kTimestampBaselineFromBottom = 12;
constexpr int kTimestampRegionW           = 160;
constexpr int kTimestampRegionH           = 30;

// Half-width of the column invalidated around a playhead position. Three
// pixels total — generous enough to cover 1px line plus subpixel rounding.
constexpr int kPlayheadHalfPx = 1;

struct AppState {
    int     width                 = 1400;
    int     height                = 800;
    bool    loading               = false;
    float   load_progress         = 0.0f;

    int64_t playhead_sample       = 0;
    int     zoom_level            = 0;
    int64_t viewport_start_sample = 0;
    bool    follow_mode           = false;

    // Companion files discovered alongside the loaded audio. Chunk E just
    // records these; later chunks will parse their contents.
    std::string warpmarkers_path;
    std::string settings_path;

    // If a drop arrives mid-load, the path is stashed here and processed
    // after the active load returns. Empty means "no pending drop."
    std::string pending_drop_path;

    // For redraw-time diagnostics (acceptance criterion 15).
    std::chrono::steady_clock::time_point stats_last_report =
        std::chrono::steady_clock::now();
    double  stats_max_redraw_ms = 0.0;
    int     stats_over_1ms_count = 0;
};

int top_strip_height(int window_height) {
    return static_cast<int>(std::lround(window_height * kTopStripRatio));
}

int bottom_strip_height(int window_height) {
    return static_cast<int>(std::lround(window_height * kBottomStripRatio));
}

GuiRect waveform_area(const AppState& a) {
    const int top_h = top_strip_height(a.height);
    const int bot_h = bottom_strip_height(a.height);
    return GuiRect{0, top_h, a.width, a.height - top_h - bot_h};
}

double samples_per_pixel_at(int zoom_level,
                            int waveform_width_px,
                            int64_t total_frames,
                            int sample_rate) {
    if (zoom_level == kFitFileLevel) {
        if (waveform_width_px <= 0) return 1.0;
        double spp = static_cast<double>(total_frames) /
                     static_cast<double>(waveform_width_px);
        if (spp < 1e-9) spp = 1e-9;
        return spp;
    }
    return kZoomMsPerPixel[zoom_level] *
           static_cast<double>(sample_rate) / 1000.0;
}

// Largest numeric level L (in [0, kNumZoomLevels)) whose samples_visible does
// not exceed total_frames. Returns -1 if even level 0 shows more than the
// file — in which case fit-file is the only valid level.
int max_valid_numeric_level(int waveform_width_px,
                            int64_t total_frames,
                            int sample_rate) {
    int best = -1;
    for (int L = 0; L < kNumZoomLevels; L++) {
        const double spp =
            samples_per_pixel_at(L, waveform_width_px, total_frames, sample_rate);
        const double visible = spp * waveform_width_px;
        if (visible <= static_cast<double>(total_frames)) best = L;
        else break; // table is monotonic
    }
    return best;
}

int64_t samples_visible(const AppState& a, const GuiAudio& audio) {
    const GuiRect area = waveform_area(a);
    const double spp = samples_per_pixel_at(
        a.zoom_level, area.w, audio.total_frames(), audio.sample_rate());
    return static_cast<int64_t>(std::llround(spp * area.w));
}

double current_samples_per_pixel(const AppState& a, const GuiAudio& audio) {
    const GuiRect area = waveform_area(a);
    return samples_per_pixel_at(
        a.zoom_level, area.w, audio.total_frames(), audio.sample_rate());
}

void clamp_viewport_start(AppState& a, const GuiAudio& audio) {
    const int64_t visible = samples_visible(a, audio);
    const int64_t total   = audio.total_frames();
    if (visible >= total) {
        a.viewport_start_sample = 0;
        return;
    }
    if (a.viewport_start_sample < 0) a.viewport_start_sample = 0;
    const int64_t max_start = total - visible;
    if (a.viewport_start_sample > max_start) a.viewport_start_sample = max_start;
}

double playhead_pixel_x(const AppState& a, const GuiAudio& audio) {
    const double spp = current_samples_per_pixel(a, audio);
    if (spp <= 0.0) return -1.0;
    return static_cast<double>(a.playhead_sample - a.viewport_start_sample) / spp;
}

// Computes a viewport_start such that `sample` renders at pixel column
// `target_pixel_x`, then clamps. Used for playhead-centered zoom.
int64_t viewport_start_for_pixel(int64_t sample,
                                 double target_pixel_x,
                                 double samples_per_pixel) {
    const double start = static_cast<double>(sample) -
                         target_pixel_x * samples_per_pixel;
    if (start <= 0.0) return 0;
    return static_cast<int64_t>(std::llround(start));
}

// Shrink-and-pad: produce a union rectangle covering both inputs. Used to
// bundle the two playhead-column invalidations into a single expose when
// they overlap (e.g., arrow key at zoom level 0 moves by 1 pixel).
GuiRect union_rect(GuiRect a, GuiRect b) {
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return GuiRect{x0, y0, x1 - x0, y1 - y0};
}

// Renders the waveform sub-region covered by `exposed`, clipped to each
// channel's rect. Re-derives sub-viewport samples so render_waveform only
// iterates the columns that actually changed.
void render_waveform_exposed(cairo_t* cr,
                             GuiRect channel_area,
                             GuiRect exposed,
                             const GuiAudio& audio,
                             int channel,
                             int64_t viewport_start,
                             double samples_per_pixel,
                             GuiColor color) {
    const int xa = std::max(channel_area.x, exposed.x);
    const int xb = std::min(channel_area.x + channel_area.w,
                            exposed.x + exposed.w);
    if (xa >= xb) return;
    const int ya = std::max(channel_area.y, exposed.y);
    const int yb = std::min(channel_area.y + channel_area.h,
                            exposed.y + exposed.h);
    if (ya >= yb) return;

    GuiRect sub{xa, channel_area.y, xb - xa, channel_area.h};
    const int ox = xa - channel_area.x;
    const int64_t sub_start = viewport_start +
        static_cast<int64_t>(std::llround(ox * samples_per_pixel));
    const int64_t sub_end = sub_start +
        static_cast<int64_t>(std::llround(sub.w * samples_per_pixel));
    render_waveform(cr, sub, audio, channel, sub_start, sub_end, color);
}

bool rects_intersect(GuiRect a, GuiRect b) {
    if (a.x + a.w <= b.x || b.x + b.w <= a.x) return false;
    if (a.y + a.h <= b.y || b.y + b.h <= a.y) return false;
    return true;
}

GuiRect playhead_invalidate_rect(const GuiRect& area, double px_x) {
    const int col = static_cast<int>(std::floor(px_x));
    const int x0 = std::max(area.x, col - kPlayheadHalfPx);
    const int x1 = std::min(area.x + area.w, col + kPlayheadHalfPx + 1);
    if (x1 <= x0) return GuiRect{area.x, area.y, 0, 0};
    return GuiRect{x0, area.y, x1 - x0, area.h};
}

GuiRect timestamp_invalidate_rect(int window_height) {
    return GuiRect{0, window_height - kTimestampRegionH,
                   kTimestampRegionW, kTimestampRegionH};
}

// Ensure `p` exists with `contents`. If the file already exists, leave it
// alone. Returns true on success or if file already exists. Failures are
// non-fatal — the audio load still proceeds.
bool create_if_missing(const std::filesystem::path& p,
                       const std::string& contents) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return true;
    std::ofstream f(p);
    if (!f) {
        std::fprintf(stderr,
                     "warptempo_gui: could not create '%s'\n",
                     p.string().c_str());
        return false;
    }
    f << contents;
    return static_cast<bool>(f);
}

} // namespace

int main(int argc, char** argv) {
    const char* cli_path = nullptr;
    if (argc == 1) {
        // Empty window; wait for a drag-and-drop.
    } else if (argc == 2) {
        cli_path = argv[1];
    } else {
        std::fprintf(stderr, "usage: warptempo_gui [<audio_file>]\n");
        return 1;
    }

    AppState app;
    GuiAudio audio;
    GuiX11   gui;
    if (!gui.init(app.width, app.height, "Warptempo")) {
        return 1;
    }

    // -- Navigation/viewport helpers ----------------------------------------

    auto invalidate_waveform_area = [&]() {
        const GuiRect a = waveform_area(app);
        gui.invalidate_region(a.x, a.y, a.w, a.h);
    };

    auto invalidate_timestamp_area = [&]() {
        const GuiRect t = timestamp_invalidate_rect(app.height);
        gui.invalidate_region(t.x, t.y, t.w, t.h);
    };

    auto invalidate_playhead_columns = [&](double old_px, double new_px) {
        const GuiRect area = waveform_area(app);
        const GuiRect r_old = playhead_invalidate_rect(area, old_px);
        const GuiRect r_new = playhead_invalidate_rect(area, new_px);
        // Union when close (common case: 1px nudges overlap) — one expose.
        if (rects_intersect(r_old, r_new) ||
            std::abs(new_px - old_px) < 4.0) {
            const GuiRect u = union_rect(r_old, r_new);
            if (u.w > 0 && u.h > 0) {
                gui.invalidate_region(u.x, u.y, u.w, u.h);
            }
        } else {
            if (r_old.w > 0) gui.invalidate_region(r_old.x, r_old.y, r_old.w, r_old.h);
            if (r_new.w > 0) gui.invalidate_region(r_new.x, r_new.y, r_new.w, r_new.h);
        }
    };

    // move_playhead_to: update playhead, keep viewport so playhead stays
    // visible. Invalidate only what changed.
    auto move_playhead_to = [&](int64_t new_sample) {
        if (audio.total_frames() <= 0) return;
        const int64_t total = audio.total_frames();
        if (new_sample < 0) new_sample = 0;
        if (new_sample > total - 1) new_sample = total - 1;

        const double old_px = playhead_pixel_x(app, audio);
        const int64_t old_vp = app.viewport_start_sample;
        const int64_t visible = samples_visible(app, audio);

        app.playhead_sample = new_sample;

        const int64_t vp_end = app.viewport_start_sample + visible;
        bool viewport_changed = false;

        if (new_sample < app.viewport_start_sample) {
            app.viewport_start_sample = new_sample;
            viewport_changed = true;
        } else if (new_sample >= vp_end) {
            const double spp = current_samples_per_pixel(app, audio);
            const int64_t one_px = static_cast<int64_t>(std::llround(spp));
            app.viewport_start_sample =
                new_sample - (visible - std::max<int64_t>(one_px, 1));
            viewport_changed = true;
        }
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) viewport_changed = true;

        if (viewport_changed) {
            invalidate_waveform_area();
        } else {
            const double new_px = playhead_pixel_x(app, audio);
            invalidate_playhead_columns(old_px, new_px);
        }
        invalidate_timestamp_area();
    };

    auto move_playhead_pixels = [&](int delta_px) {
        if (audio.total_frames() <= 0) return;
        const double spp = current_samples_per_pixel(app, audio);
        const int64_t delta_samples =
            static_cast<int64_t>(std::llround(delta_px * spp));
        move_playhead_to(app.playhead_sample + delta_samples);
    };

    // Apply a zoom change. The numeric target is derived inside; this helper
    // handles the playhead-centered viewport recompute so zoom_in/zoom_out
    // share exactly the same logic.
    auto apply_zoom_change = [&](int new_zoom_level) {
        if (audio.total_frames() <= 0) return;
        if (new_zoom_level == app.zoom_level) return;

        const double old_spp = current_samples_per_pixel(app, audio);
        const double old_px  = (old_spp > 0.0)
            ? static_cast<double>(app.playhead_sample -
                                  app.viewport_start_sample) / old_spp
            : 0.0;

        app.zoom_level = new_zoom_level;

        if (app.zoom_level == kFitFileLevel) {
            app.viewport_start_sample = 0;
        } else {
            const double new_spp = current_samples_per_pixel(app, audio);
            app.viewport_start_sample =
                viewport_start_for_pixel(app.playhead_sample, old_px, new_spp);
            clamp_viewport_start(app, audio);
        }

        invalidate_waveform_area();
        invalidate_timestamp_area();
    };

    auto zoom_in = [&]() {
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        if (max_num < 0) return; // no numeric level valid; only fit-file
        if (app.zoom_level == kFitFileLevel) {
            apply_zoom_change(max_num);
        } else if (app.zoom_level > 0) {
            apply_zoom_change(app.zoom_level - 1);
        }
        // else at level 0 already — no-op.
    };

    auto zoom_out = [&]() {
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        if (app.zoom_level == kFitFileLevel) return; // already fully out
        if (max_num < 0 || app.zoom_level >= max_num) {
            apply_zoom_change(kFitFileLevel);
        } else {
            apply_zoom_change(app.zoom_level + 1);
        }
    };

    auto scroll_viewport = [&](int64_t delta_samples) {
        if (audio.total_frames() <= 0) return;
        const int64_t old_vp = app.viewport_start_sample;
        app.viewport_start_sample += delta_samples;
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) {
            invalidate_waveform_area();
            // Timestamp shows playhead time, which didn't change — no need
            // to invalidate it. Kept simple for consistency anyway.
        }
    };

    auto center_viewport_on_playhead = [&]() {
        if (audio.total_frames() <= 0) return;
        const int64_t visible = samples_visible(app, audio);
        const int64_t old_vp = app.viewport_start_sample;
        app.viewport_start_sample = app.playhead_sample - visible / 2;
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) {
            invalidate_waveform_area();
        }
    };

    // -- Redraw -------------------------------------------------------------

    gui.set_on_redraw([&](cairo_t* cr, int x, int y, int w, int h) {
        const auto t_start = std::chrono::steady_clock::now();

        cairo_save(cr);
        cairo_rectangle(cr, x, y, w, h);
        cairo_clip(cr);

        render_background(cr, x, y, w, h);

        if (app.loading) {
            const int bar_y = app.height - kProgressBarHeight;
            render_progress_bar(cr, 0, bar_y, app.width, kProgressBarHeight,
                                app.load_progress);
        } else if (audio.total_frames() > 0) {
            const GuiRect area = waveform_area(app);
            const GuiRect exposed{x, y, w, h};
            const double spp = current_samples_per_pixel(app, audio);
            const int64_t vp_start = app.viewport_start_sample;

            const int rc = audio.render_channels();
            if (rc == 1) {
                render_waveform_exposed(cr, area, exposed, audio, 0,
                                        vp_start, spp, kWaveformColor);
            } else if (rc >= 2) {
                const int ch_h = (area.h - kChannelGapPx) / 2;
                const GuiRect ch0{area.x, area.y, area.w, ch_h};
                const GuiRect ch1{area.x, area.y + ch_h + kChannelGapPx,
                                  area.w, ch_h};
                render_waveform_exposed(cr, ch0, exposed, audio, 0,
                                        vp_start, spp, kWaveformColor);
                render_waveform_exposed(cr, ch1, exposed, audio, 1,
                                        vp_start, spp, kWaveformColor);
            }

            // Playhead on top of the waveform, within the waveform area.
            const double px_x = playhead_pixel_x(app, audio);
            if (rects_intersect(exposed, area)) {
                render_playhead(cr, area, px_x, kPlayheadColor);
            }

            // Timestamp in the bottom status strip.
            const GuiRect ts = timestamp_invalidate_rect(app.height);
            if (rects_intersect(exposed, ts)) {
                const double seconds = (audio.sample_rate() > 0)
                    ? static_cast<double>(app.playhead_sample) /
                      static_cast<double>(audio.sample_rate())
                    : 0.0;
                const int baseline_y = app.height - kTimestampBaselineFromBottom;
                render_timestamp(cr, kTimestampPadX, baseline_y,
                                 seconds, kTimestampColor);
            }
        }

        cairo_restore(cr);

        const auto t_end = std::chrono::steady_clock::now();
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(t_end - t_start).count();
        if (elapsed_ms > app.stats_max_redraw_ms)
            app.stats_max_redraw_ms = elapsed_ms;
        if (elapsed_ms > 1.0) app.stats_over_1ms_count++;
        const double since_last = std::chrono::duration<double>(
            t_end - app.stats_last_report).count();
        if (since_last >= 1.0) {
            if (app.stats_max_redraw_ms > 1.0) {
                std::fprintf(stderr,
                    "[warptempo_gui] redraw max=%.2fms in last %.1fs "
                    "(%d redraws > 1ms)\n",
                    app.stats_max_redraw_ms, since_last,
                    app.stats_over_1ms_count);
            }
            app.stats_max_redraw_ms = 0.0;
            app.stats_over_1ms_count = 0;
            app.stats_last_report = t_end;
        }
    });

    gui.set_on_resize([&](int w, int h) {
        app.width  = w;
        app.height = h;
        if (app.loading || audio.total_frames() <= 0) return;

        // A numeric zoom level may have been valid at the old width but show
        // more samples than the file at the new width — promote to fit-file.
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        if (app.zoom_level != kFitFileLevel) {
            if (max_num < 0 || app.zoom_level > max_num) {
                app.zoom_level = kFitFileLevel;
                app.viewport_start_sample = 0;
            }
        }
        clamp_viewport_start(app, audio);
    });

    gui.set_on_key([&](KeySym keysym, unsigned int /*mods*/) {
        if (app.loading || audio.total_frames() <= 0) {
            if (keysym == XK_Escape) gui.request_exit();
            return;
        }
        switch (keysym) {
        case XK_Escape: gui.request_exit();               break;
        case XK_Left:   move_playhead_pixels(-1);         break;
        case XK_Right:  move_playhead_pixels(+1);         break;
        case XK_Up:     zoom_in();                        break;
        case XK_Down:   zoom_out();                       break;
        case XK_f:      app.follow_mode = !app.follow_mode; break;
        case XK_c:      center_viewport_on_playhead();    break;
        case XK_Home:   move_playhead_to(0);              break;
        case XK_End:    move_playhead_to(audio.total_frames() - 1); break;
        default: break;
        }
    });

    gui.set_on_close([&]() { gui.request_exit(); });

    gui.set_on_button_press([&](unsigned int button, int x, int y,
                                unsigned int mods) {
        if (app.loading || audio.total_frames() <= 0) return;
        const GuiRect area = waveform_area(app);
        const bool inside_waveform =
            x >= area.x && x < area.x + area.w &&
            y >= area.y && y < area.y + area.h;
        const bool ctrl = (mods & ControlMask) != 0;
        const bool alt  = (mods & Mod1Mask) != 0;

        if (button == 1) {
            if (inside_waveform) {
                const double spp = current_samples_per_pixel(app, audio);
                const int64_t sample = app.viewport_start_sample +
                    static_cast<int64_t>(std::llround((x - area.x) * spp));
                move_playhead_to(sample);
            }
        } else if (button == 4 || button == 5) {
            // Wheel in flag strip or window margins is ignored per spec.
            if (!inside_waveform) return;
            if (ctrl) return; // reserved
            if (alt) {
                const int64_t step = std::max<int64_t>(
                    1, samples_visible(app, audio) / 10);
                scroll_viewport(button == 4 ? -step : +step);
            } else {
                if (button == 4) zoom_out();
                else             zoom_in();
            }
        }
    });

    gui.set_on_button_release([](unsigned int, int, int, unsigned int) {});
    gui.set_on_motion        ([](int, int, unsigned int) {});

    // -- File loading --------------------------------------------------------

    // Loads `path` into a fresh GuiAudio, preflights via libsndfile, and
    // swaps the new audio in on success. On failure, the prior audio (if
    // any) remains loaded unchanged. Resets per-file navigation state;
    // follow_mode is intentionally preserved across reloads.
    auto load_file = [&](const std::string& path) -> bool {
        // Preflight.
        {
            SF_INFO probe_info;
            std::memset(&probe_info, 0, sizeof(probe_info));
            SNDFILE* probe = sf_open(path.c_str(), SFM_READ, &probe_info);
            if (!probe) {
                std::fprintf(stderr,
                             "warptempo_gui: '%s': %s\n",
                             path.c_str(), sf_strerror(nullptr));
                return false;
            }
            sf_close(probe);
        }

        app.loading       = true;
        app.load_progress = 0.0f;
        gui.invalidate_region(0, 0, app.width, app.height);
        gui.drain_events();

        GuiAudio next;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = next.load(path, [&](float p) {
            app.load_progress = p;
            const int bar_y = app.height - kProgressBarHeight;
            gui.invalidate_region(0, bar_y, app.width, kProgressBarHeight);
            gui.drain_events();
        });
        const auto t1 = std::chrono::steady_clock::now();

        if (!ok) {
            app.loading       = false;
            app.load_progress = 0.0f;
            gui.invalidate_region(0, 0, app.width, app.height);
            return false;
        }

        audio = std::move(next);
        app.loading       = false;
        app.load_progress = 0.0f;

        app.playhead_sample       = 0;
        app.viewport_start_sample = 0;
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        app.zoom_level = (max_num >= 0) ? 0 : kFitFileLevel;
        clamp_viewport_start(app, audio);

        // Companion files: discover paths, create defaults if missing.
        std::filesystem::path apath(path);
        std::filesystem::path parent = apath.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        const std::filesystem::path wm_path  = parent / ".warpmarkers";
        const std::filesystem::path set_path = parent / ".settings";
        app.warpmarkers_path = wm_path.string();
        app.settings_path    = set_path.string();

        create_if_missing(wm_path, "00:00.000|1.00\n");
        std::string settings_default =
            "title=\n"
            "audio_input=" + apath.filename().string() + "\n"
            "scale=1\n"
            "N=4096\n";
        create_if_missing(set_path, settings_default);

        const double load_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr,
                     "[warptempo_gui] loaded %s: sr=%d, channels=%d, frames=%lld, "
                     "pyramid_levels=%d, load_time=%.1f ms\n",
                     path.c_str(), audio.sample_rate(), audio.channels(),
                     static_cast<long long>(audio.total_frames()),
                     audio.num_levels(), load_ms);

        gui.invalidate_region(0, 0, app.width, app.height);
        return true;
    };

    // Process `path` and any drops that arrived while the load was running.
    // Pending slot is last-wins, not a queue; rapid drags collapse.
    auto load_then_drain = [&](std::string path) {
        while (true) {
            load_file(path);
            if (app.pending_drop_path.empty()) break;
            path = std::move(app.pending_drop_path);
            app.pending_drop_path.clear();
        }
    };

    gui.set_drop_accept_predicate([&](int x, int y) -> bool {
        const GuiRect area = waveform_area(app);
        return x >= area.x && x < area.x + area.w &&
               y >= area.y && y < area.y + area.h;
    });

    gui.set_on_file_drop([&](const std::string& path) {
        if (app.loading) {
            app.pending_drop_path = path;
            return;
        }
        load_then_drain(path);
    });

    // Paint the initial background before any synchronous load begins so the
    // window isn't briefly blank on fast disks.
    gui.invalidate_region(0, 0, app.width, app.height);
    gui.drain_events();

    if (cli_path) {
        if (!load_file(cli_path)) {
            gui.shutdown();
            return 1;
        }
        // Any drops queued during the startup load run now.
        while (!app.pending_drop_path.empty()) {
            std::string next = std::move(app.pending_drop_path);
            app.pending_drop_path.clear();
            load_file(next);
        }
    }

    gui.run();
    gui.shutdown();
    return 0;
}
