#include "gui_render.h"
#include "gui_audio.h"

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

// V.B Addendum 2: extra vertical padding (top + bottom) added to the inner
// padding of flag rects, hit rects, and the iteration popup edit bg. The
// horizontal pad (`hl_pad`) is unchanged. Value applies symmetrically:
// rects grow by 2*kVPadExtraPx in height, with kVPadExtraPx on each side.
constexpr double kVPadExtraPx = 1.0;

namespace {

// True if the marker at `idx` should render as disabled. Per chunk U
// patch 3, `disabled` is allowed on any marker — a locally set flag
// always counts. For an active (non-locally-disabled) `label_ref`, the
// cascade rule applies: the ref inherits its target label_def's
// disabled state.
bool effective_disabled(const std::vector<GuiMarker>& markers, int idx) {
    if (idx < 0 || idx >= static_cast<int>(markers.size())) return false;
    const auto& m = markers[idx];
    if (m.disabled) return true;
    if (!m.label_ref.empty()) {
        // Walk all markers to find the definition. O(N^2) across the list
        // but N is small (hundreds max).
        for (const auto& other : markers) {
            if (!other.label_def.empty() &&
                other.label_def == m.label_ref) {
                return other.disabled;
            }
        }
    }
    return false;
}

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
std::string flag_text(const std::vector<GuiMarker>& markers, int idx) {
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

std::string flag_text_for_marker(const std::vector<GuiMarker>& markers, int idx) {
    if (idx < 0 || idx >= static_cast<int>(markers.size())) return {};
    return flag_text(markers, idx);
}

double resolve_inherited_tempo(const std::vector<GuiMarker>& markers, int index) {
    for (int i = index - 1; i >= 0; --i) {
        const auto& m = markers[i];
        if (!m.tempo_inherits && m.label_ref.empty()) {
            return m.tempo_base;
        }
    }
    return 1.0;
}

std::string resolve_inherited_tempo_scale(
    const std::vector<GuiMarker>& markers, int index) {
    for (int i = index - 1; i >= 0; --i) {
        const auto& m = markers[i];
        if (!m.tempo_inherits && m.label_ref.empty()) {
            return m.tempo_scale;
        }
    }
    return {};
}

void render_background(cairo_t* cr, int x, int y, int w, int h) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
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

    // Cache layout (must match gui_audio.cpp): level 0 = raw samples;
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
                // Strides match gui_audio.cpp's kStrides[].
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
                     GuiColor color) {
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

    // Inverted-triangle indicator: tip at the top of the waveform area on
    // the same integer-snapped column as the line, base 12 px above in the
    // flag-strip region, 12 px wide. Filled solid. Flags are rendered after
    // the playhead in the redraw path, so overlapping flags overpaint this
    // triangle (flags take z-order precedence).
    const double tip_x  = area.x + col;
    const double tip_y  = area.y;
    const double base_y = area.y - 12.0;
    const double half_w = 6.0;
    cairo_move_to(cr, tip_x - half_w, base_y);
    cairo_line_to(cr, tip_x + half_w, base_y);
    cairo_line_to(cr, tip_x,          tip_y);
    cairo_close_path(cr);
    cairo_fill(cr);
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
    cairo_set_font_size(cr, 14.0);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, buf);
    cairo_restore(cr);
}

void render_markers(cairo_t* cr,
                    GuiRect waveform_area,
                    const std::vector<GuiMarker>& markers,
                    long long viewport_start_sample,
                    long long viewport_end_sample,
                    int sample_rate,
                    GuiColor enabled_color,
                    GuiColor disabled_color,
                    GuiColor selected_color,
                    const std::set<int>& selected_set,
                    int /*last_selected*/,
                    const TrimRange& trim) {
    (void)enabled_color;
    (void)disabled_color;
    (void)selected_color;
    (void)selected_set;
    if (waveform_area.w <= 0 || waveform_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(waveform_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double sr = static_cast<double>(sample_rate);
    const double y0 = static_cast<double>(waveform_area.y);
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
            const int64_t pos = static_cast<int64_t>(m.time_seconds * sr);
            if (marker_out_of_trim(pos, trim) != out_of_trim_pass) continue;
            const double x_raw =
                (ms - static_cast<double>(viewport_start_sample))
                    / samples_per_pixel;
            const double x_px = waveform_area.x + std::round(x_raw) + 0.5;
            cairo_move_to(cr, x_px, y0);
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
                           const std::vector<GuiMarker>& markers,
                           long long viewport_start_sample,
                           long long viewport_end_sample,
                           int sample_rate,
                           Emit&& emit) {
    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(top_strip_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double sr           = static_cast<double>(sample_rate);
    const double baseline_y =
        static_cast<double>(top_strip_area.y + top_strip_area.h) - 13.0;
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
                  const std::vector<GuiMarker>& markers,
                  long long viewport_start_sample,
                  long long viewport_end_sample,
                  int sample_rate,
                  GuiColor enabled_color,
                  GuiColor disabled_color,
                  GuiColor selected_color,
                  GuiColor highlight_color,
                  double font_size,
                  const std::set<int>& selected_set,
                  int last_selected,
                  const TrimRange& trim,
                  const FlagEditorOverlay& editor) {
    (void)enabled_color;
    (void)disabled_color;
    (void)selected_color;
    (void)highlight_color;
    (void)last_selected;
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    const double hl_pad = 2.0;
    const double sr_d   = static_cast<double>(sample_rate);

    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &uniform_ext);

    iterate_visible_flags(cr, top_strip_area, markers,
                          viewport_start_sample, viewport_end_sample,
                          sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& text, const cairo_text_extents_t& ext) {
            // V.B Addendum 2: when the iter popup above this flag owns
            // the editor, suppress the flag's selection fill so the
            // focused element (the iter popup) is the only one filled.
            const bool is_iter_focus = (i == editor.iter_editor_target);
            const bool is_selected   =
                !is_iter_focus && selected_set.count(i) > 0;
            const bool is_editing    = (i == editor.marker_index);
            const bool is_parse_fail = is_editing && editor.is_red;

            const int64_t source_pos =
                static_cast<int64_t>(markers[i].time_seconds * sr_d);
            const bool out_of_trim = marker_out_of_trim(source_pos, trim);

            std::string draw_text = is_editing ? editor.pending : text;
            cairo_text_extents_t draw_ext = ext;
            if (is_editing) {
                cairo_text_extents(cr, draw_text.c_str(), &draw_ext);
            }

            if (is_selected) {
                GuiColor fill = is_parse_fail ? kAccent : kMarker;
                if (out_of_trim) fill = dim(fill);
                cairo_set_source_rgb(cr, fill.r, fill.g, fill.b);
                cairo_rectangle(cr,
                                text_left,
                                baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx,
                                hl_pad + draw_ext.x_bearing + draw_ext.width + hl_pad,
                                uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx);
                cairo_fill(cr);
            }

            const GuiColor txt = out_of_trim ? dim(kText) : kText;
            cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
            cairo_move_to(cr, text_left + hl_pad, baseline_y);
            cairo_show_text(cr, draw_text.c_str());

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
                    text_left + hl_pad + cursor_x_offset;
                cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
                cairo_set_line_width(cr, 1.0);
                cairo_move_to(cr, std::round(cur_x) + 0.5,
                              baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx);
                cairo_line_to(cr, std::round(cur_x) + 0.5,
                              baseline_y + uniform_ext.y_bearing
                                  + uniform_ext.height + hl_pad + kVPadExtraPx);
                cairo_stroke(cr);
            }

            if constexpr (kDebugPerf) perf_counters::flag_drawn++;
        });

    cairo_restore(cr);
}

std::vector<FlagHitRect> compute_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiMarker>& markers,
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
    const double hl_pad = 2.0;

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

std::string transient_flag_text(const GuiTransient& m) {
    std::string text;
    if (m.is_begin_time)    text = "b=";
    else if (m.is_end_time) text = "e=";
    if (m.is_inserted) {
        text += 'i';
    } else if (m.has_displacement) {
        text += "d*";
    } else {
        text += 'd';
    }
    return text;
}

template <typename Emit>
void iterate_visible_transient_flags(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiTransient>& transients,
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
    (void)sr;
    const double baseline_y =
        static_cast<double>(top_strip_area.y + top_strip_area.h) - 13.0;
    const double pad          = 4.0;

    double rightmost_right_edge = -1e18;

    for (size_t i = 0; i < transients.size(); ++i) {
        const auto& m = transients[i];
        const double ms = static_cast<double>(m.effective_frame());
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
                              const std::vector<GuiTransient>& transients,
                              long long viewport_start_sample,
                              long long viewport_end_sample,
                              int sample_rate,
                              GuiColor enabled_color,
                              GuiColor disabled_color,
                              GuiColor selected_color,
                              const std::set<int>& selected_set,
                              int /*last_selected*/,
                              const TrimRange& trim) {
    (void)enabled_color;
    (void)disabled_color;
    (void)selected_color;
    (void)selected_set;
    if (waveform_area.w <= 0 || waveform_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    const double span = static_cast<double>(viewport_end_sample -
                                            viewport_start_sample);
    const double samples_per_pixel = span / static_cast<double>(waveform_area.w);
    if (samples_per_pixel <= 0.0) return;

    const double y0 = static_cast<double>(waveform_area.y);
    const double y1 = static_cast<double>(waveform_area.y + waveform_area.h);

    cairo_save(cr);
    cairo_set_line_width(cr, 1.0);

    // Two passes — one path per color — split by in-trim vs out-of-trim.
    // Disabled transients are skipped entirely (no stem); selection has no
    // effect on stems under the brief H palette rules. is_inserted does
    // not affect stem color either.
    for (int pass = 0; pass < 2; pass++) {
        const bool out_of_trim_pass = (pass == 1);
        const GuiColor c = out_of_trim_pass ? dim(kMarker) : kMarker;
        cairo_set_source_rgb(cr, c.r, c.g, c.b);
        for (size_t i = 0; i < transients.size(); ++i) {
            const auto& m = transients[i];
            if (m.disabled) continue;
            const int64_t pos = m.effective_frame();
            const double ms = static_cast<double>(pos);
            if (ms < static_cast<double>(viewport_start_sample)) continue;
            if (ms >= static_cast<double>(viewport_end_sample)) continue;
            if (marker_out_of_trim(pos, trim) != out_of_trim_pass) continue;
            const double x_raw =
                (ms - static_cast<double>(viewport_start_sample))
                    / samples_per_pixel;
            const double x_px = waveform_area.x + std::round(x_raw) + 0.5;
            cairo_move_to(cr, x_px, y0);
            cairo_line_to(cr, x_px, y1);
        }
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

void render_transient_flags(cairo_t* cr,
                            GuiRect top_strip_area,
                            const std::vector<GuiTransient>& transients,
                            long long viewport_start_sample,
                            long long viewport_end_sample,
                            int sample_rate,
                            GuiColor enabled_color,
                            GuiColor disabled_color,
                            GuiColor selected_color,
                            GuiColor highlight_color,
                            double font_size,
                            const std::set<int>& selected_set,
                            int last_selected,
                            const TrimRange& trim) {
    (void)enabled_color;
    (void)disabled_color;
    (void)selected_color;
    (void)highlight_color;
    (void)last_selected;
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    const double hl_pad = 2.0;

    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "1.23*1.2345:a.aa", &uniform_ext);

    iterate_visible_transient_flags(cr, top_strip_area, transients,
                                    viewport_start_sample, viewport_end_sample,
                                    sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& text, const cairo_text_extents_t& ext) {
            const bool is_selected = selected_set.count(i) > 0;
            const bool out_of_trim =
                marker_out_of_trim(transients[i].effective_frame(), trim);

            if (is_selected) {
                const GuiColor fill = out_of_trim ? dim(kMarker) : kMarker;
                cairo_set_source_rgb(cr, fill.r, fill.g, fill.b);
                cairo_rectangle(cr,
                                text_left,
                                baseline_y + uniform_ext.y_bearing - hl_pad - kVPadExtraPx,
                                hl_pad + ext.x_bearing + ext.width + hl_pad,
                                uniform_ext.height + 2 * hl_pad + 2 * kVPadExtraPx);
                cairo_fill(cr);
            }

            const GuiColor txt = out_of_trim ? dim(kText) : kText;
            cairo_set_source_rgb(cr, txt.r, txt.g, txt.b);
            cairo_move_to(cr, text_left + hl_pad, baseline_y);
            cairo_show_text(cr, text.c_str());
            if constexpr (kDebugPerf) perf_counters::flag_drawn++;
        });

    cairo_restore(cr);
}

std::vector<FlagHitRect> compute_transient_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiTransient>& transients,
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
    const double hl_pad = 2.0;

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

void render_dirty_indicator(cairo_t* cr, double cx, double cy,
                            GuiColor color) {
    cairo_save(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_arc(cr, cx, cy, 3.0, 0.0, 6.283185307179586);
    cairo_fill(cr);
    cairo_restore(cr);
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
    cairo_set_font_size(cr, 14.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, buf, &ext);
    cairo_restore(cr);
    return ext.x_advance;
}
