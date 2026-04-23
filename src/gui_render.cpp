#include "gui_render.h"
#include "gui_audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// True if the label defined by marker at `def_index` is disabled. The
// storage rule: `disabled` is only meaningful on label-defining markers.
bool effective_disabled(const std::vector<GuiMarker>& markers, int idx) {
    if (idx < 0 || idx >= static_cast<int>(markers.size())) return false;
    const auto& m = markers[idx];
    if (!m.label_def.empty()) return m.disabled;
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

// The flag text rules from chunk I Part 2:
//   owned            → "1.28"
//   owned + def      → "1.28 (a.01)"
//   inherit          → "(1.28)"  (value resolved by walk-backward)
//   inherit + def    → "(1.28) (a.01)"
//   label reference  → "a.01"
std::string flag_text(const std::vector<GuiMarker>& markers, int idx) {
    const auto& m = markers[idx];
    if (!m.label_ref.empty()) return m.label_ref;

    double tempo = m.tempo_base;
    if (m.tempo_inherits) {
        tempo = resolve_inherited_tempo(markers, idx);
    }

    char tbuf[32];
    std::snprintf(tbuf, sizeof(tbuf), "%.2f", tempo);
    std::string text;
    if (m.tempo_inherits) {
        text = "(";
        text += tbuf;
        text += ")";
    } else {
        text = tbuf;
    }
    if (!m.label_def.empty()) {
        text += " (";
        text += m.label_def;
        text += ")";
    }
    return text;
}

} // namespace

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

    int level;
    if (samples_per_pixel < 1.0) {
        level = 0;
    } else {
        level = static_cast<int>(std::floor(std::log2(samples_per_pixel)));
    }
    if (level < 0) level = 0;
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

    const double x_px = area.x + std::floor(playhead_pixel_x + 0.5) + 0.5;

    cairo_save(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x_px, area.y);
    cairo_line_to(cr, x_px, area.y + area.h);
    cairo_stroke(cr);
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
                    int /*last_selected*/) {
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

    // Two passes — one path per color — for the unselected markers, split by
    // enabled/disabled. Selected markers are drawn on top afterwards so
    // their selected_color wins regardless of z-order within the unselected
    // set.
    for (int pass = 0; pass < 2; pass++) {
        const bool this_disabled = (pass == 1);
        const GuiColor& c = this_disabled ? disabled_color : enabled_color;
        cairo_set_source_rgb(cr, c.r, c.g, c.b);
        for (size_t i = 0; i < markers.size(); ++i) {
            if (selected_set.count(static_cast<int>(i))) continue;
            const bool dis = effective_disabled(markers, static_cast<int>(i));
            if (dis != this_disabled) continue;
            const auto& m = markers[i];
            const double ms = m.time_seconds * sr;
            if (ms < static_cast<double>(viewport_start_sample)) continue;
            if (ms >= static_cast<double>(viewport_end_sample)) continue;
            const double x_px = waveform_area.x +
                (ms - static_cast<double>(viewport_start_sample))
                    / samples_per_pixel + 0.5;
            cairo_move_to(cr, x_px, y0);
            cairo_line_to(cr, x_px, y1);
        }
        cairo_stroke(cr);
    }

    for (int idx : selected_set) {
        if (idx < 0 || idx >= static_cast<int>(markers.size())) continue;
        const auto& m = markers[idx];
        const double ms = m.time_seconds * sr;
        if (ms < static_cast<double>(viewport_start_sample)) continue;
        if (ms >= static_cast<double>(viewport_end_sample)) continue;
        GuiColor c = selected_color;
        if (effective_disabled(markers, idx)) {
            c.r *= 0.70; c.g *= 0.70; c.b *= 0.70;
        }
        cairo_set_source_rgb(cr, c.r, c.g, c.b);
        const double x_px = waveform_area.x +
            (ms - static_cast<double>(viewport_start_sample))
                / samples_per_pixel + 0.5;
        cairo_move_to(cr, x_px, y0);
        cairo_line_to(cr, x_px, y1);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}

namespace {

// Shared greedy-pack iteration used by both render_flags and
// compute_flag_hit_rects. Invokes `emit(i, text_left, baseline_y, ext)` for
// each flag that survives elision, in left-to-right order. The cairo font
// face/size are assumed to already be set on `cr` by the caller.
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
    const double left_offset  = 3.0;
    const double pad          = 4.0;

    double rightmost_right_edge = -1e18;

    for (size_t i = 0; i < markers.size(); ++i) {
        const auto& m = markers[i];
        const double ms = m.time_seconds * sr;
        if (ms < static_cast<double>(viewport_start_sample)) continue;
        if (ms >= static_cast<double>(viewport_end_sample)) continue;

        const double x_px = top_strip_area.x +
            (ms - static_cast<double>(viewport_start_sample))
                / samples_per_pixel + 0.5;
        const double text_left = x_px + left_offset;
        if (text_left < rightmost_right_edge + pad) continue;

        const std::string text = flag_text(markers, static_cast<int>(i));
        if (text.empty()) continue;

        cairo_text_extents_t ext;
        cairo_text_extents(cr, text.c_str(), &ext);

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
                  int last_selected) {
    if (top_strip_area.w <= 0 || top_strip_area.h <= 0) return;
    if (viewport_end_sample <= viewport_start_sample) return;
    if (sample_rate <= 0) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    const double hl_pad = 2.0;

    iterate_visible_flags(cr, top_strip_area, markers,
                          viewport_start_sample, viewport_end_sample,
                          sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& text, const cairo_text_extents_t& ext) {
            const bool is_selected = selected_set.count(i) > 0;
            const bool is_last     = (i == last_selected) && is_selected;
            const bool dis         = effective_disabled(markers, i);

            // Only the last-selected flag gets the background highlight.
            if (is_last) {
                cairo_set_source_rgb(cr, highlight_color.r,
                                     highlight_color.g, highlight_color.b);
                cairo_rectangle(cr,
                                text_left + ext.x_bearing - hl_pad,
                                baseline_y + ext.y_bearing - hl_pad,
                                ext.width  + 2 * hl_pad,
                                ext.height + 2 * hl_pad);
                cairo_fill(cr);
            }

            GuiColor c = is_selected ? selected_color
                                     : (dis ? disabled_color : enabled_color);
            if (is_selected && dis) {
                c.r *= 0.70; c.g *= 0.70; c.b *= 0.70;
            }
            cairo_set_source_rgb(cr, c.r, c.g, c.b);
            cairo_move_to(cr, text_left, baseline_y);
            cairo_show_text(cr, text.c_str());
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

    iterate_visible_flags(cr, top_strip_area, markers,
                          viewport_start_sample, viewport_end_sample,
                          sample_rate,
        [&](int i, double text_left, double baseline_y,
            const std::string& /*text*/, const cairo_text_extents_t& ext) {
            FlagHitRect r;
            r.marker_index = i;
            r.x = text_left + ext.x_bearing;
            r.y = baseline_y + ext.y_bearing;
            r.w = ext.width;
            r.h = ext.height;
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
