#pragma once
#include "gui_markers.h"
#include "gui_transients.h"

#include <cairo/cairo.h>
#include <cstdint>
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

// Trim boundaries in source-frame samples. Threaded through the marker /
// flag renderers so they can apply uniform out-of-trim dimming under the
// brief H palette consolidation. Values match the convention in
// compute_trim_samples (gui_main.cpp).
struct TrimRange {
    int64_t begin;
    int64_t end;
};

// Brief H palette: bases shared across the renderer module and
// gui_main.cpp. kPlayhead is the foreground reference and must never be
// passed to dim() — preserve that invariant in subsequent phases.
inline constexpr GuiColor kBackground = {0.10, 0.10, 0.12};
inline constexpr GuiColor kWaveform   = {0.55, 0.75, 0.90};
inline constexpr GuiColor kMarker     = {0.30, 0.28, 0.22};
inline constexpr GuiColor kPlayhead   = {0.95, 0.85, 0.35};
inline constexpr GuiColor kAccent     = {0.75, 0.20, 0.18};
inline constexpr GuiColor kText       = {0.99, 0.99, 0.99};

// Half-blend toward background. The single derivation function for
// "subordinate" / "out-of-trim" state under the new palette.
constexpr GuiColor dim(GuiColor c) {
    return GuiColor{
        c.r * 0.5 + kBackground.r * 0.5,
        c.g * 0.5 + kBackground.g * 0.5,
        c.b * 0.5 + kBackground.b * 0.5,
    };
}

// Out-of-trim predicate. Caller computes source-frame position from its
// own native field (time_seconds*sample_rate for GuiMarker,
// effective_frame() for GuiTransient) and passes it through here.
inline bool marker_out_of_trim(int64_t source_frame_pos,
                               const TrimRange& trim) {
    return source_frame_pos < trim.begin || source_frame_pos >= trim.end;
}

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
                    int last_selected,
                    const TrimRange& trim);

// Editor overlay used by V.A1's top-flag editor. When `marker_index >= 0`
// and matches a flag the renderer is about to draw, that flag's text is
// replaced with `pending` and the background paints either highlight or
// `red_color` depending on `is_red`. A 1px-wide cursor is drawn at the
// x-position corresponding to `cursor_pos` (byte index into `pending`).
// `cursor_visible` toggles the bar on/off for blink. Pass marker_index =
// -1 to disable the overlay (normal rendering).
//
// V.B Addendum 2: `iter_editor_target` carries the marker index whose
// iteration popup (drawn above the flag strip) currently owns the
// IterationBracket-kind editor. The flag rect for that marker must
// suppress its last-selected background highlight so only the focused
// element (the iter popup) shows the highlight. -1 means "no iteration
// popup is being edited".
struct FlagEditorOverlay {
    int         marker_index       = -1;
    int         iter_editor_target = -1;
    std::string pending;
    int         cursor_pos         = 0;
    bool        is_red             = false;
    bool        cursor_visible     = false;
    GuiColor    red_color          = {0.75, 0.20, 0.18};
};

// Draws flag annotations in `top_strip_area` above visible markers. Iterates
// left-to-right and greedily skips any flag whose left edge would collide
// with the previously-rendered flag's right edge (+ small pad). Flag text
// is the canonical post-pipe payload: owned tempo `1.28`, owned+scale
// `1.28*1.2345`, owned+def `1.28:a.01`, owned+scale+def `1.28*1.2345:a.01`,
// inherit `pass`, label reference `a.01`.
//
// Brief H three-state model: each flag renders in one of three states.
//   1. Not selected: text in `kText`, no background fill.
//   2. Selected, editor not engaged: background fill in `kMarker`, text
//      in `kText`. No cursor.
//   3. Selected, editor engaged: state 2 plus a 1-px blinking cursor.
// Parse-fail variant of state 2/3: fill is `kAccent` instead of `kMarker`.
// Markers whose source-frame position lies outside `trim` wrap every
// color in `dim()` uniformly — no element of the flag escapes the dim.
//
// V.B Addendum 2: when `editor.iter_editor_target == i`, that flag's
// selection fill is suppressed so the iter popup above it owns the
// highlight exclusively.
//
// `enabled_color`, `disabled_color`, `selected_color`, `highlight_color`,
// and `last_selected` are unused under the new palette and remain in the
// signature only until the phase 6 cleanup. Disabled markers render
// identically to enabled markers in the top strip; the only disabled
// signal lives in the marker stem (handled by `render_markers`).
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
                  const FlagEditorOverlay& editor = {});

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
                              int last_selected,
                              const TrimRange& trim);

// Flag text for transients is `[b=|e=]<status>` where status is `I`
// (inserted), `D` (detected), or `D*` (detected with user displacement).
//
// Brief H two-state model (no flag editor exists for transients):
//   1. Not selected: text in `kText`, no background fill.
//   2. Selected: background fill in `kMarker`, text in `kText`.
// Markers whose effective_frame lies outside `trim` wrap every color in
// `dim()` uniformly. As with `render_flags`, `enabled_color`,
// `disabled_color`, `selected_color`, `highlight_color`, and
// `last_selected` are unused under the new palette and remain in the
// signature only until the phase 6 cleanup.
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
                            const TrimRange& trim);

std::vector<FlagHitRect> compute_transient_flag_hit_rects(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiTransient>& transients,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size);

// Returns the text that render_flags would draw for `markers[idx]`. Used
// by the GUI text editor to seed the editable payload (the on-screen rect
// content) when entering edit mode on a flag.
std::string flag_text_for_marker(const std::vector<GuiMarker>& markers, int idx);

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

// Modal-dialog button rectangle in screen coords. Returned by the layout
// helper so the click handler can hit-test.
struct DialogButtonRect {
    int x;
    int y;
    int w;
    int h;
};

// Layout of an N-button dialog: the buttons' rects (left-to-right, parallel
// to the labels passed to compute_dialog_layout) and the panel's outer
// bounds. Panel width scales to fit the wider of prompt text or button row.
struct DialogLayout {
    DialogButtonRect              panel;
    std::vector<DialogButtonRect> buttons;
};

// Compute the dialog's on-screen layout for the given prompt + ordered
// button labels. Cairo is needed for font measurement; the caller's `cr`
// can be the same pixmap context.
DialogLayout compute_dialog_layout(cairo_t* cr,
                                   int window_w,
                                   int window_h,
                                   const char* prompt_text,
                                   const std::vector<std::string>& button_labels);

// Paint a modal dialog: a semi-transparent overlay across the window, a
// centered panel with the prompt text, and one button per label. The
// button at `focused_button_index` renders with a highlighted outline.
void render_dialog(cairo_t* cr,
                   const DialogLayout& layout,
                   const char* prompt_text,
                   const std::vector<std::string>& button_labels,
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
