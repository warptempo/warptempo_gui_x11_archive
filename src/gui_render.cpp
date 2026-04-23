#include "gui_render.h"
#include "gui_audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
                     GuiColor color) {
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

    cairo_save(cr);
    cairo_set_source_rgb(cr, color.r, color.g, color.b);
    cairo_set_line_width(cr, 1.0);

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

        const double x_px = area.x + i + 0.5;
        cairo_move_to(cr, x_px, y_top);
        cairo_line_to(cr, x_px, y_bottom);
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

    const double x_px = area.x + playhead_pixel_x + 0.5;

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

    // Round to nearest millisecond before splitting so the displayed value
    // matches what a user would read off a stopwatch.
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
