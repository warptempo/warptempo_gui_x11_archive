#include "render.h"
#include "app_state.h"
#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace perf_counters {
    int wf_cols              = 0;
    int wf_pyramid_samples   = 0;
    int flag_measure         = 0;
    int flag_drawn           = 0;
    int flag_elided          = 0;
}

// kVPadExtraPx and kFlagBottomLiftPx now live in render.h so the
// iteration popup in main.cpp can reference the same values.

// Brief J: lift the flag rect's bottom edge above the strip-boundary row by
// kFlagBottomLiftPx pixels. The boundary row coincides with the waveform
// area's first row and paints unreliably outside narrow playhead-column
// invalidates; lifting it places the bottom edge solidly inside the top-
// strip clip in every code path. The lift figure also matches the
// playhead's downward triangle height, leaving room for the stem connector
// between flag bottom and marker stem. If the flag lift changes,
// kMarkerConnectorRows should track it.
constexpr double kMarkerConnectorRows  = 11.0;

namespace {

// Flag text mirrors the canonical line's PAYLOAD (post-pipe). All
// metadata (b=/e=/#) is invisible in the rect; the `|` separator sits to
// the left of the rect, anchoring it to the marker column. Disabled state
// is conveyed by color (dimmed via `effective_disabled`), not by glyphs.
//
// Variants:
//   label_ref              → "a.42"
//   inherit, no def        → "pass"
//   inherit, with def      → "pass:a.42"
//   owning, no scale       → "1.23"
//   owning, with scale     → "1.23*1.2345"
//   def, no scale          → "1.23:a.03"
//   def, with scale        → "1.23*1.2345:a.03"
std::string flag_text(const std::vector<GuiWarpMarker>& markers, int idx) {
    const auto& m = markers[idx];

    if (!m.label_ref.empty()) {
        return m.label_ref;
    }

    std::string text;
    if (m.tempo_inherits) {
        text = "pass";
    } else {
        char tbuf[32];
        std::snprintf(tbuf, sizeof(tbuf), "%.2f", m.tempo_base);
        text = tbuf;
        if (!m.tempo_scale.empty()) {
            text += "*";
            text += m.tempo_scale;
        }
    }
    if (!m.label_def.empty()) {
        text += ":";
        text += m.label_def;
    }
    return text;
}

} // namespace

std::string flag_text_for_marker(const std::vector<GuiWarpMarker>& markers, int idx) {
    if (idx < 0 || idx >= static_cast<int>(markers.size())) return {};
    return flag_text(markers, idx);
}

double resolve_inherited_tempo(const std::vector<GuiWarpMarker>& markers, int index) {
    for (int i = index - 1; i >= 0; --i) {
        const auto& m = markers[i];
        if (!m.tempo_inherits && m.label_ref.empty()) {
            return m.tempo_base;
        }
    }
    return 1.0;
}

std::string resolve_inherited_tempo_scale(
    const std::vector<GuiWarpMarker>& markers, int index) {
    for (int i = index - 1; i >= 0; --i) {
        const auto& m = markers[i];
        if (!m.tempo_inherits && m.label_ref.empty()) {
            return m.tempo_scale;
        }
    }
    return {};
}

// X.7.8b-3: promoted from an inline function at the original
// main.cpp:112-200 anonymous namespace. Body is verbatim — no
// captures, no identifier rewrites needed.
std::string compute_hover_popup_text(
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

void render_background(cairo_t* cr, int x, int y, int w, int h) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, kBackground.r, kBackground.g, kBackground.b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_restore(cr);
}

void render_progress_bar(cairo_t* cr, int x, int y, int w, int h,
                         float progress_fraction) {
    if (progress_fraction <= 0.0f || w <= 0 || h <= 0) return;
    if (progress_fraction > 1.0f) progress_fraction = 1.0f;
    const int filled = static_cast<int>(progress_fraction * w + 0.5f);
    if (filled <= 0) return;
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.40);
    cairo_rectangle(cr, x, y, filled, h);
    cairo_fill(cr);
    cairo_restore(cr);
}

void render_waveform(cairo_t* cr,
                     GuiRect area,
                     const GuiAudio& audio,
                     int channel,
                     long long viewport_start_sample,
                     long long viewport_end_sample,
                     long long trim_begin_sample,
                     long long trim_end_sample,
                     GuiColor bright_color,
                     GuiColor dim_color) {
    if (area.w <= 0 || area.h <= 2) return;
    if (viewport_end_sample <= viewport_start_sample) return;

    const int num_levels = audio.num_levels();
    if (num_levels <= 0) return;

    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(area.w);

    // Cache layout (must match audio.cpp): level 0 = raw samples;
    // levels 1, 2, 3 = stride 32, 1024, 32768. Pick the coarsest cache
    // level whose stride is <= spp; below stride 32, fall through to raw.
    int level;
    if      (samples_per_pixel >= 32768.0) level = 3;
    else if (samples_per_pixel >= 1024.0)  level = 2;
    else if (samples_per_pixel >= 32.0)    level = 1;
    else                                   level = 0;
    if (level > num_levels - 1) level = num_levels - 1;

    const double y_center = area.y + area.h * 0.5;
    const double half_h   = area.h * 0.5;

    struct ColLine { double x, y0, y1; bool bright; };
    std::vector<ColLine> lines;
    lines.reserve(static_cast<size_t>(area.w));

    for (int i = 0; i < area.w; i++) {
        const double f0 = static_cast<double>(viewport_start_sample) +
                          (span * i)     / area.w;
        const double f1 = static_cast<double>(viewport_start_sample) +
                          (span * (i+1)) / area.w;
        const long long s0 = static_cast<long long>(std::llround(f0));
        long long       s1 = static_cast<long long>(std::llround(f1));
        if (s1 <= s0) s1 = s0 + 1;

        const auto mm = audio.get_peak_range(channel, level, s0, s1);
        const double min_val = mm.first;
        const double max_val = mm.second;

        if constexpr (kDebugPerf) {
            perf_counters::wf_cols++;
            if (level <= 0) {
                perf_counters::wf_pyramid_samples +=
                    static_cast<int>(s1 - s0);
            } else {
                // Strides match audio.cpp's kStrides[].
                constexpr long long kCacheStrides[] = { 0, 32, 1024, 32768 };
                const long long stride = kCacheStrides[level];
                const long long i0 = s0 / stride;
                const long long i1 = (s1 + stride - 1) / stride;
                perf_counters::wf_pyramid_samples +=
                    static_cast<int>(i1 - i0);
            }
        }

        const double y_top    = y_center - max_val * half_h;
        const double y_bottom = y_center - min_val * half_h;
        const double x_px     = area.x + i + 0.5;

        // Per spec: a column that straddles a trim boundary is assigned by
        // the midpoint of its source-sample range.
        const long long mid = s0 + (s1 - s0) / 2;
        const bool bright = (mid >= trim_begin_sample && mid < trim_end_sample);

        lines.push_back({x_px, y_top, y_bottom, bright});
    }

    cairo_save(cr);
    cairo_set_line_width(cr, 1.0);

    cairo_set_source_rgb(cr, bright_color.r, bright_color.g, bright_color.b);
    for (const auto& L : lines) {
        if (!L.bright) continue;
        cairo_move_to(cr, L.x, L.y0);
        cairo_line_to(cr, L.x, L.y1);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, dim_color.r, dim_color.g, dim_color.b);
    for (const auto& L : lines) {
        if (L.bright) continue;
        cairo_move_to(cr, L.x, L.y0);
        cairo_line_to(cr, L.x, L.y1);
    }
    cairo_stroke(cr);

    cairo_restore(cr);
}

void render_playhead(cairo_t* cr,
                     GuiRect area,
                     double  playhead_pixel_x,
                     GuiColor color,
                     cairo_surface_t* triangle_surface) {
    if (area.w <= 0 || area.h <= 0) return;
    if (playhead_pixel_x < 0.0) return;
    if (playhead_pixel_x > static_cast<double>(area.w - 1)) return;

    const double col  = std::floor(playhead_pixel_x + 0.5);
    const double x_px = area.x + col + 0.5;

    cairo_save(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x_px, area.y);
    cairo_line_to(cr, x_px, area.y + area.h);
    cairo_stroke(cr);

    // Inverted-triangle indicator: stamped from a hand-authored PNG mask so
    // every pixel is explicit (no rasterizer ambiguity). Asset is 17×9 with
    // the tip at column index 8 (image-local); integer division places that
    // tip column at `area.x + col`. The bottom row sits one pixel above
    // `area.y` so the stem stroke beginning at `area.y` is visually adjacent.
    if (triangle_surface) {
        const int img_w = cairo_image_surface_get_width(triangle_surface);
        const int img_h = cairo_image_surface_get_height(triangle_surface);
        const double dst_x = static_cast<double>(area.x + col - img_w / 2);
        const double dst_y = static_cast<double>(area.y - img_h);
        cairo_set_source_rgb(cr, color.r, color.g, color.b);
        cairo_mask_surface(cr, triangle_surface, dst_x, dst_y);
    }
    cairo_restore(cr);
}

void render_timestamp(cairo_t* cr,
                      int      x,
                      int      y,
                      double   seconds,
                      GuiColor color) {
    if (seconds < 0.0) seconds = 0.0;
    // Files longer than ~99 minutes wrap the two-digit minute field to 3
    // digits. An absurdly long input is clamped here so the formatter stays
    // within its fixed buffer.
    if (seconds > 5999.999) seconds = 5999.999;

    int total_ms = static_cast<int>(seconds * 1000.0 + 0.5);
    const int minutes = total_ms / 60000;
    total_ms         -= minutes * 60000;
    const int secs    = total_ms / 1000;
    const int ms      = total_ms - secs * 1000;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", minutes, secs, ms);

    cairo_save(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, kFlagFontSize);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, buf);
    cairo_restore(cr);
}

void render_markers(cairo_t* cr,
                    GuiRect waveform_area,
                    const std::vector<GuiWarpMarker>& markers,
                    long long viewport_start_sample,
                    long long viewport_end_sample,
                    int sample_rate,
                    const TrimRange& trim) {
    if (waveform_area.w <= 0 || waveform_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(waveform_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double sr = static_cast<double>(sample_rate);
    const double y_conn_top =
        static_cast<double>(waveform_area.y) - kMarkerConnectorRows;
    const double y1 = static_cast<double>(waveform_area.y + waveform_area.h);

    cairo_save(cr);
    cairo_set_line_width(cr, 1.0);

    // Two passes — one path per color — split by in-trim vs out-of-trim.
    // Disabled markers are skipped entirely (no stem); selection has no
    // effect on stems under the brief H palette rules.
    for (int pass = 0; pass < 2; pass++) {
        const bool out_of_trim_pass = (pass == 1);
        const GuiColor c = out_of_trim_pass ? dim(kMarker) : kMarker;
        cairo_set_source_rgb(cr, c.r, c.g, c.b);
        for (size_t i = 0; i < markers.size(); ++i) {
            if (effective_disabled(markers, static_cast<int>(i))) continue;
            const auto& m = markers[i];
            const double ms = m.time_seconds * sr;
            if (ms < static_cast<double>(viewport_start_sample)) continue;
            if (ms >= static_cast<double>(viewport_end_sample)) continue;
            const int64_t pos = static_cast<int64_t>(
                std::llround(m.time_seconds * sr));
            if (marker_out_of_trim(pos, trim) != out_of_trim_pass) continue;
            const double x_raw =
                (ms - static_cast<double>(viewport_start_sample))
                    / samples_per_pixel;
            const double x_px = waveform_area.x + std::round(x_raw) + 0.5;
            cairo_move_to(cr, x_px, y_conn_top);
            cairo_line_to(cr, x_px, y1);
        }
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

namespace {

// Shared greedy-pack iteration used by both render_flags and
// compute_flag_hit_rects. Invokes `emit(i, text_left, baseline_y, ext)` for
// each flag that survives elision, in left-to-right order. The cairo font
// face/size are assumed to already be set on `cr` by the caller.
// `text_left` is snapped to the marker's integer pixel column so the flag's
// left edge coincides with the marker/playhead column.
template <typename Emit>
void iterate_visible_flags(cairo_t* cr,
                           GuiRect top_strip_area,
                           const std::vector<GuiWarpMarker>& markers,
                           long long viewport_start_sample,
                           long long viewport_end_sample,
                           int sample_rate,
                           Emit&& emit) {
    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(top_strip_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double sr           = static_cast<double>(sample_rate);
    // Place the rect's bottom edge exactly at the strip bottom (which is
    // the waveform area top, since the strips are contiguous). Solving the
    // rect formula (rect_bottom = baseline_y + y_bearing + height + hl_pad
    // + kVPadExtraPx) for baseline_y, using a representative monospace
    // measurement so the result tracks font metrics rather than a magic
    // constant.
    cairo_text_extents_t base_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &base_ext);
    const double hl_pad_helper = kFlagInnerPadPx;
    const double baseline_y =
        static_cast<double>(top_strip_area.y + top_strip_area.h)
      - kFlagBottomLiftPx
      - base_ext.y_bearing
      - base_ext.height
      - hl_pad_helper
      - kVPadExtraPx;
    const double pad          = 4.0;

    double rightmost_right_edge = -1e18;

    for (size_t i = 0; i < markers.size(); ++i) {
        const auto& m = markers[i];
        const double ms = m.time_seconds * sr;
        if (ms < static_cast<double>(viewport_start_sample)) continue;
        if (ms >= static_cast<double>(viewport_end_sample)) continue;

        const double x_raw =
            (ms - static_cast<double>(viewport_start_sample)) /
            samples_per_pixel;
        const double text_left =
            static_cast<double>(top_strip_area.x) + std::round(x_raw);
        if (text_left < rightmost_right_edge + pad) {
            if constexpr (kDebugPerf) perf_counters::flag_elided++;
            continue;
        }

        const std::string text = flag_text(markers, static_cast<int>(i));
        if (text.empty()) continue;

        cairo_text_extents_t ext;
        cairo_text_extents(cr, text.c_str(), &ext);
        if constexpr (kDebugPerf) perf_counters::flag_measure++;

        emit(static_cast<int>(i), text_left, baseline_y, text, ext);
        rightmost_right_edge = text_left + ext.width;
    }
}

} // namespace

void render_flags(cairo_t* cr,
                  GuiRect top_strip_area,
                  const std::vector<GuiWarpMarker>& markers,
                  long long viewport_start_sample,
                  long long viewport_end_sample,
                  int sample_rate,
                  double font_size,
                  const std::set<int>& selected_set,
                  const TrimRange& trim,
                  double playhead_pixel_x,
                  const FlagEditorOverlay& editor) {
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    const double hl_pad = kFlagInnerPadPx;
    const double sr_d   = static_cast<double>(sample_rate);

    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &uniform_ext);

    // Brief Y.5: collect emit args during the left-to-right iterate pass,
    // then paint the collected list in REVERSE order. The pack rule inside
    // iterate_visible_flags still elides right-of-collision flags (leftmost
    // wins), and reverse paint order makes the leftmost flag's pixels land
    // on top — so when an editor's pending text grows past its original
    // flag width into the right neighbor's territory, the editor's bg-fill
    // and text occlude the right neighbor instead of being overwritten by
    // it. In all static (no-edit) states the bg-fills are kBackground and
    // text rects don't overlap, so reverse paint order produces pixels
    // identical to forward order.
    struct FlagEmit {
        int                  i;
        double               text_left;
        double               baseline_y;
        std::string          text;
        cairo_text_extents_t ext;
    };
    std::vector<FlagEmit> emits;
    iterate_visible_flags(cr, top_strip_area, markers,
                          viewport_start_sample, viewport_end_sample,
                          sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& text, const cairo_text_extents_t& ext) {
            emits.push_back({i, text_left, baseline_y, text, ext});
        });

    auto paint_one = [&](const FlagEmit& e) {
        // V.B Addendum 2 / Brief X.2: when the above-strip popup (iter
        // or BPM) anchored on this flag owns the editor, suppress the
        // flag's selection fill so the focused element (the popup) is
        // the only one filled.
        const bool is_popup_focus = (e.i == editor.popup_editor_target);
        const bool is_selected    =
            !is_popup_focus && selected_set.count(e.i) > 0;
        const bool is_editing    = (e.i == editor.marker_index);
        const bool is_parse_fail = is_editing && editor.is_red;

        const int64_t source_pos = static_cast<int64_t>(
            std::llround(markers[e.i].time_seconds * sr_d));
        const bool out_of_trim = marker_out_of_trim(source_pos, trim);

        const int marker_col_px = static_cast<int>(std::round(e.text_left));
        const int playhead_col_px = static_cast<int>(std::round(playhead_pixel_x));
        const bool is_playhead_on =
            playhead_pixel_x >= 0.0 && marker_col_px == playhead_col_px;

        std::string draw_text = is_editing ? editor.pending : e.text;
        cairo_text_extents_t draw_ext = e.ext;
        if (is_editing) {
            cairo_text_extents(cr, draw_text.c_str(), &draw_ext);
        }

        // Brief Y.4 sub-bug A: opaque canvas-bg fill under the text,
        // sized to the painted extent + kFlagInnerPadPx on each side.
        // Drawn unconditionally — in non-edit states the fill matches
        // the strip-clear color, so pixels stay identical; during an
        // edit it occludes neighbor text once pending text widens
        // past the original flag width. Y/height reuse the outline
        // rect math below so the fill sits exactly under the outline.
        const double bg_top =
            e.baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx;
        const double bg_h_full =
            uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx;
        render_flag_text_bg_fill(cr,
            e.text_left + hl_pad, draw_ext.x_advance, bg_top, bg_h_full);

        if (is_selected || is_playhead_on) {
            GuiColor stroke_col = is_parse_fail ? kAccent : kMarker;
            if (out_of_trim) stroke_col = dim(stroke_col);
            cairo_set_source_rgb(cr,
                stroke_col.r, stroke_col.g, stroke_col.b);
            const double rx = std::round(e.text_left) + 0.5;
            const double ry = std::round(
                e.baseline_y + uniform_ext.y_bearing
              - hl_pad - kVPadExtraPx) + 0.5;
            const int rw = static_cast<int>(std::round(
                hl_pad + draw_ext.x_bearing + draw_ext.width + hl_pad));
            const int rh = static_cast<int>(std::round(
                uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx));
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, rx, ry,
                            static_cast<double>(rw),
                            static_cast<double>(rh));
            cairo_stroke(cr);
        }

        const GuiColor txt = out_of_trim ? dim(kText) : kText;
        cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
        cairo_move_to(cr, e.text_left + hl_pad, e.baseline_y);
        cairo_show_text(cr, draw_text.c_str());

        // Brief seven: foreground/background swap over the selected
        // substring. Drawn after the regular text paint and before the
        // cursor so the cursor stays visible inside the highlight rect
        // (standard inverted-cursor look). The fill color is the
        // current text color (`txt`); the re-paint color is the canvas
        // background, dimmed in lockstep when out-of-trim.
        if (is_editing && editor.has_selection) {
            const int sel_a = editor.selection_start;
            const int sel_b = editor.selection_end;
            cairo_text_extents_t a_ext;
            cairo_text_extents(cr,
                draw_text.substr(0,
                    static_cast<size_t>(sel_a)).c_str(),
                &a_ext);
            cairo_text_extents_t b_ext;
            cairo_text_extents(cr,
                draw_text.substr(0,
                    static_cast<size_t>(sel_b)).c_str(),
                &b_ext);
            const double hi_x = e.text_left + hl_pad + a_ext.x_advance;
            const double hi_w = b_ext.x_advance - a_ext.x_advance;
            const double hi_y = e.baseline_y + uniform_ext.y_bearing
                              - hl_pad - kVPadExtraPx;
            const double hi_h = uniform_ext.height + 2 * hl_pad
                              + 2 * kVPadExtraPx;
            cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
            cairo_rectangle(cr, hi_x, hi_y, hi_w, hi_h);
            cairo_fill(cr);
            const GuiColor bg_swap =
                out_of_trim ? dim(kBackground) : kBackground;
            cairo_set_source_rgb(cr,
                bg_swap.r, bg_swap.g, bg_swap.b);
            cairo_move_to(cr, hi_x, e.baseline_y);
            cairo_show_text(cr,
                draw_text.substr(static_cast<size_t>(sel_a),
                                 static_cast<size_t>(sel_b - sel_a))
                    .c_str());
        }

        if (is_editing && editor.cursor_visible) {
            double cursor_x_offset = 0.0;
            if (editor.cursor_pos > 0) {
                cairo_text_extents_t pext;
                const std::string before =
                    draw_text.substr(0,
                        static_cast<size_t>(editor.cursor_pos));
                cairo_text_extents(cr, before.c_str(), &pext);
                cursor_x_offset = pext.x_advance;
            }
            const double cur_x =
                e.text_left + hl_pad + cursor_x_offset;
            cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, std::round(cur_x) + 0.5,
                          e.baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx);
            cairo_line_to(cr, std::round(cur_x) + 0.5,
                          e.baseline_y + uniform_ext.y_bearing
                              + uniform_ext.height + hl_pad + kVPadExtraPx);
            cairo_stroke(cr);
        }

        if constexpr (kDebugPerf) perf_counters::flag_drawn++;
    };

    for (auto it = emits.rbegin(); it != emits.rend(); ++it) {
        paint_one(*it);
    }

    cairo_restore(cr);
}

std::vector<FlagHitRect> compute_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<FlagHitRect> out;
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return out;
    if (viewport_end_sample <= viewport_start_sample) return out;
    if (sample_rate <= 0) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    // Mirror render_flags: uniform y/height for the hit rect so clicks
    // register consistently across flag types. Left edge + width match the
    // corrected visual highlight box so clicks anywhere inside the painted
    // background register on the marker.
    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    iterate_visible_flags(cr, top_strip_area, markers,
                          viewport_start_sample, viewport_end_sample,
                          sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& /*text*/, const cairo_text_extents_t& ext) {
            FlagHitRect r;
            r.marker_index = i;
            r.x = text_left;
            r.y = baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx;
            r.w = hl_pad + ext.x_bearing + ext.width + hl_pad;
            r.h = uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx;
            out.push_back(r);
        });

    cairo_restore(cr);
    return out;
}

// ---------- Transient-marker rendering (chunk S.2.2) ----------

namespace {

std::string transient_flag_text(const GuiTransientMarker& m) {
    (void)m;
    return "t";
}

template <typename Emit>
void iterate_visible_transient_flags(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiTransientMarker>& transients,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    Emit&& emit) {
    (void)cr;
    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(top_strip_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double sr           = static_cast<double>(sample_rate);
    // Mirror iterate_visible_flags: place the rect bottom at the strip
    // bottom by deriving baseline_y from a representative monospace
    // measurement.
    cairo_text_extents_t base_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &base_ext);
    const double hl_pad_helper = kFlagInnerPadPx;
    const double baseline_y =
        static_cast<double>(top_strip_area.y + top_strip_area.h)
      - kFlagBottomLiftPx
      - base_ext.y_bearing
      - base_ext.height
      - hl_pad_helper
      - kVPadExtraPx;
    const double pad          = 4.0;

    double rightmost_right_edge = -1e18;

    for (size_t i = 0; i < transients.size(); ++i) {
        const auto& m = transients[i];
        const double ms = m.time_seconds * sr;
        if (ms < static_cast<double>(viewport_start_sample)) continue;
        if (ms >= static_cast<double>(viewport_end_sample)) continue;

        const double x_raw =
            (ms - static_cast<double>(viewport_start_sample)) /
            samples_per_pixel;
        const double text_left =
            static_cast<double>(top_strip_area.x) + std::round(x_raw);
        if (text_left < rightmost_right_edge + pad) {
            if constexpr (kDebugPerf) perf_counters::flag_elided++;
            continue;
        }

        const std::string text = transient_flag_text(m);
        if (text.empty()) continue;

        cairo_text_extents_t ext;
        cairo_text_extents(cr, text.c_str(), &ext);
        if constexpr (kDebugPerf) perf_counters::flag_measure++;

        emit(static_cast<int>(i), text_left, baseline_y, text, ext);
        rightmost_right_edge = text_left + ext.width;
    }
}

} // namespace

void render_transient_markers(cairo_t* cr,
                              GuiRect waveform_area,
                              const std::vector<GuiTransientMarker>& transients,
                              long long viewport_start_sample,
                              long long viewport_end_sample,
                              int sample_rate,
                              const TrimRange& trim) {
    if (waveform_area.w <= 0 || waveform_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(waveform_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double sr = static_cast<double>(sample_rate);

    const double y_conn_top =
        static_cast<double>(waveform_area.y) - kMarkerConnectorRows;
    const double y1 = static_cast<double>(waveform_area.y + waveform_area.h);

    cairo_save(cr);
    cairo_set_line_width(cr, 1.0);

    // Two passes — one path per color — split by in-trim vs out-of-trim.
    // Disabled transients are skipped entirely (no stem); selection has no
    // effect on stems under the brief H palette rules.
    for (int pass = 0; pass < 2; pass++) {
        const bool out_of_trim_pass = (pass == 1);
        const GuiColor c = out_of_trim_pass ? dim(kMarker) : kMarker;
        cairo_set_source_rgb(cr, c.r, c.g, c.b);
        for (size_t i = 0; i < transients.size(); ++i) {
            const auto& m = transients[i];
            if (m.disabled) continue;
            const double ms = m.time_seconds * sr;
            const int64_t pos = static_cast<int64_t>(std::nearbyint(ms));
            if (ms < static_cast<double>(viewport_start_sample)) continue;
            if (ms >= static_cast<double>(viewport_end_sample)) continue;
            if (marker_out_of_trim(pos, trim) != out_of_trim_pass) continue;
            const double x_raw =
                (ms - static_cast<double>(viewport_start_sample))
                    / samples_per_pixel;
            const double x_px = waveform_area.x + std::round(x_raw) + 0.5;
            cairo_move_to(cr, x_px, y_conn_top);
            cairo_line_to(cr, x_px, y1);
        }
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

void render_transient_flags(cairo_t* cr,
                            GuiRect top_strip_area,
                            const std::vector<GuiTransientMarker>& transients,
                            long long viewport_start_sample,
                            long long viewport_end_sample,
                            int sample_rate,
                            double font_size,
                            const std::set<int>& selected_set,
                            const TrimRange& trim,
                            double playhead_pixel_x) {
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    const double hl_pad = kFlagInnerPadPx;

    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &uniform_ext);

    // Brief Y.5: collect-then-reverse-paint, mirroring render_flags.
    // Transient flags have no editor and thus no widening-text case, so
    // visually this is a no-op today (all bg-fills are kBackground; all
    // text rects are non-overlapping). Structurally it keeps every flag /
    // popup paint loop on the same leftmost-wins discipline so future
    // edits don't have to reason about which loop is which.
    struct TransientEmit {
        int                  i;
        double               text_left;
        double               baseline_y;
        std::string          text;
        cairo_text_extents_t ext;
    };
    std::vector<TransientEmit> emits;
    iterate_visible_transient_flags(cr, top_strip_area, transients,
                                    viewport_start_sample, viewport_end_sample,
                                    sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& text, const cairo_text_extents_t& ext) {
            emits.push_back({i, text_left, baseline_y, text, ext});
        });

    const double sr = static_cast<double>(sample_rate);
    auto paint_one = [&](const TransientEmit& e) {
        const bool is_selected = selected_set.count(e.i) > 0;
        const bool out_of_trim =
            marker_out_of_trim(static_cast<int64_t>(std::nearbyint(
                transients[e.i].time_seconds * sr)), trim);

        const int marker_col_px = static_cast<int>(std::round(e.text_left));
        const int playhead_col_px = static_cast<int>(std::round(playhead_pixel_x));
        const bool is_playhead_on =
            playhead_pixel_x >= 0.0 && marker_col_px == playhead_col_px;

        // Brief Y.4 sub-bug A: opaque canvas-bg fill under the text.
        // Transient flags have no editor (no growing pending text),
        // but the fill is added for symmetry with warp flags — in the
        // static states it sits under identical-color canvas pixels
        // and is visually invisible.
        const double bg_top =
            e.baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx;
        const double bg_h_full =
            uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx;
        render_flag_text_bg_fill(cr,
            e.text_left + hl_pad, e.ext.x_advance, bg_top, bg_h_full);

        if (is_selected || is_playhead_on) {
            const GuiColor stroke_col =
                out_of_trim ? dim(kMarker) : kMarker;
            cairo_set_source_rgb(cr,
                stroke_col.r, stroke_col.g, stroke_col.b);
            const double rx = std::round(e.text_left) + 0.5;
            const double ry = std::round(
                e.baseline_y + uniform_ext.y_bearing
              - hl_pad - kVPadExtraPx) + 0.5;
            const int rw = static_cast<int>(std::round(
                hl_pad + e.ext.x_bearing + e.ext.width + hl_pad));
            const int rh = static_cast<int>(std::round(
                uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx));
            cairo_set_line_width(cr, 1.0);
            cairo_rectangle(cr, rx, ry,
                            static_cast<double>(rw),
                            static_cast<double>(rh));
            cairo_stroke(cr);
        }

        const GuiColor txt = out_of_trim ? dim(kText) : kText;
        cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
        cairo_move_to(cr, e.text_left + hl_pad, e.baseline_y);
        cairo_show_text(cr, e.text.c_str());
        if constexpr (kDebugPerf) perf_counters::flag_drawn++;
    };

    for (auto it = emits.rbegin(); it != emits.rend(); ++it) {
        paint_one(*it);
    }

    cairo_restore(cr);
}

std::vector<FlagHitRect> compute_transient_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiTransientMarker>& transients,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<FlagHitRect> out;
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return out;
    if (viewport_end_sample <= viewport_start_sample) return out;
    if (sample_rate <= 0) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    iterate_visible_transient_flags(cr, top_strip_area, transients,
                                    viewport_start_sample, viewport_end_sample,
                                    sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& /*text*/, const cairo_text_extents_t& ext) {
            FlagHitRect r;
            r.marker_index = i;
            r.x = text_left;
            r.y = baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx;
            r.w = hl_pad + ext.x_bearing + ext.width + hl_pad;
            r.h = uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx;
            out.push_back(r);
        });

    cairo_restore(cr);
    return out;
}

double measure_timestamp_width(cairo_t* cr, double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    if (seconds > 5999.999) seconds = 5999.999;
    int total_ms = static_cast<int>(seconds * 1000.0 + 0.5);
    const int minutes = total_ms / 60000;
    total_ms         -= minutes * 60000;
    const int secs    = total_ms / 1000;
    const int ms      = total_ms - secs * 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", minutes, secs, ms);

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, kFlagFontSize);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_restore(cr);
    return ext.x_advance;
}

namespace {
    double g_advance = 0.0;
    bool   g_metrics_initialized = false;
} // namespace

double monospace_advance() { return g_advance; }

void init_monospace_grid_metrics(cairo_t* cr) {
    if (g_metrics_initialized) return;
    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, kFlagFontSize);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "M", &ext);
    g_advance = ext.x_advance;
    cairo_restore(cr);
    g_metrics_initialized = true;
}

double flag_pending_text_left_x(
    const AppState& app, const GuiAudio& audio,
    int marker_idx)
{
    const auto& mv = app.warpmarkers.markers();
    if (marker_idx < 0 ||
        marker_idx >= static_cast<int>(mv.size())) return -1.0;
    const GuiRect top = top_strip_area(app);
    if (top.w <= 0) return -1.0;
    const double spp = current_samples_per_pixel(app, audio);
    if (spp <= 0.0) return -1.0;
    const int64_t vp_start = app.viewport_start_sample;
    const int64_t vp_end = vp_start +
        static_cast<int64_t>(std::llround(spp * top.w));
    const double sr = static_cast<double>(audio.sample_rate());
    const double ms = mv[marker_idx].time_seconds * sr;
    if (ms <  static_cast<double>(vp_start)) return -1.0;
    if (ms >= static_cast<double>(vp_end))   return -1.0;
    const double samples_per_pixel =
        static_cast<double>(vp_end - vp_start) /
        static_cast<double>(top.w);
    const double x_raw =
        (ms - static_cast<double>(vp_start)) / samples_per_pixel;
    const double text_left =
        static_cast<double>(top.x) + std::nearbyint(x_raw);
    return text_left + kFlagInnerPadPx;
}
