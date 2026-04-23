#include "gui_audio.h"
#include "gui_markers.h"
#include "gui_playback.h"
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
constexpr GuiColor kWaveformDimColor  = {0.30, 0.38, 0.45};
constexpr GuiColor kPlayheadColor     = {0.95, 0.85, 0.35};
constexpr GuiColor kTimestampColor    = {0.80, 0.80, 0.82};
constexpr GuiColor kMarkerColor       = {0.85, 0.82, 0.78};
constexpr GuiColor kMarkerDimColor    = {0.45, 0.42, 0.40};
constexpr GuiColor kSelectedColor     = {1.00, 0.95, 0.70};
constexpr GuiColor kFlagHighlightColor= {0.30, 0.28, 0.22};
constexpr GuiColor kDirtyColor        = {0.90, 0.65, 0.35};
constexpr double   kFlagFontSize      = 13.0;

// Half-width in pixels of the selection-hit window when clicking on a marker.
constexpr int kMarkerHitHalfPx = 3;

// Double-click detection thresholds. X11 doesn't synthesize DoubleClick; we
// roll it from ButtonPress timing + position deltas.
constexpr int kDoubleClickMs      = 300;
constexpr int kDoubleClickPixels  = 5;

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
// Region width includes room for the dirty indicator 8 px past the text edge.
constexpr int kTimestampRegionW           = 200;
constexpr int kTimestampRegionH           = 30;
constexpr double kDirtyGapPx              = 8.0;

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

    // Playback state. `playback_cursor` is the last sample read from the
    // audio thread; `is_playing` mirrors the audio thread's flag so the
    // main loop can detect natural end-of-playback. `playback_speed` is
    // authoritative on the main thread and pushed to the playback engine
    // on every change.
    int64_t playback_cursor = 0;
    bool    is_playing      = false;
    float   playback_speed  = 1.0f;

    // Companion files discovered alongside the loaded audio. Chunk E just
    // records these; later chunks will parse their contents.
    std::string warpmarkers_path;
    std::string settings_path;

    // Parsed warp markers for the currently loaded audio. Empty on load
    // failure or before the first audio load.
    GuiMarkers  markers;

    // Index into markers.markers(). -1 means nothing selected.
    int         selected_marker      = -1;

    // True if the marker list has been modified since load or last save.
    bool        dirty                = false;

    // True until the first save in this session; used to log a one-time
    // notice if the on-disk file had content the canonical form drops.
    bool        first_save_pending   = true;

    // If a drop arrives mid-load, the path is stashed here and processed
    // after the active load returns. Empty means "no pending drop."
    std::string pending_drop_path;

    // Double-click detection state. Tracks the most recent left-button
    // press so the next one can compare and upgrade to a double-click.
    std::chrono::steady_clock::time_point last_click_time =
        std::chrono::steady_clock::time_point{};
    int          last_click_x        = -10000;
    int          last_click_y        = -10000;
    bool         last_click_consumed = true;

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

GuiRect top_strip_area(const AppState& a) {
    return GuiRect{0, 0, a.width, top_strip_height(a.height)};
}

// Scan `markers` for begin_time / end_time flags; fall back to full file if
// either is absent. Returns the pair in source samples.
std::pair<long long, long long> compute_trim_samples(
    const std::vector<GuiMarker>& markers, int sample_rate,
    long long total_frames) {
    long long begin = 0;
    long long end   = total_frames;
    for (const auto& m : markers) {
        if (m.is_begin_time) {
            begin = static_cast<long long>(std::llround(
                m.time_seconds * static_cast<double>(sample_rate)));
        }
        if (m.is_end_time) {
            end = static_cast<long long>(std::llround(
                m.time_seconds * static_cast<double>(sample_rate)));
        }
    }
    if (begin < 0) begin = 0;
    if (end > total_frames) end = total_frames;
    if (end < begin) end = begin;
    return {begin, end};
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
                             int64_t trim_begin,
                             int64_t trim_end,
                             GuiColor bright_color,
                             GuiColor dim_color) {
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
    render_waveform(cr, sub, audio, channel, sub_start, sub_end,
                    trim_begin, trim_end, bright_color, dim_color);
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

    AppState    app;
    GuiAudio    audio;
    GuiPlayback playback;
    GuiX11      gui;
    if (!gui.init(app.width, app.height, "Warptempo")) {
        return 1;
    }

    // -- Trim helpers --------------------------------------------------------

    auto trim_range = [&]() -> std::pair<int64_t, int64_t> {
        if (audio.total_frames() <= 0) return {0, 0};
        return compute_trim_samples(
            app.markers.markers(), audio.sample_rate(), audio.total_frames());
    };
    auto trim_begin_sample = [&]() -> int64_t { return trim_range().first; };
    auto trim_end_sample   = [&]() -> int64_t { return trim_range().second; };
    auto sample_in_trim = [&](int64_t s) -> bool {
        const auto tr = trim_range();
        return s >= tr.first && s < tr.second;
    };
    auto is_playhead_in_trim = [&]() -> bool {
        return sample_in_trim(app.playhead_sample);
    };

    // -- Navigation/viewport helpers ----------------------------------------

    // Viewport changes repaint the waveform area and the top strip together:
    // flag positions depend on the viewport, so any pan/zoom has to refresh
    // flags as well as waveform. Playhead-only moves keep using the narrow
    // column invalidation below.
    auto invalidate_waveform_area = [&]() {
        const GuiRect a = waveform_area(app);
        const int y0 = 0;
        const int y1 = a.y + a.h;
        gui.invalidate_region(0, y0, app.width, y1 - y0);
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
    // visible. Invalidate only what changed. Clamps into the current trim
    // range rather than the whole file so arrow navigation can't wander
    // into dimmed territory.
    auto move_playhead_to = [&](int64_t new_sample) {
        if (audio.total_frames() <= 0) return;
        const int64_t tb = trim_begin_sample();
        const int64_t te = trim_end_sample();
        const int64_t lo = tb;
        const int64_t hi = (te > tb) ? te - 1 : tb;
        if (new_sample < lo) new_sample = lo;
        if (new_sample > hi) new_sample = hi;

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
            const GuiRect area       = waveform_area(app);
            const GuiRect top_strip  = top_strip_area(app);
            const GuiRect exposed{x, y, w, h};
            const double  spp        = current_samples_per_pixel(app, audio);
            const int64_t vp_start   = app.viewport_start_sample;
            const int64_t vp_end     = vp_start +
                static_cast<int64_t>(std::llround(spp * area.w));
            const int     sr         = audio.sample_rate();

            const auto trim = compute_trim_samples(
                app.markers.markers(), sr, audio.total_frames());
            const int64_t trim_begin = trim.first;
            const int64_t trim_end   = trim.second;

            const int rc = audio.render_channels();
            if (rc == 1) {
                render_waveform_exposed(cr, area, exposed, audio, 0,
                                        vp_start, spp,
                                        trim_begin, trim_end,
                                        kWaveformColor, kWaveformDimColor);
            } else if (rc >= 2) {
                const int ch_h = (area.h - kChannelGapPx) / 2;
                const GuiRect ch0{area.x, area.y, area.w, ch_h};
                const GuiRect ch1{area.x, area.y + ch_h + kChannelGapPx,
                                  area.w, ch_h};
                render_waveform_exposed(cr, ch0, exposed, audio, 0,
                                        vp_start, spp,
                                        trim_begin, trim_end,
                                        kWaveformColor, kWaveformDimColor);
                render_waveform_exposed(cr, ch1, exposed, audio, 1,
                                        vp_start, spp,
                                        trim_begin, trim_end,
                                        kWaveformColor, kWaveformDimColor);
            }

            // Markers: vertical lines in the waveform area, beneath the
            // playhead. Cairo's outer clip confines painting to `exposed`.
            if (rects_intersect(exposed, area)) {
                render_markers(cr, area, app.markers.markers(),
                               vp_start, vp_end, sr,
                               kMarkerColor, kMarkerDimColor,
                               kSelectedColor, app.selected_marker);
            }

            // Playhead on top of the waveform, within the waveform area.
            const double px_x = playhead_pixel_x(app, audio);
            if (rects_intersect(exposed, area)) {
                render_playhead(cr, area, px_x, kPlayheadColor);
            }

            // Flag annotations in the top strip.
            if (rects_intersect(exposed, top_strip)) {
                render_flags(cr, top_strip, app.markers.markers(),
                             vp_start, vp_end, sr,
                             kMarkerColor, kMarkerDimColor,
                             kSelectedColor, kFlagHighlightColor,
                             kFlagFontSize, app.selected_marker);
            }

            // Timestamp in the bottom status strip.
            const GuiRect ts = timestamp_invalidate_rect(app.height);
            if (rects_intersect(exposed, ts)) {
                const double seconds = (sr > 0)
                    ? static_cast<double>(app.playhead_sample) /
                      static_cast<double>(sr)
                    : 0.0;
                const int baseline_y = app.height - kTimestampBaselineFromBottom;
                render_timestamp(cr, kTimestampPadX, baseline_y,
                                 seconds, kTimestampColor);

                if (app.dirty) {
                    const double w = measure_timestamp_width(cr, seconds);
                    const double cx = static_cast<double>(kTimestampPadX) +
                                      w + kDirtyGapPx + 3.0;
                    const double cy =
                        static_cast<double>(baseline_y) - 5.0;
                    render_dirty_indicator(cr, cx, cy, kDirtyColor);
                }
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

    // Invalidate a narrow column around a marker's on-screen x (same width
    // as the playhead invalidation). No-op if the marker is off-screen.
    auto invalidate_marker_column = [&](int marker_idx) {
        if (marker_idx < 0 ||
            marker_idx >= static_cast<int>(app.markers.markers().size())) return;
        if (audio.total_frames() <= 0) return;
        const double spp = current_samples_per_pixel(app, audio);
        if (spp <= 0.0) return;
        const GuiRect area = waveform_area(app);
        const int sr = audio.sample_rate();
        const double ms = app.markers.markers()[marker_idx].time_seconds *
                          static_cast<double>(sr);
        const double vp = static_cast<double>(app.viewport_start_sample);
        const int64_t visible = samples_visible(app, audio);
        if (ms < vp) return;
        if (ms >= vp + static_cast<double>(visible)) return;
        const double px = area.x + (ms - vp) / spp;
        const GuiRect r = playhead_invalidate_rect(area, px);
        if (r.w > 0 && r.h > 0) {
            gui.invalidate_region(r.x, r.y, r.w, r.h);
        }
    };

    auto invalidate_top_strip = [&]() {
        const GuiRect ts = top_strip_area(app);
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
    };

    // Switches the selection to `new_sel` (may be -1) and invalidates the
    // minimum necessary regions: old and new marker columns, plus the flag
    // strip. No playhead movement is done here; callers decide whether to
    // follow the selection with move_playhead_to.
    auto set_selection = [&](int new_sel) {
        if (new_sel == app.selected_marker) return;
        const int old_sel = app.selected_marker;
        app.selected_marker = new_sel;
        invalidate_marker_column(old_sel);
        invalidate_marker_column(new_sel);
        invalidate_top_strip();
    };

    auto invalidate_dirty_and_timestamp = [&]() {
        const GuiRect t = timestamp_invalidate_rect(app.height);
        gui.invalidate_region(t.x, t.y, t.w, t.h);
    };

    // True if marker i's resolved sample falls inside the current trim
    // range. Trim enforcement hides out-of-range markers from Tab cycling.
    auto marker_in_trim = [&](int i) -> bool {
        const auto& mv = app.markers.markers();
        if (i < 0 || i >= static_cast<int>(mv.size())) return false;
        const int sr = audio.sample_rate();
        const int64_t s = static_cast<int64_t>(std::llround(
            mv[i].time_seconds * static_cast<double>(sr)));
        return sample_in_trim(s);
    };

    // Tab / Shift+Tab: cycle through markers. Rules per spec:
    //   With selection: strict next/prev by time.
    //   No selection: pick the nearest marker in the requested direction
    //   (>= playhead for next, <= playhead for prev).
    // Trim range acts as a hard filter — markers outside trim are invisible
    // to cycling.
    auto select_next_marker = [&]() {
        const auto& mv = app.markers.markers();
        if (mv.empty()) return;
        int new_sel = -1;
        const int sr = audio.sample_rate();
        if (app.selected_marker < 0) {
            const double ph_s = (sr > 0)
                ? static_cast<double>(app.playhead_sample) /
                  static_cast<double>(sr)
                : 0.0;
            for (size_t i = 0; i < mv.size(); ++i) {
                if (!marker_in_trim(static_cast<int>(i))) continue;
                if (mv[i].time_seconds >= ph_s) {
                    new_sel = static_cast<int>(i);
                    break;
                }
            }
        } else {
            const double cur_t = mv[app.selected_marker].time_seconds;
            for (size_t i = app.selected_marker + 1; i < mv.size(); ++i) {
                if (!marker_in_trim(static_cast<int>(i))) continue;
                if (mv[i].time_seconds > cur_t) {
                    new_sel = static_cast<int>(i);
                    break;
                }
            }
        }
        if (new_sel < 0 || new_sel == app.selected_marker) return;
        set_selection(new_sel);
        const int64_t sample = static_cast<int64_t>(std::llround(
            mv[new_sel].time_seconds * static_cast<double>(sr)));
        move_playhead_to(sample);
    };

    auto select_prev_marker = [&]() {
        const auto& mv = app.markers.markers();
        if (mv.empty()) return;
        int new_sel = -1;
        const int sr = audio.sample_rate();
        if (app.selected_marker < 0) {
            const double ph_s = (sr > 0)
                ? static_cast<double>(app.playhead_sample) /
                  static_cast<double>(sr)
                : 0.0;
            for (int i = static_cast<int>(mv.size()) - 1; i >= 0; --i) {
                if (!marker_in_trim(i)) continue;
                if (mv[i].time_seconds <= ph_s) {
                    new_sel = i;
                    break;
                }
            }
        } else {
            const double cur_t = mv[app.selected_marker].time_seconds;
            for (int i = app.selected_marker - 1; i >= 0; --i) {
                if (!marker_in_trim(i)) continue;
                if (mv[i].time_seconds < cur_t) {
                    new_sel = i;
                    break;
                }
            }
        }
        if (new_sel < 0 || new_sel == app.selected_marker) return;
        set_selection(new_sel);
        const int64_t sample = static_cast<int64_t>(std::llround(
            mv[new_sel].time_seconds * static_cast<double>(sr)));
        move_playhead_to(sample);
    };

    // Core drop logic shared by `s`, `Shift+S`, double-click, and
    // Shift+double-click. `time_seconds` is the location (not necessarily the
    // playhead); `inherit` controls whether the new marker inherits its
    // tempo from the nearest earlier owner. Moves the playhead to the new
    // marker to keep drop-then-select behavior consistent.
    auto drop_marker = [&](double time_seconds, bool inherit) {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double one_sample = 1.0 / static_cast<double>(sr);
        const auto& mv = app.markers.markers();
        for (const auto& m : mv) {
            if (std::abs(m.time_seconds - time_seconds) < one_sample) {
                std::fprintf(stderr,
                    "warptempo_gui: marker already exists at %.3fs\n",
                    time_seconds);
                return;
            }
        }
        GuiMarker nm;
        nm.time_seconds    = time_seconds;
        nm.tempo_inherits  = inherit;
        // For inherit markers, pre-seed the cached tempo from the nearest
        // earlier owning marker. `insert_marker` returns the index; we
        // compute the source now based on the marker list as it will be
        // after insertion — equivalent to resolving from the insertion
        // point, since the walk-backward stops at the first owner.
        if (inherit) {
            // Find the index we'll be inserted at, then walk backward.
            int insertion_idx = 0;
            for (const auto& m : mv) {
                if (m.time_seconds < time_seconds) insertion_idx++;
                else break;
            }
            nm.tempo_base  = resolve_inherited_tempo(mv, insertion_idx);
            nm.tempo_scale = resolve_inherited_tempo_scale(mv, insertion_idx);
        } else {
            nm.tempo_base = 1.0;
            nm.tempo_scale.clear();
        }
        const int new_idx = app.markers.insert_marker(std::move(nm));
        app.selected_marker = new_idx;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();

        // Move the playhead to the new marker for consistency with click-
        // to-select behavior. Done last so invalidations in the helper
        // don't double-paint with the ones above.
        const int64_t sample = static_cast<int64_t>(std::llround(
            time_seconds * static_cast<double>(sr)));
        move_playhead_to(sample);
    };

    auto drop_marker_at_playhead = [&]() {
        if (!is_playhead_in_trim()) return;
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double t = static_cast<double>(app.playhead_sample) /
                         static_cast<double>(sr);
        drop_marker(t, /*inherit=*/false);
    };

    auto drop_inherit_marker_at_playhead = [&]() {
        if (!is_playhead_in_trim()) return;
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double t = static_cast<double>(app.playhead_sample) /
                         static_cast<double>(sr);
        drop_marker(t, /*inherit=*/true);
    };

    auto delete_selected_marker = [&]() {
        if (!is_playhead_in_trim()) return;
        if (app.selected_marker < 0) return;
        const auto& mv = app.markers.markers();
        if (app.selected_marker >= static_cast<int>(mv.size())) return;
        const GuiMarker& doomed = mv[app.selected_marker];
        if (doomed.time_seconds == 0.0) {
            std::fprintf(stderr,
                "warptempo_gui: cannot delete first marker (time 0)\n");
            return;
        }
        if (!doomed.label_def.empty()) {
            // Reject if any other marker references this label.
            std::string refs;
            int ref_count = 0;
            for (size_t i = 0; i < mv.size(); ++i) {
                if (static_cast<int>(i) == app.selected_marker) continue;
                if (!mv[i].label_ref.empty() &&
                    mv[i].label_ref == doomed.label_def) {
                    char tbuf[32];
                    std::snprintf(tbuf, sizeof(tbuf), "%.3fs",
                                  mv[i].time_seconds);
                    if (!refs.empty()) refs += ", ";
                    refs += tbuf;
                    ++ref_count;
                }
            }
            if (ref_count > 0) {
                std::fprintf(stderr,
                    "warptempo_gui: cannot delete marker: label '%s' is "
                    "referenced at %s\n",
                    doomed.label_def.c_str(), refs.c_str());
                return;
            }
        }
        app.markers.remove_marker(app.selected_marker);
        app.selected_marker = -1;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Toggle the tempo source of the selected marker between owned and
    // inherit. Caches current tempo_base/tempo_scale on the marker itself
    // — both states — so round-tripping through the toggle is lossless.
    auto toggle_inherits = [&]() {
        if (!is_playhead_in_trim()) return;
        if (app.selected_marker < 0) return;
        GuiMarker* m = app.markers.marker_mut(app.selected_marker);
        if (!m) return;
        if (app.selected_marker == 0) {
            std::fprintf(stderr,
                "warptempo_gui: first marker cannot inherit tempo\n");
            return;
        }
        if (!m->label_ref.empty()) {
            // Label refs always effectively inherit; toggling is meaningless.
            return;
        }
        m->tempo_inherits = !m->tempo_inherits;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Toggle the disabled flag on the selected marker's label definition.
    // Silent no-op if the marker has no label definition (including label
    // references, which inherit disabled state from their definition).
    auto toggle_disabled = [&]() {
        if (!is_playhead_in_trim()) return;
        if (app.selected_marker < 0) return;
        GuiMarker* m = app.markers.marker_mut(app.selected_marker);
        if (!m) return;
        if (m->label_def.empty()) return;
        m->disabled = !m->disabled;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    auto toggle_begin_time = [&]() {
        if (!is_playhead_in_trim()) return;
        if (app.selected_marker < 0) return;
        auto& mv_mut = *app.markers.marker_mut(app.selected_marker);
        const bool new_state = !mv_mut.is_begin_time;
        if (new_state) {
            // Clear any other begin-time flag so the invariant "at most one"
            // holds. Silent per spec.
            const auto& mv = app.markers.markers();
            for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
                if (i == app.selected_marker) continue;
                if (mv[i].is_begin_time) {
                    app.markers.marker_mut(i)->is_begin_time = false;
                }
            }
        }
        mv_mut.is_begin_time = new_state;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    auto toggle_end_time = [&]() {
        if (!is_playhead_in_trim()) return;
        if (app.selected_marker < 0) return;
        auto& mv_mut = *app.markers.marker_mut(app.selected_marker);
        const bool new_state = !mv_mut.is_end_time;
        if (new_state) {
            const auto& mv = app.markers.markers();
            for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
                if (i == app.selected_marker) continue;
                if (mv[i].is_end_time) {
                    app.markers.marker_mut(i)->is_end_time = false;
                }
            }
        }
        mv_mut.is_end_time = new_state;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Nudge the selected marker's tempo. Silent no-op if nothing is selected,
    // if the selection is a label reference (tempo is inherited from its
    // definition), if the selection is an inherit marker (tempo is computed),
    // or if the playhead is outside the trim range. Clamps to [0.01, 9.99].
    auto adjust_tempo = [&](double delta) {
        if (!is_playhead_in_trim()) return;
        if (app.selected_marker < 0) return;
        GuiMarker* m = app.markers.marker_mut(app.selected_marker);
        if (!m) return;
        if (!m->label_ref.empty()) return;
        if (m->tempo_inherits) return;
        double new_tempo = m->tempo_base + delta;
        if (new_tempo < 0.01) new_tempo = 0.01;
        if (new_tempo > 9.99) new_tempo = 9.99;
        if (new_tempo == m->tempo_base) return;
        m->tempo_base = new_tempo;
        app.dirty = true;
        invalidate_marker_column(app.selected_marker);
        invalidate_top_strip();
        invalidate_dirty_and_timestamp();
    };

    // Clear any b= / e= flags so the whole file becomes editable again.
    // No-op if no marker carries either flag.
    auto clear_trim = [&]() {
        bool changed = false;
        for (auto& m : app.markers.markers_mut()) {
            if (m.is_begin_time || m.is_end_time) {
                m.is_begin_time = false;
                m.is_end_time   = false;
                changed = true;
            }
        }
        if (!changed) return;
        app.dirty = true;
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    auto save_markers = [&]() {
        if (app.warpmarkers_path.empty()) return;
        if (app.first_save_pending && app.markers.had_nonstandard_content()) {
            std::fprintf(stderr,
                "warptempo_gui: first save in this session will discard "
                "comments and freeform text from %s. Canonical format will "
                "be written.\n",
                app.warpmarkers_path.c_str());
        }
        const bool ok = app.markers.save(app.warpmarkers_path);
        if (!ok) {
            std::fprintf(stderr,
                "warptempo_gui: save failed: %s\n",
                app.warpmarkers_path.c_str());
            return;
        }
        app.first_save_pending = false;
        if (app.dirty) {
            app.dirty = false;
            invalidate_dirty_and_timestamp();
        }
    };

    // Space-bar: start/stop playback. Playback runs from the playhead to
    // trim_end (or total_frames if no e= marker). Pressing space with the
    // playhead at or past trim-end is a silent no-op.
    auto toggle_playback = [&]() {
        if (playback.is_playing()) {
            // Stop the audio thread; leave app.is_playing true so the next
            // tick sees the transition and snaps the playhead to the final
            // cursor position (acceptance criterion: "Playhead stays at the
            // position playback reached").
            playback.stop();
            return;
        }
        const int64_t end = trim_end_sample();
        if (app.playhead_sample >= end) return;
        // Clamp the start position into the trim range in case the playhead
        // is sitting at trim_end - 1 (valid) or somehow slipped.
        const int64_t start = std::max(app.playhead_sample, trim_begin_sample());
        app.playback_cursor = start;
        app.is_playing = true;
        playback.set_speed(app.playback_speed);
        playback.play(start, end);
    };

    // Helpers for Shift+<digit> speed selection.
    auto set_playback_speed = [&](float s) {
        app.playback_speed = s;
        playback.set_speed(s);
    };

    gui.set_on_key([&](KeySym keysym, unsigned int mods) {
        if (app.loading || audio.total_frames() <= 0) {
            if (keysym == XK_Escape) gui.request_exit();
            return;
        }
        const bool ctrl  = (mods & ControlMask) != 0;
        const bool shift = (mods & ShiftMask)   != 0;

        // Space-bar is modifier-independent.
        if (keysym == XK_space) { toggle_playback(); return; }

        // Shift+<digit> selects a playback speed. Shift+0 is 1.00, Shift+1
        // is 0.10, Shift+9 is 0.90. Applies immediately whether or not
        // playback is active — the audio callback picks up the new atomic
        // on the next buffer.
        if (shift && !ctrl) {
            switch (keysym) {
            case XK_0: set_playback_speed(1.0f); return;
            case XK_1: set_playback_speed(0.1f); return;
            case XK_2: set_playback_speed(0.2f); return;
            case XK_3: set_playback_speed(0.3f); return;
            case XK_4: set_playback_speed(0.4f); return;
            case XK_5: set_playback_speed(0.5f); return;
            case XK_6: set_playback_speed(0.6f); return;
            case XK_7: set_playback_speed(0.7f); return;
            case XK_8: set_playback_speed(0.8f); return;
            case XK_9: set_playback_speed(0.9f); return;
            default: break;
            }
        }

        // XLookupKeysym with index 0 returns the unshifted keysym, so a
        // Shift+letter press arrives as the lowercase XK_* with ShiftMask in
        // mods — disambiguate via the `shift` bool, not uppercase keysyms.
        if (keysym == XK_s) {
            if (ctrl)       save_markers();
            else if (shift) drop_inherit_marker_at_playhead();
            else            drop_marker_at_playhead();
            return;
        }
        if (keysym == XK_i && !ctrl) {
            if (shift) toggle_disabled();
            else       toggle_inherits();
            return;
        }

        if (keysym == XK_Tab && !shift) { select_next_marker(); return; }
        if (keysym == XK_Tab && shift)  { select_prev_marker(); return; }
        if (keysym == XK_ISO_Left_Tab)  { select_prev_marker(); return; }

        // Tempo nudge. Plain `=` and `-` only — no Shift / Ctrl variants yet.
        if (keysym == XK_equal && !shift && !ctrl) {
            adjust_tempo(+0.01); return;
        }
        if (keysym == XK_minus && !shift && !ctrl) {
            adjust_tempo(-0.01); return;
        }

        // `l` clears any b= / e= flags. Trim authoring, not navigation — so
        // it honors the modifier guard for future extensions.
        if (keysym == XK_l && !shift && !ctrl) { clear_trim(); return; }

        switch (keysym) {
        case XK_Escape: gui.request_exit();               break;
        case XK_Left:   move_playhead_pixels(-1);         break;
        case XK_Right:  move_playhead_pixels(+1);         break;
        case XK_Up:     zoom_in();                        break;
        case XK_Down:   zoom_out();                       break;
        case XK_f:      app.follow_mode = !app.follow_mode; break;
        case XK_c:      center_viewport_on_playhead();    break;
        case XK_Home:   move_playhead_to(trim_begin_sample()); break;
        case XK_End:    move_playhead_to(trim_end_sample() - 1); break;
        case XK_b:      toggle_begin_time();              break;
        case XK_e:      toggle_end_time();                break;
        case XK_Delete: delete_selected_marker();         break;
        // TODO: growing binding set will want an in-GUI help overlay.
        default: break;
        }
    });

    gui.set_on_close([&]() { gui.request_exit(); });

    // A waveform click during playback reseats the audio thread's cursor to
    // the new playhead. Without this, the visual playhead jumps but the audio
    // keeps playing from its old position. Only click-driven playhead moves
    // restart; arrow keys, Tab, Home/End deliberately do not.
    auto click_restart_playback_if_active = [&]() {
        if (playback.is_playing()) {
            playback.stop();
            playback.play(app.playhead_sample, trim_end_sample());
        }
    };

    gui.set_on_button_press([&](unsigned int button, int x, int y,
                                unsigned int mods) {
        if (app.loading || audio.total_frames() <= 0) return;
        const GuiRect area = waveform_area(app);
        const GuiRect top  = top_strip_area(app);
        const bool inside_waveform =
            x >= area.x && x < area.x + area.w &&
            y >= area.y && y < area.y + area.h;
        const bool inside_top =
            x >= top.x && x < top.x + top.w &&
            y >= top.y && y < top.y + top.h;
        const bool ctrl  = (mods & ControlMask) != 0;
        const bool shift = (mods & ShiftMask)   != 0;
        const bool alt   = (mods & Mod1Mask) != 0;

        if (button == 1) {
            // Detect double-click from timing + position deltas.
            const auto now = std::chrono::steady_clock::now();
            const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - app.last_click_time).count();
            const bool is_double =
                !app.last_click_consumed &&
                dt_ms <= kDoubleClickMs &&
                std::abs(x - app.last_click_x) <= kDoubleClickPixels &&
                std::abs(y - app.last_click_y) <= kDoubleClickPixels;

            // A double-click in the waveform area creates a new marker at
            // the click position (not the playhead). Shift forces inherit.
            // Rejection on duplicate positions is handled inside drop_marker.
            // Clicks in dimmed (outside-trim) regions are silent no-ops.
            if (is_double && inside_waveform) {
                const double spp = current_samples_per_pixel(app, audio);
                const int click_rel_x = x - area.x;
                const int sr = audio.sample_rate();
                const int64_t sample = app.viewport_start_sample +
                    static_cast<int64_t>(std::llround(click_rel_x * spp));
                if (!sample_in_trim(sample)) {
                    app.last_click_consumed = true;
                    return;
                }
                const double t = (sr > 0)
                    ? static_cast<double>(sample) / static_cast<double>(sr)
                    : 0.0;
                drop_marker(t, /*inherit=*/shift);
                // Consume this click so a triple-click doesn't double-fire.
                app.last_click_consumed = true;
                return;
            }

            // Store this click for the next one to compare against.
            app.last_click_time     = now;
            app.last_click_x        = x;
            app.last_click_y        = y;
            app.last_click_consumed = false;

            if (inside_waveform) {
                const double spp = current_samples_per_pixel(app, audio);
                const int click_rel_x = x - area.x;
                const int sr = audio.sample_rate();
                const int64_t click_sample = app.viewport_start_sample +
                    static_cast<int64_t>(std::llround(click_rel_x * spp));

                // Hit-test markers in pixel space within ±kMarkerHitHalfPx.
                // Markers themselves are gated on trim so a flag line in a
                // dimmed region doesn't accidentally win the hit test.
                int best_hit = -1;
                int best_dist = kMarkerHitHalfPx + 1;
                const auto& mv = app.markers.markers();
                const double vp = static_cast<double>(app.viewport_start_sample);
                const int64_t visible = samples_visible(app, audio);
                for (size_t i = 0; i < mv.size(); ++i) {
                    if (!marker_in_trim(static_cast<int>(i))) continue;
                    const double ms = mv[i].time_seconds *
                                      static_cast<double>(sr);
                    if (ms < vp) continue;
                    if (ms >= vp + static_cast<double>(visible)) continue;
                    const int m_px = static_cast<int>(std::llround(
                        (ms - vp) / spp));
                    const int d = std::abs(m_px - click_rel_x);
                    if (d <= kMarkerHitHalfPx && d < best_dist) {
                        best_dist = d;
                        best_hit  = static_cast<int>(i);
                    }
                }

                if (best_hit >= 0) {
                    set_selection(best_hit);
                    if (!ctrl) {
                        const int64_t sample =
                            static_cast<int64_t>(std::llround(
                                mv[best_hit].time_seconds *
                                static_cast<double>(sr)));
                        move_playhead_to(sample);
                        click_restart_playback_if_active();
                    }
                } else if (!ctrl) {
                    // Ctrl+click on empty space is a no-op (doesn't deselect).
                    // Clicks in dimmed regions are silent — no selection
                    // change, no playhead move.
                    if (!sample_in_trim(click_sample)) return;
                    set_selection(-1);
                    move_playhead_to(click_sample);
                    click_restart_playback_if_active();
                }
            } else if (inside_top) {
                // Flag strip. Hit-test actual rendered flag rects. A flag
                // click behaves like clicking the marker line (select +
                // move playhead), modulated by Ctrl. A click that misses a
                // flag deselects and moves the playhead to the click's
                // horizontal position (matching empty-waveform behavior).
                cairo_surface_t* scratch_s = cairo_image_surface_create(
                    CAIRO_FORMAT_ARGB32, 1, 1);
                cairo_t* scratch_cr = cairo_create(scratch_s);
                const double spp = current_samples_per_pixel(app, audio);
                const int64_t vp_start = app.viewport_start_sample;
                const int64_t vp_end = vp_start +
                    static_cast<int64_t>(std::llround(spp * area.w));
                auto rects = compute_flag_hit_rects(
                    scratch_cr, top, app.markers.markers(),
                    vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
                cairo_destroy(scratch_cr);
                cairo_surface_destroy(scratch_s);

                int hit = -1;
                for (const auto& r : rects) {
                    if (x >= r.x && x < r.x + r.w &&
                        y >= r.y && y < r.y + r.h) {
                        if (!marker_in_trim(r.marker_index)) continue;
                        hit = r.marker_index;
                        break;
                    }
                }

                if (hit >= 0) {
                    set_selection(hit);
                    if (!ctrl) {
                        const int sr = audio.sample_rate();
                        const int64_t sample =
                            static_cast<int64_t>(std::llround(
                                app.markers.markers()[hit].time_seconds *
                                static_cast<double>(sr)));
                        move_playhead_to(sample);
                        click_restart_playback_if_active();
                    }
                } else if (!ctrl) {
                    const int click_rel_x = x - area.x;
                    if (click_rel_x >= 0 && click_rel_x < area.w) {
                        const int64_t sample = app.viewport_start_sample +
                            static_cast<int64_t>(std::llround(click_rel_x * spp));
                        if (!sample_in_trim(sample)) return;
                        set_selection(-1);
                        move_playhead_to(sample);
                        click_restart_playback_if_active();
                    } else {
                        set_selection(-1);
                    }
                }
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

        // Stop and tear down the audio device before the sample buffer it
        // borrows is replaced. Playing into a freed buffer would crash the
        // audio thread. Order (stop → shutdown → load → init) is fixed.
        playback.stop();
        playback.shutdown();
        app.is_playing     = false;
        app.playback_cursor = 0;

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

        // Reset playback bookkeeping; the device is brought up after markers
        // are parsed so the initial playhead has the final trim-begin.
        app.playback_speed = 1.0f;

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

        // Load the markers file. Parse failures are non-fatal: we log each
        // error to stderr and leave app.markers empty. The GUI still works
        // as a waveform viewer.
        app.markers.clear();
        app.selected_marker    = -1;
        app.dirty              = false;
        app.first_save_pending = true;
        const bool markers_ok = app.markers.load(wm_path.string());
        if (!markers_ok) {
            for (const auto& err : app.markers.errors()) {
                if (err.line_number > 0) {
                    std::fprintf(stderr,
                                 "warptempo_gui: %s:%d: %s\n",
                                 wm_path.string().c_str(),
                                 err.line_number, err.message.c_str());
                } else {
                    std::fprintf(stderr,
                                 "warptempo_gui: %s: %s\n",
                                 wm_path.string().c_str(),
                                 err.message.c_str());
                }
            }
        } else {
            std::fprintf(stderr,
                         "[warptempo_gui] parsed %zu markers from %s\n",
                         app.markers.markers().size(),
                         wm_path.string().c_str());
        }

        // Initial playhead: land at trim-begin if a b= marker was parsed,
        // otherwise sample 0. Must happen after marker parse so the trim
        // range reflects the on-disk state. Scroll the viewport so the
        // playhead is visible rather than lurking off the left edge.
        app.playhead_sample = trim_begin_sample();
        if (app.zoom_level != kFitFileLevel) {
            app.viewport_start_sample = app.playhead_sample;
            clamp_viewport_start(app, audio);
        }

        // Bring up the audio device bound to the new sample buffer. Init
        // failure disables playback but leaves the rest of the GUI usable.
        if (!playback.init(audio.sample_rate(), audio.channels(),
                           audio.samples_ptr(), audio.total_frames())) {
            std::fprintf(stderr,
                "warptempo_gui: playback disabled; space bar will no-op.\n");
        }

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

    // Follow-mode scroll: when the playhead drifts off the visible viewport
    // during playback, advance the viewport by one pixel-page so the cursor
    // stays in view. Only the first move beyond vp_end triggers a scroll,
    // and it lands the playhead ~10% into the new viewport so there's room
    // to keep playing.
    auto follow_scroll_if_needed = [&]() {
        const int64_t visible = samples_visible(app, audio);
        if (visible <= 0) return;
        const int64_t vp_end = app.viewport_start_sample + visible;
        if (app.playhead_sample < app.viewport_start_sample ||
            app.playhead_sample >= vp_end) {
            const int64_t lead = visible / 10;
            const int64_t old_vp = app.viewport_start_sample;
            app.viewport_start_sample =
                std::max<int64_t>(0, app.playhead_sample - lead);
            clamp_viewport_start(app, audio);
            if (app.viewport_start_sample != old_vp) invalidate_waveform_area();
        }
    };

    // Tick: runs once per event-loop iteration. During playback, snapshots
    // the audio thread's cursor and mirrors it into the main-thread playhead,
    // invalidating just the columns and timestamp that changed. Also
    // detects natural end-of-playback via the atomic playing flag.
    gui.set_on_tick([&]() {
        if (app.loading || audio.total_frames() <= 0) return;

        const bool ma_playing = playback.is_playing();
        if (!app.is_playing && !ma_playing) return;

        if (ma_playing) {
            const int64_t cur = playback.cursor();
            if (cur != app.playhead_sample) {
                const double old_px = playhead_pixel_x(app, audio);
                app.playback_cursor = cur;
                app.playhead_sample = cur;
                const double new_px = playhead_pixel_x(app, audio);
                invalidate_playhead_columns(old_px, new_px);
                invalidate_timestamp_area();
                if (app.follow_mode) follow_scroll_if_needed();
            }
            app.is_playing = true;
            return;
        }

        // Playing was true last tick, now false — natural end. Snap the
        // playhead to the final cursor position and refresh the column /
        // timestamp one last time.
        if (app.is_playing) {
            const int64_t cur = playback.cursor();
            const double old_px = playhead_pixel_x(app, audio);
            app.playback_cursor = cur;
            app.playhead_sample = cur;
            const double new_px = playhead_pixel_x(app, audio);
            invalidate_playhead_columns(old_px, new_px);
            invalidate_timestamp_area();
            if (app.follow_mode) follow_scroll_if_needed();
            app.is_playing = false;
        }
    });

    // Idle timeout: wake the poll loop every ~16 ms during playback so the
    // tick can advance the playhead even in the absence of input events.
    gui.set_idle_timeout_provider([&]() -> int {
        return (app.is_playing || playback.is_playing()) ? 16 : -1;
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
    // Tear the audio device down before the sample buffer goes out of scope.
    playback.shutdown();
    gui.shutdown();
    return 0;
}
