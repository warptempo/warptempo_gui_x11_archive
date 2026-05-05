#pragma once
#include "gui_markers.h"
#include "gui_transients.h"

#include <cairo/cairo.h>
#include <cmath>
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
inline constexpr GuiColor kMarker     = {0.57, 0.27, 0.68};
inline constexpr GuiColor kPlayhead   = {0.95, 0.85, 0.35};
inline constexpr GuiColor kAccent     = {0.75, 0.20, 0.18};
inline constexpr GuiColor kText       = {0.99, 0.99, 0.99};

// Flag rect geometry. Internal padding around the text glyph bounding box,
// applied symmetrically (horizontal and vertical). Brief Q raised this from
// 2 to 3 for breathing room. The single source of truth — both render and
// hit-rect computation must use this value, and so must the iteration popup
// in gui_main.cpp.
constexpr double kFlagInnerPadPx = 3.0;

// Extra vertical inner padding added on top of kFlagInnerPadPx on each side.
// (V.B Addendum 2: rects grow by 2*kVPadExtraPx in height; the horizontal
// pad is unaffected.) Was previously file-private to gui_render.cpp; moved
// here in Brief Q to share with gui_main.cpp's iteration popup.
constexpr double kVPadExtraPx = 1.0;

// Brief J/K: the flag rect's painted bottom edge sits this many pixels above
// the strip-bottom row (= waveform_area.y). Moving the rect off the strip-
// boundary row eliminates the boundary-paint fragility from Brief I.
constexpr double kFlagBottomLiftPx = 11.0;

// Half-blend toward background. The single derivation function for
// "subordinate" / "out-of-trim" state under the new palette.
constexpr GuiColor dim(GuiColor c) {
    return GuiColor{
        c.r * 0.5 + kBackground.r * 0.5,
        c.g * 0.5 + kBackground.g * 0.5,
        c.b * 0.5 + kBackground.b * 0.5,
    };
}

// Brief Y.4 sub-bug A: paints an opaque kBackground-colored rect under
// flag and iter popup text glyphs so the editor's growing pending text
// occludes neighbor text rather than blending with it. Without this fill,
// static flag/popup text paints directly on the canvas, and a widening
// edit shares pixels with adjacent flags' glyphs (both sets are visible
// blended). The fill matches the strip-clear color exactly, so in every
// non-edit state nothing changes visually; during an edit it does the
// occlusion work once pending text widens past the original flag width.
//
// Drawn before any outline (selection purple, editor parse-fail red) and
// before the text glyphs. Applies symmetrically to top-strip flags and
// iter popups (the symmetry is the architectural target — pack collision
// in compute_iter_popup_hits uses the same x_advance + 2 * kFlagInnerPadPx
// width by construction, so the pack rule and the visual occlusion rule
// agree).
//
// `text_left` is the actual text painting x — i.e., where cairo_move_to
// would place the cursor for cairo_show_text. The helper subtracts
// kFlagInnerPadPx itself to derive the fill rect's left edge. `bg_top`
// and `bg_height` reuse the existing outline/highlight rect math at the
// caller, so the fill aligns with the outline that gets painted on top.
inline void render_flag_text_bg_fill(cairo_t* cr,
                                     double text_left,
                                     double text_x_advance,
                                     double bg_top,
                                     double bg_height) {
    const double pad = kFlagInnerPadPx;
    const double x = std::round(text_left - pad);
    const double y = std::round(bg_top);
    const double w = std::round(text_x_advance + 2.0 * pad);
    const double h = std::round(bg_height);
    if (w <= 0.0 || h <= 0.0) return;
    cairo_save(cr);
    cairo_set_source_rgb(cr,
        kBackground.r, kBackground.g, kBackground.b);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_restore(cr);
}

// Out-of-trim predicate. Caller computes source-frame position from its
// own native field (time_seconds*sample_rate for GuiMarker,
// effective_frame() for GuiTransient) and passes it through here.
// The trim is treated as the closed interval [begin, end] for the dim-
// vs-active flag-color decision, so a marker landing exactly on the end
// boundary (the e=-marker itself) renders active, not dimmed. Other
// trim consumers (heatmap stripe, playback bounds, engine timemap) use
// their own direct comparisons against trim.begin / trim.end and are
// unaffected by this predicate.
inline bool marker_out_of_trim(int64_t source_frame_pos,
                               const TrimRange& trim) {
    return source_frame_pos < trim.begin || source_frame_pos > trim.end;
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
// `triangle_surface` is the pre-loaded playhead-triangle indicator (loaded by
// GuiX11); it's stamped above the stem via cairo_mask_surface, tinted with
// `color`. May be nullptr — in that case the indicator is skipped.
void render_playhead(cairo_t* cr,
                     GuiRect area,
                     double  playhead_pixel_x,
                     GuiColor color,
                     cairo_surface_t* triangle_surface);

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
// reference inherits the disabled flag of its defining marker). Disabled
// markers are skipped entirely; selection has no effect on stems under the
// brief H palette rules.
void render_markers(cairo_t* cr,
                    GuiRect waveform_area,
                    const std::vector<GuiMarker>& markers,
                    long long viewport_start_sample,
                    long long viewport_end_sample,
                    int sample_rate,
                    const TrimRange& trim);

// Editor overlay used by V.A1's top-flag editor. When `marker_index >= 0`
// and matches a flag the renderer is about to draw, that flag's text is
// replaced with `pending` and the background paints either kMarker (normal)
// or kAccent (when `is_red` indicates parse failure). A 1px-wide cursor is
// drawn at the x-position corresponding to `cursor_pos` (byte index into
// `pending`). `cursor_visible` toggles the bar on/off for blink. Pass
// marker_index = -1 to disable the overlay (normal rendering).
//
// V.B Addendum 2 / Brief X.2: `popup_editor_target` carries the marker
// index whose above-strip popup (iter or BPM) currently owns the
// IterationBracket- or BpmBracket-kind editor. The flag rect for that
// marker must suppress its last-selected background highlight so only
// the focused element (the popup) shows the highlight. -1 means "no
// popup is being edited". Iter and BPM modes are mutually exclusive, so
// at most one of them populates this field at a time.
struct FlagEditorOverlay {
    int         marker_index        = -1;
    int         popup_editor_target = -1;
    std::string pending;
    int         cursor_pos          = 0;
    bool        is_red              = false;
    bool        cursor_visible      = false;
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
// V.B Addendum 2 / Brief X.2: when `editor.popup_editor_target == i`,
// that flag's selection fill is suppressed so the above-strip popup
// (iter or BPM) owns the highlight exclusively.
//
// Disabled markers render identically to enabled markers in the top strip;
// the only disabled signal lives in the marker stem (handled by
// `render_markers`).
void render_flags(cairo_t* cr,
                  GuiRect top_strip_area,
                  const std::vector<GuiMarker>& markers,
                  long long viewport_start_sample,
                  long long viewport_end_sample,
                  int sample_rate,
                  double font_size,
                  const std::set<int>& selected_set,
                  const TrimRange& trim,
                  double playhead_pixel_x,
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
                              const TrimRange& trim);

// Flag text for transients is `[b=|e=]<status>` where status is `I`
// (inserted), `D` (detected), or `D*` (detected with user displacement).
//
// Brief H two-state model (no flag editor exists for transients):
//   1. Not selected: text in `kText`, no background fill.
//   2. Selected: background fill in `kMarker`, text in `kText`.
// Markers whose effective_frame lies outside `trim` wrap every color in
// `dim()` uniformly.
void render_transient_flags(cairo_t* cr,
                            GuiRect top_strip_area,
                            const std::vector<GuiTransient>& transients,
                            long long viewport_start_sample,
                            long long viewport_end_sample,
                            int sample_rate,
                            double font_size,
                            const std::set<int>& selected_set,
                            const TrimRange& trim,
                            double playhead_pixel_x);

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

// Returns the pixel width of the baseline-style monospace timestamp at the
// size used by render_timestamp. Needed so callers can position adjacent UI.
double measure_timestamp_width(cairo_t* cr, double seconds);

// One-line toggle for the render-path perf instrumentation (chunk M). When
// false, all perf_counters increments and [dbg perf] stderr emissions in
// the redraw path are compiled out.
constexpr bool kDebugPerf = false;

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
