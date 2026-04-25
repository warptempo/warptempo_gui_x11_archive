#pragma once
#include "gui_markers.h"
#include "gui_transients.h"

#include <cairo/cairo.h>
#include <set>
#include <string>
#include <vector>

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

// Screen-coord rect of one rendered flag, keyed back to its marker index.
// Emitted in the same order flags appear left-to-right.
struct FlagHitRect {
    int    marker_index;
    double x;
    double y;
    double w;
    double h;
};

// All rendering helpers take a Cairo context and pixel-space rectangles; they
// have no X11 or event-loop dependencies.

void render_background(cairo_t* cr, int x, int y, int w, int h);

void render_progress_bar(cairo_t* cr, int x, int y, int w, int h,
                         float progress_fraction);

// Draws one channel's waveform into `area`, displaying source samples in
// [viewport_start_sample, viewport_end_sample). Columns whose source-sample
// midpoint falls inside [trim_begin_sample, trim_end_sample) paint with
// `bright_color`; the rest paint with `dim_color`. Pass a wide range
// (0 .. total_frames) to disable dimming.
void render_waveform(cairo_t* cr,
                     GuiRect area,
                     const GuiAudio& audio,
                     int channel,
                     long long viewport_start_sample,
                     long long viewport_end_sample,
                     long long trim_begin_sample,
                     long long trim_end_sample,
                     GuiColor bright_color,
                     GuiColor dim_color);

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

// Draws vertical 1-pixel lines across `waveform_area` for each marker whose
// resolved sample falls inside [viewport_start_sample, viewport_end_sample).
// Effective disabled state is computed inline from the marker list (a label
// reference inherits the disabled flag of its defining marker). Markers whose
// indices appear in `selected_set` paint in `selected_color`; if such a
// marker is also effectively disabled, the selected color is dimmed.
// `last_selected` is unused by this renderer (flag-only visual) but kept in
// the signature for symmetry with render_flags.
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
                    int last_selected);

// Draws flag annotations in `top_strip_area` above visible markers. Iterates
// left-to-right and greedily skips any flag whose left edge would collide
// with the previously-rendered flag's right edge (+ small pad). Flag text
// derives from the marker's state: owned tempo `1.28`, owned+def
// `1.28 (a.01)`, inherit `(1.28)` where the value is resolved by walking
// backward, inherit+def `(1.28) (a.01)`, reference `a.01`. Scale factor is
// not surfaced. Flags whose indices appear in `selected_set` paint text in
// `selected_color`; only the `last_selected` flag (if rendered and in the
// set) gets the background highlight rectangle in `highlight_color`.
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
                  int last_selected);

// Same greedy-pack and elision logic as render_flags, without drawing —
// returns the screen-coord rects of the flags that would be rendered. The
// caller uses these for hit-testing. A minimal image-surface cairo_t works
// fine as `cr` since only font metrics are needed.
std::vector<FlagHitRect> compute_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size);

// Transient-marker analogues (chunk S.2.2). Same pixel layout as the warp
// versions; the visual differences are which list is drawn (transients
// instead of warp markers) and the supplied color set. `disabled` is taken
// directly from each transient (no label-cascade like warp markers).
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
                              int last_selected);

// Flag text for transients is `b=I`, `e=I`, or `I` based on the trim flag.
// S.2.2 only authors `inserted` transients, so the second character is
// always `I`. S.3 will extend with `D` / `D*` for detected / nudged.
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
                            int last_selected);

std::vector<FlagHitRect> compute_transient_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiTransient>& transients,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size);

// Walks backward from `index` through `markers` to find the nearest marker
// that owns its tempo (tempo_inherits == false and not a label reference).
// Returns 1.0 if no such marker exists (shouldn't happen given the time-0
// invariant, but defensive for edge cases during authoring).
double resolve_inherited_tempo(const std::vector<GuiMarker>& markers, int index);

// Companion to resolve_inherited_tempo: returns the scale string of the
// inherited source, or "" if none.
std::string resolve_inherited_tempo_scale(
    const std::vector<GuiMarker>& markers, int index);

// Draws the dirty-state indicator (a small filled circle) at (cx, cy).
void render_dirty_indicator(cairo_t* cr, double cx, double cy,
                            GuiColor color);

// Returns the pixel width of the baseline-style monospace timestamp at the
// size used by render_timestamp. Needed so callers can position adjacent UI.
double measure_timestamp_width(cairo_t* cr, double seconds);

// Unsaved-work dialog: rectangle of one of the three buttons, in screen
// coords. Returned by the layout helper so the click handler can hit-test.
struct DialogButtonRect {
    int x;
    int y;
    int w;
    int h;
};

// Layout of the entire dialog: the three buttons' rects (Save / Discard /
// Cancel in left-to-right order) and the panel's outer bounds. Panel width
// scales to fit the prompt text; height is fixed for the three-button row.
struct DialogLayout {
    DialogButtonRect panel;
    DialogButtonRect save;
    DialogButtonRect discard;
    DialogButtonRect cancel;
};

// Compute the dialog's on-screen layout against the current window
// dimensions and the prompt text. Cairo is needed for font measurement;
// the caller's `cr` can be the same pixmap context.
DialogLayout compute_dialog_layout(cairo_t* cr,
                                   int window_w,
                                   int window_h,
                                   const char* prompt_text);

// Paint the unsaved-work dialog: a semi-transparent overlay across the
// window, a centered panel with the prompt text, and three buttons. The
// button at `focused_button_index` (0 = Save, 1 = Discard, 2 = Cancel)
// renders with a highlighted outline; the others paint plain.
void render_dialog(cairo_t* cr,
                   const DialogLayout& layout,
                   const char* prompt_text,
                   int focused_button_index,
                   int window_w,
                   int window_h,
                   GuiColor text_color,
                   GuiColor panel_color,
                   GuiColor button_color,
                   GuiColor focus_color);

// One-line toggle for the render-path perf instrumentation (chunk M). When
// false, all perf_counters increments and [dbg perf] stderr emissions in
// the redraw path are compiled out.
constexpr bool kDebugPerf = true;

// Hot-loop counters for perf instrumentation. Incremented by the render
// helpers on every relevant inner-loop step; the caller zeroes them with
// perf_counters::reset() before a measured pass and reads the totals
// afterwards. Single-threaded, no synchronization.
namespace perf_counters {
    extern int wf_cols;              // pixel columns drawn by render_waveform
    extern int wf_pyramid_samples;   // peak-pyramid samples read
    extern int flag_measure;         // cairo_text_extents calls in flag render
    extern int flag_drawn;           // flags emitted (not elided)
    extern int flag_elided;          // viewport-hit flags skipped by greedy pack
    inline void reset() {
        wf_cols = 0;
        wf_pyramid_samples = 0;
        flag_measure = 0;
        flag_drawn = 0;
        flag_elided = 0;
    }
}
