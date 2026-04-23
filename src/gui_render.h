#pragma once
#include <cairo/cairo.h>

class GuiAudio;

struct GuiRect {
    int x;
    int y;
    int w;
    int h;
};

struct GuiColor {
    double r;
    double g;
    double b;
};

// All rendering helpers take a Cairo context and pixel-space rectangles; they
// have no X11 or event-loop dependencies.

void render_background(cairo_t* cr, int x, int y, int w, int h);

void render_progress_bar(cairo_t* cr, int x, int y, int w, int h,
                         float progress_fraction);

// Draws one channel's waveform into `area`, displaying source samples in
// [viewport_start_sample, viewport_end_sample). Chooses a pyramid level from
// the columns-per-sample ratio and issues one batched stroke.
void render_waveform(cairo_t* cr,
                     GuiRect area,
                     const GuiAudio& audio,
                     int channel,
                     long long viewport_start_sample,
                     long long viewport_end_sample,
                     GuiColor color);

// Draws a thin 1px vertical line across `area` at column `playhead_pixel_x`
// (offset from area.x, float for subpixel centering). No-op if outside.
void render_playhead(cairo_t* cr,
                     GuiRect area,
                     double  playhead_pixel_x,
                     GuiColor color);

// Formats `seconds` as MM:SS.mmm and paints it with Cairo's monospace face.
// Baseline of the text lands at (x, y).
void render_timestamp(cairo_t* cr,
                      int      x,
                      int      y,
                      double   seconds,
                      GuiColor color);
