#include "app_state.h"
#include "audio.h"
#include "warpmarkers.h"
#include "flag_editor.h"
#include "playback.h"
#include "render.h"
#include "render_pipeline.h"
#include "render_view.h"
#include "selection.h"
#include "tab_mode.h"
#include "text_display.h"
#include "text_editor.h"
#include "transientmarkers.h"
#include "transientmarkers_ops.h"
#include "undo.h"
#include "viewport.h"
#include "warpmarkers_ops.h"
#include "x11.h"

#include <cairo/cairo.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sndfile.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int      kProgressBarHeight = 4;
constexpr double   kTopStripRatio     = 0.10;
constexpr double   kBottomStripRatio  = 0.10;
constexpr int      kChannelGapPx      = 2;
constexpr double   kFlagFontSize      = 13.0;

// Half-width in pixels of the selection-hit window when clicking on a marker.
constexpr int kMarkerHitHalfPx = 3;

// Double-click detection thresholds. X11 doesn't synthesize DoubleClick; we
// roll it from ButtonPress timing + position deltas.
constexpr int kDoubleClickMs      = 300;
constexpr int kDoubleClickPixels  = 5;

// Time the cursor must dwell on a popup-eligible flag rect before the
// hover popup appears. Distinct from kDoubleClickMs (point-event window
// vs continuous-state duration) even though they currently share a value.
constexpr int kHoverDelayMs       = 500;

// ms-per-pixel for each numeric zoom level. Level 0 is most zoomed in.
// kNumZoomLevels (in app_state.h) is the count of entries here; the
// static_assert below pins them together so the table can't drift.
constexpr double kZoomMsPerPixel[] = {
    1.25, 2.6, 5.2, 10.4, 20.8, 41.7, 83.3, 166.7
};
static_assert(sizeof(kZoomMsPerPixel) / sizeof(kZoomMsPerPixel[0])
              == static_cast<size_t>(kNumZoomLevels),
              "kZoomMsPerPixel size must match kNumZoomLevels");

// Timestamp text layout (bottom-left of the status strip).
constexpr int kTimestampPadX              = 8;
constexpr int kTimestampBaselineFromBottom = 12;
// Region width includes room for the A/B tab letter and the dirty indicator
// past the timestamp text edge.
constexpr int kTimestampRegionW           = 200;
constexpr int kTimestampRegionH           = 30;
constexpr double kDirtyGapPx              = 8.0;
constexpr double kTabLetterGapPx          = 10.0;

// Half-width of the column invalidated around a playhead position. Wide
// enough to cover the playhead line, the 12px-wide triangle indicator
// (±6 px of playhead_x), and subpixel rounding margin.
constexpr int kPlayheadHalfPx = 8;

// Pixel gap between the popup's top edge and the flag rect's top edge
// (mirrors text_display::kVerticalGapPx). Used for iteration popup
// hit-testing and vertical placement.
constexpr double kIterPopupVerticalGapPx = 4.0;
// V.B Addendum 2: extra inner padding on top/bottom of the iteration
// popup's bg rect (mirrors render.cpp::kVPadExtraPx and
// text_display.cpp::kVPadExtraPx). The flag hit-rect already grew by
// 2*kVPadExtraPx vertically, so the iter popup's hit_rect inherits that
// height and the gap to the flag rect is preserved automatically; the
// edit-state text baseline is shifted up by kIterPopupVPadExtraPx so the
// pending text clears the popup rect's bottom inner padding.
constexpr double kIterPopupVPadExtraPx = 1.0;

// Brief X.3 BPM-sweep math primitive. Given a span's measured duration
// (seconds), the user-asserted beat count for that span, and a target BPM,
// return the (base_tempo, scale) pair the engine needs so that one cell of
// the BPM sweep renders at exactly the target tempo.
//
// base_tempo is rounded to 2 decimals via banker's rounding (std::nearbyint
// with the default FE_TONEAREST mode); scale is rounded to 6 decimals the
// same way. The bash-script port uses an epsilon nudge before rounding to
// work around shell-level numerics — that nudge does not apply in C++ and
// is intentionally omitted here. The C++ port may diverge from the bash
// script on tie cases; this is documented behavior.
struct BaseTempoScale {
    double base_tempo;
    double scale;
    double ratio;
};

inline std::optional<BaseTempoScale> compute_base_tempo_scale(
    double duration_seconds, int beats, int target_bpm) {
    if (!(duration_seconds > 0.0)) return std::nullopt;
    if (beats      <= 0) return std::nullopt;
    if (target_bpm <= 0) return std::nullopt;

    const double desired_duration =
        static_cast<double>(beats) * 60.0 /
        static_cast<double>(target_bpm);
    if (!std::isfinite(desired_duration) ||
        desired_duration == 0.0) return std::nullopt;

    const double ratio = duration_seconds / desired_duration;
    if (!std::isfinite(ratio)) return std::nullopt;

    const double base_tempo =
        std::nearbyint(ratio * 100.0) / 100.0;
    if (!std::isfinite(base_tempo) ||
        base_tempo == 0.0) return std::nullopt;

    const double scale =
        std::nearbyint((ratio / base_tempo) * 1e6) / 1e6;
    if (!std::isfinite(scale)) return std::nullopt;

    return BaseTempoScale{base_tempo, scale, ratio};
}

// On-screen popup geometry for one iteration popup. `flag_rect` is the
// underlying flag's rect (used to anchor); `hit_rect` is the clickable
// region; `text` is the current popup text (for paint and seed-on-edit).
struct IterPopupHit {
    int          marker_index;
    GuiRect      flag_rect;
    GuiRect      hit_rect;
    std::string  text;
};

// Compute iteration popup hit-rects for visible owning markers in
// `top_strip_area`. Uses `compute_flag_hit_rects` for the underlying flag
// positions (so popups inherit the flag-strip greedy elision). Each
// popup sits kIterPopupVerticalGapPx above its flag's top edge. The
// hit_rect height matches the flag's height; width is the monospace
// extent of the popup's current text plus a small horizontal pad so
// edits with longer pending strings stay clickable.
inline std::vector<IterPopupHit> compute_iter_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<IterPopupHit> out;
    auto rects = compute_flag_hit_rects(
        cr, top_strip_area, markers,
        viewport_start_sample, viewport_end_sample,
        sample_rate, font_size);
    if (rects.empty()) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    // The widest possible iteration text drives a uniform hit-rect width
    // so popups don't visibly jiggle in size as values change. Matches
    // the [%+0.2f,%+0.2f] format with single-digit integer parts.
    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "[+0.00,+0.00]", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    // Greedy left-to-right elision over popup positions. Brief Y.4 sub-bug
    // B: collision is computed against the popup's actual painted-text
    // width plus 2 * kFlagInnerPadPx — i.e., the on-screen extent of the
    // bg-fill rect, not the uniform [+0.00,+0.00] hit_rect.w. The hit_rect
    // stays uniform-width so click targets are stable as values change;
    // pack and paint are separate concerns. With this rule, two adjacent
    // owning markers whose painted popup texts (e.g. "[ ]") don't actually
    // overlap will both render, even if their uniform hit rects do — which
    // matches the flag pack in iterate_visible_flags. No editor exemption.
    const double pop_pad = 4.0;
    double rightmost_right_edge = -1e18;
    for (const auto& r : rects) {
        const int idx = r.marker_index;
        if (idx < 0 || idx >= static_cast<int>(markers.size())) continue;
        if (!iter_popup_eligible_marker(markers[idx])) continue;
        IterPopupHit h;
        h.marker_index = idx;
        h.flag_rect.x = static_cast<int>(std::lround(r.x));
        h.flag_rect.y = static_cast<int>(std::lround(r.y));
        h.flag_rect.w = static_cast<int>(std::lround(r.w));
        h.flag_rect.h = static_cast<int>(std::lround(r.h));
        h.text = format_iter_bracket_text(markers[idx]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, h.text.c_str(), &ext);
        const int popup_w =
            static_cast<int>(std::ceil(uniform_ext.x_advance + 2 * hl_pad));
        const int popup_h = h.flag_rect.h;
        h.hit_rect.x = h.flag_rect.x;
        h.hit_rect.y = h.flag_rect.y -
            static_cast<int>(std::lround(kIterPopupVerticalGapPx)) -
            popup_h;
        h.hit_rect.w = popup_w;
        h.hit_rect.h = popup_h;

        // Pack collision uses the painted-extent width (matches the bg-
        // fill rect from sub-bug A), not h.hit_rect.w. By construction
        // the pack rule and the visual occlusion rule agree.
        const double pack_w = ext.x_advance + 2.0 * hl_pad;
        const double left = static_cast<double>(h.hit_rect.x);
        if (left < rightmost_right_edge + pop_pad) continue;
        rightmost_right_edge = left + pack_w;
        out.push_back(h);
    }
    cairo_restore(cr);
    return out;
}

// Brief X.2 BPM popup geometry. Mirrors IterPopupHit.
struct BpmPopupHit {
    int          marker_index;
    GuiRect      flag_rect;
    GuiRect      hit_rect;
    std::string  text;
};

// Brief X.2: compute BPM popup hit-rects for visible owning markers that
// carry a stored BPM value. Mirrors compute_iter_popup_hits in shape:
// uniform hit_rect.w sized to "99@[999,999]" so click targets are stable
// as values change; pack collision uses the painted-text width so static
// states pixel-match neighbors.
inline std::vector<BpmPopupHit> compute_bpm_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<BpmPopupHit> out;
    auto rects = compute_flag_hit_rects(
        cr, top_strip_area, markers,
        viewport_start_sample, viewport_end_sample,
        sample_rate, font_size);
    if (rects.empty()) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "99@[999,999]", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    const double pop_pad = 4.0;
    double rightmost_right_edge = -1e18;
    for (const auto& r : rects) {
        const int idx = r.marker_index;
        if (idx < 0 || idx >= static_cast<int>(markers.size())) continue;
        if (!bpm_popup_eligible_marker(markers[idx])) continue;
        if (!markers[idx].bpm_is_popup_owner) continue;
        BpmPopupHit h;
        h.marker_index = idx;
        h.flag_rect.x = static_cast<int>(std::lround(r.x));
        h.flag_rect.y = static_cast<int>(std::lround(r.y));
        h.flag_rect.w = static_cast<int>(std::lround(r.w));
        h.flag_rect.h = static_cast<int>(std::lround(r.h));
        h.text = format_bpm_bracket_text(markers[idx]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, h.text.c_str(), &ext);
        const int popup_w =
            static_cast<int>(std::ceil(uniform_ext.x_advance + 2 * hl_pad));
        const int popup_h = h.flag_rect.h;
        h.hit_rect.x = h.flag_rect.x;
        h.hit_rect.y = h.flag_rect.y -
            static_cast<int>(std::lround(kIterPopupVerticalGapPx)) -
            popup_h;
        h.hit_rect.w = popup_w;
        h.hit_rect.h = popup_h;

        const double pack_w = ext.x_advance + 2.0 * hl_pad;
        const double left = static_cast<double>(h.hit_rect.x);
        if (left < rightmost_right_edge + pop_pad) continue;
        rightmost_right_edge = left + pack_w;
        out.push_back(h);
    }
    cairo_restore(cr);
    return out;
}

// V.A3b hover-popup text. Computes the same resolution math the engine
// uses when emitting the .timemap, so the popup matches what the engine
// will produce. Pass markers emit "= TEMPO" or "= TEMPO*SCALE" (single
// equals; resolved tempo of the nearest prior owning marker). Label_ref
// markers emit "~= BASE*COMBINED_SCALE" (tilde-equals, mirroring engine
// behavior). BASE is rendered at 2 decimals; COMBINED_SCALE is
// `def_scale * multiplier` when the def has a typed scale, else just
// `multiplier`, rendered at 4 decimals. Returns "" when the marker
// doesn't qualify for a hover popup (owning, missing def, malformed).
inline std::string compute_hover_popup_text(
    const std::vector<GuiWarpMarker>& mv, int idx, int sample_rate) {
    if (idx < 0 || idx >= static_cast<int>(mv.size())) return "";
    const GuiWarpMarker& m = mv[idx];

    if (m.tempo_inherits) {
        // resolve_inherited_tempo walks backward from `walk-1`. Starting
        // at idx+1 lets it return idx's resolved tempo if idx happens to
        // be the only inheriting marker in front of an owning origin.
        const int walk = idx + 1;
        const double tval = resolve_inherited_tempo(mv, walk);
        const std::string sc = resolve_inherited_tempo_scale(mv, walk);
        char tbuf[32];
        std::snprintf(tbuf, sizeof(tbuf), "%.2f", tval);
        std::string out = "= ";
        out += tbuf;
        if (!sc.empty()) {
            out += "*";
            out += sc;
        }
        return out;
    }

    if (!m.label_ref.empty()) {
        int def_idx = -1;
        for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
            if (mv[i].label_def == m.label_ref) {
                def_idx = i;
                break;
            }
        }
        if (def_idx < 0) return "";
        if (def_idx + 1 >= static_cast<int>(mv.size())) return "";
        if (idx     + 1 >= static_cast<int>(mv.size())) return "";
        const double sr_d = static_cast<double>(sample_rate);
        if (sr_d <= 0.0) return "";

        const double lr_src_dist =
            (mv[idx + 1].time_seconds - mv[idx].time_seconds) * sr_d;
        const double def_src_dist =
            (mv[def_idx + 1].time_seconds - mv[def_idx].time_seconds) * sr_d;
        if (def_src_dist <= 0.0 || lr_src_dist <= 0.0) return "";

        const GuiWarpMarker& def = mv[def_idx];
        double      def_base;
        std::string def_scale_str;
        bool        def_has_typed_scale;
        if (def.tempo_inherits) {
            // Pass-def: fall back to inheritance walk. The resolved tempo
            // is treated as a fully-effective number with no separate
            // typed scale (inheritance returns base*scale).
            def_base = resolve_inherited_tempo(mv, def_idx);
            def_scale_str = "";
            def_has_typed_scale = false;
        } else {
            def_base = def.tempo_base;
            def_scale_str = def.tempo_scale;
            def_has_typed_scale = !def_scale_str.empty();
        }
        double def_scale_val = 1.0;
        if (def_has_typed_scale) {
            try { def_scale_val = std::stod(def_scale_str); }
            catch (...) { def_scale_val = 1.0; }
        }
        const double def_eff_tempo = def_base * def_scale_val;
        if (def_base == 0.0 || def_eff_tempo == 0.0) return "";

        // settings.scale cancels in the engine's multiplier expression:
        //   multiplier = (lr_src_dist * def_eff_tempo)
        //              / (def_base * def_src_dist)
        const double multiplier =
            (lr_src_dist * def_eff_tempo) / (def_base * def_src_dist);
        const double combined_scale = def_has_typed_scale
            ? (def_scale_val * multiplier)
            : multiplier;

        char base_buf[32];
        std::snprintf(base_buf, sizeof(base_buf), "%.2f", def_base);
        char scale_buf[32];
        std::snprintf(scale_buf, sizeof(scale_buf), "%.4f", combined_scale);
        std::string out = "~= ";
        out += base_buf;
        out += "*";
        out += scale_buf;
        return out;
    }

    return "";
}

// OpKind, UndoEntry, DragState, UndoHistory, PlayheadDragState,
// HoverPopupState, DialogTrigger, PromptState, ViewState, AppState live in
// app_state.h (extracted in brief X.7.1 alongside the Viewport struct).


// Parsed contents of .settings, separated into tab-handled keys (typed with
// presence flags so defaults can be applied per key) and the pass-through
// vector that preserves any other lines verbatim in their original order.
struct ParsedSettings {
    bool    has_tab_a_vp   = false;
    int64_t tab_a_vp       = 0;
    bool    has_tab_a_zoom = false;
    int     tab_a_zoom     = 0;
    bool    has_tab_a_ph   = false;
    int64_t tab_a_ph       = 0;
    bool    has_tab_b_vp   = false;
    int64_t tab_b_vp       = 0;
    bool    has_tab_b_zoom = false;
    int     tab_b_zoom     = 0;
    bool    has_tab_b_ph   = false;
    int64_t tab_b_ph       = 0;
    bool    has_follow     = false;
    bool    follow         = true;
    std::vector<std::pair<std::string, std::string>> passthrough;
};

// Off-screen pixel cache for the waveform subsystem. Lives for the life
// of the redraw lambda; recreated when the waveform area is resized;
// re-rendered when any input to render_waveform has changed. The main
// redraw just blits this surface onto the pixmap and paints markers /
// flags / playhead / timestamp on top. No implicit Cairo state from the
// main pixmap context leaks in — render_waveform does its own
// save/restore and does not depend on the caller's transform. (If a
// future chunk introduces a non-identity transform on the pixmap, this
// assumption must be revisited.)
struct WaveformCache {
    cairo_surface_t* surface = nullptr;
    int              width   = 0;     // surface width (== area.w when valid)
    int              height  = 0;     // surface height (== area.h when valid)

    // Fingerprint of the last successful render. Compared against the
    // current redraw's inputs to decide whether to re-render.
    int64_t   fp_vp_start   = 0;
    int64_t   fp_vp_end     = 0;
    int64_t   fp_trim_begin = 0;
    int64_t   fp_trim_end   = 0;
    int       fp_area_w     = 0;
    int       fp_area_h     = 0;
    long long fp_audio_gen  = -1;     // -1 = never rendered

    bool dirty = true;

    void destroy_surface() {
        if (surface) {
            cairo_surface_destroy(surface);
            surface = nullptr;
        }
        width  = 0;
        height = 0;
        dirty  = true;
        fp_audio_gen = -1;
    }

    ~WaveformCache() { destroy_surface(); }
};

} // namespace

// Geometry helpers — public to viewport.cpp via app_state.h. The strip-height
// helpers and samples_per_pixel_at remain main-private (`static`).

static int top_strip_height(int window_height) {
    return static_cast<int>(std::lround(window_height * kTopStripRatio));
}

static int bottom_strip_height(int window_height) {
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

// Scan warp markers and transients for begin_time / end_time flags; fall
// back to full file if either is absent. The S.1 invariant is that at most
// one b= and one e= exist across both files; if both lists somehow carry
// the same flag (only reachable via hand-edit), the warp-side value wins
// for determinism and a one-line stderr warning is emitted.
std::pair<long long, long long> compute_trim_samples(
    const std::vector<GuiWarpMarker>& warp_markers,
    const std::vector<GuiTransientMarker>& transients,
    int sample_rate, long long total_frames) {
    long long begin = 0;
    long long end   = total_frames;
    bool have_begin_warp  = false;
    bool have_end_warp    = false;
    bool have_begin_trans = false;
    bool have_end_trans   = false;

    for (const auto& m : warp_markers) {
        if (m.is_begin_time) {
            begin = static_cast<long long>(std::llround(
                m.time_seconds * static_cast<double>(sample_rate)));
            have_begin_warp = true;
        }
        if (m.is_end_time) {
            end = static_cast<long long>(std::llround(
                m.time_seconds * static_cast<double>(sample_rate)));
            have_end_warp = true;
        }
    }
    for (const auto& t : transients) {
        if (t.is_begin_time) {
            if (have_begin_warp) {
                have_begin_trans = true;
            } else {
                begin = static_cast<long long>(t.effective_frame());
                have_begin_trans = true;
            }
        }
        if (t.is_end_time) {
            if (have_end_warp) {
                have_end_trans = true;
            } else {
                end = static_cast<long long>(t.effective_frame());
                have_end_trans = true;
            }
        }
    }
    if (have_begin_warp && have_begin_trans) {
        std::fprintf(stderr,
            "warptempo_gui: duplicate b= flag (warp + transient); "
            "using warp value\n");
    }
    if (have_end_warp && have_end_trans) {
        std::fprintf(stderr,
            "warptempo_gui: duplicate e= flag (warp + transient); "
            "using warp value\n");
    }
    if (begin < 0) begin = 0;
    if (end > total_frames) end = total_frames;
    if (end < begin) end = begin;
    return {begin, end};
}

static double samples_per_pixel_at(int zoom_level,
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

// Apply a position delta to one transient's effective frame.
//   I:                src_frame += delta.
//   D, no displace:   set has_displacement=true unless the new effective
//                     equals src_frame (delta==0 → no-op).
//   D, with displace: update displaced_frame; if the new effective lands
//                     back on the anchor src_frame, revert the
//                     displacement.
// Caller is responsible for delta != 0; this is a noop on delta == 0.
void apply_transient_position_delta(GuiTransientMarker& m, int64_t delta) {
    if (delta == 0) return;
    if (m.is_inserted) {
        m.src_frame += delta;
        return;
    }
    const int64_t old_eff = m.effective_frame();
    const int64_t new_eff = old_eff + delta;
    if (new_eff == m.src_frame) {
        m.has_displacement = false;
        m.displaced_frame  = 0;
    } else {
        m.has_displacement = true;
        m.displaced_frame  = new_eff;
    }
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

bool rects_intersect(GuiRect a, GuiRect b) {
    if (a.x + a.w <= b.x || b.x + b.w <= a.x) return false;
    if (a.y + a.h <= b.y || b.y + b.h <= a.y) return false;
    return true;
}

GuiRect playhead_invalidate_rect(const GuiRect& area, double px_x) {
    const int col = static_cast<int>(std::floor(px_x + 0.5));
    const int x0 = std::max(area.x, col - kPlayheadHalfPx);
    const int x1 = std::min(area.x + area.w, col + kPlayheadHalfPx + 1);
    if (x1 <= x0) return GuiRect{area.x, 0, 0, 0};
    // Envelope extends up from the top of the window to the bottom of the
    // waveform area so it covers the playhead line inside the waveform AND
    // the chunk-P triangle indicator in the flag strip above it.
    const int y0 = 0;
    const int y1 = area.y + area.h;
    return GuiRect{x0, y0, x1 - x0, y1 - y0};
}

GuiRect timestamp_invalidate_rect(int window_height, int window_width,
                                  bool wide_strip) {
    if (wide_strip) {
        return GuiRect{0, window_height - kTimestampRegionH,
                       window_width, kTimestampRegionH};
    }
    return GuiRect{0, window_height - kTimestampRegionH,
                   kTimestampRegionW, kTimestampRegionH};
}

namespace {

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

std::string trim_ws(const std::string& s) {
    size_t a = 0;
    while (a < s.size() &&
           std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a &&
           std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parse_int64_full(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = static_cast<int64_t>(v);
    return true;
}

bool parse_int_full(const std::string& s, int& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    if (v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

// Parse `.settings`. Missing file → empty result (all has_* false, empty
// passthrough). Returns false only on a file-open failure of an existing
// file; per-line errors are silent-skip. Tab values are stored raw, without
// range validation — the caller clamps against the current audio file.
bool parse_settings_file(const std::string& path, ParsedSettings& out) {
    out = ParsedSettings{};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return true;  // nothing to load
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const std::string trimmed = trim_ws(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string key   = trim_ws(trimmed.substr(0, eq));
        const std::string value = trim_ws(trimmed.substr(eq + 1));
        if (key.empty()) continue;

        if (key == "tab_a_viewport_start") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_a_vp = true; out.tab_a_vp = v; }
        } else if (key == "tab_a_zoom") {
            int v;
            if (parse_int_full(value, v)) { out.has_tab_a_zoom = true; out.tab_a_zoom = v; }
        } else if (key == "tab_a_playhead") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_a_ph = true; out.tab_a_ph = v; }
        } else if (key == "tab_b_viewport_start") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_b_vp = true; out.tab_b_vp = v; }
        } else if (key == "tab_b_zoom") {
            int v;
            if (parse_int_full(value, v)) { out.has_tab_b_zoom = true; out.tab_b_zoom = v; }
        } else if (key == "tab_b_playhead") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_b_ph = true; out.tab_b_ph = v; }
        } else if (key == "follow") {
            std::string lower = value;
            for (char& c : lower) c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            if (lower == "true")       { out.has_follow = true; out.follow = true;  }
            else if (lower == "false") { out.has_follow = true; out.follow = false; }
            // Any other value: silent-skip; default (true) applies at the call site.
        } else {
            out.passthrough.emplace_back(key, value);
        }
    }
    return true;
}

// First-open default `.settings` template. Line ordering must match
// write_settings_file's output (passthrough fields first, then follow=,
// then the tab_a_* / tab_b_* triplets) — keep these in sync if either
// side changes.
std::string format_default_settings_template(const std::string& stem,
                                             const std::string& ext_no_dot) {
    std::string s;
    s += "title=";       s += stem; s += "-rendered\n";
    s += "audio_input="; s += stem; s += '.'; s += ext_no_dot; s += '\n';
    s += "scale=1.000000\n";
    s += "engine=warptempo\n";
    s += "N=4096\n";
    s += "fftw_threads=16\n";
    s += "transients_tau_back_ms=30.0\n";
    s += "limiter_enabled=false\n";
    s += "follow=true\n";
    s += "tab_a_viewport_start=0\n";
    s += "tab_a_zoom=0\n";
    s += "tab_a_playhead=0\n";
    s += "tab_b_viewport_start=0\n";
    s += "tab_b_zoom=0\n";
    s += "tab_b_playhead=0\n";
    return s;
}

// Atomic write: pass-through lines first in their original order, then the
// six canonical tab lines. Matches the `.warpmarkers` write pattern
// (tmp → fsync → rename). Best-effort: failure is logged by the caller.
bool write_settings_file(
    const std::string& path,
    const ViewState& tab_a,
    const ViewState& tab_b,
    bool follow,
    const std::vector<std::pair<std::string, std::string>>& passthrough) {
    std::string data;
    for (const auto& kv : passthrough) {
        data += kv.first;
        data += '=';
        data += kv.second;
        data += '\n';
    }
    data += "follow=";
    data += follow ? "true" : "false";
    data += '\n';
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_a.viewport_start_sample));
    data += "tab_a_viewport_start="; data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%d", tab_a.zoom_level);
    data += "tab_a_zoom=";            data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_a.playhead_sample));
    data += "tab_a_playhead=";        data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_b.viewport_start_sample));
    data += "tab_b_viewport_start="; data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%d", tab_b.zoom_level);
    data += "tab_b_zoom=";            data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_b.playhead_sample));
    data += "tab_b_playhead=";        data += buf; data += '\n';

    mode_t mode = 0644;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;

    const std::string tmp_path = path + ".tmp";
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;

    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written,
                                  data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp_path.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp_path.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    ::chmod(tmp_path.c_str(), mode);
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    return true;
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

    AppState     app;
    GuiAudio     audio;
    GuiPlayback  playback;
    GuiX11       gui;
    WaveformCache wf_cache;
    if (!gui.init(app.width, app.height, "Warptempo")) {
        return 1;
    }

    // -- Viewport + invalidation helpers ------------------------------------
    //
    // X.7.1: the viewport-mutator and invalidation lambdas have been hoisted
    // onto the Viewport struct in viewport.{cpp,h}. The lambdas below are
    // one-line forwarders so callsites elsewhere in main() don't need to
    // change. `bottom_strip_wide` stays inline because it's still called
    // from the redraw lambda; `invalidate_timestamp_area` is gone — its
    // body was byte-identical to invalidate_timestamp_area, so all of its
    // former callsites now call invalidate_timestamp_area directly.

    auto bottom_strip_wide = [&]() -> bool {
        return app.prompt.active || !app.queue_progress_text.empty();
    };

    // V.A3b Addendum 3: forward-declared so the viewport methods below can
    // invoke it. The body is assigned later (after hit_test_flag and
    // clear_hover_popup are in scope). Guarded inside Viewport with a
    // truthiness check because callbacks are wired after this assignment.
    std::function<void()> recompute_hover_at_cursor;

    // X.7.3: forward-declared so the Undo struct can capture references.
    // Bodies are assigned later at their original definition sites — same
    // forward-declare-then-assign pattern as recompute_hover_at_cursor.
    std::function<void()> clear_hover_popup;
    std::function<void()> stop_playback_if_playing;

    // X.7.4: forward-declared so the Transients struct can capture
    // references. Bodies are assigned later at their original definition
    // sites — same pattern as clear_hover_popup / stop_playback_if_playing.
    std::function<FlagLoc(bool, bool, int)> find_flag;
    std::function<void()>                    open_prompt_detect_confirm;

    // X.7.6: forward-declared so GuiRenderView can capture a reference.
    // Body is assigned later at its original definition site — same
    // pattern as the four prior promotions.
    std::function<void()> refresh_active_tab_from_app;

    Viewport viewport(app, audio, gui, playback, recompute_hover_at_cursor);
    Selection selection(app, audio, viewport, playback);
    Undo undo(app, viewport, selection,
              clear_hover_popup, stop_playback_if_playing);
    GuiTransientMarkersOps transients(app, audio, viewport, selection, undo,
                                      clear_hover_popup, stop_playback_if_playing,
                                      find_flag, open_prompt_detect_confirm);
    GuiWarpMarkersOps warpops(app, audio, gui, viewport, selection, undo,
                              clear_hover_popup, stop_playback_if_playing,
                              find_flag);
    GuiFlagEditor flag_editor(app, viewport, undo, clear_hover_popup);
    GuiRenderView render_view(app, audio, playback, gui, selection,
                              clear_hover_popup, refresh_active_tab_from_app);
    GuiTabMode tab_mode(app, audio, viewport, selection,
                        clear_hover_popup, stop_playback_if_playing);

    auto trim_begin_sample           = [&]() { return viewport.trim_begin_sample(); };
    auto trim_end_sample             = [&]() { return viewport.trim_end_sample(); };
    auto invalidate_timestamp_area   = [&]() { viewport.invalidate_timestamp_area(); };
    auto invalidate_playhead_columns = [&](double a, double b) { viewport.invalidate_playhead_columns(a, b); };
    auto move_playhead_to            = [&](int64_t s) { viewport.move_playhead_to(s); };
    auto move_playhead_pixels        = [&](int dx) { viewport.move_playhead_pixels(dx); };
    auto zoom_in                     = [&]() { viewport.zoom_in(); };
    auto zoom_out                    = [&]() { viewport.zoom_out(); };
    auto scroll_viewport             = [&](int64_t d) { viewport.scroll_viewport(d); };
    auto center_viewport_on_playhead = [&]() { viewport.center_viewport_on_playhead(); };
    auto follow_scroll_if_needed     = [&]() { viewport.follow_scroll_if_needed(); };

    // -- Redraw -------------------------------------------------------------

    gui.set_on_redraw([&](cairo_t* cr, int x, int y, int w, int h) {
        using clock = std::chrono::steady_clock;
        const auto t_start = clock::now();

        if constexpr (kDebugPerf) perf_counters::reset();

        double t_waveform_ms = 0.0;
        double t_markers_ms  = 0.0;
        double t_flags_ms    = 0.0;
        double t_playhead_ms = 0.0;
        double t_ts_ms       = 0.0;
        double t_dirty_ms    = 0.0;
        double t_flush_ms    = 0.0;

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

            // In render-view the audio buffer is already render-domain
            // (trim already baked in at render time, b=/e= flags stripped
            // from the .renderwarpmarkers/.rendertransientmarkers
            // sidecars). The
            // source's authoring markers carry b=/e= in source-frame
            // coordinates that don't map onto the rendered audio's
            // timeline, so feeding them to compute_trim_samples here
            // produces a patchy color split. Use the render-view markers
            // instead, which collapse to [0, total_frames] and dim
            // nothing.
            const auto trim = app.render_view_enabled
                ? compute_trim_samples(
                      app.render_view_markers, app.render_view_transients,
                      sr, audio.total_frames())
                : compute_trim_samples(
                      app.warpmarkers.markers(), app.transientmarkers.markers(),
                      sr, audio.total_frames());
            const int64_t trim_begin = trim.first;
            const int64_t trim_end   = trim.second;
            const TrimRange trim_struct{trim_begin, trim_end};

            const int rc = audio.render_channels();
            {
                const auto wf0 = clock::now();

                // Cache surface lifecycle: (re)create when dimensions don't
                // match the current waveform area. Size mismatch implies a
                // window resize; content is stale regardless.
                if (!wf_cache.surface ||
                    wf_cache.width  != area.w ||
                    wf_cache.height != area.h) {
                    wf_cache.destroy_surface();
                    if (area.w > 0 && area.h > 0) {
                        wf_cache.surface = cairo_image_surface_create(
                            CAIRO_FORMAT_ARGB32, area.w, area.h);
                        wf_cache.width  = area.w;
                        wf_cache.height = area.h;
                        wf_cache.dirty  = true;
                    }
                }

                // Cache invalidation: any change to the inputs of
                // render_waveform forces a re-render. Checked here (not at
                // mutation sites) so new mutation paths can never forget.
                if (wf_cache.surface &&
                    (wf_cache.fp_audio_gen  != app.audio_generation ||
                     wf_cache.fp_vp_start   != vp_start             ||
                     wf_cache.fp_vp_end     != vp_end               ||
                     wf_cache.fp_trim_begin != trim_begin           ||
                     wf_cache.fp_trim_end   != trim_end             ||
                     wf_cache.fp_area_w     != area.w               ||
                     wf_cache.fp_area_h     != area.h)) {
                    wf_cache.dirty = true;
                }

                if (wf_cache.surface && wf_cache.dirty) {
                    cairo_t* ccr = cairo_create(wf_cache.surface);
                    // Clear to transparent — the pixmap's background fill
                    // shows through wherever the waveform strokes don't paint.
                    cairo_save(ccr);
                    cairo_set_operator(ccr, CAIRO_OPERATOR_CLEAR);
                    cairo_paint(ccr);
                    cairo_restore(ccr);
                    const GuiRect cache_area{0, 0, area.w, area.h};
                    if (rc == 1) {
                        render_waveform(ccr, cache_area, audio, 0,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveform, dim(kWaveform));
                    } else if (rc >= 2) {
                        const int ch_h = (cache_area.h - kChannelGapPx) / 2;
                        const GuiRect ch0{0, 0, cache_area.w, ch_h};
                        const GuiRect ch1{0, ch_h + kChannelGapPx,
                                          cache_area.w, ch_h};
                        render_waveform(ccr, ch0, audio, 0,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveform, dim(kWaveform));
                        render_waveform(ccr, ch1, audio, 1,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveform, dim(kWaveform));
                    }
                    cairo_destroy(ccr);
                    wf_cache.fp_audio_gen  = app.audio_generation;
                    wf_cache.fp_vp_start   = vp_start;
                    wf_cache.fp_vp_end     = vp_end;
                    wf_cache.fp_trim_begin = trim_begin;
                    wf_cache.fp_trim_end   = trim_end;
                    wf_cache.fp_area_w     = area.w;
                    wf_cache.fp_area_h     = area.h;
                    wf_cache.dirty = false;
                }

                // Blit the cache into the pixmap, clipped to the exposed
                // rect's intersection with the waveform area. Cairo handles
                // the intersection via the outer clip plus this inner clip.
                if (wf_cache.surface && rects_intersect(exposed, area)) {
                    cairo_save(cr);
                    cairo_rectangle(cr, area.x, area.y, area.w, area.h);
                    cairo_clip(cr);
                    cairo_set_source_surface(cr, wf_cache.surface,
                                             area.x, area.y);
                    cairo_paint(cr);
                    cairo_restore(cr);
                }

                const auto wf1 = clock::now();
                t_waveform_ms =
                    std::chrono::duration<double, std::milli>(wf1 - wf0).count();
            }

            // Markers: vertical lines in the waveform area, beneath the
            // playhead. Cairo's outer clip confines painting to `exposed`.
            if (rects_intersect(exposed, area) ||
                rects_intersect(exposed, top_strip)) {
                const auto m0 = clock::now();
                if (app.render_view_enabled) {
                    // Render-view: dark blue base, sky-tint when selected.
                    // The render's warpmarkers list is strict-monotonic on
                    // time_seconds (engine-written), so render_markers'
                    // usual ordering assumption holds. Selection is
                    // visual-only — it does not flow into commit.
                    // Brief F Section 3: when sub-mode is 'T', paint the
                    // render's transient list using the transient renderer
                    // (matches source-view's transient appearance).
                    if (app.active_mode == 'T') {
                        render_transient_markers(
                            cr, area, app.render_view_transients,
                            vp_start, vp_end, sr,
                            trim_struct);
                    } else {
                        render_markers(cr, area, app.render_view_markers,
                                       vp_start, vp_end, sr,
                                       trim_struct);
                    }
                } else if (app.active_mode == 'T') {
                    render_transient_markers(
                        cr, area, app.transientmarkers.markers(),
                        vp_start, vp_end, sr,
                        trim_struct);
                } else {
                    render_markers(cr, area, app.warpmarkers.markers(),
                                   vp_start, vp_end, sr,
                                   trim_struct);
                }
                const auto m1 = clock::now();
                t_markers_ms =
                    std::chrono::duration<double, std::milli>(m1 - m0).count();
            }

            // Brief E: precompute the playhead's pixel column so flag
            // renderers can light the outline of the marker the playhead
            // sits on. Same value is reused by render_playhead below.
            const double px_x = playhead_pixel_x(app, audio);

            // Flag annotations in the top strip.
            if (rects_intersect(exposed, top_strip)) {
                const auto f0 = clock::now();
                if (app.render_view_enabled) {
                    // Render-view: dark-blue flags, no editor overlay.
                    // Selection is visual-only (sky-tint on selected,
                    // dark-blue otherwise). Iteration popups are
                    // suppressed by the iteration_mode_enabled toggle
                    // being forced false on entry to render-view.
                    // Sub-mode 'T' (transients): paint via
                    // render_transient_flags from app.render_view_transients
                    // (no popups; transient markers are not popup-eligible).
                    if (app.active_mode != 'T') {
                    render_flags(cr, top_strip, app.render_view_markers,
                                 vp_start, vp_end, sr,
                                 kFlagFontSize,
                                 app.selected_markers,
                                 trim_struct,
                                 px_x,
                                 FlagEditorOverlay{});

                    // V.A3b hover popup paint, render-view variant.
                    // Mirrors the source-view branch below but reads
                    // from app.render_view_markers and uses the cached
                    // source sample rate (the render's audio sr is
                    // typically equal but the brief specifies source-
                    // axis presentation).
                    if (app.hover_popup.visible) {
                        const auto& mv = app.render_view_markers;
                        const int hidx = app.hover_popup.marker_index;
                        const bool eligible =
                            (hidx >= 0 &&
                             hidx < static_cast<int>(mv.size()) &&
                             (mv[hidx].tempo_inherits ||
                              !mv[hidx].label_ref.empty()) &&
                             !app.hover_popup.cached_text.empty());
                        if (eligible) {
                            auto rects = compute_flag_hit_rects(
                                cr, top_strip, mv,
                                vp_start, vp_end, sr, kFlagFontSize);
                            GuiRect anchor{0, 0, 0, 0};
                            for (const auto& r : rects) {
                                if (r.marker_index == hidx) {
                                    anchor.x = static_cast<int>(
                                        std::lround(r.x)) +
                                        static_cast<int>(kFlagInnerPadPx);
                                    anchor.y = static_cast<int>(
                                        std::lround(r.y));
                                    anchor.w = static_cast<int>(
                                        std::lround(r.w));
                                    anchor.h = static_cast<int>(
                                        std::lround(r.h));
                                    break;
                                }
                            }
                            if (anchor.w > 0 && anchor.h > 0) {
                                const int64_t pos = static_cast<int64_t>(
                                    std::llround(
                                        mv[hidx].time_seconds *
                                        static_cast<double>(sr)));
                                const bool oot =
                                    marker_out_of_trim(pos, trim_struct);
                                text_display::State td;
                                td.anchor   = anchor;
                                td.content  = app.hover_popup.cached_text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    text_display::Position::Top;
                                text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }
                    } else {
                        render_transient_flags(
                            cr, top_strip, app.render_view_transients,
                            vp_start, vp_end, sr,
                            kFlagFontSize,
                            app.selected_markers,
                            trim_struct,
                            px_x);
                    }
                } else if (app.active_mode == 'T') {
                    render_transient_flags(
                        cr, top_strip, app.transientmarkers.markers(),
                        vp_start, vp_end, sr,
                        kFlagFontSize,
                        app.selected_markers,
                        trim_struct,
                        px_x);
                } else {
                    FlagEditorOverlay overlay;
                    // Only the V.A1 FlagPayload kind paints into the flag
                    // rect; the V.B IterationBracket kind owns the popup
                    // above the rect and leaves the flag's normal text
                    // alone. When the iter popup is the focused editor
                    // target, the flag rect below must suppress its
                    // last-selected highlight (V.B Addendum 2).
                    if (text_editor::is_active(app.top_flag_editor) &&
                        app.top_flag_editor.kind ==
                            text_editor::Kind::FlagPayload) {
                        overlay.marker_index   = app.top_flag_editor.target;
                        overlay.pending        = app.top_flag_editor.pending;
                        overlay.cursor_pos     = app.top_flag_editor.cursor_pos;
                        overlay.is_red         = app.top_flag_editor.red;
                        overlay.cursor_visible =
                            text_editor::cursor_visible_now(
                                app.top_flag_editor);
                    } else if (text_editor::is_active(app.top_flag_editor) &&
                               app.top_flag_editor.kind ==
                                   text_editor::Kind::IterationBracket) {
                        overlay.popup_editor_target =
                            app.top_flag_editor.target;
                    } else if (text_editor::is_active(app.top_flag_editor) &&
                               app.top_flag_editor.kind ==
                                   text_editor::Kind::BpmBracket) {
                        // Brief X.2: same flag-rect highlight suppression
                        // as iter — the popup above owns the highlight.
                        // Modes are mutually exclusive so the shared
                        // popup_editor_target channel is safe.
                        overlay.popup_editor_target =
                            app.top_flag_editor.target;
                    }
                    render_flags(cr, top_strip, app.warpmarkers.markers(),
                                 vp_start, vp_end, sr,
                                 kFlagFontSize,
                                 app.selected_markers,
                                 trim_struct,
                                 px_x,
                                 overlay);

                    // V.A3b hover popup. Drawn on top of the flag strip,
                    // strictly after render_flags. Motion + tick already
                    // gate visibility; redraw just paints what state says.
                    // The popup text was precomputed at hover-entry into
                    // `app.hover_popup.cached_text` so this redraw branch
                    // doesn't have to repeat the parser-mirroring math.
                    if (app.hover_popup.visible &&
                        !app.iteration_mode_enabled) {
                        const auto& mv = app.warpmarkers.markers();
                        const int hidx = app.hover_popup.marker_index;
                        const bool eligible =
                            (hidx >= 0 &&
                             hidx < static_cast<int>(mv.size()) &&
                             (mv[hidx].tempo_inherits ||
                              !mv[hidx].label_ref.empty()) &&
                             !app.hover_popup.cached_text.empty());
                        if (eligible) {
                            auto rects = compute_flag_hit_rects(
                                cr, top_strip, mv,
                                vp_start, vp_end, sr, kFlagFontSize);
                            GuiRect anchor{0, 0, 0, 0};
                            for (const auto& r : rects) {
                                if (r.marker_index == hidx) {
                                    // anchor.x is the flag rect's text-origin
                                    // (rect's geometric x + render_flags' hl_pad
                                    // = kFlagInnerPadPx), so the popup's
                                    // leading character sits at the same column
                                    // as the flag's leading character.
                                    anchor.x = static_cast<int>(std::lround(r.x)) +
                                               static_cast<int>(kFlagInnerPadPx);
                                    anchor.y = static_cast<int>(std::lround(r.y));
                                    anchor.w = static_cast<int>(std::lround(r.w));
                                    anchor.h = static_cast<int>(std::lround(r.h));
                                    break;
                                }
                            }
                            if (anchor.w > 0 && anchor.h > 0) {
                                const int64_t pos = static_cast<int64_t>(
                                    std::llround(
                                        mv[hidx].time_seconds *
                                        static_cast<double>(sr)));
                                const bool oot =
                                    marker_out_of_trim(pos, trim_struct);
                                text_display::State td;
                                td.anchor   = anchor;
                                td.content  = app.hover_popup.cached_text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    text_display::Position::Top;
                                text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }

                    // V.B iteration popups. Persistent per-flag annotations
                    // when iteration mode is on. Each owning marker gets a
                    // popup above its flag rect; pass markers and label_ref
                    // markers are excluded (no own tempo to vary). When the
                    // top_flag_editor is active in IterationBracket kind on
                    // marker `T`, popup `T` paints the editor's pending
                    // text (with the [] brackets visible during edit) and
                    // a 1-px cursor at cursor_pos; other popups paint
                    // their formatted iter text normally.
                    if (app.iteration_mode_enabled) {
                        const auto& mv = app.warpmarkers.markers();
                        auto hits = compute_iter_popup_hits(
                            cr, top_strip, mv,
                            vp_start, vp_end, sr, kFlagFontSize);
                        const bool editor_on_iter =
                            text_editor::is_active(app.top_flag_editor) &&
                            app.top_flag_editor.kind ==
                                text_editor::Kind::IterationBracket;
                        // Brief Y.5: paint hits in REVERSE so the leftmost
                        // popup paints last (on top). The compute_iter_popup
                        // _hits pack walks left-to-right and elides right-of-
                        // collision popups; reverse paint makes the editor's
                        // widening pending text occlude its right neighbor
                        // rather than vice-versa, matching the leftmost-wins
                        // pack rule. In static (non-edit) states the bg-fills
                        // are kBackground and text rects don't overlap, so
                        // the pixels are identical to the previous order.
                        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                            const auto& h = *it;
                            // Anchor for text_display: x at the flag's
                            // text-origin (flag.x + kFlagInnerPadPx, mirrors
                            // hover popup), y/w/h from the flag rect itself.
                            GuiRect anchor{
                                h.flag_rect.x +
                                    static_cast<int>(kFlagInnerPadPx),
                                h.flag_rect.y,
                                h.flag_rect.w,
                                h.flag_rect.h
                            };
                            const int64_t pos = static_cast<int64_t>(
                                std::llround(
                                    mv[h.marker_index].time_seconds *
                                    static_cast<double>(sr)));
                            const bool oot =
                                marker_out_of_trim(pos, trim_struct);
                            if (editor_on_iter &&
                                app.top_flag_editor.target == h.marker_index) {
                                // Editor branch: state 2/3 of the three-state
                                // model. Background fills with kAccent on
                                // parse failure, otherwise kMarker; text and
                                // (blink-gated) cursor in kText. Out-of-trim
                                // wraps every color in dim() uniformly.
                                const std::string& pending =
                                    app.top_flag_editor.pending;
                                cairo_save(cr);
                                cairo_select_font_face(cr, "monospace",
                                    CAIRO_FONT_SLANT_NORMAL,
                                    CAIRO_FONT_WEIGHT_NORMAL);
                                cairo_set_font_size(cr, kFlagFontSize);
                                cairo_text_extents_t pext;
                                cairo_text_extents(cr, pending.c_str(), &pext);
                                cairo_text_extents_t uext;
                                cairo_text_extents(cr, "[+0.00,+0.00]", &uext);
                                const double hl_pad = kFlagInnerPadPx;
                                const double bg_w =
                                    pext.x_advance + 2.0 * hl_pad;
                                const double bg_x =
                                    static_cast<double>(anchor.x) - hl_pad;
                                const double bg_y =
                                    static_cast<double>(h.hit_rect.y);
                                const double bg_h =
                                    static_cast<double>(h.hit_rect.h);
                                // Brief Y.4 sub-bug A: opaque canvas-bg
                                // fill under the popup text, drawn before
                                // the colored outline below. Width tracks
                                // pending text + 2 * kFlagInnerPadPx, so
                                // it occludes neighbor popup text once
                                // pending widens past the original [ ].
                                render_flag_text_bg_fill(cr,
                                    static_cast<double>(anchor.x),
                                    pext.x_advance, bg_y, bg_h);
                                GuiColor bg_col = app.top_flag_editor.red
                                    ? kAccent : kMarker;
                                if (oot) bg_col = dim(bg_col);
                                cairo_set_source_rgb(cr,
                                    bg_col.r, bg_col.g, bg_col.b);
                                const double sx = std::round(bg_x) + 0.5;
                                const double sy = std::round(bg_y) + 0.5;
                                const int sw = static_cast<int>(
                                    std::round(bg_w));
                                const int sh = static_cast<int>(
                                    std::round(bg_h));
                                cairo_set_line_width(cr, 1.0);
                                cairo_rectangle(cr, sx, sy,
                                    static_cast<double>(sw),
                                    static_cast<double>(sh));
                                cairo_stroke(cr);

                                const double baseline_y =
                                    static_cast<double>(anchor.y)
                                  - kIterPopupVerticalGapPx
                                  - kIterPopupVPadExtraPx
                                  - (uext.height + uext.y_bearing);
                                const GuiColor txt = oot ? dim(kText) : kText;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_move_to(cr,
                                    static_cast<double>(anchor.x), baseline_y);
                                cairo_show_text(cr, pending.c_str());

                                if (text_editor::cursor_visible_now(
                                        app.top_flag_editor)) {
                                    std::string left = pending.substr(
                                        0, static_cast<size_t>(
                                            app.top_flag_editor.cursor_pos));
                                    cairo_text_extents_t lext;
                                    cairo_text_extents(cr, left.c_str(), &lext);
                                    const double cx =
                                        static_cast<double>(anchor.x) +
                                        lext.x_advance;
                                    cairo_set_source_rgb(cr,
                                        txt.r, txt.g, txt.b);
                                    cairo_set_line_width(cr, 1.0);
                                    cairo_move_to(cr, cx, bg_y);
                                    cairo_line_to(cr, cx, bg_y + bg_h);
                                    cairo_stroke(cr);
                                }
                                cairo_restore(cr);
                            } else {
                                // Brief Y.4 sub-bug A: paint the canvas-bg
                                // fill under the popup text before the
                                // text is drawn. In the static (non-edit)
                                // case the fill matches strip-clear
                                // exactly, so pixels are identical to
                                // today; the fill does the occlusion
                                // work only when an adjacent popup is
                                // being edited and grows over this one.
                                cairo_save(cr);
                                cairo_select_font_face(cr, "monospace",
                                    CAIRO_FONT_SLANT_NORMAL,
                                    CAIRO_FONT_WEIGHT_NORMAL);
                                cairo_set_font_size(cr, kFlagFontSize);
                                cairo_text_extents_t hext;
                                cairo_text_extents(cr, h.text.c_str(), &hext);
                                render_flag_text_bg_fill(cr,
                                    static_cast<double>(anchor.x),
                                    hext.x_advance,
                                    static_cast<double>(h.hit_rect.y),
                                    static_cast<double>(h.hit_rect.h));
                                cairo_restore(cr);

                                text_display::State td;
                                td.anchor   = anchor;
                                td.content  = h.text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    text_display::Position::Top;
                                text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }

                    // Brief X.2 BPM popups. Parallel to the iter block
                    // above; mutually exclusive with iteration mode (only
                    // one mode's popups paint at a time). At most one
                    // marker has bpm_is_popup_owner=true so hits is normally
                    // a single entry, but the reverse-paint and
                    // bg-fill-under-text patterns mirror iter for
                    // consistency.
                    if (app.bpm_mode_enabled) {
                        const auto& mv = app.warpmarkers.markers();
                        auto hits = compute_bpm_popup_hits(
                            cr, top_strip, mv,
                            vp_start, vp_end, sr, kFlagFontSize);
                        const bool editor_on_bpm =
                            text_editor::is_active(app.top_flag_editor) &&
                            app.top_flag_editor.kind ==
                                text_editor::Kind::BpmBracket;
                        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                            const auto& h = *it;
                            GuiRect anchor{
                                h.flag_rect.x +
                                    static_cast<int>(kFlagInnerPadPx),
                                h.flag_rect.y,
                                h.flag_rect.w,
                                h.flag_rect.h
                            };
                            const int64_t pos = static_cast<int64_t>(
                                std::llround(
                                    mv[h.marker_index].time_seconds *
                                    static_cast<double>(sr)));
                            const bool oot =
                                marker_out_of_trim(pos, trim_struct);
                            if (editor_on_bpm &&
                                app.top_flag_editor.target == h.marker_index) {
                                const std::string& pending =
                                    app.top_flag_editor.pending;
                                cairo_save(cr);
                                cairo_select_font_face(cr, "monospace",
                                    CAIRO_FONT_SLANT_NORMAL,
                                    CAIRO_FONT_WEIGHT_NORMAL);
                                cairo_set_font_size(cr, kFlagFontSize);
                                cairo_text_extents_t pext;
                                cairo_text_extents(cr, pending.c_str(), &pext);
                                cairo_text_extents_t uext;
                                cairo_text_extents(cr, "99@[999,999]", &uext);
                                const double hl_pad = kFlagInnerPadPx;
                                const double bg_w =
                                    pext.x_advance + 2.0 * hl_pad;
                                const double bg_x =
                                    static_cast<double>(anchor.x) - hl_pad;
                                const double bg_y =
                                    static_cast<double>(h.hit_rect.y);
                                const double bg_h =
                                    static_cast<double>(h.hit_rect.h);
                                render_flag_text_bg_fill(cr,
                                    static_cast<double>(anchor.x),
                                    pext.x_advance, bg_y, bg_h);
                                GuiColor bg_col = app.top_flag_editor.red
                                    ? kAccent : kMarker;
                                if (oot) bg_col = dim(bg_col);
                                cairo_set_source_rgb(cr,
                                    bg_col.r, bg_col.g, bg_col.b);
                                const double sx = std::round(bg_x) + 0.5;
                                const double sy = std::round(bg_y) + 0.5;
                                const int sw = static_cast<int>(
                                    std::round(bg_w));
                                const int sh = static_cast<int>(
                                    std::round(bg_h));
                                cairo_set_line_width(cr, 1.0);
                                cairo_rectangle(cr, sx, sy,
                                    static_cast<double>(sw),
                                    static_cast<double>(sh));
                                cairo_stroke(cr);

                                const double baseline_y =
                                    static_cast<double>(anchor.y)
                                  - kIterPopupVerticalGapPx
                                  - kIterPopupVPadExtraPx
                                  - (uext.height + uext.y_bearing);
                                const GuiColor txt = oot ? dim(kText) : kText;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_move_to(cr,
                                    static_cast<double>(anchor.x), baseline_y);
                                cairo_show_text(cr, pending.c_str());

                                if (text_editor::cursor_visible_now(
                                        app.top_flag_editor)) {
                                    std::string left = pending.substr(
                                        0, static_cast<size_t>(
                                            app.top_flag_editor.cursor_pos));
                                    cairo_text_extents_t lext;
                                    cairo_text_extents(cr, left.c_str(), &lext);
                                    const double cx =
                                        static_cast<double>(anchor.x) +
                                        lext.x_advance;
                                    cairo_set_source_rgb(cr,
                                        txt.r, txt.g, txt.b);
                                    cairo_set_line_width(cr, 1.0);
                                    cairo_move_to(cr, cx, bg_y);
                                    cairo_line_to(cr, cx, bg_y + bg_h);
                                    cairo_stroke(cr);
                                }
                                cairo_restore(cr);
                            } else {
                                cairo_save(cr);
                                cairo_select_font_face(cr, "monospace",
                                    CAIRO_FONT_SLANT_NORMAL,
                                    CAIRO_FONT_WEIGHT_NORMAL);
                                cairo_set_font_size(cr, kFlagFontSize);
                                cairo_text_extents_t hext;
                                cairo_text_extents(cr, h.text.c_str(), &hext);
                                render_flag_text_bg_fill(cr,
                                    static_cast<double>(anchor.x),
                                    hext.x_advance,
                                    static_cast<double>(h.hit_rect.y),
                                    static_cast<double>(h.hit_rect.h));
                                cairo_restore(cr);

                                text_display::State td;
                                td.anchor   = anchor;
                                td.content  = h.text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    text_display::Position::Top;
                                text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }
                }
                const auto f1 = clock::now();
                t_flags_ms =
                    std::chrono::duration<double, std::milli>(f1 - f0).count();
            }

            // Playhead drawn last so its stem and triangle paint over any
            // marker connector pixels they share a column with — the brief
            // mandates the playhead never be occluded by marker stems or
            // flag annotations. The triangle indicator lives in the top
            // strip, so render whenever either the waveform or top strip is
            // exposed; otherwise a flag-strip-only repaint would erase the
            // triangle.
            if (rects_intersect(exposed, area) ||
                rects_intersect(exposed, top_strip)) {
                const auto p0 = clock::now();
                render_playhead(cr, area, px_x, kPlayhead,
                                gui.playhead_triangle_surface());
                const auto p1 = clock::now();
                t_playhead_ms =
                    std::chrono::duration<double, std::milli>(p1 - p0).count();
            }

            // Bottom strip: either the prompt overlay (when active) or
            // the regular elements (timestamp / tab letter / dirty / render
            // -view filename). The prompt is modal — while active, it
            // owns the strip and the regular elements are not visible.
            const GuiRect ts = timestamp_invalidate_rect(
                app.height, app.width, bottom_strip_wide());
            if (rects_intersect(exposed, ts)) {
                const int baseline_y = app.height - kTimestampBaselineFromBottom;
                if (app.prompt.active) {
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_move_to(cr, kTimestampPadX, baseline_y);
                    cairo_show_text(cr, app.prompt.text.c_str());
                    cairo_text_extents_t pext;
                    cairo_text_extents(cr, app.prompt.text.c_str(), &pext);
                    const double label_gap = kTabLetterGapPx * 2.0;
                    double cursor_x = static_cast<double>(kTimestampPadX) +
                                      pext.x_advance + label_gap;
                    for (const auto& label : app.prompt.response_labels) {
                        cairo_move_to(cr, cursor_x, baseline_y);
                        cairo_show_text(cr, label.c_str());
                        cairo_text_extents_t lext;
                        cairo_text_extents(cr, label.c_str(), &lext);
                        cursor_x += lext.x_advance + label_gap;
                    }
                    cairo_restore(cr);
                } else if (!app.queue_progress_text.empty()) {
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_move_to(cr, kTimestampPadX, baseline_y);
                    cairo_show_text(cr, app.queue_progress_text.c_str());
                    cairo_restore(cr);
                } else {
                    // In source-view, sr is the loaded file's sample rate
                    // and playhead_sample is in source-frames. In render
                    // -view the active `audio` is the render, so its sr
                    // is what the engine wrote out — but the playhead is
                    // in render-frame coords. Render-view timestamp is
                    // render-domain (zero at render sample 0); source-time
                    // and render-time advance at different rates because
                    // of warping, so the same arithmetic suffices.
                    double seconds = 0.0;
                    if (sr > 0) {
                        seconds = static_cast<double>(app.playhead_sample) /
                                  static_cast<double>(sr);
                    }
                    {
                        const auto s0 = clock::now();
                        render_timestamp(cr, kTimestampPadX, baseline_y,
                                         seconds, kText);
                        const auto s1 = clock::now();
                        t_ts_ms =
                            std::chrono::duration<double, std::milli>(s1 - s0).count();
                    }

                    // A/B tab letter between timestamp and dirty indicator.
                    // Same font/size/color as the timestamp; no background.
                    // Suppressed in render-view since the Tab key is gated
                    // out there and the letter would carry no meaning.
                    const double tw = measure_timestamp_width(cr, seconds);
                    double right_after_letter =
                        static_cast<double>(kTimestampPadX) + tw;
                    if (!app.render_view_enabled) {
                        const double letter_x =
                            static_cast<double>(kTimestampPadX) + tw +
                            kTabLetterGapPx;
                        const char letter_buf[2] = { app.active_tab, '\0' };
                        cairo_save(cr);
                        cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                        cairo_select_font_face(cr, "monospace",
                                               CAIRO_FONT_SLANT_NORMAL,
                                               CAIRO_FONT_WEIGHT_NORMAL);
                        cairo_set_font_size(cr, 14.0);
                        cairo_text_extents_t ext;
                        cairo_text_extents(cr, letter_buf, &ext);
                        cairo_move_to(cr, letter_x, baseline_y);
                        cairo_show_text(cr, letter_buf);
                        right_after_letter = letter_x + ext.x_advance;
                        cairo_restore(cr);
                    }

                    if (app.dirty) {
                        const auto d0 = clock::now();
                        const double cx = right_after_letter + kTabLetterGapPx;
                        cairo_save(cr);
                        cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                        cairo_select_font_face(cr, "monospace",
                                               CAIRO_FONT_SLANT_NORMAL,
                                               CAIRO_FONT_WEIGHT_NORMAL);
                        cairo_set_font_size(cr, 14.0);
                        cairo_move_to(cr, cx, baseline_y);
                        cairo_show_text(cr, "*");
                        cairo_restore(cr);
                        const auto d1 = clock::now();
                        t_dirty_ms =
                            std::chrono::duration<double, std::milli>(d1 - d0).count();
                    }

                    // Chunk W: render-view filename. Right-aligned in the
                    // bottom strip so it doesn't conflict with the
                    // timestamp / tab letter / dirty indicator on the left.
                    if (app.render_view_enabled &&
                        app.render_view_index >= 0 &&
                        app.render_view_index <
                            static_cast<int>(app.render_view_list.size())) {
                        const auto& e =
                            app.render_view_list[app.render_view_index];
                        const std::string label =
                            e.batch_folder.filename().string() + "/" +
                            e.basename + ".wav";
                        cairo_save(cr);
                        cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                        cairo_select_font_face(cr, "monospace",
                                               CAIRO_FONT_SLANT_NORMAL,
                                               CAIRO_FONT_WEIGHT_NORMAL);
                        cairo_set_font_size(cr, 14.0);
                        cairo_text_extents_t ext;
                        cairo_text_extents(cr, label.c_str(), &ext);
                        const double rx = static_cast<double>(app.width) -
                                          static_cast<double>(kTimestampPadX) -
                                          ext.x_advance;
                        cairo_move_to(cr, rx, baseline_y);
                        cairo_show_text(cr, label.c_str());
                        cairo_restore(cr);
                    }
                }
            }
        }

        cairo_restore(cr);

        // Force any pending Cairo ops out to the X server so the flush cost
        // is captured here rather than attributed elsewhere. The subsequent
        // flush in GuiX11::dispatch_event is a cheap no-op.
        {
            const auto fl0 = clock::now();
            cairo_surface_flush(cairo_get_target(cr));
            const auto fl1 = clock::now();
            t_flush_ms =
                std::chrono::duration<double, std::milli>(fl1 - fl0).count();
        }

        const auto t_end = clock::now();
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(t_end - t_start).count();

        if constexpr (kDebugPerf) {
            if (elapsed_ms > 3.0) {
                double e2e_ms = -1.0;
                if (app.last_input_event_time.time_since_epoch().count() != 0) {
                    e2e_ms = std::chrono::duration<double, std::milli>(
                        t_end - app.last_input_event_time).count();
                }
                std::fprintf(stderr,
                    "[dbg perf] total=%.2f ms waveform=%.2f markers=%.2f "
                    "flags=%.2f playhead=%.2f ts=%.2f dirty=%.2f flush=%.2f "
                    "pixel_area=%dx%d wf_cols=%d wf_pyramid_samples=%d "
                    "flag_measure=%d flag_drawn=%d flag_elided=%d "
                    "e2e=%.2f\n",
                    elapsed_ms, t_waveform_ms, t_markers_ms, t_flags_ms,
                    t_playhead_ms, t_ts_ms, t_dirty_ms, t_flush_ms,
                    w, h,
                    perf_counters::wf_cols, perf_counters::wf_pyramid_samples,
                    perf_counters::flag_measure, perf_counters::flag_drawn,
                    perf_counters::flag_elided,
                    e2e_ms);
            }
        }

        if constexpr (kDebugPerf) {
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
                if (playback.is_playing()) playback.resync_predictor();
            }
        }
        clamp_viewport_start(app, audio);
    });

    auto invalidate_marker_column = [&](int i) { viewport.invalidate_marker_column(i); };
    auto invalidate_top_strip     = [&]() { viewport.invalidate_top_strip(); };

    // V.A3b: a warp marker is hover-popup-eligible iff its rect doesn't
    // already display a numeric tempo: pass markers (with or without a
    // label_def) and label_ref markers. Owning markers display their tempo
    // in the rect, so no popup is needed. Transient mode has no pass
    // concept and is never eligible.
    //
    // V.B: when iteration mode is on, the hover popup for these same
    // marker types is suppressed entirely — the persistent iteration
    // popups occupy that visual space, and stacking a transient hover
    // hint on top would just clutter the strip.
    auto popup_eligible_marker = [&](int idx) -> bool {
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
    };

    // Reset the hover popup state. If the popup was visible, invalidate the
    // top strip so the next paint erases it. Safe to call from any path.
    clear_hover_popup = [&]() {
        const bool was_visible = app.hover_popup.visible;
        app.hover_popup = HoverPopupState{};
        if (was_visible) invalidate_top_strip();
    };

    auto set_single_selection        = [&](int idx) { selection.set_single_selection(idx); };
    auto clear_selection             = [&]() { selection.clear_selection(); };
    auto toggle_selection_membership = [&](int idx) { return selection.toggle_selection_membership(idx); };

    // -- Undo/redo helpers --------------------------------------------------
    //
    // X.7.3: the undo-cluster lambdas have been hoisted onto the Undo struct
    // in undo.{cpp,h}. The lambdas below are one-line forwarders so callsites
    // elsewhere in main() don't need to change. apply_post_restore_rules_warp
    // and apply_post_restore_rules_transient have no callers outside the
    // undo cluster, so their forwarders are dropped — they remain public on
    // the Undo struct for consistency.

    auto recompute_dirty = [&]() { undo.recompute_dirty(); };

    auto push_undo_both = [&](std::vector<GuiWarpMarker> warp_pre,
                              std::vector<GuiTransientMarker> trans_pre,
                              char op_mode, OpKind op_kind, int hint_last) {
        undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                            op_mode, op_kind, hint_last);
    };

    // -- X.7.5b flag-editor cluster forwarders ------------------------------
    // Bodies live on GuiFlagEditor (flag_editor.{h,cpp}). The lambdas
    // remain as one-line forwarders for the call sites elsewhere in this
    // function; build_locked_prefix had no external callers and was
    // dropped. The struct-method names match the original lambda names.
    auto exit_top_flag_edit_no_commit = [&]() { flag_editor.exit_top_flag_edit_no_commit(); };
    auto enter_top_flag_edit          = [&](int idx) { flag_editor.enter_top_flag_edit(idx); };
    auto commit_top_flag_edit         = [&]() { flag_editor.commit_top_flag_edit(); };
    auto enter_iter_edit              = [&](int idx) { flag_editor.enter_iter_edit(idx); };
    auto commit_iter_edit             = [&]() { flag_editor.commit_iter_edit(); };
    auto bulk_clear_iter_values       = [&]() { flag_editor.bulk_clear_iter_values(); };
    auto enter_bpm_edit                = [&](int idx) { flag_editor.enter_bpm_edit(idx); };
    auto commit_bpm_edit              = [&]() { flag_editor.commit_bpm_edit(); };
    auto bulk_clear_bpm_values        = [&]() { flag_editor.bulk_clear_bpm_values(); };
    auto enter_bpm_mode               = [&]() { flag_editor.enter_bpm_mode(); };
    auto exit_bpm_mode                = [&]() { flag_editor.exit_bpm_mode(); };

    // Cross-file flag scan. `want_begin` selects the b= scan vs the e=
    // scan. The (excl_trans, excl_idx) pair excludes one marker from the
    // search — used by toggle to skip the marker the user just toggled.
    // Pass excl_idx == -1 for no exclusion. Warp list is scanned first;
    // on a duplicate (parser-protected, only via hand-edit), the warp-
    // side hit wins to match compute_trim_samples.
    find_flag = [&](bool want_begin, bool excl_trans, int excl_idx)
        -> FlagLoc {
        FlagLoc f;
        const int sr = audio.sample_rate();
        const auto& mv = app.warpmarkers.markers();
        for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
            const bool has = want_begin ? mv[i].is_begin_time
                                        : mv[i].is_end_time;
            if (!has) continue;
            if (!excl_trans && i == excl_idx) continue;
            f.valid     = true;
            f.transient = false;
            f.idx       = i;
            f.frame     = static_cast<int64_t>(std::llround(
                mv[i].time_seconds * static_cast<double>(sr)));
            return f;
        }
        const auto& tv = app.transientmarkers.markers();
        for (int i = 0; i < static_cast<int>(tv.size()); ++i) {
            const bool has = want_begin ? tv[i].is_begin_time
                                        : tv[i].is_end_time;
            if (!has) continue;
            if (excl_trans && i == excl_idx) continue;
            f.valid     = true;
            f.transient = true;
            f.idx       = i;
            f.frame     = tv[i].effective_frame();
            return f;
        }
        return f;
    };

    // Gesture-stop: called at the top of any handler that will move the
    // visible playhead (keys, button press, Ctrl+wheel, undo/redo, tab
    // switch). Stops the audio thread and keeps the LSP in sync with the
    // visible playhead so the next Space-to-play captures the right
    // launch position. Does NOT return-to-launch — the gesture is about
    // to commit a new playhead position.
    stop_playback_if_playing = [&]() {
        if (!playback.is_playing() && !app.is_playing) return;
        playback.stop();
        app.is_playing        = false;
        app.last_space_sample = app.playhead_sample;
    };

    auto do_undo = [&]() { undo.do_undo(); };
    auto do_redo = [&]() { undo.do_redo(); };

    // Tab / Shift+Tab: cycle through markers. Rules per spec:
    //   0 or 1 selected: cycle through all markers.
    //     1 selected acts as the anchor; 0 selected falls back to playhead.
    //   2+ selected: cycle within the selection set only, anchored on
    //     last_selected_marker. Wraps at the set's extremes.
    auto select_next_marker = [&]() { selection.select_next_marker(); };
    auto select_prev_marker = [&]() { selection.select_prev_marker(); };

    // X.7.5a: the warp-authoring lambdas have been hoisted onto the
    // GuiWarpMarkersOps struct in warpmarkers_ops.{cpp,h}. The lambdas
    // below are one-line forwarders so callsites elsewhere in main()
    // don't need to change. compute_selection_delta_bounds and
    // apply_selection_shift had no external callers and are not
    // forwarded; their bodies live as public methods on the struct
    // (called only from within the cluster).
    auto drop_marker                    = [&](double t, bool inh) { warpops.drop_marker(t, inh); };
    auto drop_marker_at_playhead        = [&]() { warpops.drop_marker_at_playhead(); };
    auto drop_inherit_marker_at_playhead = [&]() { warpops.drop_inherit_marker_at_playhead(); };
    auto delete_selected_marker         = [&]() { warpops.delete_selected_marker(); };
    auto force_delete_selected_marker   = [&]() { warpops.force_delete_selected_marker(); };
    auto toggle_inherits                = [&]() { warpops.toggle_inherits(); };
    auto toggle_disabled                = [&]() { warpops.toggle_disabled(); };
    auto toggle_begin_time              = [&]() { warpops.toggle_begin_time(); };
    auto toggle_end_time                = [&]() { warpops.toggle_end_time(); };
    auto adjust_tempo                   = [&](double d) { warpops.adjust_tempo(d); };
    auto clear_trim                     = [&]() { warpops.clear_trim(); };

    // -- Transient-mode editing helpers (chunk S.2.2) -----------------------

    // X.7.4: the transient-authoring lambdas have been hoisted onto the
    // Transients struct in transients.{cpp,h}. The lambdas below are
    // one-line forwarders so callsites elsewhere in main() don't need to
    // change. compute_transient_delta_bounds and merge_detection had no
    // external callers and are not forwarded; run_detect_now is called as
    // transients.run_detect_now() directly from proceed_with_trigger.
    auto drop_transient_at_position          = [&](double t) { transients.drop_transient_at_position(t); };
    auto drop_transient_at_playhead          = [&]() { transients.drop_transient_at_playhead(); };
    auto delete_selected_transient           = [&]() { transients.delete_selected_transient(); };
    auto toggle_transient_disabled           = [&]() { transients.toggle_transient_disabled(); };
    auto nudge_selected_transients           = [&](int dir) { transients.nudge_selected_transients(dir); };
    auto jump_transient_selection_to_playhead = [&]() { transients.jump_transient_selection_to_playhead(); };
    auto toggle_transient_begin_time         = [&]() { transients.toggle_transient_begin_time(); };
    auto toggle_transient_end_time           = [&]() { transients.toggle_transient_end_time(); };

    // X.7.7: the mode/tab-management lambdas have been hoisted onto the
    // GuiTabMode struct in tab_mode.{cpp,h}. The two forwarders below cover
    // the only external callsites: refresh_active_tab_from_app for
    // save_markers + the std::function ref captured by GuiRenderView, and
    // toggle_active_mode for the `t` keypress. active_view_state,
    // switch_active_mode_to, and the transient prune_live_selection
    // forwarder had no callers outside the cluster and were dropped.
    // switch_active_tab_to is new — it replaces the inline Ctrl+Tab block
    // in the keyboard handler and has no forwarder.
    refresh_active_tab_from_app = [&]() { tab_mode.refresh_active_tab_from_app(); };
    auto toggle_active_mode     = [&]() { tab_mode.toggle_active_mode(); };

    auto save_markers = [&]() -> bool {
        if (app.warpmarkers_path.empty()) return false;
        if (app.first_save_pending && app.warpmarkers.had_nonstandard_content()) {
            std::fprintf(stderr,
                "warptempo_gui: first save in this session will discard "
                "comments and freeform text from %s. Canonical format will "
                "be written.\n",
                app.warpmarkers_path.c_str());
        }
        // Capture the active tab's current values before any writes — both
        // the .warpmarkers and .settings paths see a consistent snapshot.
        refresh_active_tab_from_app();

        const bool ok = app.warpmarkers.save(app.warpmarkers_path);
        if (!ok) {
            std::fprintf(stderr,
                "warptempo_gui: save failed: %s\n",
                app.warpmarkers_path.c_str());
            return false;
        }

        // Transients sibling write. Empty list deletes the on-disk file so
        // a project never carries a stale empty .transientmarkers.
        if (!app.transientmarkers_path.empty()) {
            if (app.transientmarkers.markers().empty()) {
                if (!app.transientmarkers.delete_file(app.transientmarkers_path)) {
                    std::fprintf(stderr,
                        "warptempo_gui: failed to delete: %s\n",
                        app.transientmarkers_path.c_str());
                }
            } else {
                if (!app.transientmarkers.save(app.transientmarkers_path)) {
                    std::fprintf(stderr,
                        "warptempo_gui: transient save failed: %s\n",
                        app.transientmarkers_path.c_str());
                    return false;
                }
            }
        }

        app.first_save_pending = false;
        // Save rebinds the saved reference to the current timeline position
        // without touching either stack — undo still reverts the last op.
        const bool was_dirty = app.dirty;
        app.history.mark_saved();
        recompute_dirty();
        if (was_dirty != app.dirty) {
            invalidate_timestamp_area();
        }

        // Best-effort .settings write. Failure is logged but does not fail
        // the overall save — the .warpmarkers write is the primary target.
        if (!app.settings_path.empty()) {
            if (!write_settings_file(app.settings_path,
                                     app.tab_a, app.tab_b,
                                     app.follow_mode,
                                     app.settings_passthrough)) {
                std::fprintf(stderr,
                    "warptempo_gui: settings save failed: %s: %s\n",
                    app.settings_path.c_str(),
                    std::strerror(errno));
            }
        }
        return true;
    };

    // Snap the visible playhead back to where Space was last pressed and
    // refresh the affected regions. Used by both Space-to-stop and natural
    // end-of-playback.
    auto restore_playhead_to_lsp = [&]() {
        const double old_px = playhead_pixel_x(app, audio);
        app.playhead_sample = app.last_space_sample;
        const double new_px = playhead_pixel_x(app, audio);
        invalidate_playhead_columns(old_px, new_px);
        invalidate_timestamp_area();
        // The triangle shares the top strip with any selected-flag
        // highlight; restore jumps can uncover/cover both, so invalidate
        // the flag strip too.
        const GuiRect ts = top_strip_area(app);
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
        app.is_playing      = false;
        app.playback_cursor = app.playhead_sample;
    };

    // -- Unsaved-work dialog + blank-state revert (chunk Q) -----------------

    auto invalidate_all = [&]() { viewport.invalidate_all(); };

    // Drop the currently loaded audio and reset all per-file UI state to
    // what the GUI looks like when launched with no argument. Leaves the
    // window open so the user can drop another file or quit. Bound to
    // Ctrl+W (proceed path from either the clean branch or the dialog).
    auto revert_to_blank = [&]() {
        // Stop the audio thread before the sample buffer it borrows goes
        // away. Same invariant as load_file.
        playback.stop();
        playback.shutdown();
        app.is_playing      = false;
        app.playback_cursor = 0;

        audio = GuiAudio{};
        app.audio_generation++;
        wf_cache.destroy_surface();

        app.playhead_sample       = 0;
        app.viewport_start_sample = 0;
        app.zoom_level            = 0;
        app.follow_mode           = true;
        app.last_space_sample     = 0;
        app.playback_speed        = 1.0f;

        app.warpmarkers.clear();
        app.transientmarkers.clear();
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        app.active_mode    = 'W';
        app.drag          = DragState{};
        app.playhead_drag = PlayheadDragState{};
        app.hover_popup   = HoverPopupState{};
        app.history.reset();
        app.dirty              = false;
        app.warp_dirty         = false;
        app.transient_dirty    = false;
        app.first_save_pending = true;

        app.warpmarkers_path.clear();
        app.transientmarkers_path.clear();
        app.settings_path.clear();
        app.source_audio_path.clear();
        app.pending_drop_path.clear();
        app.settings_passthrough.clear();

        app.tab_a = ViewState{};
        app.tab_b = ViewState{};
        app.active_tab = 'A';

        invalidate_all();
    };

    auto proceed_with_trigger = [&](DialogTrigger t) {
        switch (t) {
        case DialogTrigger::CLOSE_WINDOW:
            gui.request_exit();
            break;
        case DialogTrigger::REVERT_TO_BLANK:
            revert_to_blank();
            break;
        case DialogTrigger::DETECT_TRANSIENTS:
            transients.run_detect_now();
            break;
        }
    };

    auto open_prompt_unsaved = [&](DialogTrigger t) {
        app.prompt.active          = true;
        app.prompt.text            = "Save unsaved changes?";
        app.prompt.response_keys   = {'s', 'd', 'c'};
        app.prompt.response_labels = {"[S]ave", "[D]iscard", "[C]ancel"};
        app.prompt.trigger         = t;
        clear_hover_popup();
        invalidate_all();
    };

    open_prompt_detect_confirm = [&]() {
        app.prompt.active          = true;
        app.prompt.text            =
            "Re-detect transients? Existing detection will be replaced.";
        app.prompt.response_keys   = {'d', 'c'};
        app.prompt.response_labels = {"[D]etect", "[C]ancel"};
        app.prompt.trigger         = DialogTrigger::DETECT_TRANSIENTS;
        clear_hover_popup();
        invalidate_all();
    };

    // Single-key response dispatch. The trigger captured at prompt-open
    // time selects which response set is in play; the key picks the
    // response. On a Save failure, the prompt mutates in place to a
    // retry/discard/cancel state — same trigger, new text and response
    // set — rather than dismissing.
    auto prompt_activate_response = [&](char k) {
        if (!app.prompt.active) return;
        const DialogTrigger trigger = app.prompt.trigger;

        if (trigger == DialogTrigger::CLOSE_WINDOW ||
            trigger == DialogTrigger::REVERT_TO_BLANK) {
            if (k == 's' || k == 'r') {
                const bool ok = save_markers();
                if (!ok) {
                    app.prompt.text            = "Save failed.";
                    app.prompt.response_keys   = {'r', 'd', 'c'};
                    app.prompt.response_labels =
                        {"[R]etry", "[D]iscard", "[C]ancel"};
                    invalidate_all();
                    return;
                }
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == 'd') {
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == 'c') {
                app.prompt.active = false;
                invalidate_all();
                return;
            }
            return;
        }

        if (trigger == DialogTrigger::DETECT_TRANSIENTS) {
            if (k == 'd') {
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == 'c') {
                app.prompt.active = false;
                invalidate_all();
                return;
            }
            return;
        }
    };

    // Route a close / revert gesture through the prompt when history is
    // dirty; otherwise proceed immediately. Centralizes the decision so
    // Ctrl+Q, Ctrl+W, and the WM-close callback share identical behavior.
    auto request_close_or_revert = [&](DialogTrigger t) {
        if (app.prompt.active) return; // already gated; ignore re-entry
        if (app.dirty) open_prompt_unsaved(t);
        else           proceed_with_trigger(t);
    };

    // -- Transient detection (chunk S.3) ------------------------------------
    //
    // X.7.4: detection lambdas have been hoisted onto the Transients struct.
    // Forwarders below for detect_transients and clear_all_transients keep
    // the existing keypress callsites working unchanged.
    auto detect_transients     = [&]() { transients.detect_transients(); };
    auto clear_all_transients  = [&]() { transients.clear_all_transients(); };

    // Space-bar: start/stop playback. Playback runs from the playhead to
    // trim_end (or total_frames if no e= marker). Pressing space with the
    // playhead at or past trim-end is a silent no-op. Space-to-stop
    // returns the visible playhead to the position where Space-to-play
    // was last pressed (return-to-launch).
    auto toggle_playback = [&]() {
        if (playback.is_playing()) {
            playback.stop();
            restore_playhead_to_lsp();
            return;
        }
        const int64_t end = trim_end_sample();
        if (app.playhead_sample >= end) return;
        // Clamp the start position into the trim range in case the playhead
        // is sitting at trim_end - 1 (valid) or somehow slipped.
        const int64_t start = std::max(app.playhead_sample, trim_begin_sample());
        app.last_space_sample = app.playhead_sample;
        app.playback_cursor = start;
        app.is_playing = true;
        playback.set_speed(app.playback_speed);
        playback.play(start, end);
    };

    // Helpers for Shift+<digit> speed selection.
    auto set_playback_speed = [&](float s) {
        app.playback_speed = s;
        playback.set_speed(s);
        // Speed change without resync would cause a backward cursor jump:
        // the predictor would retroactively apply the new speed to the
        // entire elapsed-since-anchor period.
        if (playback.is_playing()) playback.resync_predictor();
    };

    // Hit-test a marker line in the waveform area. Returns index or -1.
    // Mode-aware: iterates the active list (render-view markers when
    // render-view is on; otherwise warp markers or transients).
    auto hit_test_marker_line = [&](int mouse_x) -> int {
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
        // list drives hit-testing. 'T' reads transient frames via
        // effective_frame() (matching source-view's transient branch).
        const bool rv_trans = rv && app.active_mode == 'T';
        const int n =
            rv_trans
                ? static_cast<int>(app.render_view_transients.size())
                : rv
                    ? static_cast<int>(app.render_view_markers.size())
                    : (app.active_mode == 'T')
                        ? static_cast<int>(app.transientmarkers.markers().size())
                        : static_cast<int>(app.warpmarkers.markers().size());
        for (int i = 0; i < n; ++i) {
            double ms;
            if (rv_trans) {
                ms = static_cast<double>(
                    app.render_view_transients[i].effective_frame());
            } else if (rv) {
                ms = app.render_view_markers[i].time_seconds *
                     static_cast<double>(sr);
            } else if (app.active_mode == 'T') {
                ms = static_cast<double>(
                    app.transientmarkers.markers()[i].effective_frame());
            } else {
                ms = app.warpmarkers.markers()[i].time_seconds *
                     static_cast<double>(sr);
            }
            if (ms < vp) continue;
            if (ms >= vp + static_cast<double>(visible)) continue;
            const int m_px = static_cast<int>(std::llround((ms - vp) / spp));
            const int d = std::abs(m_px - click_rel_x);
            if (d <= kMarkerHitHalfPx && d < best_dist) {
                best_dist = d;
                best_hit  = i;
            }
        }
        return best_hit;
    };

    // Hit-test a flag rectangle in the top strip. Returns marker index or -1.
    // Mode-aware: dispatches to the warp- or transient-flag pack.
    auto hit_test_flag = [&](int mouse_x, int mouse_y) -> int {
        // Brief F Section 3: render-view's transient sub-view paints no
        // flags; short-circuit to no-hit so click and hover paths see a
        // bare top strip.
        if (app.render_view_enabled &&
            app.active_mode == 'T') {
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
            static_cast<int64_t>(std::llround(spp * area.w));
        std::vector<FlagHitRect> rects;
        if (app.render_view_enabled) {
            rects = compute_flag_hit_rects(
                scratch_cr, top, app.render_view_markers,
                vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
        } else if (app.active_mode == 'T') {
            rects = compute_transient_flag_hit_rects(
                scratch_cr, top, app.transientmarkers.markers(),
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
    };

    // V.B: iteration popup hit-test. Returns the marker index whose
    // iteration popup contains (mouse_x, mouse_y), or -1. Always returns
    // -1 when iteration mode is off or in transient mode (no popups).
    auto hit_test_iter_popup = [&](int mouse_x, int mouse_y) -> int {
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
            static_cast<int64_t>(std::llround(spp * area.w));
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
                return h.marker_index;
            }
        }
        return -1;
    };

    // Brief X.2: BPM popup hit-test. Mirrors hit_test_iter_popup. The
    // two modes are mutually exclusive so at most one of these returns
    // a positive index for a given (x, y).
    auto hit_test_bpm_popup = [&](int mouse_x, int mouse_y) -> int {
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
            static_cast<int64_t>(std::llround(spp * area.w));
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
                return h.marker_index;
            }
        }
        return -1;
    };

    // V.A3b Addendum 3: re-evaluate hover at the cursor's last on_motion
    // coordinates. Called after viewport mutations (zoom, scroll, center,
    // playhead-driven viewport shift) so a stationary cursor's hover state
    // tracks the rects that just slid under it. Mirrors the on_motion
    // hover-detection branch: same gating, same hit-test, same state
    // transitions; the tick handler still drives the dwell-to-visible flip.
    recompute_hover_at_cursor = [&]() {
        if (app.last_mouse_x < 0 || app.last_mouse_y < 0) return;
        // Dialog / drag / editor / queue still suppress hover in either
        // view. Source-view also requires warp mode + iter mode off;
        // render-view bypasses the mode checks because hover always
        // applies against the loaded render's warpmarkers.
        if (app.prompt.active ||
            app.drag.active ||
            app.playhead_drag.active ||
            text_editor::is_active(app.top_flag_editor) ||
            app.queue_running) {
            clear_hover_popup();
            return;
        }
        if (!app.render_view_enabled &&
            (app.active_mode != 'W' || app.iteration_mode_enabled)) {
            clear_hover_popup();
            return;
        }
        const int hit = hit_test_flag(app.last_mouse_x, app.last_mouse_y);
        if (hit != app.hover_popup.marker_index) {
            if (app.hover_popup.visible) invalidate_top_strip();
            app.hover_popup.marker_index = hit;
            app.hover_popup.visible      = false;
            app.hover_popup.entry_time   =
                std::chrono::steady_clock::now();
            // Precompute the popup's display text at rect-entry so the
            // delay-completion paint doesn't have to recompute it. Empty
            // when `hit` is not popup-eligible (the redraw branch then
            // skips paint and keeps the strip clean).
            if (app.render_view_enabled) {
                app.hover_popup.cached_text =
                    popup_eligible_marker(hit)
                        ? compute_hover_popup_text(
                              app.render_view_markers, hit,
                              app.render_view_src_sr)
                        : std::string();
            } else {
                app.hover_popup.cached_text =
                    popup_eligible_marker(hit)
                        ? compute_hover_popup_text(
                              app.warpmarkers.markers(), hit,
                              audio.sample_rate())
                        : std::string();
            }
        }
    };

    // X.7.5a: the drag and selection-shift lambdas have been hoisted onto
    // the GuiWarpMarkersOps struct in warpmarkers_ops.{cpp,h}. The
    // forwarders below preserve the in-main callsites unchanged.
    auto begin_drag                 = [&](int hit, int x) { return warpops.begin_drag(hit, x); };
    auto apply_drag_motion          = [&](double d) { warpops.apply_drag_motion(d); };
    auto commit_drag                = [&]() { warpops.commit_drag(); };
    auto nudge_selected_markers     = [&](int dir) { warpops.nudge_selected_markers(dir); };
    auto jump_selection_to_playhead = [&]() { warpops.jump_selection_to_playhead(); };

    // X.7.6: the render-view cluster has been hoisted onto the GuiRenderView
    // struct in render_view.{cpp,h}. The forwarders below preserve the
    // in-main callsites unchanged. rendersettings_path, apply_rendersettings_for,
    // and wav_stat_tuple have no remaining external callers (they were only
    // invoked by other render-view lambdas) so no forwarders are kept for
    // them.
    auto enumerate_render_view_list                  = [&]() { return render_view.enumerate_render_view_list(); };
    auto write_rendersettings_for                    = [&](const AppState::RenderViewEntry& e) { render_view.write_rendersettings_for(e); };
    auto stash_render_view_selection_to_active_entry = [&]() { render_view.stash_render_view_selection_to_active_entry(); };
    auto load_render_view_at                         = [&](int index) { return render_view.load_render_view_at(index); };
    auto restore_source_audio                        = [&]() { render_view.restore_source_audio(); };

    // Shared wheel handler covering source-view and render-view. Ctrl+Alt =
    // fine-pan (2% of viewport), Alt = coarse-pan (10%), plain = zoom.
    // Ctrl+wheel moves the playhead by one pixel (and stops playback),
    // matching the bare Left/Right keyboard binding; this applies to both
    // views since the playhead is interactive in render-view too.
    auto handle_wheel = [&](unsigned int button,
                            bool ctrl, bool alt,
                            bool inside_waveform, bool inside_top) {
        if (!inside_waveform && !inside_top) return;
        if (ctrl && alt) {
            const int64_t step = std::max<int64_t>(
                1, samples_visible(app, audio) / 50);
            scroll_viewport(button == 4 ? -step : +step);
            return;
        }
        if (ctrl) {
            stop_playback_if_playing();
            move_playhead_pixels(button == 4 ? -1 : +1);
            return;
        }
        if (alt) {
            const int64_t step = std::max<int64_t>(
                1, samples_visible(app, audio) / 10);
            scroll_viewport(button == 4 ? -step : +step);
            return;
        }
        if (button == 4) zoom_out();
        else             zoom_in();
    };

    // Multi-render queue runner. Owns the queue_running / cancel-flag
    // bookkeeping and per-entry progress display; the caller owns batch
    // folder creation, RenderRequest construction, and the post-summary
    // log. Returns rendered count and whether Esc cut the run short.
    struct RenderBatchResult {
        int  rendered  = 0;
        bool cancelled = false;
    };
    auto run_render_batch =
        [&](const std::vector<RenderRequest>& reqs,
            const std::string& batch_label) -> RenderBatchResult {
        RenderBatchResult result;
        if (reqs.empty()) return result;

        const int total = static_cast<int>(reqs.size());

        app.queue_cancel_requested = false;
        app.queue_running          = true;
        clear_hover_popup();

        for (int i = 0; i < total; ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%s: rendering %d of %d...",
                          batch_label.c_str(), i + 1, total);
            app.queue_progress_text = buf;
            invalidate_timestamp_area();
            // First drain surfaces the progress-text paint before the
            // engine starts; otherwise the new "rendering K of N" only
            // appears after the entry completes.
            gui.drain_events();

            if (do_render(reqs[i])) ++result.rendered;

            // Second drain surfaces X events queued during the render —
            // Esc presses, expose events. The cancel flag becomes
            // visible to the next iteration through this drain.
            gui.drain_events();
            if (app.queue_cancel_requested) {
                result.cancelled = true;
                break;
            }
        }

        app.queue_running          = false;
        app.queue_cancel_requested = false;
        // Invalidate the wide bottom-strip rect before clearing the
        // progress text. bottom_strip_wide() reads queue_progress_text;
        // clearing first would shrink the invalidated rect to the narrow
        // timestamp width, leaving the trailing pixels of the final
        // "rendering N of N..." string undamaged.
        invalidate_timestamp_area();
        app.queue_progress_text.clear();

        return result;
    };

    gui.set_on_key([&](KeySym keysym, unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        const bool ctrl  = (mods & ControlMask) != 0;
        const bool shift = (mods & ShiftMask)   != 0;
        const bool alt   = (mods & Mod1Mask)    != 0;

        // Bottom-strip prompt owns input while active. Only the prompt's
        // own response keys (case-insensitive) and Esc (rightmost
        // response = Cancel by convention) do anything; everything else
        // is swallowed so marker edits / playback / viewport keys cannot
        // sneak in while the prompt is up.
        if (app.prompt.active) {
            char k = 0;
            if (keysym >= XK_a && keysym <= XK_z) {
                k = static_cast<char>('a' + (keysym - XK_a));
            } else if (keysym >= XK_A && keysym <= XK_Z) {
                k = static_cast<char>('a' + (keysym - XK_A));
            }
            if (keysym == XK_Escape) {
                if (!app.prompt.response_keys.empty()) {
                    prompt_activate_response(app.prompt.response_keys.back());
                }
                return;
            }
            if (k != 0) {
                for (char rk : app.prompt.response_keys) {
                    if (k == rk) {
                        prompt_activate_response(rk);
                        return;
                    }
                }
            }
            return;
        }

        // Blank / loading state: only the quit / close-gesture bindings run;
        // everything else no-ops. Dialog can't fire here because dirty is
        // always false in blank state (history is reset on revert).
        if (app.loading || audio.total_frames() <= 0) {
            if (ctrl && !shift && !alt && keysym == XK_q) {
                request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
            }
            return;
        }

        // V.A1 top-flag editor owns the keyboard while active. Routes here
        // BEFORE queue/drag/playhead Esc handlers so Esc cancels the edit
        // first; Esc with no active edit falls through to the rest.
        if (text_editor::is_active(app.top_flag_editor)) {
            (void)ctrl; (void)alt; // Modifiers swallowed except Shift→colon.
            const auto action = text_editor::handle_key(
                app.top_flag_editor, keysym, mods);
            if (action == text_editor::KeyAction::CommitRequested) {
                if (app.top_flag_editor.kind ==
                        text_editor::Kind::IterationBracket) {
                    commit_iter_edit();
                } else if (app.top_flag_editor.kind ==
                        text_editor::Kind::BpmBracket) {
                    commit_bpm_edit();
                } else {
                    commit_top_flag_edit();
                }
                return;
            }
            if (action == text_editor::KeyAction::CancelRequested) {
                exit_top_flag_edit_no_commit();
                return;
            }
            if (action == text_editor::KeyAction::Consumed) {
                invalidate_top_strip();
                return;
            }
            // NotConsumed: editor saw nothing useful; fall through is wrong
            // because the editor must own all keys while active. Treat as
            // consumed.
            return;
        }

        // Chunk W: render-view input gate. While render-view is active
        // only keys driving navigation / playback / exit / commit are
        // honored; every authoring key is silently dropped so a stray
        // press can't mutate state through a swapped-out view.
        // Allowlist:
        //   - r (no mods)            → toggle render-view off
        //   - Shift+Left/Right       → previous/next render
        //   - Ctrl+Alt+C             → commit displayed render's markers
        //   - Space                  → playback toggle
        //   - Left/Right (no mods)   → playhead-by-pixel scrub
        //   - Home/End (no mods)     → playhead to trim begin/end
        //   - Esc                    → top-level no-op (chunk Q)
        //   - t (no mods)            → toggle warp/transient sub-view (Brief F)
        //   - Ctrl+Q / Ctrl+W        → close-prompt routing (Brief F)
        //   - Up/Down (no mods)      → zoom in/out (Brief S.2)
        //   - =/- (no mods)          → zoom in/out symbol-key alias (Brief S.2)
        if (app.render_view_enabled) {
            const bool is_r =
                (keysym == XK_r && !ctrl && !shift && !alt);
            const bool is_nav =
                ((keysym == XK_Left || keysym == XK_Right) &&
                 shift && !ctrl && !alt);
            const bool is_commit =
                (ctrl && alt && !shift &&
                 (keysym == XK_c || keysym == XK_C));
            const bool is_playback = (keysym == XK_space);
            const bool is_scrub =
                ((keysym == XK_Left || keysym == XK_Right) &&
                 !ctrl && !shift && !alt);
            const bool is_jump =
                ((keysym == XK_Home || keysym == XK_End) &&
                 !ctrl && !shift && !alt);
            const bool is_esc = (keysym == XK_Escape);
            const bool is_sub_view_toggle =
                (keysym == XK_t && !ctrl && !shift && !alt);
            const bool is_ctrl_q =
                (ctrl && !shift && !alt && keysym == XK_q);
            const bool is_ctrl_w =
                (ctrl && !shift && !alt && keysym == XK_w);
            const bool is_zoom =
                ((keysym == XK_Up || keysym == XK_Down) &&
                 !ctrl && !shift && !alt);
            const bool is_zoom_symbol =
                ((keysym == XK_equal || keysym == XK_minus) &&
                 !ctrl && !shift && !alt);
            if (!(is_r || is_nav || is_commit || is_playback ||
                  is_scrub || is_jump || is_esc ||
                  is_sub_view_toggle || is_ctrl_q || is_ctrl_w ||
                  is_zoom || is_zoom_symbol)) {
                return;
            }
        }

        // Esc during a render-all run requests cancellation between
        // entries. Only fires while the queue walker is active; outside
        // that window Esc retains its other meanings (drag-cancel, etc).
        // Mid-engine Esc presses are queued by X and surface here on the
        // next gui.drain_events() — they take effect after the in-flight
        // entry completes (chunk U does not implement mid-engine cancel).
        if (keysym == XK_Escape && app.queue_running) {
            app.queue_cancel_requested = true;
            return;
        }

        // Escape during a playhead drag ends the gesture at its current
        // position (no restore — the drag already committed its visible
        // progress per motion event, so there's nothing to revert).
        if (keysym == XK_Escape && app.playhead_drag.active) {
            app.playhead_drag = PlayheadDragState{};
            return;
        }

        // Ctrl+Q: quit (via unsaved-work dialog when dirty).
        if (ctrl && !shift && !alt && keysym == XK_q) {
            request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
            return;
        }

        // Ctrl+W: revert to blank state (via unsaved-work dialog when dirty).
        if (ctrl && !shift && !alt && keysym == XK_w) {
            request_close_or_revert(DialogTrigger::REVERT_TO_BLANK);
            return;
        }

        // Ctrl+E: snapshot current authoring state into the in-memory
        // render queue. No disk writes; on-disk authoring files are untouched.
        // Settings are not snapshotted per-entry — the queue walker uses
        // the live settings_passthrough at execution time, mirroring the
        // chunk-U convention. (Chunk W: snapshots moved from disk to memory.)
        if (ctrl && !alt && !shift &&
            (keysym == XK_e || keysym == XK_E)) {
            if (app.source_audio_path.empty()) return;
            AppState::QueuedRender q;
            q.source_audio_path = app.source_audio_path;
            q.markers           = app.warpmarkers.markers();
            q.transients        = app.transientmarkers.markers();
            app.queued_renders.push_back(std::move(q));
            std::fprintf(stderr,
                "warptempo_gui: queued render (%zu in queue)\n",
                app.queued_renders.size());
            return;
        }

        // Ctrl+Alt+R: single render into the source directory using `title`
        // from settings. Mirrors the pre-Chunk-W non-batch path inside
        // do_render: empty batch_folder/batch_basename triggers the
        // engine/limiter-prefix naming. Title-not-set is a hard error
        // surfaced from do_render. Does not consult the in-memory queue and
        // does not write any sidecars beyond the .peaks pyramid that
        // do_render already deposits.
        if (ctrl && alt && !shift &&
            (keysym == XK_r || keysym == XK_R)) {
            if (app.source_audio_path.empty()) return;

            RenderRequest req;
            req.source_audio_path    = app.source_audio_path;
            req.markers              = app.warpmarkers.markers();
            req.transients           = app.transientmarkers.markers();
            req.settings_passthrough = app.settings_passthrough;
            for (const auto& m : app.transientmarkers.markers()) {
                if (m.disabled) continue;
                req.transient_frames.push_back(m.effective_frame());
            }
            // Empty batch_folder/basename selects the source-dir naming
            // convention inside do_render.
            do_render(req);
            return;
        }

        // Ctrl+Alt+E: render the in-memory queue as one batch. Each
        // queued entry produces a sibling .wav (+ .warpmarkers /
        // .transientmarkers when non-empty / .peaks sidecars) inside a fresh
        // batch folder `<source_parent>/renders/<index>_render_all_in_queue/`.
        // The index is one greater than the highest pre-existing batch index
        // in that renders folder (regardless of command tag). Filenames
        // inside the batch are the entry position zero-padded to fit the
        // queue size: 01..10 for 10 entries, 1..7 for 7, 001..100 for 100.
        //
        // Empty queue is a silent no-op (no implicit-batch fallback —
        // single-shot rendering belongs on Ctrl+Alt+R now).
        //
        // Esc between entries drops the remainder. The current render
        // cannot be interrupted (no mid-engine cancellation); its sidecars
        // are written if it succeeds, then the loop exits and the rest of
        // the queue is discarded. The batch folder is left as-is on disk —
        // partial batches just contain fewer files than the queue had.
        // The in-memory queue is cleared after execution whether all
        // entries ran or Esc cut it short.
        if (ctrl && alt && !shift &&
            (keysym == XK_e || keysym == XK_E)) {
            if (app.source_audio_path.empty()) return;
            if (app.queued_renders.empty()) return;

            std::vector<AppState::QueuedRender> entries =
                std::move(app.queued_renders);
            app.queued_renders.clear();

            std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path queue_root = src_parent / "renders";

            // Resolve the next batch index: max+1 over directory entries
            // matching `<digits>_<anything>`. Empty / missing renders/
            // folder seeds index 1.
            int next_index = 1;
            std::error_code ec;
            if (std::filesystem::is_directory(queue_root, ec)) {
                int max_idx = 0;
                for (const auto& de :
                     std::filesystem::directory_iterator(queue_root, ec)) {
                    if (!de.is_directory()) continue;
                    const std::string name = de.path().filename().string();
                    int v = 0;
                    size_t i = 0;
                    while (i < name.size() &&
                           name[i] >= '0' && name[i] <= '9') {
                        v = v * 10 + (name[i] - '0');
                        ++i;
                    }
                    if (i == 0 || i >= name.size() || name[i] != '_') continue;
                    if (v > max_idx) max_idx = v;
                }
                next_index = max_idx + 1;
            }

            const std::string command_tag = "render_all_in_queue";
            const std::filesystem::path batch_folder =
                queue_root /
                (std::to_string(next_index) + "_" + command_tag);
            std::filesystem::create_directories(batch_folder, ec);
            if (ec) {
                std::fprintf(stderr,
                    "warptempo_gui: render-all: could not create '%s': %s\n",
                    batch_folder.string().c_str(), ec.message().c_str());
                return;
            }

            // Width-to-fit zero-padding for filename indices. pad_width is
            // computed from the queue size and clamped to a sane upper
            // bound so the snprintf below has a known-bounded output.
            const int total = static_cast<int>(entries.size());
            int pad_width = 1;
            for (int n = total; n >= 10; n /= 10) ++pad_width;
            if (pad_width > 9) pad_width = 9;

            std::vector<RenderRequest> reqs;
            reqs.reserve(total);
            for (int i = 0; i < total; ++i) {
                const auto& q = entries[i];
                char num_buf[16];
                std::snprintf(num_buf, sizeof(num_buf),
                              "%0*d", pad_width, i + 1);
                std::fprintf(stderr,
                    "warptempo_gui: rendering entry %d of %d: %s/%s.wav\n",
                    i + 1, total,
                    batch_folder.filename().string().c_str(), num_buf);

                RenderRequest req;
                req.source_audio_path    = q.source_audio_path;
                req.markers              = q.markers;
                req.transients           = q.transients;
                req.settings_passthrough = app.settings_passthrough;
                for (const auto& m : q.transients) {
                    if (m.disabled) continue;
                    req.transient_frames.push_back(m.effective_frame());
                }
                req.batch_folder   = batch_folder.string();
                req.batch_basename = num_buf;
                reqs.push_back(std::move(req));
            }

            const auto result = run_render_batch(reqs, "render queue");
            if (result.cancelled) {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                    result.rendered, total);
            } else {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries into %s\n",
                    result.rendered, total,
                    batch_folder.filename().string().c_str());
            }
            return;
        }

        // Brief T: Ctrl+Alt+I renders the Cartesian product of the
        // per-marker iter ranges authored in iteration mode. Output lands
        // in `<source_parent>/renders/<N>_render_iterations/`, with one
        // .wav per cell named `<seq>_<delta_csv>.wav`. The CSV holds the
        // swept markers' deltas in timeline order, formatted `%+0.2f`;
        // markers with no iter range authored are excluded from the CSV
        // and contribute one fixed value (their authored tempo_base) to
        // the product. Per-cell progress and Esc cancellation are handled
        // by run_render_batch. Silent no-op outside iteration mode.
        if (ctrl && alt && !shift &&
            (keysym == XK_i || keysym == XK_I)) {
            if (app.source_audio_path.empty()) return;
            if (!app.iteration_mode_enabled) return;

            // Snapshot markers in timeline order (GuiWarpMarkers guarantees
            // strict-monotonic by time_seconds). For each owning marker
            // build its per-cell delta list: a single 0.0 when no iter
            // range is authored, otherwise integer-cents enumeration from
            // iter_start to iter_end inclusive. Integer-cents avoids the
            // float-accumulation drift a naive `for (d=start; d<=end;
            // d+=0.01)` would suffer across many steps.
            const std::vector<GuiWarpMarker> base_markers =
                app.warpmarkers.markers();
            std::vector<int>                 eligible_indices;
            std::vector<std::vector<double>> per_marker_deltas;
            std::vector<bool>                is_swept;
            for (int i = 0; i < static_cast<int>(base_markers.size()); ++i) {
                const GuiWarpMarker& m = base_markers[i];
                if (!iter_popup_eligible_marker(m)) continue;
                eligible_indices.push_back(i);
                const bool swept =
                    !std::isnan(m.iter_start) && !std::isnan(m.iter_end);
                is_swept.push_back(swept);
                std::vector<double> deltas;
                if (swept) {
                    const int start_cents = static_cast<int>(
                        std::lround(m.iter_start * 100.0));
                    const int end_cents = static_cast<int>(
                        std::lround(m.iter_end * 100.0));
                    // commit_iter_edit enforces start <= end, but a stray
                    // hand-edit of memory could violate it. Treat that as
                    // no sweep rather than producing zero cells.
                    if (start_cents > end_cents) {
                        deltas.push_back(0.0);
                        is_swept.back() = false;
                    } else {
                        for (int c = start_cents; c <= end_cents; ++c) {
                            deltas.push_back(static_cast<double>(c) / 100.0);
                        }
                    }
                } else {
                    deltas.push_back(0.0);
                }
                per_marker_deltas.push_back(std::move(deltas));
            }

            bool any_swept = false;
            for (bool s : is_swept) {
                if (s) { any_swept = true; break; }
            }
            if (!any_swept) {
                std::fprintf(stderr,
                    "warptempo_gui: render-iterations: no iter ranges "
                    "authored; nothing to render\n");
                return;
            }

            size_t total_cells = 1;
            for (const auto& d : per_marker_deltas) total_cells *= d.size();
            if (total_cells == 0) return;

            std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path queue_root = src_parent / "renders";

            // Resolve the next batch index: max+1 over `<digits>_<anything>`
            // entries. Mirrors render_all_in_queue's scanner.
            int next_index = 1;
            std::error_code ec;
            if (std::filesystem::is_directory(queue_root, ec)) {
                int max_idx = 0;
                for (const auto& de :
                     std::filesystem::directory_iterator(queue_root, ec)) {
                    if (!de.is_directory()) continue;
                    const std::string name = de.path().filename().string();
                    int v = 0;
                    size_t i = 0;
                    while (i < name.size() &&
                           name[i] >= '0' && name[i] <= '9') {
                        v = v * 10 + (name[i] - '0');
                        ++i;
                    }
                    if (i == 0 || i >= name.size() || name[i] != '_') continue;
                    if (v > max_idx) max_idx = v;
                }
                next_index = max_idx + 1;
            }

            const std::string command_tag = "render_iterations";
            const std::filesystem::path batch_folder =
                queue_root /
                (std::to_string(next_index) + "_" + command_tag);
            std::filesystem::create_directories(batch_folder, ec);
            if (ec) {
                std::fprintf(stderr,
                    "warptempo_gui: render-iterations: could not create "
                    "'%s': %s\n",
                    batch_folder.string().c_str(), ec.message().c_str());
                return;
            }

            const int total = static_cast<int>(total_cells);
            int pad_width = 1;
            for (int n = total; n >= 10; n /= 10) ++pad_width;
            if (pad_width > 9) pad_width = 9;

            // Snapshot transients once — every cell shares the same
            // transient configuration, only marker tempo_base values
            // differ across cells.
            const std::vector<GuiTransientMarker> base_transients =
                app.transientmarkers.markers();
            std::vector<int64_t> base_transient_frames;
            for (const auto& t : base_transients) {
                if (t.disabled) continue;
                base_transient_frames.push_back(t.effective_frame());
            }

            // Cartesian product enumeration. `indices[k]` holds the
            // current cell coordinate along the k-th eligible marker
            // (timeline order). Rightmost dimension increments fastest:
            // consecutive cells differ in the last marker's delta first.
            const size_t num_dims = per_marker_deltas.size();
            std::vector<size_t> indices(num_dims, 0);

            std::vector<RenderRequest> reqs;
            reqs.reserve(total);
            for (int cell = 0; cell < total; ++cell) {
                std::string delta_csv;
                for (size_t k = 0; k < num_dims; ++k) {
                    if (!is_swept[k]) continue;
                    const double d = per_marker_deltas[k][indices[k]];
                    char dbuf[16];
                    std::snprintf(dbuf, sizeof(dbuf), "%+0.2f", d);
                    if (!delta_csv.empty()) delta_csv += ',';
                    delta_csv += dbuf;
                }

                char num_buf[16];
                std::snprintf(num_buf, sizeof(num_buf),
                              "%0*d", pad_width, cell + 1);
                std::string basename = num_buf;
                basename += '_';
                basename += delta_csv;

                std::vector<GuiWarpMarker> cell_markers = base_markers;
                for (size_t k = 0; k < num_dims; ++k) {
                    const int mi = eligible_indices[k];
                    cell_markers[mi].tempo_base =
                        base_markers[mi].tempo_base +
                        per_marker_deltas[k][indices[k]];
                    // The engine doesn't consume iter values; clear them
                    // so the request is quiet.
                    cell_markers[mi].iter_start =
                        std::numeric_limits<double>::quiet_NaN();
                    cell_markers[mi].iter_end =
                        std::numeric_limits<double>::quiet_NaN();
                }

                RenderRequest req;
                req.source_audio_path    = app.source_audio_path;
                req.markers              = std::move(cell_markers);
                req.transients           = base_transients;
                req.transient_frames     = base_transient_frames;
                req.settings_passthrough = app.settings_passthrough;
                req.batch_folder         = batch_folder.string();
                req.batch_basename       = std::move(basename);
                reqs.push_back(std::move(req));

                // Increment rightmost dimension; carry left on overflow.
                // The last cell leaves indices in an overflowed state but
                // the loop exits before that's read.
                for (int k = static_cast<int>(num_dims) - 1; k >= 0; --k) {
                    ++indices[k];
                    if (indices[k] < per_marker_deltas[k].size()) break;
                    indices[k] = 0;
                }
            }

            const auto result = run_render_batch(reqs, "render iterations");
            if (result.cancelled) {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                    result.rendered, total);
            } else {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries into %s\n",
                    result.rendered, total,
                    batch_folder.filename().string().c_str());
            }
            return;
        }

        // Brief X.3: Ctrl+Alt+M sweeps every BPM in the popup-owner's
        // [bpm_lo, bpm_hi] range, computing (base_tempo, scale) per cell
        // and rendering one .wav per cell into
        // `<source_parent>/renders/<N>_render_basetempo/`. The scale value
        // is encoded in the filename so Ctrl+Alt+C can later extract and
        // commit it back into source settings_passthrough. Mirrors the
        // iter render handler's structure; the substantive difference is
        // per-cell mutation of settings_passthrough's `scale` entry, in
        // addition to per-cell marker mutation. Silent no-op outside
        // BPM mode / warp / loaded audio / committed popup.
        if (ctrl && alt && !shift &&
            (keysym == XK_m || keysym == XK_M)) {
            if (app.active_mode != 'W') return;
            if (!app.bpm_mode_enabled) return;
            if (app.source_audio_path.empty()) return;
            if (audio.sample_rate() <= 0) return;
            if (audio.total_frames() <= 0) return;

            const std::vector<GuiWarpMarker> base_markers =
                app.warpmarkers.markers();
            int owner_idx = -1;
            for (int i = 0; i < static_cast<int>(base_markers.size()); ++i) {
                if (base_markers[i].bpm_is_popup_owner) {
                    owner_idx = i;
                    break;
                }
            }
            if (owner_idx < 0) return;
            const GuiWarpMarker& owner = base_markers[owner_idx];
            if (owner.bpm_beats <= 0) return;
            if (owner.bpm_lo    <= 0) return;
            if (owner.bpm_hi    <= 0) return;

            // Find the span endpoint: first effectively-enabled marker
            // after the owner. If none, the span runs to end-of-audio.
            int endpoint_idx = -1;
            for (int i = owner_idx + 1;
                 i < static_cast<int>(base_markers.size()); ++i) {
                if (effective_disabled(base_markers, i)) continue;
                endpoint_idx = i;
                break;
            }
            const double audio_total_seconds =
                static_cast<double>(audio.total_frames()) /
                static_cast<double>(audio.sample_rate());
            const double duration_seconds =
                (endpoint_idx >= 0)
                    ? (base_markers[endpoint_idx].time_seconds -
                       owner.time_seconds)
                    : (audio_total_seconds - owner.time_seconds);
            if (!(duration_seconds > 0.0)) return;

            std::vector<int> bpm_values;
            for (int b = owner.bpm_lo; b <= owner.bpm_hi; ++b) {
                bpm_values.push_back(b);
            }
            if (bpm_values.empty()) return;

            std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path queue_root = src_parent / "renders";

            int next_index = 1;
            std::error_code ec;
            if (std::filesystem::is_directory(queue_root, ec)) {
                int max_idx = 0;
                for (const auto& de :
                     std::filesystem::directory_iterator(queue_root, ec)) {
                    if (!de.is_directory()) continue;
                    const std::string name = de.path().filename().string();
                    int v = 0;
                    size_t i = 0;
                    while (i < name.size() &&
                           name[i] >= '0' && name[i] <= '9') {
                        v = v * 10 + (name[i] - '0');
                        ++i;
                    }
                    if (i == 0 || i >= name.size() || name[i] != '_') continue;
                    if (v > max_idx) max_idx = v;
                }
                next_index = max_idx + 1;
            }

            const std::string command_tag = "render_basetempo";
            const std::filesystem::path batch_folder =
                queue_root /
                (std::to_string(next_index) + "_" + command_tag);

            int pad_width = 1;
            for (int n = static_cast<int>(bpm_values.size());
                 n >= 10; n /= 10) ++pad_width;
            if (pad_width > 9) pad_width = 9;

            const std::vector<GuiTransientMarker> base_transients =
                app.transientmarkers.markers();
            std::vector<int64_t> base_transient_frames;
            for (const auto& t : base_transients) {
                if (t.disabled) continue;
                base_transient_frames.push_back(t.effective_frame());
            }

            std::vector<RenderRequest> reqs;
            reqs.reserve(bpm_values.size());
            int seq = 1;
            for (int bpm : bpm_values) {
                const auto computed = compute_base_tempo_scale(
                    duration_seconds, owner.bpm_beats, bpm);
                if (!computed) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-basetempo: rejected cell "
                        "bpm=%d (duration=%.6f, beats=%d)\n",
                        bpm, duration_seconds, owner.bpm_beats);
                    continue;
                }

                std::vector<GuiWarpMarker> cell_markers = base_markers;
                cell_markers[owner_idx].tempo_base  = computed->base_tempo;
                cell_markers[owner_idx].tempo_scale.clear();

                std::vector<std::pair<std::string, std::string>>
                    cell_settings = app.settings_passthrough;
                char scale_buf[32];
                std::snprintf(scale_buf, sizeof(scale_buf),
                              "%.6f", computed->scale);
                bool found_scale = false;
                for (auto& kv : cell_settings) {
                    if (kv.first == "scale") {
                        kv.second = scale_buf;
                        found_scale = true;
                        break;
                    }
                }
                if (!found_scale) {
                    cell_settings.emplace_back("scale", scale_buf);
                }

                char num_buf[16];
                std::snprintf(num_buf, sizeof(num_buf),
                              "%0*d", pad_width, seq);
                char rest_buf[96];
                std::snprintf(rest_buf, sizeof(rest_buf),
                              "_bpm=%d;basetempo=%.2f;scale=%.6f",
                              bpm, computed->base_tempo, computed->scale);
                std::string basename = num_buf;
                basename += rest_buf;

                RenderRequest req;
                req.source_audio_path    = app.source_audio_path;
                req.markers              = std::move(cell_markers);
                req.transients           = base_transients;
                req.transient_frames     = base_transient_frames;
                req.settings_passthrough = std::move(cell_settings);
                req.batch_folder         = batch_folder.string();
                req.batch_basename       = std::move(basename);
                reqs.push_back(std::move(req));
                ++seq;
            }

            if (reqs.empty()) {
                std::fprintf(stderr,
                    "warptempo_gui: render-basetempo: no valid cells; "
                    "nothing to render\n");
                return;
            }

            std::filesystem::create_directories(batch_folder, ec);
            if (ec) {
                std::fprintf(stderr,
                    "warptempo_gui: render-basetempo: could not create "
                    "'%s': %s\n",
                    batch_folder.string().c_str(), ec.message().c_str());
                return;
            }

            const int total = static_cast<int>(reqs.size());
            const auto result = run_render_batch(reqs, "basetempo");
            if (result.cancelled) {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                    result.rendered, total);
            } else {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries into %s\n",
                    result.rendered, total,
                    batch_folder.filename().string().c_str());
            }
            return;
        }

        // Chunk W: Ctrl+Alt+C commits the displayed render's markers
        // and transients into authoring memory. Single cross-file undo
        // entry; both warp_dirty and transient_dirty are recomputed.
        // After the commit succeeds: render-view exits, the parked
        // source audio is restored, and <source_parent>/renders/ is
        // recursively wiped — by definition the user has chosen one
        // render's parameters as the new baseline, so the prior batch
        // outputs are stale and shouldn't accumulate. Silent no-op
        // outside render-view.
        if (ctrl && alt && !shift &&
            (keysym == XK_c || keysym == XK_C)) {
            if (!app.render_view_enabled) return;
            if (app.render_view_index < 0) return;

            // Addendum 3: app.render_view_markers / _transients are now
            // render-domain (loaded from .renderwarpmarkers /
            // .rendertransientmarkers for display). The commit promotes
            // the render's *source-domain*
            // markers into authoring memory, so reload them from the
            // adjacent .warpmarkers / .transientmarkers sidecars at commit
            // time. Failure to read the source-domain warpmarkers aborts —
            // committing render-domain values into authoring would corrupt
            // the source coordinate system.
            const auto& cur_e =
                app.render_view_list[app.render_view_index];
            std::vector<GuiWarpMarker>    src_warp;
            std::vector<GuiTransientMarker> src_trans;
            {
                const std::filesystem::path wm =
                    cur_e.batch_folder / (cur_e.basename + ".warpmarkers");
                GuiWarpMarkers m;
                if (!m.load(wm.string())) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: commit aborted, failed "
                        "to load %s\n", wm.string().c_str());
                    return;
                }
                src_warp = m.markers();
            }
            {
                const std::filesystem::path tm = cur_e.batch_folder /
                    (cur_e.basename + ".transientmarkers");
                std::error_code ec;
                if (std::filesystem::exists(tm, ec)) {
                    GuiTransientMarkers t;
                    if (t.load(tm.string())) {
                        src_trans = t.markers();
                    }
                    // Load failure: treat as empty transients (the
                    // load() call already logged its own diagnostics).
                }
            }

            std::vector<GuiWarpMarker>    warp_pre  = app.warpmarkers.markers();
            std::vector<GuiTransientMarker> trans_pre = app.transientmarkers.markers();
            const int                 hint_last = app.last_selected_marker;

            app.warpmarkers.markers_mut()    = std::move(src_warp);
            app.transientmarkers.markers_mut() = std::move(src_trans);
            app.selected_markers.clear();
            app.last_selected_marker = -1;
            // Brief J.2 Section 4: the active tab's per-mode slots
            // referenced the OLD app.warpmarkers/transients we just
            // replaced. Clear them so restore_source_audio loads
            // empty into the live pair (and so a later mode flip
            // doesn't surface stale indices).
            {
                ViewState& t = (app.active_tab == 'B') ? app.tab_b
                                                       : app.tab_a;
                t.warp_selected.clear();
                t.warp_last_selected      = -1;
                t.transient_selected.clear();
                t.transient_last_selected = -1;
            }

            push_undo_both(std::move(warp_pre), std::move(trans_pre),
                           'W', OpKind::Other, hint_last);
            recompute_dirty();

            // Brief X.3: if the displayed render's basename carries a
            // `scale=<float>` token (BPM-sweep render filenames do; iter
            // and queue render filenames don't), extract the float and
            // overwrite (or append) the `scale` entry in
            // app.settings_passthrough. Settings has no undo by
            // convention; this mutation is permanent until the next
            // Ctrl+S overwrites or the user manually edits the file.
            // Conservative parse: any failure logs and skips, leaving
            // markers+transients commit unaffected.
            {
                const std::string& bn = cur_e.basename;
                const auto sp = bn.find("scale=");
                if (sp != std::string::npos) {
                    const size_t value_start = sp + 6;
                    size_t value_end = bn.size();
                    for (size_t i = value_start; i < bn.size(); ++i) {
                        if (bn[i] == ';') { value_end = i; break; }
                        if (i + 4 <= bn.size() &&
                            bn.compare(i, 4, ".wav") == 0) {
                            value_end = i;
                            break;
                        }
                    }
                    const std::string val_s =
                        bn.substr(value_start, value_end - value_start);
                    double parsed = 0.0;
                    bool   ok     = false;
                    if (!val_s.empty()) {
                        try {
                            size_t consumed = 0;
                            parsed = std::stod(val_s, &consumed);
                            ok = (consumed == val_s.size()) &&
                                 std::isfinite(parsed);
                        } catch (...) {
                            ok = false;
                        }
                    }
                    if (!ok) {
                        std::fprintf(stderr,
                            "warptempo_gui: render-view: could not parse "
                            "scale from basename '%s'; settings unchanged\n",
                            bn.c_str());
                    } else {
                        char fmt_buf[32];
                        std::snprintf(fmt_buf, sizeof(fmt_buf),
                                      "%.6f", parsed);
                        bool found_scale = false;
                        for (auto& kv : app.settings_passthrough) {
                            if (kv.first == "scale") {
                                kv.second = fmt_buf;
                                found_scale = true;
                                break;
                            }
                        }
                        if (!found_scale) {
                            app.settings_passthrough.emplace_back(
                                "scale", fmt_buf);
                        }
                    }
                }
            }

            const std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path renders_root =
                src_parent / "renders";

            restore_source_audio();
            app.render_view_enabled = false;
            app.render_view_list.clear();
            app.render_view_markers.clear();
            app.render_view_transients.clear();
            app.render_view_index             = -1;
            app.render_view_src_F_begin       = 0;
            app.render_view_src_F_end         = 0;
            app.last_render_view_path.clear();

            std::error_code ec;
            if (std::filesystem::is_directory(renders_root, ec)) {
                std::filesystem::remove_all(renders_root, ec);
                if (ec) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: failed to wipe "
                        "%s: %s\n",
                        renders_root.string().c_str(),
                        ec.message().c_str());
                }
            }

            std::fprintf(stderr,
                "warptempo_gui: render-view: committed render and wiped "
                "renders/\n");
            gui.invalidate_region(0, 0, app.width, app.height);
            return;
        }

        // Ctrl+Shift+Alt+T: clear every transient marker (undoable).
        // Checked before Ctrl+Alt+T so the more-specific binding wins.
        if (ctrl && shift && alt &&
            (keysym == XK_t || keysym == XK_T)) {
            clear_all_transients();
            return;
        }

        // Ctrl+Alt+T: run transient detection (with a confirmation dialog
        // when there's already a prior detection in the list).
        if (ctrl && alt && !shift &&
            (keysym == XK_t || keysym == XK_T)) {
            detect_transients();
            return;
        }

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

        // Ctrl+Z undo / Ctrl+Shift+Z redo. Placed before the XK_s save
        // handling so modifier dispatch reads left-to-right in the source.
        // Both are silent no-ops when their respective stack is empty.
        if (ctrl && keysym == XK_z) {
            if (shift) do_redo();
            else       do_undo();
            return;
        }

        // `t` (no modifiers) toggles transient mode globally. Brief
        // J.2: render-view shares the global active_mode flag, so a
        // single handler serves both views. Render-view inherits the
        // engine / transients_enabled precondition checks from
        // toggle_active_mode.
        if (keysym == XK_t && !ctrl && !shift && !alt) {
            toggle_active_mode();
            return;
        }

        // V.B `i` (no modifiers) toggles iteration mode in warp. Silent
        // no-op in transient mode (transient flags carry no tempo to
        // iterate). The editor-active branch above already swallows any
        // keystroke while a popup edit is in flight, so this code only
        // runs with no active editor. Toggling repaints the top strip
        // so iteration popups appear or vanish in one frame.
        if (keysym == XK_i && !ctrl && !shift && !alt) {
            if (app.active_mode == 'W') {
                // Brief X.2: mutual exclusion. Toggling iter ON forces
                // BPM mode off; toggling iter OFF leaves BPM untouched.
                const bool turning_on = !app.iteration_mode_enabled;
                if (turning_on && app.bpm_mode_enabled) {
                    app.bpm_mode_enabled = false;
                }
                app.iteration_mode_enabled = !app.iteration_mode_enabled;
                clear_hover_popup();
                invalidate_top_strip();
            }
            return;
        }
        // V.B Shift+I: bulk-clear every marker's iter values AND exit
        // iteration mode in one keystroke ("stop authoring this mode").
        // Only fires while iteration mode is on; otherwise silent no-op.
        if (keysym == XK_i && !ctrl && shift && !alt) {
            if (app.active_mode == 'W' && app.iteration_mode_enabled) {
                bulk_clear_iter_values();
                app.iteration_mode_enabled = false;
                invalidate_top_strip();
            }
            return;
        }

        // Brief X.2 `m` (no modifiers): toggle BPM mode in warp. Silent
        // no-op in transient mode. Mutual exclusion with iter mode is
        // handled inside enter_bpm_mode.
        if (keysym == XK_m && !ctrl && !shift && !alt) {
            if (app.active_mode == 'W') {
                if (app.bpm_mode_enabled) {
                    exit_bpm_mode();
                } else {
                    enter_bpm_mode();
                }
            }
            return;
        }
        // Brief X.2 Shift+M: bulk-clear every marker's BPM values AND
        // exit BPM mode in one keystroke ("stop authoring this mode").
        // Only fires while BPM mode is on; otherwise silent no-op.
        if (keysym == XK_m && !ctrl && shift && !alt) {
            if (app.active_mode == 'W' && app.bpm_mode_enabled) {
                bulk_clear_bpm_values();
                exit_bpm_mode();
            }
            return;
        }

        // Chunk W: plain `r` toggles render analysis mode. Source audio
        // must be loaded; otherwise silent no-op (nothing to base the
        // renders folder lookup on). Toggle-on enumerates the renders
        // folder and loads either the last-displayed render (if its
        // path is still in the list) or the first entry; an empty
        // enumeration aborts the toggle. Iteration mode is forcibly
        // disabled on entry per the chunk W brief; the prior value is
        // not restored on toggle-off — the user re-enables it
        // explicitly if desired.
        if (keysym == XK_r && !ctrl && !shift && !alt) {
            if (app.source_audio_path.empty()) return;
            if (app.loading) return;
            if (!app.render_view_enabled) {
                std::vector<AppState::RenderViewEntry> list =
                    enumerate_render_view_list();
                if (list.empty()) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: no renders found "
                        "under %s/renders/\n",
                        std::filesystem::path(app.source_audio_path)
                            .parent_path().string().c_str());
                    return;
                }
                // Brief F Section 4: migrate persisted selection from
                // the prior render-view session (still on the old
                // app.render_view_list) into the freshly enumerated
                // list, keyed by wav_path. Entries that disappeared
                // since last session simply lose their persisted state;
                // newly added entries start with default-empty
                // persistence (no match → load_render_view_at clears).
                if (!app.render_view_list.empty()) {
                    std::map<std::string,
                        AppState::RenderViewEntry*> prior;
                    for (auto& pe : app.render_view_list) {
                        prior[pe.wav_path.string()] = &pe;
                    }
                    for (auto& ne : list) {
                        auto it = prior.find(ne.wav_path.string());
                        if (it == prior.end()) continue;
                        const auto& src = *it->second;
                        ne.state           = src.state;
                        ne.persisted_size  = src.persisted_size;
                        ne.persisted_mtime = src.persisted_mtime;
                    }
                }
                int target = 0;
                if (!app.last_render_view_path.empty()) {
                    for (size_t i = 0; i < list.size(); ++i) {
                        if (list[i].wav_path.string() ==
                            app.last_render_view_path) {
                            target = static_cast<int>(i);
                            break;
                        }
                    }
                }
                app.render_view_src_sr    = audio.sample_rate();
                app.render_view_src_total = audio.total_frames();
                app.render_view_list      = std::move(list);
                app.iteration_mode_enabled = false;
                // Brief X.2: BPM mode is force-off on render-view entry,
                // mirroring iter. Stored values persist (in-memory only,
                // never serialized) and re-appear if the user toggles
                // BPM mode back on after exiting render-view.
                app.bpm_mode_enabled       = false;
                app.render_view_enabled    = true;
                // Brief J.2: render-view shares the global active_mode
                // flag, so the user's chosen mode carries across the
                // view-domain transition without per-entry restore.
                if (!load_render_view_at(target)) {
                    app.render_view_enabled = false;
                    app.render_view_list.clear();
                }
            } else {
                // Capture the just-viewed render's zoom/viewport/playhead
                // before restoring source-audio state. Not done on the
                // Ctrl+Alt+C commit path — the renders folder is wiped
                // immediately after commit, so the write would be lost.
                if (app.render_view_index >= 0 &&
                    app.render_view_index <
                        static_cast<int>(app.render_view_list.size())) {
                    write_rendersettings_for(
                        app.render_view_list[app.render_view_index]);
                }
                // Brief F Section 4: stash the live selection onto
                // the active entry so the next toggle-on can restore
                // it (gated by the wav's stat tuple still matching).
                // render_view_list is intentionally NOT cleared here
                // — re-entry migrates its persisted_* fields into the
                // freshly enumerated list.
                stash_render_view_selection_to_active_entry();
                restore_source_audio();
                app.render_view_enabled = false;
                app.render_view_markers.clear();
                app.render_view_transients.clear();
                app.render_view_index             = -1;
                app.render_view_src_F_begin       = 0;
                app.render_view_src_F_end         = 0;
            }
            return;
        }

        // XLookupKeysym with index 0 returns the unshifted keysym, so a
        // Shift+letter press arrives as the lowercase XK_* with ShiftMask in
        // mods — disambiguate via the `shift` bool, not uppercase keysyms.
        if (keysym == XK_s) {
            if (ctrl)                          save_markers();
            else if (app.active_mode == 'T')   drop_transient_at_playhead();
            else if (shift)                    drop_inherit_marker_at_playhead();
            else                               drop_marker_at_playhead();
            return;
        }
        // Shift+P: toggle inherit (warp only). Plain `p` is unbound.
        if (keysym == XK_p && !ctrl && !alt && shift) {
            if (app.active_mode == 'T') return;
            toggle_inherits();
            return;
        }
        // Ctrl+D: toggle disabled (warp + transient). Plain `d` and Shift+D are unbound.
        if (keysym == XK_d && ctrl && !alt && !shift) {
            if (app.active_mode == 'T') toggle_transient_disabled();
            else                        toggle_disabled();
            return;
        }
        if (keysym == XK_Delete && !ctrl) {
            if (app.active_mode == 'T') {
                delete_selected_transient();
                return;
            }
            if (shift) force_delete_selected_marker();
            else       delete_selected_marker();
            return;
        }

        // Ctrl+Tab toggles A/B navigational tabs. Stops playback, saves
        // current viewport/zoom/playhead to the leaving tab, restores the
        // target tab. Does not mark the document dirty.
        if (ctrl && !shift && keysym == XK_Tab) {
            tab_mode.switch_active_tab_to(app.active_tab == 'A' ? 'B' : 'A');
            return;
        }

        if (keysym == XK_Tab && !shift) { select_next_marker(); return; }
        if (keysym == XK_Tab && shift)  { select_prev_marker(); return; }
        if (keysym == XK_ISO_Left_Tab)  { select_prev_marker(); return; }

        // Tempo nudge. Ctrl+Up / Ctrl+Down only. Bare `=` / `-` were the
        // previous binding; they now zoom (see below) so the keyboard has
        // a symbol-key alias for the bare Up/Down zoom chord.
        if (ctrl && !shift && !alt && keysym == XK_Up) {
            adjust_tempo(+0.01); return;
        }
        if (ctrl && !shift && !alt && keysym == XK_Down) {
            adjust_tempo(-0.01); return;
        }
        if (keysym == XK_equal && !shift && !ctrl && !alt) {
            zoom_in(); return;
        }
        if (keysym == XK_minus && !shift && !ctrl && !alt) {
            zoom_out(); return;
        }

        // `l` (no modifier) clears any b= / e= flags. `Shift+L` clears the
        // selection set (UI-only — no dirty, no playhead move).
        if (keysym == XK_l && !ctrl) {
            if (shift) clear_selection();
            else       clear_trim();
            return;
        }

        // `j` jumps the selected set to the playhead, anchored on
        // last_selected_marker. All-or-nothing clamp check.
        if (keysym == XK_j && !shift && !ctrl) {
            if (app.active_mode == 'T') jump_transient_selection_to_playhead();
            else                        jump_selection_to_playhead();
            return;
        }

        // Chunk W: Shift+Left / Shift+Right navigates the render-view
        // list with wraparound. Active only when render_view_enabled is
        // true; in source-view these chords fall through to the normal
        // playhead-by-pixel handler in the switch below. Wraparound
        // mirrors the brief: Shift+Right past the end loops to index 0,
        // Shift+Left before index 0 loops to the last entry.
        if (app.render_view_enabled && shift && !ctrl && !alt &&
            (keysym == XK_Left || keysym == XK_Right)) {
            const int n = static_cast<int>(app.render_view_list.size());
            if (n <= 0) return;
            int next = app.render_view_index;
            if (keysym == XK_Left)  next = (next - 1 + n) % n;
            else                    next = (next + 1) % n;
            // Capture the outgoing render's live zoom/viewport/playhead
            // before swapping.
            if (app.render_view_index >= 0 &&
                app.render_view_index <
                    static_cast<int>(app.render_view_list.size())) {
                write_rendersettings_for(
                    app.render_view_list[app.render_view_index]);
            }
            // Brief F Section 4: stash the outgoing entry's
            // selection so re-navigating back later (in the same
            // session) restores it. load_render_view_at then loads
            // the destination's own persisted state if its stat tuple
            // still matches; otherwise leaves selection empty.
            stash_render_view_selection_to_active_entry();
            load_render_view_at(next);
            return;
        }

        // Ctrl+Left / Ctrl+Right: nudge selected markers by one pixel.
        if (ctrl && !shift && keysym == XK_Left) {
            if (app.active_mode == 'T') nudge_selected_transients(-1);
            else                        nudge_selected_markers(-1);
            return;
        }
        if (ctrl && !shift && keysym == XK_Right) {
            if (app.active_mode == 'T') nudge_selected_transients(+1);
            else                        nudge_selected_markers(+1);
            return;
        }

        // Bare-key dispatch. Every modifier-gated handler above this point
        // returns on match, so by the time we reach here, any modifier being
        // held means the chord had no binding and should be a silent no-op
        // — never fall through into a bare binding (e.g. Ctrl+Shift+Alt+E
        // must not toggle end-time via XK_e).
        if (!ctrl && !shift && !alt) {
            switch (keysym) {
            case XK_Escape: /* top-level Escape is a no-op (chunk Q) */ break;
            case XK_Left:   stop_playback_if_playing();
                            move_playhead_pixels(-1);         break;
            case XK_Right:  stop_playback_if_playing();
                            move_playhead_pixels(+1);         break;
            case XK_Up:     zoom_in();                        break;
            case XK_Down:   zoom_out();                       break;
            case XK_f: {
                const bool was_off = !app.follow_mode;
                app.follow_mode = !app.follow_mode;
                if (was_off && app.follow_mode &&
                    playback.is_playing()) {
                    playback.resync_predictor();
                }
                break;
            }
            case XK_c:      center_viewport_on_playhead();    break;
            case XK_Home:   stop_playback_if_playing();
                            move_playhead_to(trim_begin_sample()); break;
            case XK_End:    stop_playback_if_playing();
                            move_playhead_to(trim_end_sample() - 1); break;
            case XK_b:      if (app.active_mode == 'T') toggle_transient_begin_time();
                            else                        toggle_begin_time();
                            break;
            case XK_e:      if (app.active_mode == 'T') toggle_transient_end_time();
                            else                        toggle_end_time();
                            break;
            // TODO: growing binding set will want an in-GUI help overlay.
            default: break;
            }
        }
    });

    gui.set_on_close([&]() {
        // Window-manager close (title-bar X) routes through the unsaved-
        // work dialog when dirty, same as Ctrl+Q.
        request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
    });

    gui.set_on_button_press([&](unsigned int button, int x, int y,
                                unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        // Prompt-modal input handling: while the bottom-strip prompt is
        // active, all mouse events are swallowed. Responses go through
        // the keyboard.
        if (app.prompt.active) return;
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

        // Defensive: a second press during a drag is ignored (left button
        // should still be held down for a drag to exist).
        if (app.drag.active) return;

        // Chunk W: render-view mouse gate. Left-click on a marker line
        // (in the waveform area) or a flag rect (in the top strip)
        // toggles selection and jumps the playhead to the marker;
        // left-click elsewhere in the waveform area positions the
        // playhead (with playback stop) and clears the selection unless
        // Shift is held. All wheel chords (zoom, Alt/Ctrl+Alt pan,
        // Ctrl+wheel playhead-move) are pure viewport / playhead ops and
        // pass through unchanged. Drag-create and top-strip playhead
        // movement are silent no-ops so the read-only invariant on
        // marker state is preserved. Hover-popup motion still runs in
        // the motion handler against render_view_markers.
        if (app.render_view_enabled) {
            if (button == 4 || button == 5) {
                handle_wheel(button, ctrl, alt,
                             inside_waveform, inside_top);
                return;
            }
            if (button != 1) return;
            // Brief F Section 3: in transient sub-view, top-strip clicks
            // are silent no-ops (transients have no flag rects). Bail
            // before hit-testing so we don't attempt selection bookkeeping
            // on a non-existent flag pack.
            if (app.active_mode == 'T' && inside_top) return;
            int hit = -1;
            if (inside_waveform)  hit = hit_test_marker_line(x);
            else if (inside_top)  hit = hit_test_flag(x, y);
            else                  return;
            // Brief J.2 Section 3: live selection lives in the global
            // pair regardless of view domain. active_mode tells us
            // which marker list the indices map to.
            const bool sub_t = (app.active_mode == 'T');
            std::set<int>& sel = app.selected_markers;
            int& last_sel      = app.last_selected_marker;
            const int n = sub_t
                ? static_cast<int>(app.render_view_transients.size())
                : static_cast<int>(app.render_view_markers.size());
            if (hit >= 0 && hit < n) {
                if (shift) {
                    auto it = sel.find(hit);
                    if (it == sel.end()) {
                        sel.insert(hit);
                        last_sel = hit;
                    } else {
                        sel.erase(it);
                        if (last_sel == hit) {
                            last_sel = sel.empty()
                                ? -1
                                : *sel.rbegin();
                        }
                    }
                } else {
                    sel.clear();
                    sel.insert(hit);
                    last_sel = hit;
                }
                gui.invalidate_region(0, 0, app.width, app.height);
                const int sr = audio.sample_rate();
                int64_t sample;
                if (sub_t) {
                    sample = app.render_view_transients[hit].effective_frame();
                } else {
                    sample = static_cast<int64_t>(std::llround(
                        app.render_view_markers[hit].time_seconds *
                        static_cast<double>(sr)));
                }
                move_playhead_to(sample);
                // Brief F Section 2: any waveform-area press starts a
                // playhead-drag gesture. Top-strip flag-click does not.
                if (inside_waveform) {
                    app.playhead_drag.active = true;
                }
                return;
            }
            // Empty-space click in the waveform area: clear the active
            // sub-view's selection (unless Shift) and move the playhead.
            // Brief F Section 2: also start a playhead-drag gesture so
            // the motion handler's snap logic kicks in.
            if (inside_waveform) {
                if (!shift &&
                    (!sel.empty() || last_sel != -1)) {
                    sel.clear();
                    last_sel = -1;
                    gui.invalidate_region(0, 0, app.width, app.height);
                }
                stop_playback_if_playing();
                const double spp = current_samples_per_pixel(app, audio);
                int rel = x - area.x;
                if (rel < 0) rel = 0;
                if (rel >= area.w) rel = area.w - 1;
                const int64_t sample =
                    app.viewport_start_sample +
                    static_cast<int64_t>(std::llround(rel * spp));
                move_playhead_to(sample);
                app.playhead_drag.active = true;
            }
            return;
        }

        if (button == 1) {
            // Any button-1 press on the waveform / top strip stops
            // playback. Per Part 4 of chunk P patch 1: the user pressed
            // a mouse button, they want attention — even a Ctrl+press on
            // empty space (a no-op for the playhead) stops the audio.
            if (inside_waveform || inside_top) stop_playback_if_playing();

            // V.A1 / V.B editor: mouse handling.
            //   click inside top strip on the editing target: no-op
            //   click inside top strip on a different popup/flag: switch
            //     target (iter popup wins over the flag below it when
            //     iteration mode is on)
            //   click anywhere else: exit edit (no commit), then fall
            //     through so the click routes through normal handling.
            if (text_editor::is_active(app.top_flag_editor)) {
                if (inside_top) {
                    const int iter_hit = hit_test_iter_popup(x, y);
                    if (iter_hit >= 0) {
                        if (app.top_flag_editor.kind ==
                                text_editor::Kind::IterationBracket &&
                            iter_hit == app.top_flag_editor.target) {
                            return; // no-op on same popup
                        }
                        enter_iter_edit(iter_hit);
                        return;
                    }
                    const int bpm_hit = hit_test_bpm_popup(x, y);
                    if (bpm_hit >= 0) {
                        if (app.top_flag_editor.kind ==
                                text_editor::Kind::BpmBracket &&
                            bpm_hit == app.top_flag_editor.target) {
                            return; // no-op on same popup
                        }
                        enter_bpm_edit(bpm_hit);
                        return;
                    }
                    const int hit_now = hit_test_flag(x, y);
                    if (app.top_flag_editor.kind ==
                            text_editor::Kind::FlagPayload &&
                        hit_now == app.top_flag_editor.target) {
                        return; // no-op on same flag
                    }
                    if (hit_now >= 0 && app.active_mode != 'T') {
                        enter_top_flag_edit(hit_now);
                        return;
                    }
                    // Top strip click that isn't on a popup or flag: exit
                    // and fall through to normal handling.
                    exit_top_flag_edit_no_commit();
                } else {
                    exit_top_flag_edit_no_commit();
                    // Fall through so the click can drive a playhead
                    // drag, marker click, etc.
                }
            }

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
            // the click position (not the playhead). In warp mode, drops a
            // warp marker (Shift forces inherit). In transient mode, drops
            // a transient (Shift is ignored — no inherit concept). The
            // first single-click already moved the playhead via the
            // playhead-drag-press logic below.
            if (is_double && inside_waveform && !ctrl) {
                const double spp = current_samples_per_pixel(app, audio);
                const int click_rel_x = x - area.x;
                const int sr = audio.sample_rate();
                const int64_t sample = app.viewport_start_sample +
                    static_cast<int64_t>(std::llround(click_rel_x * spp));
                const double t = (sr > 0)
                    ? static_cast<double>(sample) / static_cast<double>(sr)
                    : 0.0;
                if (app.active_mode == 'T') {
                    drop_transient_at_position(t);
                } else {
                    drop_marker(t, /*inherit=*/shift);
                }
                // Consume this click so a triple-click doesn't double-fire.
                app.last_click_consumed = true;
                return;
            }

            // Store this click for the next one to compare against.
            app.last_click_time     = now;
            app.last_click_x        = x;
            app.last_click_y        = y;
            app.last_click_consumed = false;

            // Iteration popup click takes priority over flag click when
            // iteration mode is on. The popup sits above the flag rect so
            // their hit zones don't overlap, but checking the popup first
            // makes the intent unambiguous when the flag-strip extents
            // change shape.
            if (inside_top && !ctrl) {
                const int iter_hit = hit_test_iter_popup(x, y);
                if (iter_hit >= 0) {
                    enter_iter_edit(iter_hit);
                    return;
                }
                const int bpm_hit = hit_test_bpm_popup(x, y);
                if (bpm_hit >= 0) {
                    enter_bpm_edit(bpm_hit);
                    return;
                }
            }

            // Consolidated hit-test across waveform (marker line) and top
            // strip (flag rect). A flag click behaves exactly like a click
            // on its marker line.
            int hit = -1;
            bool in_click_region = false;
            if (inside_waveform) {
                hit = hit_test_marker_line(x);
                in_click_region = true;
            } else if (inside_top) {
                hit = hit_test_flag(x, y);
                in_click_region = true;
            }

            if (!in_click_region) return;

            if (ctrl) {
                // Ctrl branch: marker-reposition drag or no-op on empty.
                if (hit >= 0) {
                    // begin_drag preserves the multi-selection if `hit` is in
                    // it, else collapses to just `hit`. Motion decides whether
                    // it actually becomes a drag vs. a plain click.
                    begin_drag(hit, x);
                }
                // else: Ctrl+press on empty space is a silent no-op.
                return;
            }

            // Non-Ctrl: plain or Shift press. In the waveform area this
            // starts a playhead-drag gesture. In the top strip (flag click)
            // a warp-mode flag click enters the V.A1 text editor; in
            // transient mode we keep the legacy select-on-click behavior.
            if (inside_top) {
                if (hit >= 0) {
                    if (app.active_mode != 'T' && !shift) {
                        // V.A1: plain click on a warp flag enters edit
                        // mode. Selects the marker as well so the rest of
                        // the UI tracks (timestamp jumps, marker column
                        // highlights). Shift+click keeps the legacy
                        // multi-select toggle.
                        set_single_selection(hit);
                        const int sr = audio.sample_rate();
                        const int64_t sample = static_cast<int64_t>(std::llround(
                            app.warpmarkers.markers()[hit].time_seconds *
                            static_cast<double>(sr)));
                        move_playhead_to(sample);
                        enter_top_flag_edit(hit);
                        return;
                    }
                    if (shift) toggle_selection_membership(hit);
                    else       set_single_selection(hit);
                    const int sr = audio.sample_rate();
                    int64_t sample;
                    if (app.active_mode == 'T') {
                        sample = app.transientmarkers.markers()[hit].effective_frame();
                    } else {
                        sample = static_cast<int64_t>(std::llround(
                            app.warpmarkers.markers()[hit].time_seconds *
                            static_cast<double>(sr)));
                    }
                    move_playhead_to(sample);
                }
                return;
            }

            // Waveform-area press: start playhead drag gesture.
            {
                const int sr = audio.sample_rate();
                if (hit >= 0) {
                    // Press on a marker (within 3px).
                    if (!shift) {
                        set_single_selection(hit);
                    } else {
                        // Shift+press on marker: selection otherwise preserved;
                        // add hit if not already present.
                        const bool was_in = app.selected_markers.count(hit) > 0;
                        if (!was_in) {
                            app.selected_markers.insert(hit);
                            invalidate_marker_column(hit);
                            invalidate_top_strip();
                        }
                        app.last_selected_marker = hit;
                    }
                    int64_t sample;
                    if (app.active_mode == 'T') {
                        sample = app.transientmarkers.markers()[hit].effective_frame();
                    } else {
                        sample = static_cast<int64_t>(std::llround(
                            app.warpmarkers.markers()[hit].time_seconds *
                            static_cast<double>(sr)));
                    }
                    move_playhead_to(sample);
                    app.playhead_drag.active = true;
                } else {
                    // Press on empty waveform.
                    const double spp = current_samples_per_pixel(app, audio);
                    const int click_rel_x = x - area.x;
                    if (click_rel_x < 0 || click_rel_x >= area.w) {
                        if (!shift) clear_selection();
                        return;
                    }
                    const int64_t sample = app.viewport_start_sample +
                        static_cast<int64_t>(std::llround(click_rel_x * spp));
                    if (!shift) clear_selection();
                    move_playhead_to(sample);
                    app.playhead_drag.active = true;
                }
            }
        } else if (button == 4 || button == 5) {
            handle_wheel(button, ctrl, alt,
                         inside_waveform, inside_top);
        }
    });

    gui.set_on_button_release([&](unsigned int button, int, int,
                                  unsigned int mods) {
        if (app.prompt.active) return;
        if (button != 1) return;
        if (app.playhead_drag.active) {
            // Brief F Section 1: if the playhead snapped onto a marker
            // during the drag, commit selection on release. Plain release
            // sets the snapped marker as the single selection; Shift
            // release adds it to the existing set without removing
            // anything. Off-marker release leaves selection alone.
            const bool shift = (mods & ShiftMask) != 0;
            const int  sr    = audio.sample_rate();
            int snapped = -1;
            if (sr > 0) {
                const int64_t ph = app.playhead_sample;
                if (app.render_view_enabled) {
                    if (app.active_mode == 'T') {
                        const auto& mv = app.render_view_transients;
                        for (size_t i = 0; i < mv.size(); ++i) {
                            if (mv[i].effective_frame() == ph) {
                                snapped = static_cast<int>(i);
                                break;
                            }
                        }
                    } else {
                        const auto& mv = app.render_view_markers;
                        for (size_t i = 0; i < mv.size(); ++i) {
                            const int64_t s = static_cast<int64_t>(
                                std::llround(mv[i].time_seconds *
                                             static_cast<double>(sr)));
                            if (s == ph) {
                                snapped = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                } else if (app.active_mode == 'T') {
                    const auto& mv = app.transientmarkers.markers();
                    for (size_t i = 0; i < mv.size(); ++i) {
                        if (mv[i].effective_frame() == ph) {
                            snapped = static_cast<int>(i);
                            break;
                        }
                    }
                } else {
                    const auto& mv = app.warpmarkers.markers();
                    for (size_t i = 0; i < mv.size(); ++i) {
                        const int64_t s = static_cast<int64_t>(
                            std::llround(mv[i].time_seconds *
                                         static_cast<double>(sr)));
                        if (s == ph) {
                            snapped = static_cast<int>(i);
                            break;
                        }
                    }
                }
            }
            if (snapped >= 0) {
                if (app.render_view_enabled) {
                    // Brief J.2 Section 3: render-view writes the
                    // global live pair. active_mode tells us which
                    // marker list the indices map to.
                    if (shift) {
                        app.selected_markers.insert(snapped);
                        app.last_selected_marker = snapped;
                    } else {
                        app.selected_markers.clear();
                        app.selected_markers.insert(snapped);
                        app.last_selected_marker = snapped;
                    }
                    gui.invalidate_region(0, 0, app.width, app.height);
                } else if (shift) {
                    app.selected_markers.insert(snapped);
                    app.last_selected_marker = snapped;
                    invalidate_marker_column(snapped);
                    invalidate_top_strip();
                } else {
                    set_single_selection(snapped);
                }
            }
            app.playhead_drag = PlayheadDragState{};
            return;
        }
        if (!app.drag.active) return;
        commit_drag();
    });

    gui.set_on_motion([&](int mouse_x, int mouse_y, unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        // V.A3b Addendum 3: record latest cursor coords so viewport
        // mutators can re-evaluate hover at the cursor's last position.
        app.last_mouse_x = mouse_x;
        app.last_mouse_y = mouse_y;
        if (app.prompt.active) {
            clear_hover_popup();
            return;
        }
        // Chunk W: render-view motion handler. Brief F Section 2 adds
        // playhead-drag snap support: when a drag is in flight, snap the
        // playhead to the visible sub-view's markers (3px epsilon),
        // matching source-view's gesture. Otherwise run hover popup
        // detection against render_view_markers (suppressed in transient
        // sub-view because hit_test_flag short-circuits to -1).
        if (app.render_view_enabled) {
            if (app.playhead_drag.active) {
                clear_hover_popup();
                if ((mods & Button1Mask) == 0) {
                    app.playhead_drag = PlayheadDragState{};
                    return;
                }
                const int sr = audio.sample_rate();
                if (sr <= 0) return;
                const GuiRect area = waveform_area(app);
                const double spp = current_samples_per_pixel(app, audio);
                if (spp <= 0.0) return;
                const int hit = hit_test_marker_line(mouse_x);
                int64_t new_playhead;
                if (hit >= 0) {
                    if (app.active_mode == 'T') {
                        new_playhead =
                            app.render_view_transients[hit].effective_frame();
                    } else {
                        new_playhead = static_cast<int64_t>(std::llround(
                            app.render_view_markers[hit].time_seconds *
                            static_cast<double>(sr)));
                    }
                } else {
                    int rel = mouse_x - area.x;
                    if (rel < 0) rel = 0;
                    if (rel >= area.w) rel = area.w - 1;
                    new_playhead = app.viewport_start_sample +
                        static_cast<int64_t>(std::llround(rel * spp));
                }
                if (new_playhead != app.playhead_sample) {
                    move_playhead_to(new_playhead);
                }
                return;
            }
            const int hit = hit_test_flag(mouse_x, mouse_y);
            if (hit != app.hover_popup.marker_index) {
                if (app.hover_popup.visible) invalidate_top_strip();
                app.hover_popup.marker_index = hit;
                app.hover_popup.visible      = false;
                app.hover_popup.entry_time   =
                    std::chrono::steady_clock::now();
                app.hover_popup.cached_text =
                    popup_eligible_marker(hit)
                        ? compute_hover_popup_text(
                              app.render_view_markers, hit,
                              app.render_view_src_sr)
                        : std::string();
            }
            return;
        }
        if (app.playhead_drag.active) {
            clear_hover_popup();
            // Left button must still be held; if not, the release was lost —
            // terminate the drag. Modifier changes mid-drag are ignored.
            if ((mods & Button1Mask) == 0) {
                app.playhead_drag = PlayheadDragState{};
                return;
            }
            const int sr = audio.sample_rate();
            if (sr <= 0) return;
            const GuiRect area = waveform_area(app);
            const double spp = current_samples_per_pixel(app, audio);
            if (spp <= 0.0) return;

            // Marker snap test — uses the same 3px epsilon as marker hit-test.
            // Selection is fixed at press time and is NOT mutated here; the
            // snap is purely a playhead-positioning magnet.
            const int hit = hit_test_marker_line(mouse_x);
            int64_t new_playhead;
            if (hit >= 0) {
                if (app.active_mode == 'T') {
                    new_playhead = app.transientmarkers.markers()[hit].effective_frame();
                } else {
                    new_playhead = static_cast<int64_t>(std::llround(
                        app.warpmarkers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
                }
            } else {
                // No marker within epsilon: playhead follows cursor freely.
                int rel = mouse_x - area.x;
                if (rel < 0) rel = 0;
                if (rel >= area.w) rel = area.w - 1;
                new_playhead = app.viewport_start_sample +
                    static_cast<int64_t>(std::llround(rel * spp));
            }

            if (new_playhead != app.playhead_sample) {
                move_playhead_to(new_playhead);
            }
            return;
        }
        if (!app.drag.active) {
            // No active gesture: run hover-popup detection. Only in warp
            // mode, with no editor, no dialog (already returned), no drag,
            // and not while iteration mode owns the popup space.
            // The dwell timer is started/restarted on every transition into
            // an eligible rect; the on_tick handler flips visibility.
            if (app.active_mode == 'W' &&
                !app.iteration_mode_enabled &&
                !text_editor::is_active(app.top_flag_editor) &&
                !app.queue_running) {
                const int hit = hit_test_flag(mouse_x, mouse_y);
                if (hit != app.hover_popup.marker_index) {
                    if (app.hover_popup.visible) invalidate_top_strip();
                    app.hover_popup.marker_index = hit;
                    app.hover_popup.visible      = false;
                    app.hover_popup.entry_time   =
                        std::chrono::steady_clock::now();
                    // Precompute popup text at rect-entry so the
                    // delay-completion paint doesn't repeat the math.
                    app.hover_popup.cached_text =
                        popup_eligible_marker(hit)
                            ? compute_hover_popup_text(
                                  app.warpmarkers.markers(), hit,
                                  audio.sample_rate())
                            : std::string();
                }
            } else {
                clear_hover_popup();
            }
            return;
        }
        // A drag is active — drop any pending popup.
        clear_hover_popup();
        // Left button must still be held down — otherwise release was lost.
        if ((mods & Button1Mask) == 0) {
            commit_drag();
            return;
        }
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const GuiRect area = waveform_area(app);
        const double spp = current_samples_per_pixel(app, audio);
        const double sr_d = static_cast<double>(sr);
        const double vp_time = static_cast<double>(app.viewport_start_sample) / sr_d;
        const double mouse_time = vp_time +
            static_cast<double>(mouse_x - area.x) * spp / sr_d;
        apply_drag_motion(mouse_time - app.drag.anchor_mouse_time_seconds);

        // Track the playhead with the grabbed marker. The drag applies a
        // uniform delta across the dragging set, so the hit marker's
        // post-motion time matches the user's cursor intent. Viewport is
        // deliberately not followed — the user can pan manually if the
        // drag runs past the edge.
        const int hit_idx = app.drag.hit_marker;
        const bool transient_drag = (app.drag.drag_mode == 'T');
        const int n = transient_drag
            ? static_cast<int>(app.transientmarkers.markers().size())
            : static_cast<int>(app.warpmarkers.markers().size());
        if (hit_idx >= 0 && hit_idx < n) {
            int64_t ph;
            if (transient_drag) {
                ph = app.transientmarkers.markers()[hit_idx].effective_frame();
            } else {
                ph = static_cast<int64_t>(std::llround(
                    app.warpmarkers.markers()[hit_idx].time_seconds * sr_d));
            }
            if (ph != app.playhead_sample) {
                const double old_px = playhead_pixel_x(app, audio);
                app.playhead_sample = ph;
                if (playback.is_playing()) playback.resync_predictor();
                const double new_px = playhead_pixel_x(app, audio);
                invalidate_playhead_columns(old_px, new_px);
                invalidate_timestamp_area();
            }
        }
    });

    // -- File loading --------------------------------------------------------

    // Loads `path` into a fresh GuiAudio, preflights via libsndfile, and
    // swaps the new audio in on success. On failure, the prior audio (if
    // any) remains loaded unchanged. Resets per-file navigation state;
    // follow_mode is reapplied from the loaded file's .settings (default
    // true when absent).
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
        app.hover_popup    = HoverPopupState{};

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
        app.audio_generation++;
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

        // Companion files: discover paths, create <basename>.warpmarkers
        // and <basename>.settings if missing. Companion file convention is
        // <source_dir>/<source_basename>.<ext> (sibling, basename-prefixed),
        // not the legacy hidden `./.warpmarkers` form.
        std::filesystem::path apath(path);
        std::filesystem::path parent = apath.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        const std::string stem = apath.stem().string();
        const std::string ext = apath.extension().string();
        const std::string ext_no_dot = ext.empty() ? "" : ext.substr(1);
        const std::filesystem::path wm_path  = parent / (stem + ".warpmarkers");
        const std::filesystem::path tm_path  = parent / (stem + ".transientmarkers");
        const std::filesystem::path set_path = parent / (stem + ".settings");
        app.warpmarkers_path      = wm_path.string();
        app.transientmarkers_path = tm_path.string();
        app.settings_path         = set_path.string();
        app.source_audio_path     = path;

        create_if_missing(wm_path, "00:00.000|1.00\n");
        create_if_missing(set_path,
                          format_default_settings_template(stem, ext_no_dot));

        // Load the markers file. Parse failures are non-fatal: we log each
        // error to stderr and leave app.warpmarkers empty. The GUI still works
        // as a waveform viewer.
        app.warpmarkers.clear();
        app.transientmarkers.clear();
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        app.active_mode    = 'W';
        app.drag = DragState{};
        app.playhead_drag = PlayheadDragState{};
        // Fresh file = fresh history. Both stacks cleared; the loaded state
        // is the saved baseline (signed_distance = 0, valid).
        app.history.reset();
        app.dirty              = false;
        app.warp_dirty         = false;
        app.transient_dirty    = false;
        app.first_save_pending = true;
        const bool markers_ok = app.warpmarkers.load(wm_path.string());
        if (!markers_ok) {
            for (const auto& err : app.warpmarkers.errors()) {
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
                         app.warpmarkers.markers().size(),
                         wm_path.string().c_str());
        }

        // Load .transientmarkers if present. Missing file is fine — the
        // transient list is just empty. Parse errors are logged to stderr;
        // the warp side stays usable regardless.
        if (std::filesystem::exists(tm_path)) {
            const bool tr_ok = app.transientmarkers.load(tm_path.string());
            if (!tr_ok) {
                for (const auto& err : app.transientmarkers.errors()) {
                    if (err.line_number > 0) {
                        std::fprintf(stderr,
                                     "warptempo_gui: %s:%d: %s\n",
                                     tm_path.string().c_str(),
                                     err.line_number, err.message.c_str());
                    } else {
                        std::fprintf(stderr,
                                     "warptempo_gui: %s: %s\n",
                                     tm_path.string().c_str(),
                                     err.message.c_str());
                    }
                }
            } else {
                std::fprintf(stderr,
                             "[warptempo_gui] parsed %zu transients from %s\n",
                             app.transientmarkers.markers().size(),
                             tm_path.string().c_str());
            }
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

        // Seed both tabs with the freshly-computed default post-load state.
        // Parsed .settings values overwrite per-key below.
        ViewState default_tab;
        default_tab.viewport_start_sample = app.viewport_start_sample;
        default_tab.zoom_level            = app.zoom_level;
        default_tab.playhead_sample       = app.playhead_sample;
        app.tab_a          = default_tab;
        app.tab_b          = default_tab;
        app.active_tab     = 'A';
        app.settings_passthrough.clear();

        // Parse .settings (if present) and apply tab values with silent
        // coerce on out-of-range. Missing file → all keys default.
        {
            ParsedSettings ps;
            if (!parse_settings_file(app.settings_path, ps)) {
                std::fprintf(stderr,
                    "warptempo_gui: could not read '%s'\n",
                    app.settings_path.c_str());
            }
            const int64_t total = audio.total_frames();
            auto valid_zoom = [](int z) -> bool {
                if (z == kFitFileLevel) return true;
                return z >= 0 && z < kNumZoomLevels;
            };
            auto apply = [&](bool has_vp, int64_t vp,
                             bool has_zoom, int zoom,
                             bool has_ph, int64_t ph,
                             ViewState& dst) {
                if (has_vp   && vp   >= 0 && vp   <  total)  dst.viewport_start_sample = vp;
                if (has_zoom && valid_zoom(zoom))            dst.zoom_level            = zoom;
                if (has_ph   && ph   >= 0 && ph   <= total)  dst.playhead_sample       = ph;
            };
            apply(ps.has_tab_a_vp, ps.tab_a_vp,
                  ps.has_tab_a_zoom, ps.tab_a_zoom,
                  ps.has_tab_a_ph, ps.tab_a_ph, app.tab_a);
            apply(ps.has_tab_b_vp, ps.tab_b_vp,
                  ps.has_tab_b_zoom, ps.tab_b_zoom,
                  ps.has_tab_b_ph, ps.tab_b_ph, app.tab_b);
            app.follow_mode = ps.has_follow ? ps.follow : true;
            app.settings_passthrough = std::move(ps.passthrough);
        }

        // Activate tab A: copy its snapshot into the live AppState fields.
        app.viewport_start_sample = app.tab_a.viewport_start_sample;
        app.zoom_level            = app.tab_a.zoom_level;
        app.playhead_sample       = app.tab_a.playhead_sample;
        clamp_viewport_start(app, audio);

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

    // Tick: runs once per event-loop iteration. During playback, snapshots
    // the audio thread's cursor and mirrors it into the main-thread playhead,
    // invalidating just the columns and timestamp that changed. Also
    // detects natural end-of-playback via the atomic playing flag.
    gui.set_on_tick([&]() {
        // Blink the editor cursor independently of playback. Compare the
        // current visibility against the last painted state and invalidate
        // the top strip when it flips. Cheap: top_strip is small.
        if (text_editor::is_active(app.top_flag_editor)) {
            const bool now_visible =
                text_editor::cursor_visible_now(app.top_flag_editor);
            if (now_visible != app.top_flag_editor_blink_last) {
                app.top_flag_editor_blink_last = now_visible;
                invalidate_top_strip();
            }
        }

        // V.A3b: dwell-driven popup show. The motion handler already gates
        // on warp mode + no editor + no drag + no dialog and clears
        // hover_popup when those conditions break, so here it's enough to
        // check the elapsed time and re-validate eligibility.
        if (!app.hover_popup.visible &&
            app.hover_popup.marker_index >= 0 &&
            popup_eligible_marker(app.hover_popup.marker_index)) {
            const auto now = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - app.hover_popup.entry_time).count();
            if (ms >= kHoverDelayMs) {
                app.hover_popup.visible = true;
                invalidate_top_strip();
            }
        }

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

        // Playing was true last tick, now false — natural end. Return the
        // visible playhead to the launch position (same as Space-to-stop).
        if (app.is_playing) {
            restore_playhead_to_lsp();
            if (app.follow_mode) follow_scroll_if_needed();
        }
    });

    // Idle timeout: wake the poll loop every ~16 ms during playback so the
    // tick can advance the playhead even in the absence of input events.
    gui.set_idle_timeout_provider([&]() -> int {
        if (app.is_playing || playback.is_playing()) return gui.playback_tick_ms();
        // While the top-flag editor is active, wake periodically so the
        // cursor blink can flip. ~125ms gives ample resolution on a
        // 500ms half-period without burning CPU.
        if (text_editor::is_active(app.top_flag_editor)) return 125;
        // V.A3b: while a hover is pending (dwell timer running but popup
        // not yet visible), wake periodically so the tick-driven check
        // can flip visibility on time. Once visible, no extra wake is
        // needed — input events drive any state change.
        if (!app.hover_popup.visible && app.hover_popup.marker_index >= 0) {
            return 125;
        }
        return -1;
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
