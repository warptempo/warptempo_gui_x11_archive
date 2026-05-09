#include "app_state.h"

#include "audio.h"
#include "paint_handler.h"
#include "render.h"

#include <cairo/cairo.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// X.7.7: promoted from a lambda in main(). Body is verbatim from the
// original at main.cpp:1969-1975, with `app.settings_passthrough`
// reached through the explicit AppState argument rather than a capture.
std::string settings_get(const AppState& app, const std::string& key,
                         const std::string& dflt) {
    for (const auto& kv : app.settings_passthrough) {
        if (kv.first == key) return kv.second;
    }
    return dflt;
}

// X.7.8a: promoted from a lambda in main(). Body is verbatim from the
// original at main.cpp:942-944, with the AppState reach reached through
// the explicit argument rather than a capture.
bool bottom_strip_wide(const AppState& app) {
    return app.prompt.active || !app.queue_progress_text.empty();
}

// X.7.8b-2: hit_test_* promoted from lambdas at main.cpp:1214-1368. Bodies
// are byte-identical to the originals modulo identifier spelling: the
// captured `app` and `audio` references are now explicit arguments. The
// kMarkerHitHalfPx / kFlagFontSize constants resolve through app_state.h /
// paint_handler.h respectively.

int hit_test_marker_line(const AppState& app, const GuiAudio& audio,
                         int mouse_x) {
    const GuiRect area = waveform_area(app);
    const double spp = current_samples_per_pixel(app, audio);
    if (spp <= 0.0) return -1;
    const int sr = audio.sample_rate();
    const int click_rel_x = mouse_x - area.x;
    const double vp = static_cast<double>(app.viewport_start_sample);
    const int64_t visible = samples_visible(app, audio);
    int best_hit = -1;
    int best_dist = kMarkerHitHalfPx + 1;
    const bool rv = app.render_view_enabled;
    // Brief F Section 3: in render-view, the visible sub-view's
    // list drives hit-testing. 'P' reads phase reset time_seconds and
    // converts to source frames via the source sample rate
    // (matching source-view's phase reset branch).
    const bool rv_trans = rv && app.active_mode == 'P';
    const int n =
        rv_trans
            ? static_cast<int>(app.render_view_phase_resets.size())
            : rv
                ? static_cast<int>(app.render_view_markers.size())
                : (app.active_mode == 'P')
                    ? static_cast<int>(app.phase_reset_markers.markers().size())
                    : static_cast<int>(app.warpmarkers.markers().size());
    for (int i = 0; i < n; ++i) {
        double ms;
        if (rv_trans) {
            ms = app.render_view_phase_resets[i].time_seconds *
                 static_cast<double>(sr);
        } else if (rv) {
            ms = app.render_view_markers[i].time_seconds *
                 static_cast<double>(sr);
        } else if (app.active_mode == 'P') {
            ms = app.phase_reset_markers.markers()[i].time_seconds *
                 static_cast<double>(sr);
        } else {
            ms = app.warpmarkers.markers()[i].time_seconds *
                 static_cast<double>(sr);
        }
        if (ms < vp) continue;
        if (ms >= vp + static_cast<double>(visible)) continue;
        const int m_px = static_cast<int>(std::nearbyint((ms - vp) / spp));
        const int d = std::abs(m_px - click_rel_x);
        if (d <= kMarkerHitHalfPx && d < best_dist) {
            best_dist = d;
            best_hit  = i;
        }
    }
    return best_hit;
}

int hit_test_flag(const AppState& app, const GuiAudio& audio,
                  int mouse_x, int mouse_y) {
    // Brief F Section 3: render-view's phase reset sub-view paints no
    // flags; short-circuit to no-hit so click and hover paths see a
    // bare top strip.
    if (app.render_view_enabled &&
        app.active_mode == 'P') {
        return -1;
    }
    const GuiRect area = waveform_area(app);
    const GuiRect top  = top_strip_area(app);
    cairo_surface_t* scratch_s = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* scratch_cr = cairo_create(scratch_s);
    const double spp = current_samples_per_pixel(app, audio);
    const int64_t vp_start = app.viewport_start_sample;
    const int64_t vp_end = vp_start +
        static_cast<int64_t>(std::nearbyint(spp * area.w));
    std::vector<FlagHitRect> rects;
    if (app.render_view_enabled) {
        rects = compute_flag_hit_rects(
            scratch_cr, top, app.render_view_markers,
            vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
    } else if (app.active_mode == 'P') {
        rects = compute_phase_reset_flag_hit_rects(
            scratch_cr, top, app.phase_reset_markers.markers(),
            vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
    } else {
        rects = compute_flag_hit_rects(
            scratch_cr, top, app.warpmarkers.markers(),
            vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
    }
    cairo_destroy(scratch_cr);
    cairo_surface_destroy(scratch_s);
    for (const auto& r : rects) {
        if (mouse_x >= r.x && mouse_x < r.x + r.w &&
            mouse_y >= r.y && mouse_y < r.y + r.h) {
            return r.marker_index;
        }
    }
    return -1;
}

int hit_test_iter_popup(const AppState& app, const GuiAudio& audio,
                        int mouse_x, int mouse_y,
                        double* out_text_left_x) {
    if (!app.iteration_mode_enabled) return -1;
    if (app.active_mode != 'W') return -1;
    const GuiRect area = waveform_area(app);
    const GuiRect top  = top_strip_area(app);
    cairo_surface_t* scratch_s = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* scratch_cr = cairo_create(scratch_s);
    const double spp = current_samples_per_pixel(app, audio);
    const int64_t vp_start = app.viewport_start_sample;
    const int64_t vp_end = vp_start +
        static_cast<int64_t>(std::nearbyint(spp * area.w));
    auto hits = compute_iter_popup_hits(
        scratch_cr, top, app.warpmarkers.markers(),
        vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
    cairo_destroy(scratch_cr);
    cairo_surface_destroy(scratch_s);
    for (const auto& h : hits) {
        if (mouse_x >= h.hit_rect.x &&
            mouse_x < h.hit_rect.x + h.hit_rect.w &&
            mouse_y >= h.hit_rect.y &&
            mouse_y < h.hit_rect.y + h.hit_rect.h) {
            if (out_text_left_x) {
                *out_text_left_x =
                    static_cast<double>(h.flag_rect.x);
            }
            return h.marker_index;
        }
    }
    return -1;
}

// X.7.8b-3: promoted from a lambda at the original main.cpp:794-812.
// Body is verbatim modulo identifier spelling: the captured `app`
// reference is now an explicit argument.
bool popup_eligible_marker(const AppState& app, int idx) {
    if (idx < 0) return false;
    if (app.render_view_enabled) {
        // In render-view, hover popups apply against the loaded
        // render's warpmarkers regardless of the pre-toggle mode.
        // Iteration-mode is forced off on toggle-in so its gate is
        // implicitly satisfied here too.
        const auto& mv = app.render_view_markers;
        if (idx >= static_cast<int>(mv.size())) return false;
        const auto& m = mv[idx];
        return m.tempo_inherits || !m.label_ref.empty();
    }
    if (app.active_mode != 'W') return false;
    if (app.iteration_mode_enabled) return false;
    const auto& mv = app.warpmarkers.markers();
    if (idx >= static_cast<int>(mv.size())) return false;
    const auto& m = mv[idx];
    return m.tempo_inherits || !m.label_ref.empty();
}

int hit_test_bpm_popup(const AppState& app, const GuiAudio& audio,
                       int mouse_x, int mouse_y,
                       double* out_text_left_x) {
    if (!app.bpm_mode_enabled) return -1;
    if (app.active_mode != 'W') return -1;
    const GuiRect area = waveform_area(app);
    const GuiRect top  = top_strip_area(app);
    cairo_surface_t* scratch_s = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* scratch_cr = cairo_create(scratch_s);
    const double spp = current_samples_per_pixel(app, audio);
    const int64_t vp_start = app.viewport_start_sample;
    const int64_t vp_end = vp_start +
        static_cast<int64_t>(std::nearbyint(spp * area.w));
    auto hits = compute_bpm_popup_hits(
        scratch_cr, top, app.warpmarkers.markers(),
        vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
    cairo_destroy(scratch_cr);
    cairo_surface_destroy(scratch_s);
    for (const auto& h : hits) {
        if (mouse_x >= h.hit_rect.x &&
            mouse_x < h.hit_rect.x + h.hit_rect.w &&
            mouse_y >= h.hit_rect.y &&
            mouse_y < h.hit_rect.y + h.hit_rect.h) {
            if (out_text_left_x) {
                *out_text_left_x =
                    static_cast<double>(h.flag_rect.x);
            }
            return h.marker_index;
        }
    }
    return -1;
}
