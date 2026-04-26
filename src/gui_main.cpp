#include "gui_audio.h"
#include "gui_markers.h"
#include "gui_playback.h"
#include "gui_render.h"
#include "gui_render_pipeline.h"
#include "gui_text_editor.h"
#include "gui_transients.h"
#include "gui_x11.h"

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
constexpr GuiColor kWaveformColor     = {0.55, 0.75, 0.90};
constexpr GuiColor kWaveformDimColor  = {0.30, 0.38, 0.45};
constexpr GuiColor kPlayheadColor     = {0.95, 0.85, 0.35};
constexpr GuiColor kTimestampColor    = {0.80, 0.80, 0.82};
constexpr GuiColor kMarkerColor       = {0.85, 0.82, 0.78};
constexpr GuiColor kMarkerDimColor    = {0.45, 0.42, 0.40};
constexpr GuiColor kSelectedColor     = kPlayheadColor;
constexpr GuiColor kFlagHighlightColor= {0.30, 0.28, 0.22};
constexpr GuiColor kDirtyColor        = {0.90, 0.65, 0.35};
constexpr double   kFlagFontSize      = 13.0;

// Transient-mode color set (chunk S.2.2). Coral palette so the second
// editable layer is visually distinct from warp yellow and waveform blue.
// Disabled / selected variants follow the same dimming / brightening
// ratios used by the warp set above.
constexpr GuiColor kTransientColor          = {1.00, 0.42, 0.42};
constexpr GuiColor kTransientDimColor       = {0.50, 0.21, 0.21};
constexpr GuiColor kTransientSelectedColor  = {1.00, 0.55, 0.45};
constexpr GuiColor kTransientHighlightColor = {0.45, 0.20, 0.20};
constexpr GuiColor kTransientPlayheadColor  = {1.00, 0.42, 0.42};

// Unsaved-work dialog palette: panel = the waveform-dim tone; button =
// the flag-highlight tone; focus = the playhead/selected yellow. Text
// reuses the marker text color so it stays legible on both panel and
// button backgrounds.
constexpr GuiColor kDialogTextColor   = kMarkerColor;
constexpr GuiColor kDialogPanelColor  = kWaveformDimColor;
constexpr GuiColor kDialogButtonColor = kFlagHighlightColor;
constexpr GuiColor kDialogFocusColor  = {0.55, 0.45, 0.20};

// Half-width in pixels of the selection-hit window when clicking on a marker.
constexpr int kMarkerHitHalfPx = 3;

// Double-click detection thresholds. X11 doesn't synthesize DoubleClick; we
// roll it from ButtonPress timing + position deltas.
constexpr int kDoubleClickMs      = 300;
constexpr int kDoubleClickPixels  = 5;

// ms-per-pixel for each numeric zoom level. Level 0 is most zoomed in.
constexpr double kZoomMsPerPixel[] = {
    1.25, 2.6, 5.2, 10.4, 20.8, 41.7, 83.3, 166.7
};
constexpr int kNumZoomLevels = static_cast<int>(
    sizeof(kZoomMsPerPixel) / sizeof(kZoomMsPerPixel[0]));

// Sentinel for the fit-file level ("whole file visible"). Computed at zoom /
// resize time, not stored as a fixed ms/pixel.
constexpr int kFitFileLevel = kNumZoomLevels;

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

// Classification of each undo entry by the net effect of its op on the
// marker vector. Used by post-restore rules to decide selection and
// playhead behavior; count-preserving ops split Move vs Other so Move can
// restore the "what just moved" selection.
enum class OpKind { Create, Destroy, Move, Other };

// One entry on either stack. Carries the pre-mutation marker snapshot plus
// a pre-op selection hint (so Undo-of-Destroy / Undo-of-Move can restore
// a sensible selection anchor) and the op kind.
//
// Chunk S.2.2: every entry now also carries the pre-mutation transient
// snapshot and the mode the operation was performed in. Both lists are
// always restored on undo/redo so the inverse is symmetric regardless of
// which list the op actually touched. `op_mode` lets undo flip the active
// mode as a side effect — visual feedback for what's being undone.
struct UndoEntry {
    std::vector<GuiMarker>    snapshot;
    std::vector<GuiTransient> transient_snapshot;
    char                      op_mode              = 'W';
    OpKind                    op_kind              = OpKind::Other;
    std::set<int>             hint_selected;
    int                       hint_last_selected   = -1;
};

// Ctrl+drag state. `active` gates motion handling; the rest captures the
// pre-drag snapshot so Escape can restore positions and clamps can be
// evaluated without re-scanning the marker list on every motion event.
//
// Storing per-marker (min_allowed, max_allowed) as the spec suggests works
// for a contiguous drag set, but a non-contiguous set (e.g. indices 2 and
// 5 selected, 3 and 4 not) can be bounded more tightly by the nearest
// non-selected neighbors of every dragged marker. We precompute a single
// scalar `delta_min` / `delta_max` that's correct for both cases: the delta
// is applied uniformly, so its feasible range is the intersection of per-
// marker per-neighbor bounds. Trim is purely cosmetic and does not
// constrain edits.
struct DragState {
    bool                active = false;
    std::vector<int>    dragging_markers;   // sorted ascending
    std::vector<double> original_times;     // parallel to dragging_markers
    double              anchor_mouse_time_seconds = 0.0;
    double              delta_min = -std::numeric_limits<double>::infinity();
    double              delta_max =  std::numeric_limits<double>::infinity();
    bool                moved = false;
    // Full pre-drag marker state. Captured at button-press so commit_drag
    // can push it onto the undo stack. Escape-cancel discards it implicitly
    // (DragState is reset wholesale there).
    std::vector<GuiMarker>    pre_drag_snapshot;
    std::vector<GuiTransient> pre_drag_transient_snapshot;
    // Pre-drag selection for the undo hint; carried onto the entry at commit.
    std::set<int>          pre_drag_selected;
    int                    pre_drag_last_selected = -1;
    // Index of the marker that was clicked to start the drag. Used to track
    // the playhead during motion so the audio cursor follows the grabbed
    // marker as it moves.
    int                    hit_marker           = -1;
    // Playhead sample at drag start, restored on Escape-cancel.
    int64_t                pre_drag_playhead_sample = 0;
    // Which list this drag operates on (chunk S.2.2). The motion / commit
    // / cancel handlers dispatch on this so a drag started in transient
    // mode mutates the transient list.
    char                   drag_mode = 'W';
};

// Two-stack undo/redo history for marker mutations. Entries are full
// snapshots of the marker vector plus an op-kind tag and pre-op selection
// hint — small enough to store directly rather than diff. Both stacks are
// capped at kCap; the oldest undo entry is evicted when the cap is exceeded.
//
// The saved reference is a signed distance from the current position to the
// snapshot corresponding to what's on disk. Positive = ahead on the redo
// stack; negative = behind on the undo stack; 0 = at current. `saved_valid`
// tracks whether the saved reference is still reachable: a new mutation
// that clears the redo stack while saved was ahead orphans it (saved_valid
// becomes false), and dirty stays true until the next save rebinds it.
struct UndoHistory {
    static constexpr size_t kCap = 500;
    std::vector<UndoEntry> undo_stack;
    std::vector<UndoEntry> redo_stack;
    int  saved_distance = 0;
    bool saved_valid    = true;

    bool is_dirty() const {
        return !(saved_valid && saved_distance == 0);
    }

    // Push the pre-mutation entry. Clears the redo stack. If the saved
    // reference was on the redo stack, it's orphaned (saved_valid = false).
    // If pushing would evict the bottom of the undo stack and the saved
    // reference pointed at or below the evicted entry, it's pinned to the
    // new bottom — per Part 5 of the chunk brief, that's the least-
    // surprising user-facing behavior even though it's not strictly correct.
    void push(UndoEntry entry) {
        if (saved_valid && saved_distance > 0) saved_valid = false;
        redo_stack.clear();
        if (saved_valid) saved_distance -= 1;
        undo_stack.push_back(std::move(entry));
        if (undo_stack.size() > kCap) {
            undo_stack.erase(undo_stack.begin());
            if (saved_valid &&
                saved_distance < -static_cast<int>(undo_stack.size())) {
                saved_distance = -static_cast<int>(undo_stack.size());
            }
        }
    }

    void mark_saved() {
        saved_distance = 0;
        saved_valid    = true;
    }

    void reset() {
        undo_stack.clear();
        redo_stack.clear();
        saved_distance = 0;
        saved_valid    = true;
    }
};

// State for the plain/Shift left-button playhead-drag gesture. Modifier
// state is captured at press and not re-read during motion; the gesture
// ends on release (or on Escape, which ends at current position without
// restoring). `last_added_marker` tracks the most-recently-snapped-to
// marker *added fresh* by this gesture, so subsequent snap-to-another
// can un-toggle it without removing pre-existing selections.
struct PlayheadDragState {
    bool active             = false;
    bool shift_at_press     = false;
    int  last_added_marker  = -1;
    bool last_added_was_fresh = false;
};

// What action triggered the modal dialog; the activate-button dispatch
// switches on this together with the focused button index. Save/Discard/
// Cancel apply to the unsaved-work dialogs (CLOSE_WINDOW, REVERT_TO_BLANK);
// Detect/Cancel applies to the re-detect confirmation (DETECT_TRANSIENTS).
enum class DialogTrigger {
    CLOSE_WINDOW,
    REVERT_TO_BLANK,
    DETECT_TRANSIENTS,
};

// In-window modal dialog state. When `active` is true, the redraw paints
// a dim overlay + panel + N buttons on top of the normal pipeline; input
// handling routes through dialog-only filters and the rest of the UI is
// visually blocked. `button_labels` holds the ordered button text;
// `focused_button` is the index into that vector.
struct DialogState {
    bool                      active         = false;
    int                       focused_button = 0;
    DialogTrigger             trigger        = DialogTrigger::CLOSE_WINDOW;
    std::string               prompt_text    =
        "Unsaved changes. Save before continuing?";
    std::vector<std::string>  button_labels  = {"Save", "Discard", "Cancel"};
};

// Navigational bookmark. Holds a snapshot of the three fields that define
// what the user sees and where playback would start. Not in the undo domain.
//
// Chunk S.2.2: each tab also carries per-mode selection slots so switching
// tabs (Ctrl+Tab) and switching modes (`t`) both restore the right
// selection set for the destination cell. The active selection lives in
// AppState; these slots are the persistent snapshots.
struct Tab {
    int64_t viewport_start_sample = 0;
    int     zoom_level            = 0;
    int64_t playhead_sample       = 0;

    std::set<int> warp_selected;
    int           warp_last_selected      = -1;
    std::set<int> transient_selected;
    int           transient_last_selected = -1;
};

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

struct AppState {
    int     width                 = 1400;
    int     height                = 800;
    bool    loading               = false;
    float   load_progress         = 0.0f;

    int64_t playhead_sample       = 0;
    int     zoom_level            = 0;
    int64_t viewport_start_sample = 0;
    bool    follow_mode           = true;

    // Playback state. `playback_cursor` is the last sample read from the
    // audio thread; `is_playing` mirrors the audio thread's flag so the
    // main loop can detect natural end-of-playback. `playback_speed` is
    // authoritative on the main thread and pushed to the playback engine
    // on every change.
    int64_t playback_cursor = 0;
    bool    is_playing      = false;
    float   playback_speed  = 1.0f;

    // "Last-Space playhead": the sample where Space-to-play was last
    // pressed. Space-to-stop (and natural end) restore `playhead_sample`
    // to this value — return-to-launch. Only differs from `playhead_sample`
    // while Space-initiated playback is active; otherwise tracks it.
    int64_t last_space_sample = 0;

    // Companion files discovered alongside the loaded audio. Chunk E just
    // records these; later chunks will parse their contents.
    std::string warpmarkers_path;
    std::string settings_path;
    // Sibling `.transientmarkers` path. Computed at file load. Empty when
    // no audio is loaded.
    std::string transientmarkers_path;

    // Absolute or relative path of the currently loaded audio file. Used by
    // the chunk-Q render hotkey stub to compute the output path. Empty when
    // no file is loaded (blank state).
    std::string source_audio_path;

    // Parsed warp markers for the currently loaded audio. Empty on load
    // failure or before the first audio load.
    GuiMarkers  markers;

    // Parsed transient markers (chunk S.2.2). Authored by the GUI but not
    // yet consumed by the render pipeline (S.3 will wire that up).
    GuiTransients transients;

    // Multi-selection set + focus. `last_selected_marker` is either -1 or
    // a member of `selected_markers`; keyed operations (Tab cycling, `j`)
    // anchor on it.
    //
    // Chunk S.2.2: this pair holds the *active* selection — i.e. for the
    // current tab + current `active_mode`. The persistent per-tab per-mode
    // slots live on Tab and are saved/restored on mode/tab transitions.
    std::set<int> selected_markers;
    int           last_selected_marker = -1;

    // Spec-mandated AppState slots for transient selection. Chunk S.2.2
    // routes the active selection through `selected_markers` regardless
    // of mode, so these stay default-empty and act as the seed for the
    // first transient-mode entry on a freshly loaded file.
    std::set<int> transient_selected_markers;
    int           transient_last_selected_marker = -1;

    // Active editing mode: 'W' = warp markers, 'T' = transient markers
    // (chunk S.2.2). Toggled by `t`. Determines which list is visible /
    // edited / hit-tested and which color set is used for the playhead
    // and selected indicators.
    char active_mode = 'W';

    // Ctrl+drag state. Not reset across file loads — explicitly cleared
    // there and on button release / Escape.
    DragState     drag;

    // Playhead drag state (plain / Shift left-button). Cleared on button
    // release, Escape, and file load.
    PlayheadDragState playhead_drag;

    // Undo/redo history for marker mutations. `dirty` below becomes a
    // derived signal: dirty = history.is_dirty(). Save/load reshape the
    // saved reference rather than touching dirty directly.
    UndoHistory history;

    // True if the marker list has been modified since load or last save.
    // Derived from `history`: mirrors history.is_dirty() after every op.
    //
    // Chunk S.2.2 splits dirty into two per-file flags so save can pick
    // each file independently and the unsaved-work dialog can be triggered
    // by either. `dirty` becomes the OR of both. The undo system continues
    // to drive both flags through history; per-mode dirty is recomputed
    // after every push/undo/redo by walking the saved-distance against
    // each entry's op_mode.
    bool        warp_dirty           = false;
    bool        transient_dirty      = false;
    bool        dirty                = false;

    // True until the first save in this session; used to log a one-time
    // notice if the on-disk file had content the canonical form drops.
    bool        first_save_pending   = true;

    // If a drop arrives mid-load, the path is stashed here and processed
    // after the active load returns. Empty means "no pending drop."
    std::string pending_drop_path;

    // Double-click detection state. Tracks the most recent left-button
    // press so the next one can compare and upgrade to a double-click.
    std::chrono::steady_clock::time_point last_click_time =
        std::chrono::steady_clock::time_point{};
    int          last_click_x        = -10000;
    int          last_click_y        = -10000;
    bool         last_click_consumed = true;

    // For redraw-time diagnostics (acceptance criterion 15).
    std::chrono::steady_clock::time_point stats_last_report =
        std::chrono::steady_clock::now();
    double  stats_max_redraw_ms = 0.0;
    int     stats_over_1ms_count = 0;

    // Timestamp of the most recent user input event (key/button/motion).
    // Read by the perf-instrumentation path to compute event-to-paint
    // latency (e2e). Default-constructed (epoch zero) means "no input yet."
    std::chrono::steady_clock::time_point last_input_event_time{};

    // Identity counter for the currently loaded audio. Bumped on every
    // successful file load. Used by the waveform cache as part of its
    // invalidation fingerprint so a file swap forces a re-render.
    long long audio_generation = 0;

    // A/B navigational tabs. Ctrl+Tab toggles between them; each holds a
    // snapshot of viewport/zoom/playhead. Persisted in .settings.
    Tab  tab_a;
    Tab  tab_b;
    char active_tab = 'A';

    // Pass-through entries read from .settings on load and re-emitted
    // verbatim on save. Preserves original order. Never interpreted by
    // the GUI; exists so the wrapper script's keys survive a round-trip.
    std::vector<std::pair<std::string, std::string>> settings_passthrough;

    // Unsaved-work dialog. Active only when a close / revert gesture
    // fires while history is dirty. See Part 2 of chunk Q.
    DialogState dialog;

    // Top-flag text editor (V.A1). Active only when editing a flag rect
    // in warp mode. The editor owns the keyboard while active and
    // overlays a custom rect + cursor on top of render_flags.
    gui_text_editor::State top_flag_editor;
    // Last-painted cursor visibility, so the tick can detect a flip and
    // invalidate the top strip without redundant repaints.
    bool top_flag_editor_blink_last = false;

    // Render-queue state (chunk U). `queue_running` is true only inside the
    // Ctrl+Alt+Shift+E walker. The Esc handler checks it to scope the
    // cancel binding away from normal interaction. `queue_cancel_requested`
    // is set by Esc during a queue run and read between entries.
    bool queue_running           = false;
    bool queue_cancel_requested  = false;
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

int top_strip_height(int window_height) {
    return static_cast<int>(std::lround(window_height * kTopStripRatio));
}

int bottom_strip_height(int window_height) {
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
    const std::vector<GuiMarker>& warp_markers,
    const std::vector<GuiTransient>& transients,
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

double samples_per_pixel_at(int zoom_level,
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
    const int col = static_cast<int>(std::floor(px_x));
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

GuiRect timestamp_invalidate_rect(int window_height) {
    return GuiRect{0, window_height - kTimestampRegionH,
                   kTimestampRegionW, kTimestampRegionH};
}

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

// Atomic write: pass-through lines first in their original order, then the
// six canonical tab lines. Matches the `.warpmarkers` write pattern
// (tmp → fsync → rename). Best-effort: failure is logged by the caller.
bool write_settings_file(
    const std::string& path,
    const Tab& tab_a,
    const Tab& tab_b,
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

    // -- Trim helpers --------------------------------------------------------

    auto trim_range = [&]() -> std::pair<int64_t, int64_t> {
        if (audio.total_frames() <= 0) return {0, 0};
        return compute_trim_samples(
            app.markers.markers(), app.transients.markers(),
            audio.sample_rate(), audio.total_frames());
    };
    auto trim_begin_sample = [&]() -> int64_t { return trim_range().first; };
    auto trim_end_sample   = [&]() -> int64_t { return trim_range().second; };

    // -- Navigation/viewport helpers ----------------------------------------

    // Viewport changes repaint the waveform area and the top strip together:
    // flag positions depend on the viewport, so any pan/zoom has to refresh
    // flags as well as waveform. Playhead-only moves keep using the narrow
    // column invalidation below.
    auto invalidate_waveform_area = [&]() {
        const GuiRect a = waveform_area(app);
        const int y0 = 0;
        const int y1 = a.y + a.h;
        gui.invalidate_region(0, y0, app.width, y1 - y0);
    };

    auto invalidate_timestamp_area = [&]() {
        const GuiRect t = timestamp_invalidate_rect(app.height);
        gui.invalidate_region(t.x, t.y, t.w, t.h);
    };

    auto invalidate_playhead_columns = [&](double old_px, double new_px) {
        const GuiRect area = waveform_area(app);
        const GuiRect r_old = playhead_invalidate_rect(area, old_px);
        const GuiRect r_new = playhead_invalidate_rect(area, new_px);
        // Union when close (common case: 1px nudges overlap) — one expose.
        if (rects_intersect(r_old, r_new) ||
            std::abs(new_px - old_px) < 4.0) {
            const GuiRect u = union_rect(r_old, r_new);
            if (u.w > 0 && u.h > 0) {
                gui.invalidate_region(u.x, u.y, u.w, u.h);
            }
        } else {
            if (r_old.w > 0) gui.invalidate_region(r_old.x, r_old.y, r_old.w, r_old.h);
            if (r_new.w > 0) gui.invalidate_region(r_new.x, r_new.y, r_new.w, r_new.h);
        }
    };

    // move_playhead_to: update playhead, keep viewport so playhead stays
    // visible. Invalidate only what changed. Clamps to the full audio
    // range; trim is purely cosmetic so the playhead is free to sit in
    // the dim region.
    auto move_playhead_to = [&](int64_t new_sample) {
        if (audio.total_frames() <= 0) return;
        if (new_sample < 0) new_sample = 0;
        const int64_t total = audio.total_frames();
        if (total > 0 && new_sample >= total) new_sample = total - 1;

        const double old_px = playhead_pixel_x(app, audio);
        const int64_t old_vp = app.viewport_start_sample;
        const int64_t visible = samples_visible(app, audio);

        app.playhead_sample = new_sample;

        const int64_t vp_end = app.viewport_start_sample + visible;
        bool viewport_changed = false;

        if (new_sample < app.viewport_start_sample) {
            app.viewport_start_sample = new_sample;
            viewport_changed = true;
        } else if (new_sample >= vp_end) {
            const double spp = current_samples_per_pixel(app, audio);
            const int64_t one_px = static_cast<int64_t>(std::llround(spp));
            app.viewport_start_sample =
                new_sample - (visible - std::max<int64_t>(one_px, 1));
            viewport_changed = true;
        }
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) viewport_changed = true;

        if (viewport_changed) {
            invalidate_waveform_area();
        } else {
            const double new_px = playhead_pixel_x(app, audio);
            invalidate_playhead_columns(old_px, new_px);
        }
        invalidate_timestamp_area();
    };

    auto move_playhead_pixels = [&](int delta_px) {
        if (audio.total_frames() <= 0) return;
        const double spp = current_samples_per_pixel(app, audio);
        const int64_t delta_samples =
            static_cast<int64_t>(std::llround(delta_px * spp));
        move_playhead_to(app.playhead_sample + delta_samples);
    };

    // Apply a zoom change. The numeric target is derived inside; this helper
    // handles the playhead-centered viewport recompute so zoom_in/zoom_out
    // share exactly the same logic.
    auto apply_zoom_change = [&](int new_zoom_level) {
        if (audio.total_frames() <= 0) return;
        if (new_zoom_level == app.zoom_level) return;

        const double old_spp = current_samples_per_pixel(app, audio);
        const double old_px  = (old_spp > 0.0)
            ? static_cast<double>(app.playhead_sample -
                                  app.viewport_start_sample) / old_spp
            : 0.0;

        app.zoom_level = new_zoom_level;

        if (app.zoom_level == kFitFileLevel) {
            app.viewport_start_sample = 0;
        } else {
            const double new_spp = current_samples_per_pixel(app, audio);
            app.viewport_start_sample =
                viewport_start_for_pixel(app.playhead_sample, old_px, new_spp);
            clamp_viewport_start(app, audio);
        }

        invalidate_waveform_area();
        invalidate_timestamp_area();
    };

    auto zoom_in = [&]() {
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        if (max_num < 0) return; // no numeric level valid; only fit-file
        if (app.zoom_level == kFitFileLevel) {
            apply_zoom_change(max_num);
        } else if (app.zoom_level > 0) {
            apply_zoom_change(app.zoom_level - 1);
        }
        // else at level 0 already — no-op.
    };

    auto zoom_out = [&]() {
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        if (app.zoom_level == kFitFileLevel) return; // already fully out
        if (max_num < 0 || app.zoom_level >= max_num) {
            apply_zoom_change(kFitFileLevel);
        } else {
            apply_zoom_change(app.zoom_level + 1);
        }
    };

    auto scroll_viewport = [&](int64_t delta_samples) {
        if (audio.total_frames() <= 0) return;
        const int64_t old_vp = app.viewport_start_sample;
        app.viewport_start_sample += delta_samples;
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) {
            invalidate_waveform_area();
            // Timestamp shows playhead time, which didn't change — no need
            // to invalidate it. Kept simple for consistency anyway.
        }
    };

    auto center_viewport_on_playhead = [&]() {
        if (audio.total_frames() <= 0) return;
        const int64_t visible = samples_visible(app, audio);
        const int64_t old_vp = app.viewport_start_sample;
        app.viewport_start_sample = app.playhead_sample - visible / 2;
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) {
            invalidate_waveform_area();
        }
    };

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

            const auto trim = compute_trim_samples(
                app.markers.markers(), app.transients.markers(),
                sr, audio.total_frames());
            const int64_t trim_begin = trim.first;
            const int64_t trim_end   = trim.second;

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
                                        kWaveformColor, kWaveformDimColor);
                    } else if (rc >= 2) {
                        const int ch_h = (cache_area.h - kChannelGapPx) / 2;
                        const GuiRect ch0{0, 0, cache_area.w, ch_h};
                        const GuiRect ch1{0, ch_h + kChannelGapPx,
                                          cache_area.w, ch_h};
                        render_waveform(ccr, ch0, audio, 0,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveformColor, kWaveformDimColor);
                        render_waveform(ccr, ch1, audio, 1,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveformColor, kWaveformDimColor);
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
            if (rects_intersect(exposed, area)) {
                const auto m0 = clock::now();
                if (app.active_mode == 'T') {
                    render_transient_markers(
                        cr, area, app.transients.markers(),
                        vp_start, vp_end, sr,
                        kTransientColor, kTransientDimColor,
                        kTransientSelectedColor,
                        app.selected_markers,
                        app.last_selected_marker);
                } else {
                    render_markers(cr, area, app.markers.markers(),
                                   vp_start, vp_end, sr,
                                   kMarkerColor, kMarkerDimColor,
                                   kSelectedColor,
                                   app.selected_markers,
                                   app.last_selected_marker);
                }
                const auto m1 = clock::now();
                t_markers_ms =
                    std::chrono::duration<double, std::milli>(m1 - m0).count();
            }

            // Playhead on top of the waveform, within the waveform area.
            // The playhead's triangle indicator lives in the top strip so
            // we must also render when only the top strip is exposed,
            // otherwise a flag-strip-only repaint would erase the triangle.
            const double px_x = playhead_pixel_x(app, audio);
            if (rects_intersect(exposed, area) ||
                rects_intersect(exposed, top_strip)) {
                const auto p0 = clock::now();
                const GuiColor ph_color = (app.active_mode == 'T')
                    ? kTransientPlayheadColor : kPlayheadColor;
                render_playhead(cr, area, px_x, ph_color);
                const auto p1 = clock::now();
                t_playhead_ms =
                    std::chrono::duration<double, std::milli>(p1 - p0).count();
            }

            // Flag annotations in the top strip.
            if (rects_intersect(exposed, top_strip)) {
                const auto f0 = clock::now();
                if (app.active_mode == 'T') {
                    render_transient_flags(
                        cr, top_strip, app.transients.markers(),
                        vp_start, vp_end, sr,
                        kTransientColor, kTransientDimColor,
                        kTransientSelectedColor, kTransientHighlightColor,
                        kFlagFontSize,
                        app.selected_markers,
                        app.last_selected_marker);
                } else {
                    FlagEditorOverlay overlay;
                    if (gui_text_editor::is_active(app.top_flag_editor)) {
                        overlay.marker_index   = app.top_flag_editor.target;
                        overlay.pending        = app.top_flag_editor.pending;
                        overlay.cursor_pos     = app.top_flag_editor.cursor_pos;
                        overlay.is_red         = app.top_flag_editor.red;
                        overlay.cursor_visible =
                            gui_text_editor::cursor_visible_now(
                                app.top_flag_editor);
                    }
                    render_flags(cr, top_strip, app.markers.markers(),
                                 vp_start, vp_end, sr,
                                 kMarkerColor, kMarkerDimColor,
                                 kSelectedColor, kFlagHighlightColor,
                                 kFlagFontSize,
                                 app.selected_markers,
                                 app.last_selected_marker,
                                 overlay);
                }
                const auto f1 = clock::now();
                t_flags_ms =
                    std::chrono::duration<double, std::milli>(f1 - f0).count();
            }

            // Timestamp in the bottom status strip.
            const GuiRect ts = timestamp_invalidate_rect(app.height);
            if (rects_intersect(exposed, ts)) {
                const double seconds = (sr > 0)
                    ? static_cast<double>(app.playhead_sample) /
                      static_cast<double>(sr)
                    : 0.0;
                const int baseline_y = app.height - kTimestampBaselineFromBottom;
                {
                    const auto s0 = clock::now();
                    render_timestamp(cr, kTimestampPadX, baseline_y,
                                     seconds, kTimestampColor);
                    const auto s1 = clock::now();
                    t_ts_ms =
                        std::chrono::duration<double, std::milli>(s1 - s0).count();
                }

                // A/B tab letter between timestamp and dirty indicator.
                // Same font/size/color as the timestamp; no background.
                const double tw = measure_timestamp_width(cr, seconds);
                double right_after_letter =
                    static_cast<double>(kTimestampPadX) + tw;
                {
                    const double letter_x =
                        static_cast<double>(kTimestampPadX) + tw +
                        kTabLetterGapPx;
                    const char letter_buf[2] = { app.active_tab, '\0' };
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kTimestampColor.r,
                                         kTimestampColor.g, kTimestampColor.b);
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
                    const double cx = right_after_letter +
                                      kTabLetterGapPx + 3.0;
                    const double cy =
                        static_cast<double>(baseline_y) - 5.0;
                    render_dirty_indicator(cr, cx, cy, kDirtyColor);
                    const auto d1 = clock::now();
                    t_dirty_ms =
                        std::chrono::duration<double, std::milli>(d1 - d0).count();
                }
            }
        }

        // Unsaved-work dialog sits on top of everything else. The modal
        // paint happens after all normal rendering so the dim overlay and
        // dialog panel visually cover the waveform / flags / timestamp.
        if (app.dialog.active) {
            const DialogLayout L = compute_dialog_layout(
                cr, app.width, app.height,
                app.dialog.prompt_text.c_str(),
                app.dialog.button_labels);
            render_dialog(cr, L, app.dialog.prompt_text.c_str(),
                          app.dialog.button_labels,
                          app.dialog.focused_button,
                          app.width, app.height,
                          kDialogTextColor, kDialogPanelColor,
                          kDialogButtonColor, kDialogFocusColor);
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
            }
        }
        clamp_viewport_start(app, audio);
    });

    // Invalidate a narrow column around a marker's on-screen x (same width
    // as the playhead invalidation). No-op if the marker is off-screen.
    auto invalidate_marker_column = [&](int marker_idx) {
        if (marker_idx < 0) return;
        if (audio.total_frames() <= 0) return;
        const double spp = current_samples_per_pixel(app, audio);
        if (spp <= 0.0) return;
        const GuiRect area = waveform_area(app);
        const int sr = audio.sample_rate();
        double ms;
        if (app.active_mode == 'T') {
            const auto& tv = app.transients.markers();
            if (marker_idx >= static_cast<int>(tv.size())) return;
            ms = static_cast<double>(tv[marker_idx].effective_frame());
        } else {
            const auto& mv = app.markers.markers();
            if (marker_idx >= static_cast<int>(mv.size())) return;
            ms = mv[marker_idx].time_seconds * static_cast<double>(sr);
        }
        const double vp = static_cast<double>(app.viewport_start_sample);
        const int64_t visible = samples_visible(app, audio);
        if (ms < vp) return;
        if (ms >= vp + static_cast<double>(visible)) return;
        const double px = area.x + (ms - vp) / spp;
        const GuiRect r = playhead_invalidate_rect(area, px);
        if (r.w > 0 && r.h > 0) {
            gui.invalidate_region(r.x, r.y, r.w, r.h);
        }
    };

    auto invalidate_top_strip = [&]() {
        const GuiRect ts = top_strip_area(app);
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
    };

    auto invalidate_markers_columns = [&](const std::set<int>& idxs) {
        for (int i : idxs) invalidate_marker_column(i);
    };

    // Restore the `last_selected ∈ selected ∪ {-1}` invariant. Callers
    // invoke this after any mutation that might have invalidated the
    // previous focus (removal from the set, marker deletion).
    auto repair_last_selected = [&]() {
        if (app.last_selected_marker < 0) return;
        if (app.selected_markers.count(app.last_selected_marker)) return;
        if (app.selected_markers.empty()) {
            app.last_selected_marker = -1;
        } else {
            // Pick the largest remaining index (spec: "the largest remaining
            // index in selected_markers, or -1 if empty").
            app.last_selected_marker = *app.selected_markers.rbegin();
        }
    };

    // Replace the selection with a single marker. Invalidates the affected
    // columns and the flag strip. No playhead movement here.
    auto set_single_selection = [&](int idx) {
        const std::set<int> old = app.selected_markers;
        app.selected_markers.clear();
        if (idx >= 0) app.selected_markers.insert(idx);
        app.last_selected_marker = (idx >= 0) ? idx : -1;
        invalidate_markers_columns(old);
        invalidate_markers_columns(app.selected_markers);
        invalidate_top_strip();
    };

    auto clear_selection = [&]() {
        if (app.selected_markers.empty() && app.last_selected_marker == -1) return;
        const std::set<int> old = app.selected_markers;
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        invalidate_markers_columns(old);
        invalidate_top_strip();
    };

    // Shift+click toggle. Returns whether the marker ended up in the set.
    auto toggle_selection_membership = [&](int idx) -> bool {
        if (idx < 0) return false;
        bool added;
        auto it = app.selected_markers.find(idx);
        if (it == app.selected_markers.end()) {
            app.selected_markers.insert(idx);
            app.last_selected_marker = idx;
            added = true;
        } else {
            app.selected_markers.erase(it);
            if (app.last_selected_marker == idx) repair_last_selected();
            added = false;
        }
        invalidate_marker_column(idx);
        invalidate_top_strip();
        return added;
    };

    auto invalidate_dirty_and_timestamp = [&]() {
        const GuiRect t = timestamp_invalidate_rect(app.height);
        gui.invalidate_region(t.x, t.y, t.w, t.h);
    };

    // -- Undo/redo helpers --------------------------------------------------

    // Mirror history into per-mode dirty flags + the OR'd app.dirty signal.
    // Walks the entries between the saved baseline and the current cursor
    // (using saved_distance) and tags each by its op_mode. When saved_valid
    // is false (history mutated past the saved reference's reach), both
    // flags become true — we can't disambiguate which list diverged.
    auto recompute_dirty = [&]() {
        const auto& h = app.history;
        if (!h.saved_valid) {
            app.warp_dirty      = true;
            app.transient_dirty = true;
        } else if (h.saved_distance == 0) {
            app.warp_dirty      = false;
            app.transient_dirty = false;
        } else if (h.saved_distance < 0) {
            // Saved is `n` undos behind the current cursor. The last n
            // entries of undo_stack moved us from saved baseline to current.
            app.warp_dirty      = false;
            app.transient_dirty = false;
            const int n  = -h.saved_distance;
            const int us = static_cast<int>(h.undo_stack.size());
            for (int i = std::max(0, us - n); i < us; ++i) {
                if (h.undo_stack[i].op_mode == 'T') app.transient_dirty = true;
                else                                app.warp_dirty      = true;
            }
        } else {
            // Saved is `n` redos ahead. The top n entries of redo_stack
            // would, if redone, take us back to the saved state.
            app.warp_dirty      = false;
            app.transient_dirty = false;
            const int n  = h.saved_distance;
            const int rs = static_cast<int>(h.redo_stack.size());
            for (int i = std::max(0, rs - n); i < rs; ++i) {
                if (h.redo_stack[i].op_mode == 'T') app.transient_dirty = true;
                else                                app.warp_dirty      = true;
            }
        }
        app.dirty = app.warp_dirty || app.transient_dirty;
    };

    // Look up a key in app.settings_passthrough, returning its value or
    // the default if the key isn't present. Used by `t`-mode entry to
    // gate on engine= and transients_enabled= without a typed parser
    // (the chunk doesn't otherwise interpret these keys).
    auto settings_get = [&](const std::string& key,
                            const std::string& dflt) -> std::string {
        for (const auto& kv : app.settings_passthrough) {
            if (kv.first == key) return kv.second;
        }
        return dflt;
    };

    // Drop out-of-range indices from selected_markers after a restore.
    // Spec: if the sanitized set no longer contains last_selected_marker,
    // set last_selected_marker = -1; otherwise leave it alone. `n` is the
    // count of the active list (warp or transient).
    auto sanitize_selection_after_restore = [&](int n) {
        std::set<int> cleaned;
        for (int idx : app.selected_markers) {
            if (idx >= 0 && idx < n) cleaned.insert(idx);
        }
        app.selected_markers = std::move(cleaned);
        if (!app.selected_markers.count(app.last_selected_marker)) {
            app.last_selected_marker = -1;
        }
    };

    // Push the pre-mutation entry onto the undo stack. Every warp-mode
    // mutation site calls this at the point the mutation is confirmed to
    // land. Caller passes the warp pre-state + op kind + selection hint;
    // the transient pre-state is captured here from the live AppState
    // (warp ops don't touch transients, so post-mutation == pre-mutation
    // for that list).
    auto push_undo = [&](std::vector<GuiMarker> pre_state, OpKind op_kind,
                         std::set<int> hint_sel, int hint_last) {
        UndoEntry e;
        e.snapshot           = std::move(pre_state);
        e.transient_snapshot = app.transients.markers();
        e.op_kind            = op_kind;
        e.op_mode            = 'W';
        e.hint_selected      = std::move(hint_sel);
        e.hint_last_selected = hint_last;
        app.history.push(std::move(e));
    };

    // Transient counterpart. Symmetric: caller passes the transient pre-
    // state; warp pre-state is captured from live AppState (transient ops
    // don't touch warp markers).
    auto push_undo_transient = [&](std::vector<GuiTransient> pre_state,
                                   OpKind op_kind,
                                   std::set<int> hint_sel, int hint_last) {
        UndoEntry e;
        e.snapshot           = app.markers.markers();
        e.transient_snapshot = std::move(pre_state);
        e.op_kind            = op_kind;
        e.op_mode            = 'T';
        e.hint_selected      = std::move(hint_sel);
        e.hint_last_selected = hint_last;
        app.history.push(std::move(e));
    };

    // Cross-file undo: both pre-states explicit. Used by the b/e toggle
    // helpers, which may mutate either or both lists in a single op.
    auto push_undo_both = [&](std::vector<GuiMarker> warp_pre,
                              std::vector<GuiTransient> trans_pre,
                              char op_mode, OpKind op_kind,
                              std::set<int> hint_sel, int hint_last) {
        UndoEntry e;
        e.snapshot           = std::move(warp_pre);
        e.transient_snapshot = std::move(trans_pre);
        e.op_kind            = op_kind;
        e.op_mode            = op_mode;
        e.hint_selected      = std::move(hint_sel);
        e.hint_last_selected = hint_last;
        app.history.push(std::move(e));
    };

    // -- V.A1 top-flag editor helpers ---------------------------------------

    // Build the locked-prefix string for `m` — exactly what the canonical
    // serializer would write before the pipe character, with the pipe
    // included. The editor renders this prefix outside the editable rect
    // (left-anchored at the marker column); the pipe is part of the
    // prefix but visually anchors to the marker line.
    auto build_locked_prefix = [&](const GuiMarker& m) -> std::string {
        std::string out;
        if (m.is_begin_time)    out = "b=";
        else if (m.is_end_time) out = "e=";
        if (m.disabled) out += '#';
        // MM:SS.SSS
        double sec = m.time_seconds;
        if (sec < 0) sec = 0;
        long total_ms = static_cast<long>(std::llround(sec * 1000.0));
        const long mn = total_ms / 60000;
        total_ms     -= mn * 60000;
        const long s  = total_ms / 1000;
        const long ms = total_ms - s * 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02ld:%02ld.%03ld", mn, s, ms);
        out += buf;
        out += '|';
        return out;
    };

    auto exit_top_flag_edit_no_commit = [&]() {
        if (!gui_text_editor::is_active(app.top_flag_editor)) return;
        gui_text_editor::deactivate(app.top_flag_editor);
        invalidate_top_strip();
    };

    auto enter_top_flag_edit = [&](int idx) {
        if (idx < 0) return;
        const auto& mv = app.markers.markers();
        if (idx >= static_cast<int>(mv.size())) return;
        // Discard any prior edit silently before switching targets.
        if (gui_text_editor::is_active(app.top_flag_editor) &&
            app.top_flag_editor.target != idx) {
            gui_text_editor::deactivate(app.top_flag_editor);
        }
        gui_text_editor::enter(
            app.top_flag_editor, idx,
            build_locked_prefix(mv[idx]),
            flag_text_for_marker(mv, idx));
        invalidate_top_strip();
    };

    // Validate `pending` as a single canonical line and, on success, write
    // the parsed marker's fields back onto markers_[idx]. Cascade-renames
    // label_def changes onto every other marker that referenced the old
    // name. Pushes one undo entry covering all touched markers.
    //
    // On failure: sets `red`, leaves pending/cursor intact, leaves the
    // editor active.
    auto commit_top_flag_edit = [&]() {
        if (!gui_text_editor::is_active(app.top_flag_editor)) return;
        const int idx = app.top_flag_editor.target;
        const auto& mv_const = app.markers.markers();
        if (idx < 0 || idx >= static_cast<int>(mv_const.size())) {
            // Editor target became invalid (e.g. file reload). Drop edit.
            exit_top_flag_edit_no_commit();
            return;
        }

        const std::string candidate =
            app.top_flag_editor.locked_prefix + app.top_flag_editor.pending;

        GuiMarker parsed;
        std::string err;
        bool ok = gui_markers_internal::parse_single_canonical_line(
            candidate, parsed, &err);

        // Cross-marker checks (edit target excluded).
        if (ok && !parsed.label_ref.empty()) {
            bool found = false;
            for (int i = 0; i < static_cast<int>(mv_const.size()); ++i) {
                if (i == idx) continue;
                if (mv_const[i].label_def == parsed.label_ref) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ok = false;
                err = "reference to undefined label: " + parsed.label_ref;
            }
        }
        if (ok && !parsed.label_def.empty()) {
            for (int i = 0; i < static_cast<int>(mv_const.size()); ++i) {
                if (i == idx) continue;
                if (mv_const[i].label_def == parsed.label_def) {
                    ok = false;
                    err = "duplicate label definition: " + parsed.label_def;
                    break;
                }
            }
        }

        if (!ok) {
            app.top_flag_editor.red = true;
            invalidate_top_strip();
            std::fprintf(stderr,
                "warptempo_gui: edit rejected: %s\n", err.c_str());
            return;
        }

        // Capture pre-state for undo BEFORE mutating.
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;

        const std::string old_def = mv_const[idx].label_def;
        const std::string new_def = parsed.label_def;

        GuiMarker* m = app.markers.marker_mut(idx);
        if (!m) {
            exit_top_flag_edit_no_commit();
            return;
        }

        // Time stays locked; preserve it (parse already produced the
        // same value via the locked prefix, but be explicit).
        const double preserved_time = m->time_seconds;

        // Inherit-cache preservation: typing `idem` leaves the cached
        // tempo_base / tempo_scale alone so a future toggle can restore.
        // Typing an explicit tempo overwrites the cache (the user
        // committed to a new owned value). label_def is independent of
        // tempo source — `idem:LABEL` carries a def at this position
        // while inheriting the tempo from a prior owning marker.
        if (parsed.tempo_inherits) {
            m->tempo_inherits = true;
            // Keep existing tempo_base / tempo_scale as the cache.
            m->label_def      = parsed.label_def;
            m->label_ref.clear();
        } else {
            m->tempo_inherits = false;
            m->tempo_base     = parsed.tempo_base;
            m->tempo_scale    = parsed.tempo_scale;
            m->label_def      = parsed.label_def;
            m->label_ref      = parsed.label_ref;
        }
        m->time_seconds = preserved_time;
        // is_begin_time / is_end_time / disabled live in locked prefix —
        // parse_single_canonical_line populated them; reapply.
        m->is_begin_time = parsed.is_begin_time;
        m->is_end_time   = parsed.is_end_time;
        m->disabled      = parsed.disabled;

        // Cascade rename: if label_def changed and old_def was non-empty,
        // every other marker that referenced old_def gets its ref updated
        // to the new name (or cleared if new_def is empty — the user
        // converted a def to non-def).
        int n_refs_renamed = 0;
        if (!old_def.empty() && old_def != new_def) {
            auto& mv_mut = app.markers.markers_mut();
            for (int i = 0; i < static_cast<int>(mv_mut.size()); ++i) {
                if (i == idx) continue;
                if (mv_mut[i].label_ref == old_def) {
                    mv_mut[i].label_ref = new_def;
                    ++n_refs_renamed;
                }
            }
            std::fprintf(stderr,
                "[warptempo_gui] renamed label_def '%s' -> '%s'; "
                "updated %d refs\n",
                old_def.c_str(), new_def.c_str(), n_refs_renamed);
        }

        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        recompute_dirty();

        gui_text_editor::deactivate(app.top_flag_editor);

        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Cross-file flag scan. `want_begin` selects the b= scan vs the e=
    // scan. The (excl_trans, excl_idx) pair excludes one marker from the
    // search — used by toggle to skip the marker the user just toggled.
    // Pass excl_idx == -1 for no exclusion. Warp list is scanned first;
    // on a duplicate (parser-protected, only via hand-edit), the warp-
    // side hit wins to match compute_trim_samples.
    struct FlagLoc {
        bool    valid     = false;
        bool    transient = false;
        int     idx       = -1;
        int64_t frame     = 0;
    };
    auto find_flag = [&](bool want_begin, bool excl_trans, int excl_idx)
        -> FlagLoc {
        FlagLoc f;
        const int sr = audio.sample_rate();
        const auto& mv = app.markers.markers();
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
        const auto& tv = app.transients.markers();
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

    // Three callers converge on the same rule: after a marker move, the
    // playhead follows last_selected to its new position and, if that
    // position is now outside the viewport, the viewport recenters (same
    // math as the `c` key). Callers: live Ctrl+Left/Right/wheel nudges,
    // and apply_post_restore_rules on undo/redo of Move ops. Invalidation
    // is left to the caller — all three sites already invalidate the
    // waveform strip, which covers the playhead column.
    auto sync_playhead_to_last_selected = [&]() {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const int last = app.last_selected_marker;
        if (last < 0) return;

        int64_t target_sample = 0;
        if (app.active_mode == 'T') {
            const auto& tv = app.transients.markers();
            if (last >= static_cast<int>(tv.size())) return;
            target_sample = tv[last].effective_frame();
        } else {
            const auto& mv = app.markers.markers();
            if (last >= static_cast<int>(mv.size())) return;
            target_sample = static_cast<int64_t>(std::llround(
                mv[last].time_seconds * static_cast<double>(sr)));
        }
        app.playhead_sample = target_sample;

        const int64_t visible = samples_visible(app, audio);
        const bool offscreen =
            target_sample <  app.viewport_start_sample ||
            target_sample >= app.viewport_start_sample + visible;
        if (offscreen) {
            app.viewport_start_sample = target_sample - visible / 2;
            clamp_viewport_start(app, audio);
        }
    };

    // Apply post-restore selection and playhead rules per the chunk L
    // patch 1 spec. Called after the marker vector has been restored to
    // `entry.snapshot` and before sanitize_selection_after_restore.
    // `before` is the marker vector that was current pre-restoration.
    //
    // Net direction is derived from vector-size change and op_kind:
    //   - size grew   → "creating" — select re-created set, jump playhead.
    //   - size shrank → "destroying" — clear selection, leave playhead.
    //   - size same, Move → "moving" — select moved set, jump playhead.
    //   - otherwise → "other" — selection and playhead untouched.
    //
    // Viewport recenters on the new playhead only if it would be offscreen
    // after the jump; the `c`-key centering math is reused.
    auto apply_post_restore_rules_warp = [&](const UndoEntry& entry,
                                             const std::vector<GuiMarker>& before) {
        const auto& after = app.markers.markers();
        constexpr double kEps = 1e-9;

        std::set<int> target_set;
        bool want_playhead_jump = false;

        if (after.size() > before.size()) {
            std::vector<double> before_times;
            before_times.reserve(before.size());
            for (const auto& m : before) before_times.push_back(m.time_seconds);
            std::sort(before_times.begin(), before_times.end());
            for (size_t i = 0; i < after.size(); ++i) {
                const double t = after[i].time_seconds;
                auto it = std::lower_bound(before_times.begin(),
                                           before_times.end(), t - kEps);
                const bool matched = (it != before_times.end() &&
                                      std::abs(*it - t) < kEps);
                if (!matched) target_set.insert(static_cast<int>(i));
            }
            want_playhead_jump = !target_set.empty();
        } else if (after.size() < before.size()) {
            app.selected_markers.clear();
            app.last_selected_marker = -1;
            return;
        } else if (entry.op_kind == OpKind::Move) {
            for (size_t i = 0; i < after.size(); ++i) {
                if (std::abs(after[i].time_seconds -
                             before[i].time_seconds) > kEps) {
                    target_set.insert(static_cast<int>(i));
                }
            }
            want_playhead_jump = !target_set.empty();
        } else {
            return;
        }

        if (target_set.empty()) return;

        app.selected_markers = target_set;
        if (target_set.count(entry.hint_last_selected)) {
            app.last_selected_marker = entry.hint_last_selected;
        } else {
            app.last_selected_marker = *target_set.rbegin();
        }

        if (!want_playhead_jump) return;
        sync_playhead_to_last_selected();
    };

    // Transient counterpart to apply_post_restore_rules_warp. Same shape:
    // size grew → select created (jump playhead), size shrank → clear
    // selection, equal size + Move → select moved (jump). `before` is the
    // transient vector pre-restoration; `after` is taken live from
    // app.transients.
    auto apply_post_restore_rules_transient = [&](const UndoEntry& entry,
                                                  const std::vector<GuiTransient>& before) {
        const auto& after = app.transients.markers();

        std::set<int> target_set;
        bool want_playhead_jump = false;

        if (after.size() > before.size()) {
            std::set<int64_t> before_frames;
            for (const auto& m : before) before_frames.insert(m.effective_frame());
            for (size_t i = 0; i < after.size(); ++i) {
                if (!before_frames.count(after[i].effective_frame())) {
                    target_set.insert(static_cast<int>(i));
                }
            }
            want_playhead_jump = !target_set.empty();
        } else if (after.size() < before.size()) {
            app.selected_markers.clear();
            app.last_selected_marker = -1;
            return;
        } else if (entry.op_kind == OpKind::Move) {
            for (size_t i = 0; i < after.size(); ++i) {
                if (after[i].effective_frame() != before[i].effective_frame()) {
                    target_set.insert(static_cast<int>(i));
                }
            }
            want_playhead_jump = !target_set.empty();
        } else {
            return;
        }

        if (target_set.empty()) return;

        app.selected_markers = target_set;
        if (target_set.count(entry.hint_last_selected)) {
            app.last_selected_marker = entry.hint_last_selected;
        } else {
            app.last_selected_marker = *target_set.rbegin();
        }

        if (!want_playhead_jump) return;
        sync_playhead_to_last_selected();
    };

    // Gesture-stop: called at the top of any handler that will move the
    // visible playhead (keys, button press, Ctrl+wheel, undo/redo, tab
    // switch). Stops the audio thread and keeps the LSP in sync with the
    // visible playhead so the next Space-to-play captures the right
    // launch position. Does NOT return-to-launch — the gesture is about
    // to commit a new playhead position.
    auto stop_playback_if_playing = [&]() {
        if (!playback.is_playing() && !app.is_playing) return;
        playback.stop();
        app.is_playing        = false;
        app.last_space_sample = app.playhead_sample;
    };

    // Ctrl+Z. Silent no-op on empty stack. Restores the top undo entry;
    // current state (tagged with the popped entry's op kind + hint + mode)
    // is pushed onto redo so the op's direction is reversible. Both lists
    // are restored — entries always carry both snapshots so the inverse
    // is symmetric. If the entry's op_mode differs from the active mode,
    // active_mode flips to it as a side effect (visual feedback for what
    // the user just undid).
    auto do_undo = [&]() {
        if (app.history.undo_stack.empty()) return;
        stop_playback_if_playing();
        UndoEntry entry = std::move(app.history.undo_stack.back());
        app.history.undo_stack.pop_back();

        UndoEntry redo_entry;
        redo_entry.snapshot           = app.markers.markers();
        redo_entry.transient_snapshot = app.transients.markers();
        redo_entry.op_kind            = entry.op_kind;
        redo_entry.op_mode            = entry.op_mode;
        redo_entry.hint_selected      = entry.hint_selected;
        redo_entry.hint_last_selected = entry.hint_last_selected;
        std::vector<GuiMarker>    before_w = redo_entry.snapshot;
        std::vector<GuiTransient> before_t = redo_entry.transient_snapshot;

        app.history.redo_stack.push_back(std::move(redo_entry));
        if (app.history.redo_stack.size() > UndoHistory::kCap) {
            app.history.redo_stack.erase(app.history.redo_stack.begin());
        }
        if (app.history.saved_valid) app.history.saved_distance += 1;

        app.markers.markers_mut()    = std::move(entry.snapshot);
        app.transients.markers_mut() = std::move(entry.transient_snapshot);

        // Switch active mode to match the op being undone before applying
        // post-restore rules — selection state is mode-bound, so the rules
        // and the sanitize step must run against the correct list.
        if (entry.op_mode != app.active_mode) {
            // Stash the current selection into the leaving mode's slot,
            // then restore the destination mode's slot.
            Tab& curtab = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
            if (app.active_mode == 'T') {
                curtab.transient_selected      = app.selected_markers;
                curtab.transient_last_selected = app.last_selected_marker;
                app.selected_markers           = curtab.warp_selected;
                app.last_selected_marker       = curtab.warp_last_selected;
            } else {
                curtab.warp_selected           = app.selected_markers;
                curtab.warp_last_selected      = app.last_selected_marker;
                app.selected_markers           = curtab.transient_selected;
                app.last_selected_marker       = curtab.transient_last_selected;
            }
            app.active_mode = entry.op_mode;
        }

        if (entry.op_mode == 'T') {
            apply_post_restore_rules_transient(entry, before_t);
            sanitize_selection_after_restore(
                static_cast<int>(app.transients.markers().size()));
        } else {
            apply_post_restore_rules_warp(entry, before_w);
            sanitize_selection_after_restore(
                static_cast<int>(app.markers.markers().size()));
        }
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Ctrl+Shift+Z. Mirror of do_undo. Silent no-op on empty redo stack.
    auto do_redo = [&]() {
        if (app.history.redo_stack.empty()) return;
        stop_playback_if_playing();
        UndoEntry entry = std::move(app.history.redo_stack.back());
        app.history.redo_stack.pop_back();

        UndoEntry undo_entry;
        undo_entry.snapshot           = app.markers.markers();
        undo_entry.transient_snapshot = app.transients.markers();
        undo_entry.op_kind            = entry.op_kind;
        undo_entry.op_mode            = entry.op_mode;
        undo_entry.hint_selected      = entry.hint_selected;
        undo_entry.hint_last_selected = entry.hint_last_selected;
        std::vector<GuiMarker>    before_w = undo_entry.snapshot;
        std::vector<GuiTransient> before_t = undo_entry.transient_snapshot;

        app.history.undo_stack.push_back(std::move(undo_entry));
        if (app.history.undo_stack.size() > UndoHistory::kCap) {
            app.history.undo_stack.erase(app.history.undo_stack.begin());
        }
        if (app.history.saved_valid) app.history.saved_distance -= 1;

        app.markers.markers_mut()    = std::move(entry.snapshot);
        app.transients.markers_mut() = std::move(entry.transient_snapshot);

        if (entry.op_mode != app.active_mode) {
            Tab& curtab = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
            if (app.active_mode == 'T') {
                curtab.transient_selected      = app.selected_markers;
                curtab.transient_last_selected = app.last_selected_marker;
                app.selected_markers           = curtab.warp_selected;
                app.last_selected_marker       = curtab.warp_last_selected;
            } else {
                curtab.warp_selected           = app.selected_markers;
                curtab.warp_last_selected      = app.last_selected_marker;
                app.selected_markers           = curtab.transient_selected;
                app.last_selected_marker       = curtab.transient_last_selected;
            }
            app.active_mode = entry.op_mode;
        }

        if (entry.op_mode == 'T') {
            apply_post_restore_rules_transient(entry, before_t);
            sanitize_selection_after_restore(
                static_cast<int>(app.transients.markers().size()));
        } else {
            apply_post_restore_rules_warp(entry, before_w);
            sanitize_selection_after_restore(
                static_cast<int>(app.markers.markers().size()));
        }
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Tab / Shift+Tab: cycle through markers. Rules per spec:
    //   0 or 1 selected: cycle through all markers.
    //     1 selected acts as the anchor; 0 selected falls back to playhead.
    //   2+ selected: cycle within the selection set only, anchored on
    //     last_selected_marker. Wraps at the set's extremes.
    auto cycle_selection = [&](bool forward) {
        const int sr = audio.sample_rate();
        const bool transient = (app.active_mode == 'T');
        const int n = transient
            ? static_cast<int>(app.transients.markers().size())
            : static_cast<int>(app.markers.markers().size());
        if (n == 0) return;

        // Helper to read frame-of-index in source samples regardless of mode.
        auto frame_of = [&](int i) -> int64_t {
            if (transient) return app.transients.markers()[i].effective_frame();
            return static_cast<int64_t>(std::llround(
                app.markers.markers()[i].time_seconds *
                static_cast<double>(sr)));
        };

        const int sel_size = static_cast<int>(app.selected_markers.size());
        int new_sel = -1;

        if (sel_size >= 2) {
            const int anchor = app.last_selected_marker;
            if (forward) {
                auto it = app.selected_markers.upper_bound(anchor);
                if (it == app.selected_markers.end()) {
                    it = app.selected_markers.begin();
                }
                new_sel = *it;
            } else {
                auto it = app.selected_markers.lower_bound(anchor);
                if (it == app.selected_markers.begin()) {
                    it = app.selected_markers.end();
                }
                --it;
                new_sel = *it;
            }
        } else if (sel_size == 1) {
            const int cur = *app.selected_markers.begin();
            const int64_t cur_f = frame_of(cur);
            if (forward) {
                for (int i = cur + 1; i < n; ++i) {
                    if (frame_of(i) > cur_f) { new_sel = i; break; }
                }
            } else {
                for (int i = cur - 1; i >= 0; --i) {
                    if (frame_of(i) < cur_f) { new_sel = i; break; }
                }
            }
        } else {
            const int64_t ph_f = app.playhead_sample;
            if (forward) {
                for (int i = 0; i < n; ++i) {
                    if (frame_of(i) >= ph_f) { new_sel = i; break; }
                }
            } else {
                for (int i = n - 1; i >= 0; --i) {
                    if (frame_of(i) <= ph_f) { new_sel = i; break; }
                }
            }
        }

        if (new_sel < 0) return;

        if (sel_size >= 2) {
            // Within-set cycling: only the focus changes.
            if (new_sel == app.last_selected_marker) return;
            const int old_focus = app.last_selected_marker;
            app.last_selected_marker = new_sel;
            invalidate_marker_column(old_focus);
            invalidate_marker_column(new_sel);
            invalidate_top_strip();
        } else {
            if (app.selected_markers.count(new_sel) &&
                app.last_selected_marker == new_sel) return;
            set_single_selection(new_sel);
        }

        const int64_t sample = frame_of(new_sel);
        const int64_t visible = samples_visible(app, audio);
        if (visible > 0) {
            const int64_t old_vp = app.viewport_start_sample;
            const int64_t vp_end = old_vp + visible;
            if (sample < old_vp) {
                app.viewport_start_sample = sample;
            } else if (sample >= vp_end) {
                const double spp = current_samples_per_pixel(app, audio);
                const int64_t one_px =
                    static_cast<int64_t>(std::llround(spp));
                app.viewport_start_sample =
                    sample - (visible - std::max<int64_t>(one_px, 1));
            }
            clamp_viewport_start(app, audio);
            if (app.viewport_start_sample != old_vp) {
                invalidate_waveform_area();
            }
        }
    };

    auto select_next_marker = [&]() { cycle_selection(true);  };
    auto select_prev_marker = [&]() { cycle_selection(false); };

    // Core drop logic shared by `s`, `Shift+S`, double-click, and
    // Shift+double-click. `time_seconds` is the location (not necessarily the
    // playhead); `inherit` controls whether the new marker inherits its
    // tempo from the nearest earlier owner. Moves the playhead to the new
    // marker to keep drop-then-select behavior consistent.
    auto drop_marker = [&](double time_seconds, bool inherit) {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double one_sample = 1.0 / static_cast<double>(sr);
        const auto& mv = app.markers.markers();
        for (const auto& m : mv) {
            if (std::abs(m.time_seconds - time_seconds) < one_sample) {
                std::fprintf(stderr,
                    "warptempo_gui: marker already exists at %.3fs\n",
                    time_seconds);
                return;
            }
        }
        // Snapshot pre-mutation state for undo. Captured after the dup
        // check so rejected drops don't leave a no-op entry on the stack.
        std::vector<GuiMarker> pre_state = mv;
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        GuiMarker nm;
        nm.time_seconds    = time_seconds;
        nm.tempo_inherits  = inherit;
        // For inherit markers, pre-seed the cached tempo from the nearest
        // earlier owning marker. `insert_marker` returns the index; we
        // compute the source now based on the marker list as it will be
        // after insertion — equivalent to resolving from the insertion
        // point, since the walk-backward stops at the first owner.
        if (inherit) {
            // Find the index we'll be inserted at, then walk backward.
            int insertion_idx = 0;
            for (const auto& m : mv) {
                if (m.time_seconds < time_seconds) insertion_idx++;
                else break;
            }
            nm.tempo_base  = resolve_inherited_tempo(mv, insertion_idx);
            nm.tempo_scale = resolve_inherited_tempo_scale(mv, insertion_idx);
        } else {
            nm.tempo_base = 1.0;
            nm.tempo_scale.clear();
        }
        const int new_idx = app.markers.insert_marker(std::move(nm));
        // Insertion may have shifted indices of existing selections.
        // Rebuild selected_markers to reflect the shift.
        std::set<int> shifted;
        for (int i : app.selected_markers) {
            shifted.insert(i >= new_idx ? i + 1 : i);
        }
        app.selected_markers = std::move(shifted);
        if (app.last_selected_marker >= new_idx) app.last_selected_marker += 1;
        // Newly-dropped marker becomes the sole selection per chunk I.
        app.selected_markers.clear();
        app.selected_markers.insert(new_idx);
        app.last_selected_marker = new_idx;
        push_undo(std::move(pre_state), OpKind::Create,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();

        // Move the playhead to the new marker for consistency with click-
        // to-select behavior. Done last so invalidations in the helper
        // don't double-paint with the ones above.
        const int64_t sample = static_cast<int64_t>(std::llround(
            time_seconds * static_cast<double>(sr)));
        move_playhead_to(sample);
    };

    auto drop_marker_at_playhead = [&]() {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double t = static_cast<double>(app.playhead_sample) /
                         static_cast<double>(sr);
        drop_marker(t, /*inherit=*/false);
    };

    auto drop_inherit_marker_at_playhead = [&]() {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double t = static_cast<double>(app.playhead_sample) /
                         static_cast<double>(sr);
        drop_marker(t, /*inherit=*/true);
    };

    auto delete_selected_marker = [&]() {
        if (app.selected_markers.empty()) return;
        const auto& mv = app.markers.markers();

        // Validate the batch. Reject the whole operation if any member is
        // the time-0 first marker or has a label_def referenced from outside
        // the selection set.
        for (int idx : app.selected_markers) {
            if (idx < 0 || idx >= static_cast<int>(mv.size())) {
                std::fprintf(stderr,
                    "warptempo_gui: delete rejected: stale selection index\n");
                return;
            }
            if (mv[idx].time_seconds == 0.0) {
                std::fprintf(stderr,
                    "warptempo_gui: cannot delete first marker (time 0)\n");
                return;
            }
            if (mv[idx].label_def.empty()) continue;
            std::string refs;
            int ref_count = 0;
            for (size_t i = 0; i < mv.size(); ++i) {
                if (app.selected_markers.count(static_cast<int>(i))) continue;
                if (!mv[i].label_ref.empty() &&
                    mv[i].label_ref == mv[idx].label_def) {
                    char tbuf[32];
                    std::snprintf(tbuf, sizeof(tbuf), "%.3fs",
                                  mv[i].time_seconds);
                    if (!refs.empty()) refs += ", ";
                    refs += tbuf;
                    ++ref_count;
                }
            }
            if (ref_count > 0) {
                std::fprintf(stderr,
                    "warptempo_gui: cannot delete marker: label '%s' is "
                    "referenced at %s\n",
                    mv[idx].label_def.c_str(), refs.c_str());
                return;
            }
        }

        // All validations passed — capture snapshot and selection hint
        // before mutating so the undo can restore the pre-delete selection.
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        // Delete in descending order so earlier indices stay valid.
        for (auto it = app.selected_markers.rbegin();
             it != app.selected_markers.rend(); ++it) {
            app.markers.remove_marker(*it);
        }
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        push_undo(std::move(pre_state), OpKind::Destroy,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Shift+Delete variant. Auto-cascades label_refs of any selected def
    // into the deletion batch, so the user doesn't have to hand-pick each
    // ref before deleting the def. With the cascade, the "label is
    // referenced from outside the selection" check is unnecessary — every
    // ref is now inside the batch by construction.
    auto force_delete_selected_marker = [&]() {
        if (app.selected_markers.empty()) return;
        const auto& mv = app.markers.markers();

        std::set<int> expanded = app.selected_markers;
        for (int idx : app.selected_markers) {
            if (idx < 0 || idx >= static_cast<int>(mv.size())) {
                std::fprintf(stderr,
                    "warptempo_gui: delete rejected: stale selection index\n");
                return;
            }
            if (mv[idx].label_def.empty()) continue;
            for (size_t i = 0; i < mv.size(); ++i) {
                if (!mv[i].label_ref.empty() &&
                    mv[i].label_ref == mv[idx].label_def) {
                    expanded.insert(static_cast<int>(i));
                }
            }
        }

        for (int idx : expanded) {
            if (idx < 0 || idx >= static_cast<int>(mv.size())) {
                std::fprintf(stderr,
                    "warptempo_gui: delete rejected: stale selection index\n");
                return;
            }
            if (mv[idx].time_seconds == 0.0) {
                std::fprintf(stderr,
                    "warptempo_gui: cannot delete first marker (time 0)\n");
                return;
            }
        }

        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        for (auto it = expanded.rbegin(); it != expanded.rend(); ++it) {
            app.markers.remove_marker(*it);
        }
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        push_undo(std::move(pre_state), OpKind::Destroy,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Toggle the tempo source of each selected marker between owned and
    // inherit. Markers that can't inherit (first marker, label refs) are
    // silently skipped per chunk I single-marker rules.
    auto toggle_inherits = [&]() {
        if (app.selected_markers.empty()) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        bool changed = false;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            if (idx == 0) continue;
            if (!m->label_ref.empty()) continue;
            m->tempo_inherits = !m->tempo_inherits;
            changed = true;
        }
        if (!changed) return;
        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Toggle the disabled flag on each selected marker. Per chunk U patch 3
    // the flag is allowed on any marker (cascade still applies only when the
    // toggled marker is a label_def).
    auto toggle_disabled = [&]() {
        if (app.selected_markers.empty()) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        bool changed = false;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            m->disabled = !m->disabled;
            changed = true;
        }
        if (!changed) return;
        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // `b` / `e` are single-marker operations. With 2+ selected they silent
    // no-op; with exactly 1, they use last_selected_marker as the target.
    // Re-press toggles off. Otherwise auto-replaces any existing flag
    // (across BOTH warp and transient lists) and auto-swaps with the
    // opposite flag if the resulting frame ordering would invert the trim
    // region. Equal-frame swap is refused (would collapse trim to zero
    // width).
    auto toggle_begin_time = [&]() {
        if (app.selected_markers.size() != 1) return;
        const int idx = app.last_selected_marker;
        if (idx < 0) return;
        const auto& mv = app.markers.markers();
        if (idx >= static_cast<int>(mv.size())) return;

        const int sr = audio.sample_rate();
        const int64_t m_frame = static_cast<int64_t>(std::llround(
            mv[idx].time_seconds * static_cast<double>(sr)));

        std::vector<GuiMarker>    warp_pre  = mv;
        std::vector<GuiTransient> trans_pre = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;

        if (mv[idx].is_begin_time) {
            app.markers.marker_mut(idx)->is_begin_time = false;
            push_undo_both(std::move(warp_pre), std::move(trans_pre),
                           'W', OpKind::Other,
                           std::move(hint_sel), hint_last);
            recompute_dirty();
            invalidate_waveform_area();
            invalidate_dirty_and_timestamp();
            return;
        }

        // Find the existing e= and OTHER b= across both lists.
        const FlagLoc e_loc   = find_flag(/*want_begin=*/false,
                                          /*excl_trans=*/false, -1);
        const FlagLoc b_other = find_flag(/*want_begin=*/true,
                                          /*excl_trans=*/false, idx);

        const bool needs_swap   = e_loc.valid && (m_frame >= e_loc.frame);
        const bool equal_frames = e_loc.valid && (m_frame == e_loc.frame);
        if (equal_frames) {
            std::fprintf(stderr,
                "warptempo_gui: b refused: would collapse trim region\n");
            return;
        }

        if (b_other.valid) {
            if (b_other.transient) {
                app.transients.marker_mut(b_other.idx)->is_begin_time = false;
            } else {
                app.markers.marker_mut(b_other.idx)->is_begin_time = false;
            }
        }
        if (needs_swap) {
            // The marker that had e= becomes b=; the just-toggled warp
            // marker takes e=.
            if (e_loc.transient) {
                app.transients.marker_mut(e_loc.idx)->is_end_time   = false;
                app.transients.marker_mut(e_loc.idx)->is_begin_time = true;
            } else {
                app.markers.marker_mut(e_loc.idx)->is_end_time   = false;
                app.markers.marker_mut(e_loc.idx)->is_begin_time = true;
            }
            app.markers.marker_mut(idx)->is_end_time = true;
        } else {
            app.markers.marker_mut(idx)->is_begin_time = true;
        }

        push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'W', OpKind::Other, std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    auto toggle_end_time = [&]() {
        if (app.selected_markers.size() != 1) return;
        const int idx = app.last_selected_marker;
        if (idx < 0) return;
        const auto& mv = app.markers.markers();
        if (idx >= static_cast<int>(mv.size())) return;

        const int sr = audio.sample_rate();
        const int64_t m_frame = static_cast<int64_t>(std::llround(
            mv[idx].time_seconds * static_cast<double>(sr)));

        std::vector<GuiMarker>    warp_pre  = mv;
        std::vector<GuiTransient> trans_pre = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;

        if (mv[idx].is_end_time) {
            app.markers.marker_mut(idx)->is_end_time = false;
            push_undo_both(std::move(warp_pre), std::move(trans_pre),
                           'W', OpKind::Other,
                           std::move(hint_sel), hint_last);
            recompute_dirty();
            invalidate_waveform_area();
            invalidate_dirty_and_timestamp();
            return;
        }

        const FlagLoc b_loc   = find_flag(/*want_begin=*/true,
                                          /*excl_trans=*/false, -1);
        const FlagLoc e_other = find_flag(/*want_begin=*/false,
                                          /*excl_trans=*/false, idx);

        const bool needs_swap   = b_loc.valid && (m_frame <= b_loc.frame);
        const bool equal_frames = b_loc.valid && (m_frame == b_loc.frame);
        if (equal_frames) {
            std::fprintf(stderr,
                "warptempo_gui: e refused: would collapse trim region\n");
            return;
        }

        if (e_other.valid) {
            if (e_other.transient) {
                app.transients.marker_mut(e_other.idx)->is_end_time = false;
            } else {
                app.markers.marker_mut(e_other.idx)->is_end_time = false;
            }
        }
        if (needs_swap) {
            if (b_loc.transient) {
                app.transients.marker_mut(b_loc.idx)->is_begin_time = false;
                app.transients.marker_mut(b_loc.idx)->is_end_time   = true;
            } else {
                app.markers.marker_mut(b_loc.idx)->is_begin_time = false;
                app.markers.marker_mut(b_loc.idx)->is_end_time   = true;
            }
            app.markers.marker_mut(idx)->is_begin_time = true;
        } else {
            app.markers.marker_mut(idx)->is_end_time = true;
        }

        push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'W', OpKind::Other, std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Nudge every owned-tempo selected marker by `delta`. Inherit markers
    // and label refs in the set are silently skipped. Clamps to [0.01, 9.99].
    // Only dirties / invalidates if at least one marker's tempo changed.
    auto adjust_tempo = [&](double delta) {
        if (app.selected_markers.empty()) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        bool changed = false;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            if (!m->label_ref.empty()) continue;
            if (m->tempo_inherits) continue;
            double new_tempo = m->tempo_base + delta;
            if (new_tempo < 0.01) new_tempo = 0.01;
            if (new_tempo > 9.99) new_tempo = 9.99;
            if (new_tempo == m->tempo_base) continue;
            m->tempo_base = new_tempo;
            invalidate_marker_column(idx);
            changed = true;
        }
        if (!changed) return;
        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_top_strip();
        invalidate_dirty_and_timestamp();
    };

    // Clear any b= / e= flags so the whole file becomes editable again.
    // No-op if no marker carries either flag.
    auto clear_trim = [&]() {
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        bool changed = false;
        for (auto& m : app.markers.markers_mut()) {
            if (m.is_begin_time || m.is_end_time) {
                m.is_begin_time = false;
                m.is_end_time   = false;
                changed = true;
            }
        }
        if (!changed) return;
        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // -- Transient-mode editing helpers (chunk S.2.2) -----------------------

    // Drop a transient marker at `time_seconds`. Equal-frame collisions
    // are accepted (mid-edit nudges may transit through them); save()
    // dedups. Selection collapses to the freshly-inserted index. If the
    // transient list does not yet carry a frame-0 entry after insertion,
    // a silent companion at frame 0 is inserted alongside (frame-0
    // invariant: phase reset at render start is always correct).
    auto drop_transient_at_position = [&](double time_seconds) {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const int64_t frame = static_cast<int64_t>(std::llround(
            time_seconds * static_cast<double>(sr)));
        std::vector<GuiTransient> pre_state = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;
        GuiTransient nm;
        nm.src_frame   = frame;
        nm.is_inserted = true;
        int new_idx = app.transients.insert_marker(std::move(nm));
        // Frame-0 companion. If the post-insert list's head isn't at
        // frame 0, insert one. The companion always lands at index 0,
        // so the user's marker shifts up by one.
        if (app.transients.markers().front().effective_frame() != 0) {
            GuiTransient zero;
            zero.src_frame   = 0;
            zero.is_inserted = true;
            app.transients.insert_marker(std::move(zero));
            new_idx += 1;
        }
        app.selected_markers.clear();
        app.selected_markers.insert(new_idx);
        app.last_selected_marker = new_idx;
        push_undo_transient(std::move(pre_state), OpKind::Create,
                            std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
        // Match drop_marker: move playhead to the new transient. When
        // dropping at the current playhead, this is a no-op.
        move_playhead_to(frame);
    };

    auto drop_transient_at_playhead = [&]() {
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double t = static_cast<double>(app.playhead_sample) /
                         static_cast<double>(sr);
        drop_transient_at_position(t);
    };

    // Delete every selected transient. No label/cascade rules — transients
    // don't have labels. Mirrors warp's time-0 protection: the frame-0
    // entry is the transient list's anchor (phase-reset invariant) and
    // cannot be removed.
    auto delete_selected_transient = [&]() {
        if (app.selected_markers.empty()) return;
        const auto& tv = app.transients.markers();
        for (int idx : app.selected_markers) {
            if (idx < 0 || idx >= static_cast<int>(tv.size())) {
                std::fprintf(stderr,
                    "warptempo_gui: transient delete rejected: stale index\n");
                return;
            }
            // Detected entries can't be deleted; only disabled or merged
            // away by re-running detection.
            if (!tv[idx].is_inserted) return;
            if (idx == 0 || tv[idx].effective_frame() == 0) {
                std::fprintf(stderr,
                    "warptempo_gui: cannot delete first transient (frame 0)\n");
                return;
            }
        }
        std::vector<GuiTransient> pre_state = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;
        for (auto it = app.selected_markers.rbegin();
             it != app.selected_markers.rend(); ++it) {
            app.transients.remove_marker(*it);
        }
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        push_undo_transient(std::move(pre_state), OpKind::Destroy,
                            std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Toggle the disabled flag on each selected transient. Unconditional —
    // transients have no label-def gating like warp markers do.
    auto toggle_transient_disabled = [&]() {
        if (app.selected_markers.empty()) return;
        std::vector<GuiTransient> pre_state = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;
        bool changed = false;
        for (int idx : app.selected_markers) {
            GuiTransient* m = app.transients.marker_mut(idx);
            if (!m) continue;
            m->disabled = !m->disabled;
            changed = true;
        }
        if (!changed) return;
        push_undo_transient(std::move(pre_state), OpKind::Other,
                            std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Compute (delta_min, delta_max) sample bounds for shifting the
    // currently-selected transients by a uniform delta. Same shape as the
    // warp version: nearest non-selected neighbor on each side, intersected.
    // Operates on effective_frame (the visible position) — for a
    // D-with-displacement entry, that's displaced_frame.
    // No trim clamp — transients aren't bounded by trim flags during edit.
    auto compute_transient_delta_bounds = [&](bool& ok)
        -> std::pair<int64_t, int64_t> {
        ok = false;
        const auto& tv = app.transients.markers();
        if (app.selected_markers.empty()) return {0, 0};
        for (int idx : app.selected_markers) {
            if (idx < 0 || idx >= static_cast<int>(tv.size())) return {0, 0};
            if (idx == 0 || tv[idx].effective_frame() == 0) return {0, 0};
        }
        int64_t d_min = std::numeric_limits<int64_t>::min();
        int64_t d_max = std::numeric_limits<int64_t>::max();
        for (int idx : app.selected_markers) {
            const int64_t orig = tv[idx].effective_frame();
            int prev = idx - 1;
            while (prev >= 0 && app.selected_markers.count(prev)) --prev;
            if (prev >= 0) {
                const int64_t lb = (tv[prev].effective_frame() + 1) - orig;
                if (lb > d_min) d_min = lb;
            }
            int next = idx + 1;
            while (next < static_cast<int>(tv.size()) &&
                   app.selected_markers.count(next)) ++next;
            if (next < static_cast<int>(tv.size())) {
                const int64_t ub = (tv[next].effective_frame() - 1) - orig;
                if (ub < d_max) d_max = ub;
            }
        }
        ok = true;
        return {d_min, d_max};
    };

    // Apply a position delta to one transient's effective frame.
    //   I:                src_frame += delta.
    //   D, no displace:   set has_displacement=true unless the new effective
    //                     equals src_frame (delta==0 → no-op).
    //   D, with displace: update displaced_frame; if the new effective lands
    //                     back on the anchor src_frame, revert the
    //                     displacement.
    // Caller is responsible for delta != 0; this is a noop on delta == 0.
    auto apply_transient_position_delta = [](GuiTransient& m, int64_t delta) {
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
    };

    // Nudge selected transients by +/- 1 source-pixel. Direction: -1 for
    // earlier, +1 for later. Symmetric with nudge_selected_markers.
    auto nudge_selected_transients = [&](int direction) {
        if (app.loading || audio.total_frames() <= 0) return;
        stop_playback_if_playing();
        if (app.selected_markers.empty()) return;
        const double spp = current_samples_per_pixel(app, audio);
        const int64_t step = std::max<int64_t>(1,
            static_cast<int64_t>(std::llround(spp))) *
            static_cast<int64_t>(direction);
        if (step == 0) return;

        bool ok = false;
        auto [d_min, d_max] = compute_transient_delta_bounds(ok);
        if (!ok) return;
        int64_t delta = step;
        if (delta < d_min) delta = d_min;
        if (delta > d_max) delta = d_max;
        if (delta == 0) return;

        std::vector<GuiTransient> pre_state = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;
        for (int idx : app.selected_markers) {
            GuiTransient* m = app.transients.marker_mut(idx);
            if (!m) continue;
            apply_transient_position_delta(*m, delta);
        }
        push_undo_transient(std::move(pre_state), OpKind::Move,
                            std::move(hint_sel), hint_last);
        sync_playhead_to_last_selected();
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // `j` for transient mode: shift the selection so last_selected lands
    // on the playhead. All-or-nothing clamp check.
    auto jump_transient_selection_to_playhead = [&]() {
        if (app.selected_markers.empty()) return;
        if (app.last_selected_marker < 0) return;
        const auto& tv = app.transients.markers();
        if (app.last_selected_marker >= static_cast<int>(tv.size())) return;
        const int64_t anchor_f = tv[app.last_selected_marker].effective_frame();
        const int64_t delta    = app.playhead_sample - anchor_f;
        if (delta == 0) return;

        bool ok = false;
        auto [d_min, d_max] = compute_transient_delta_bounds(ok);
        if (!ok || delta < d_min || delta > d_max) {
            std::fprintf(stderr,
                "warptempo_gui: transient jump rejected: would violate "
                "marker ordering\n");
            return;
        }
        std::vector<GuiTransient> pre_state = app.transients.markers();
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;
        for (int idx : app.selected_markers) {
            GuiTransient* m = app.transients.marker_mut(idx);
            if (!m) continue;
            apply_transient_position_delta(*m, delta);
        }
        push_undo_transient(std::move(pre_state), OpKind::Move,
                            std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Toggle b= flag on the single selected transient. Mirrors the warp
    // version's toggle / auto-replace / equal-frame refusal and likewise
    // resolves cross-file: an existing b= on the warp side is cleared, and
    // a swap target on the warp side is honored.
    auto toggle_transient_begin_time = [&]() {
        if (app.selected_markers.size() != 1) return;
        const int idx = app.last_selected_marker;
        if (idx < 0) return;
        const auto& tv = app.transients.markers();
        if (idx >= static_cast<int>(tv.size())) return;

        std::vector<GuiMarker>    warp_pre  = app.markers.markers();
        std::vector<GuiTransient> trans_pre = tv;
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;

        if (tv[idx].is_begin_time) {
            app.transients.marker_mut(idx)->is_begin_time = false;
            push_undo_both(std::move(warp_pre), std::move(trans_pre),
                           'T', OpKind::Other,
                           std::move(hint_sel), hint_last);
            recompute_dirty();
            invalidate_waveform_area();
            invalidate_dirty_and_timestamp();
            return;
        }

        const int64_t m_frame = tv[idx].effective_frame();
        const FlagLoc e_loc   = find_flag(/*want_begin=*/false,
                                          /*excl_trans=*/false, -1);
        const FlagLoc b_other = find_flag(/*want_begin=*/true,
                                          /*excl_trans=*/true, idx);

        const bool needs_swap   = e_loc.valid && (m_frame >= e_loc.frame);
        const bool equal_frames = e_loc.valid && (m_frame == e_loc.frame);
        if (equal_frames) {
            std::fprintf(stderr,
                "warptempo_gui: b refused: would collapse trim region\n");
            return;
        }

        if (b_other.valid) {
            if (b_other.transient) {
                app.transients.marker_mut(b_other.idx)->is_begin_time = false;
            } else {
                app.markers.marker_mut(b_other.idx)->is_begin_time = false;
            }
        }
        if (needs_swap) {
            if (e_loc.transient) {
                app.transients.marker_mut(e_loc.idx)->is_end_time   = false;
                app.transients.marker_mut(e_loc.idx)->is_begin_time = true;
            } else {
                app.markers.marker_mut(e_loc.idx)->is_end_time   = false;
                app.markers.marker_mut(e_loc.idx)->is_begin_time = true;
            }
            app.transients.marker_mut(idx)->is_end_time = true;
        } else {
            app.transients.marker_mut(idx)->is_begin_time = true;
        }

        push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'T', OpKind::Other, std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    auto toggle_transient_end_time = [&]() {
        if (app.selected_markers.size() != 1) return;
        const int idx = app.last_selected_marker;
        if (idx < 0) return;
        const auto& tv = app.transients.markers();
        if (idx >= static_cast<int>(tv.size())) return;

        std::vector<GuiMarker>    warp_pre  = app.markers.markers();
        std::vector<GuiTransient> trans_pre = tv;
        std::set<int>             hint_sel  = app.selected_markers;
        const int                 hint_last = app.last_selected_marker;

        if (tv[idx].is_end_time) {
            app.transients.marker_mut(idx)->is_end_time = false;
            push_undo_both(std::move(warp_pre), std::move(trans_pre),
                           'T', OpKind::Other,
                           std::move(hint_sel), hint_last);
            recompute_dirty();
            invalidate_waveform_area();
            invalidate_dirty_and_timestamp();
            return;
        }

        const int64_t m_frame = tv[idx].effective_frame();
        const FlagLoc b_loc   = find_flag(/*want_begin=*/true,
                                          /*excl_trans=*/false, -1);
        const FlagLoc e_other = find_flag(/*want_begin=*/false,
                                          /*excl_trans=*/true, idx);

        const bool needs_swap   = b_loc.valid && (m_frame <= b_loc.frame);
        const bool equal_frames = b_loc.valid && (m_frame == b_loc.frame);
        if (equal_frames) {
            std::fprintf(stderr,
                "warptempo_gui: e refused: would collapse trim region\n");
            return;
        }

        if (e_other.valid) {
            if (e_other.transient) {
                app.transients.marker_mut(e_other.idx)->is_end_time = false;
            } else {
                app.markers.marker_mut(e_other.idx)->is_end_time = false;
            }
        }
        if (needs_swap) {
            if (b_loc.transient) {
                app.transients.marker_mut(b_loc.idx)->is_begin_time = false;
                app.transients.marker_mut(b_loc.idx)->is_end_time   = true;
            } else {
                app.markers.marker_mut(b_loc.idx)->is_begin_time = false;
                app.markers.marker_mut(b_loc.idx)->is_end_time   = true;
            }
            app.transients.marker_mut(idx)->is_begin_time = true;
        } else {
            app.transients.marker_mut(idx)->is_end_time = true;
        }

        push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'T', OpKind::Other, std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    // Overwrite the active tab's snapshot with the live AppState viewport /
    // zoom / playhead. Shared by Ctrl+Tab (pre-flip) and Ctrl+S (pre-write)
    // so "remembered spot" semantics stay consistent between the two paths.
    // Also stashes the active selection into the per-mode slot so a tab
    // flip + mode flip can restore the right pair on return.
    auto refresh_active_tab_from_app = [&]() {
        Tab& t = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
        t.viewport_start_sample = app.viewport_start_sample;
        t.zoom_level            = app.zoom_level;
        t.playhead_sample       = app.playhead_sample;
        if (app.active_mode == 'T') {
            t.transient_selected      = app.selected_markers;
            t.transient_last_selected = app.last_selected_marker;
        } else {
            t.warp_selected           = app.selected_markers;
            t.warp_last_selected      = app.last_selected_marker;
        }
    };

    // Toggle active editing mode between 'W' (warp) and 'T' (transient).
    // Saves the active selection into the leaving mode's per-tab slot,
    // then restores the destination mode's slot. Visible state (viewport /
    // zoom / playhead) is unaffected. Caller decides what invalidations to
    // run; this helper just shuffles the AppState fields.
    auto switch_active_mode_to = [&](char target_mode) {
        if (target_mode == app.active_mode) return;
        Tab& t = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
        if (app.active_mode == 'T') {
            t.transient_selected      = app.selected_markers;
            t.transient_last_selected = app.last_selected_marker;
            app.selected_markers      = t.warp_selected;
            app.last_selected_marker  = t.warp_last_selected;
        } else {
            t.warp_selected           = app.selected_markers;
            t.warp_last_selected      = app.last_selected_marker;
            app.selected_markers      = t.transient_selected;
            app.last_selected_marker  = t.transient_last_selected;
        }
        app.active_mode = target_mode;
    };

    // `t` key: toggle into/out of transient mode. Entry preconditions
    // (only when going W → T): engine setting must be `warptempo` and
    // transients_enabled must not be `false`. Exit (T → W) is unconditional.
    auto toggle_active_mode = [&]() {
        if (app.active_mode == 'T') {
            switch_active_mode_to('W');
        } else {
            const std::string engine =
                settings_get("engine", "warptempo");
            const std::string te =
                settings_get("transients_enabled", "true");
            if (engine != "warptempo") {
                std::fprintf(stderr,
                    "warptempo_gui: transient mode unavailable: "
                    "engine=%s\n", engine.c_str());
                return;
            }
            if (te == "false") {
                std::fprintf(stderr,
                    "warptempo_gui: transient mode unavailable: "
                    "transients_enabled=false\n");
                return;
            }
            switch_active_mode_to('T');
        }
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    auto save_markers = [&]() -> bool {
        if (app.warpmarkers_path.empty()) return false;
        if (app.first_save_pending && app.markers.had_nonstandard_content()) {
            std::fprintf(stderr,
                "warptempo_gui: first save in this session will discard "
                "comments and freeform text from %s. Canonical format will "
                "be written.\n",
                app.warpmarkers_path.c_str());
        }
        // Capture the active tab's current values before any writes — both
        // the .warpmarkers and .settings paths see a consistent snapshot.
        refresh_active_tab_from_app();

        const bool ok = app.markers.save(app.warpmarkers_path);
        if (!ok) {
            std::fprintf(stderr,
                "warptempo_gui: save failed: %s\n",
                app.warpmarkers_path.c_str());
            return false;
        }

        // Transients sibling write. Empty list deletes the on-disk file so
        // a project never carries a stale empty .transientmarkers.
        if (!app.transientmarkers_path.empty()) {
            if (app.transients.markers().empty()) {
                if (!app.transients.delete_file(app.transientmarkers_path)) {
                    std::fprintf(stderr,
                        "warptempo_gui: failed to delete: %s\n",
                        app.transientmarkers_path.c_str());
                }
            } else {
                if (!app.transients.save(app.transientmarkers_path)) {
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
            invalidate_dirty_and_timestamp();
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

    auto invalidate_all = [&]() {
        gui.invalidate_region(0, 0, app.width, app.height);
    };

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

        app.markers.clear();
        app.transients.clear();
        app.transient_selected_markers.clear();
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        app.active_mode    = 'W';
        app.drag          = DragState{};
        app.playhead_drag = PlayheadDragState{};
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

        app.tab_a = Tab{};
        app.tab_b = Tab{};
        app.active_tab = 'A';

        invalidate_all();
    };

    // Forward decl: defined below so it can be captured by the dialog
    // helpers, but invoked only on Detect-confirmation.
    std::function<void()> run_detect_now;

    auto proceed_with_trigger = [&](DialogTrigger t) {
        switch (t) {
        case DialogTrigger::CLOSE_WINDOW:
            gui.request_exit();
            break;
        case DialogTrigger::REVERT_TO_BLANK:
            revert_to_blank();
            break;
        case DialogTrigger::DETECT_TRANSIENTS:
            if (run_detect_now) run_detect_now();
            break;
        }
    };

    auto open_unsaved_dialog = [&](DialogTrigger t) {
        app.dialog.active         = true;
        app.dialog.focused_button = 0;
        app.dialog.trigger        = t;
        app.dialog.prompt_text    =
            "Unsaved changes. Save before continuing?";
        app.dialog.button_labels  = {"Save", "Discard", "Cancel"};
        invalidate_all();
    };

    auto open_detect_confirm_dialog = [&]() {
        app.dialog.active         = true;
        // Default focus on Cancel: re-detection discards entries the
        // current detector would no longer place, so the safe default
        // is to back out.
        app.dialog.focused_button = 1;
        app.dialog.trigger        = DialogTrigger::DETECT_TRANSIENTS;
        app.dialog.prompt_text    =
            "Re-detect transients? Existing detection will be replaced.";
        app.dialog.button_labels  = {"Detect", "Cancel"};
        invalidate_all();
    };

    auto close_dialog = [&]() {
        app.dialog = DialogState{};
        invalidate_all();
    };

    // Dispatch a dialog button activation. The button index is interpreted
    // per trigger: CLOSE_WINDOW / REVERT_TO_BLANK use 0=Save 1=Discard
    // 2=Cancel; DETECT_TRANSIENTS uses 0=Detect 1=Cancel.
    auto dialog_activate_button = [&](int button) {
        if (!app.dialog.active) return;
        const DialogTrigger t = app.dialog.trigger;
        if (t == DialogTrigger::DETECT_TRANSIENTS) {
            if (button == 0) {
                close_dialog();
                proceed_with_trigger(t);
            } else {
                close_dialog();
            }
            return;
        }
        // Unsaved-work dialog: 0=Save, 1=Discard, 2=Cancel.
        switch (button) {
        case 0: { // Save
            const bool ok = save_markers();
            if (!ok) {
                app.dialog.prompt_text = "Save failed.";
                invalidate_all();
                return;
            }
            close_dialog();
            proceed_with_trigger(t);
            break;
        }
        case 1: // Discard
            close_dialog();
            proceed_with_trigger(t);
            break;
        case 2: // Cancel
        default:
            close_dialog();
            break;
        }
    };

    // Route a close / revert gesture through the dialog when history is
    // dirty; otherwise proceed immediately. Centralizes the decision so
    // Ctrl+Q, Ctrl+W, and the WM-close callback share identical behavior.
    auto request_close_or_revert = [&](DialogTrigger t) {
        if (app.dialog.active) return; // already gated; ignore re-entry
        if (app.dirty) open_unsaved_dialog(t);
        else           proceed_with_trigger(t);
    };

    // -- Transient detection (chunk S.3) ------------------------------------

    // Two-pass merge: existing D entries indexed by their immutable
    // src_frame anchor are matched against fresh detections so user edits
    // (disabled, displaced position, b=/e= flags) survive re-detection.
    // Existing I (manually inserted) entries always carry over; D entries
    // whose src_frame the new detector no longer places are dropped. The
    // merged list is sorted by effective_frame() and the frame-0 invariant
    // is restored. Does NOT push undo — detection is destructive by spec.
    auto merge_detection = [&](const std::vector<int64_t>& fresh_src_frames) {
        std::map<int64_t, GuiTransient> old_d_by_src;
        std::vector<GuiTransient> old_i;
        for (const auto& m : app.transients.markers()) {
            if (m.is_inserted) old_i.push_back(m);
            else               old_d_by_src.emplace(m.src_frame, m);
        }

        std::vector<GuiTransient> merged;
        merged.reserve(fresh_src_frames.size() + old_i.size() + 1);
        for (int64_t f : fresh_src_frames) {
            auto it = old_d_by_src.find(f);
            if (it != old_d_by_src.end()) {
                merged.push_back(it->second);
            } else {
                GuiTransient m;
                m.src_frame   = f;
                m.is_inserted = false;
                merged.push_back(m);
            }
        }
        for (auto& m : old_i) merged.push_back(std::move(m));

        std::sort(merged.begin(), merged.end(),
            [](const GuiTransient& a, const GuiTransient& b) {
                return a.effective_frame() < b.effective_frame();
            });

        if (merged.empty() || merged.front().effective_frame() > 0) {
            GuiTransient zero;
            zero.src_frame   = 0;
            zero.is_inserted = true;
            merged.insert(merged.begin(), zero);
        }

        app.transients.markers_mut() = std::move(merged);
    };

    // Run the engine's detection-only pass against the loaded source +
    // current marker set, then merge the results into app.transients.
    // After a successful merge, write the .transientmarkers sibling file
    // immediately — detection is not on the undo stack so the on-disk
    // file is the authoritative record. The transient_dirty bit is reset
    // explicitly: the merge mutated app.transients but no UndoEntry was
    // pushed, so the post-merge state is the one we want to call "saved".
    run_detect_now = [&]() {
        if (app.source_audio_path.empty())   return;
        if (audio.total_frames() <= 0)       return;

        // Clear any in-flight transient selection — indices into the
        // pre-merge list are not meaningful afterwards.
        app.transient_selected_markers.clear();
        if (app.active_mode == 'T') app.last_selected_marker = -1;

        DetectionRequest dr;
        dr.source_audio_path    = app.source_audio_path;
        dr.markers              = app.markers.markers();
        dr.settings_passthrough = app.settings_passthrough;

        std::vector<int64_t> fresh;
        if (!do_detection(dr, fresh)) {
            std::fprintf(stderr,
                "warptempo_gui: detection failed; transients unchanged\n");
            return;
        }
        std::sort(fresh.begin(), fresh.end());
        fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());

        merge_detection(fresh);

        // Write the sibling file. Empty list (only the auto frame-0 head)
        // would be unusual after detection, but the save path already
        // handles the empty case.
        if (!app.transientmarkers_path.empty()) {
            if (app.transients.markers().empty()) {
                app.transients.delete_file(app.transientmarkers_path);
            } else if (!app.transients.save(app.transientmarkers_path)) {
                std::fprintf(stderr,
                    "warptempo_gui: transient save failed: %s\n",
                    app.transientmarkers_path.c_str());
            }
        }

        // Detection is not undoable. Reset the transient-side dirty bit
        // so the dialog doesn't gate a subsequent close/revert because of
        // the merge. Warp dirty is unaffected.
        app.transient_dirty = false;
        recompute_dirty();
        invalidate_all();
        std::fprintf(stderr,
            "warptempo_gui: detection produced %zu transients\n",
            app.transients.markers().size());
    };

    // Ctrl+Alt+T entry point. Confirms before clobbering an existing
    // detection (any D entry in the list). With no prior detection (only
    // I entries or the auto frame-0 head), runs immediately.
    auto detect_transients = [&]() {
        if (app.dialog.active)             return;
        if (app.source_audio_path.empty()) return;
        if (audio.total_frames() <= 0)     return;

        bool has_prior_detection = false;
        for (const auto& m : app.transients.markers()) {
            if (!m.is_inserted) { has_prior_detection = true; break; }
        }
        if (has_prior_detection) {
            open_detect_confirm_dialog();
            return;
        }
        run_detect_now();
    };

    // Ctrl+Shift+Alt+T: drop all transients (both I and D), undoable. The
    // frame-0 invariant means a non-empty list always carries an entry at
    // 0; clearing wholesale removes that too — load() will re-materialize
    // it on the next read of an empty file (which is itself the empty
    // list, since save() removes the file when empty). The undo restores
    // the full pre-clear state.
    auto clear_all_transients = [&]() {
        if (app.transients.markers().empty()) return;

        std::vector<GuiTransient> pre_state = app.transients.markers();
        std::set<int> pre_sel = app.transient_selected_markers;
        const int pre_last = app.last_selected_marker;

        app.transients.clear();
        app.transient_selected_markers.clear();
        if (app.active_mode == 'T') app.last_selected_marker = -1;

        push_undo_transient(std::move(pre_state), OpKind::Destroy,
                            std::move(pre_sel), pre_last);
        recompute_dirty();
        invalidate_all();
    };

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
    };

    // Hit-test a marker line in the waveform area. Returns index or -1.
    // Mode-aware: iterates the active list (warp markers or transients).
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
        const int n = (app.active_mode == 'T')
            ? static_cast<int>(app.transients.markers().size())
            : static_cast<int>(app.markers.markers().size());
        for (int i = 0; i < n; ++i) {
            double ms;
            if (app.active_mode == 'T') {
                ms = static_cast<double>(
                    app.transients.markers()[i].effective_frame());
            } else {
                ms = app.markers.markers()[i].time_seconds *
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
        if (app.active_mode == 'T') {
            rects = compute_transient_flag_hit_rects(
                scratch_cr, top, app.transients.markers(),
                vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
        } else {
            rects = compute_flag_hit_rects(
                scratch_cr, top, app.markers.markers(),
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

    // Begin a Ctrl+drag. Expects caller to have already applied the initial
    // selection change (ctrl semantics). Returns true if drag was started.
    // Mode-aware: dragging applies to the active list (warp markers or
    // transients). Internally `original_times` is in seconds for both modes;
    // for transients it is `src_frame / sample_rate`, and motion is mapped
    // back to the integer src_frame at apply time.
    auto begin_drag = [&](int hit, int mouse_x) -> bool {
        if (hit < 0) return false;
        const int sr = audio.sample_rate();
        if (sr <= 0) return false;
        const bool transient = (app.active_mode == 'T');
        const int n = transient
            ? static_cast<int>(app.transients.markers().size())
            : static_cast<int>(app.markers.markers().size());
        if (hit >= n) return false;

        const double sr_d = static_cast<double>(sr);
        auto t_of = [&](int idx) -> double {
            if (transient) {
                return static_cast<double>(
                    app.transients.markers()[idx].effective_frame()) / sr_d;
            }
            return app.markers.markers()[idx].time_seconds;
        };

        // Drag target: entire selection if hit is in it, else just the hit.
        // In the single-drag case, also collapse the selection to that marker
        // so the visible selection matches what's actually being dragged.
        std::set<int> drag_set;
        if (app.selected_markers.count(hit)) {
            drag_set = app.selected_markers;
        } else {
            const std::set<int> old = app.selected_markers;
            app.selected_markers.clear();
            app.selected_markers.insert(hit);
            app.last_selected_marker = hit;
            invalidate_markers_columns(old);
            invalidate_marker_column(hit);
            invalidate_top_strip();
            drag_set.insert(hit);
        }

        // First-marker protection: refuse index 0 and any effective-time-0
        // marker.
        for (int idx : drag_set) {
            if (idx == 0 || t_of(idx) == 0.0) {
                std::fprintf(stderr,
                    "warptempo_gui: first marker cannot be dragged\n");
                return false;
            }
        }

        DragState d;
        d.active = true;
        d.drag_mode = transient ? 'T' : 'W';
        d.dragging_markers.assign(drag_set.begin(), drag_set.end());
        d.original_times.reserve(d.dragging_markers.size());
        for (int idx : d.dragging_markers) {
            d.original_times.push_back(t_of(idx));
        }

        // Anchor mouse time — computed at mouse_x in the waveform's X axis.
        const GuiRect area = waveform_area(app);
        const double spp = current_samples_per_pixel(app, audio);
        const double vp_time = static_cast<double>(app.viewport_start_sample) / sr_d;
        d.anchor_mouse_time_seconds =
            vp_time + static_cast<double>(mouse_x - area.x) * spp / sr_d;

        // Compute scalar delta_min / delta_max from per-marker neighbor
        // bounds. Correct for both contiguous and non-contiguous drag sets.
        // eps enforces a 3-pixel visual gap at the current zoom — markers
        // never stack even at the tightest clamp.
        const double eps = 3.0 * spp / sr_d;

        d.delta_min = -std::numeric_limits<double>::infinity();
        d.delta_max =  std::numeric_limits<double>::infinity();

        for (size_t k = 0; k < d.dragging_markers.size(); ++k) {
            const int idx = d.dragging_markers[k];
            const double orig_t = d.original_times[k];

            // Nearest non-dragged neighbor to the left.
            int prev = idx - 1;
            while (prev >= 0 && drag_set.count(prev)) --prev;
            if (prev >= 0) {
                const double lb = (t_of(prev) + eps) - orig_t;
                if (lb > d.delta_min) d.delta_min = lb;
            }

            // Nearest non-dragged neighbor to the right.
            int next = idx + 1;
            while (next < n && drag_set.count(next)) ++next;
            if (next < n) {
                const double ub = (t_of(next) - eps) - orig_t;
                if (ub < d.delta_max) d.delta_max = ub;
            }
        }

        d.moved = false;
        // Capture the pre-drag list state for undo. Commit pushes the
        // active-mode snapshot if motion landed; Escape-cancel discards.
        if (transient) {
            d.pre_drag_transient_snapshot = app.transients.markers();
        } else {
            d.pre_drag_snapshot = app.markers.markers();
        }
        d.pre_drag_selected      = app.selected_markers;
        d.pre_drag_last_selected = app.last_selected_marker;
        d.hit_marker             = hit;
        d.pre_drag_playhead_sample = app.playhead_sample;
        app.drag = std::move(d);
        return true;
    };

    // Apply a raw delta (mouse-derived) to the dragging markers, clamped.
    // Updates marker times in place and invalidates only the old-and-new
    // pixel columns of each moved marker plus the flag strip. The waveform
    // cache stays valid throughout the drag — viewport / trim / dimensions
    // don't change — so these narrow invalidations blit the cache over
    // tiny rects and repaint just markers + flags + playhead on top.
    auto apply_drag_motion = [&](double raw_delta) {
        if (!app.drag.active) return;
        double delta = raw_delta;
        if (delta < app.drag.delta_min) delta = app.drag.delta_min;
        if (delta > app.drag.delta_max) delta = app.drag.delta_max;

        const GuiRect area = waveform_area(app);
        const int sr       = audio.sample_rate();
        const double spp   = current_samples_per_pixel(app, audio);
        const double sr_d  = static_cast<double>(sr);
        const bool geom_ok = (sr > 0 && spp > 0.0 && area.w > 0);
        const double vp    = static_cast<double>(app.viewport_start_sample);

        auto col_rect_for_time = [&](double t_seconds) -> GuiRect {
            const double ms = t_seconds * sr_d;
            const double px = area.x + (ms - vp) / spp;
            return playhead_invalidate_rect(area, px);
        };

        const bool transient = (app.drag.drag_mode == 'T');
        bool any_changed = false;
        for (size_t k = 0; k < app.drag.dragging_markers.size(); ++k) {
            const int idx = app.drag.dragging_markers[k];
            const double new_t = app.drag.original_times[k] + delta;
            double old_t;
            if (transient) {
                GuiTransient* m = app.transients.marker_mut(idx);
                if (!m) continue;
                old_t = static_cast<double>(m->effective_frame()) / sr_d;
                const int64_t new_frame = static_cast<int64_t>(
                    std::llround(new_t * sr_d));
                const int64_t cur_frame = m->effective_frame();
                if (cur_frame == new_frame) continue;
                apply_transient_position_delta(*m, new_frame - cur_frame);
            } else {
                GuiMarker* m = app.markers.marker_mut(idx);
                if (!m) continue;
                old_t = m->time_seconds;
                if (old_t == new_t) continue;
                m->time_seconds = new_t;
            }
            any_changed = true;
            if (!geom_ok) continue;
            const GuiRect r_old = col_rect_for_time(old_t);
            const GuiRect r_new = col_rect_for_time(new_t);
            const GuiRect u = union_rect(r_old, r_new);
            if (u.w > 0 && u.h > 0) {
                gui.invalidate_region(u.x, u.y, u.w, u.h);
            }
        }
        if (any_changed) {
            app.drag.moved = true;
            // Flag strip is at most ~top_strip_height px tall; repainting
            // the whole strip takes ~0.05 ms, so don't bother per-flag.
            invalidate_top_strip();
        }
    };

    // Restore each dragged marker's original time and clear drag state.
    // Escape-cancel also restores the playhead to its pre-drag sample so
    // the audio cursor and visible state match what the user saw before.
    auto cancel_drag = [&]() {
        if (!app.drag.active) return;
        const bool transient = (app.drag.drag_mode == 'T');
        const double sr_d = static_cast<double>(audio.sample_rate());
        // Restore from the pre-drag snapshot rather than reverse-deriving
        // from original_times — for D entries, that requires reverting both
        // src_frame, has_displacement, and displaced_frame at once.
        if (transient) {
            const auto& snap = app.drag.pre_drag_transient_snapshot;
            if (!snap.empty()) {
                app.transients.markers_mut() = snap;
            }
        } else {
            for (size_t k = 0; k < app.drag.dragging_markers.size(); ++k) {
                const int idx = app.drag.dragging_markers[k];
                GuiMarker* m = app.markers.marker_mut(idx);
                if (m) m->time_seconds = app.drag.original_times[k];
            }
        }
        (void)sr_d;
        const int64_t restore_ph = app.drag.pre_drag_playhead_sample;
        app.drag = DragState{};
        move_playhead_to(restore_ph);
        invalidate_waveform_area();
    };

    // Commit the current drag. Caller ensures drag was active. Sets dirty
    // only if the markers actually moved. Playhead is left wherever it
    // ended up (tracked live in the motion handler) — no snap.
    auto commit_drag = [&]() {
        if (!app.drag.active) return;
        const bool moved = app.drag.moved;
        const bool transient = (app.drag.drag_mode == 'T');
        std::vector<GuiMarker>    snap_w =
            std::move(app.drag.pre_drag_snapshot);
        std::vector<GuiTransient> snap_t =
            std::move(app.drag.pre_drag_transient_snapshot);
        std::set<int>             hint_sel  =
            std::move(app.drag.pre_drag_selected);
        const int                 hint_last = app.drag.pre_drag_last_selected;
        app.drag = DragState{};
        if (moved) {
            if (transient) {
                push_undo_transient(std::move(snap_t), OpKind::Move,
                                    std::move(hint_sel), hint_last);
            } else {
                push_undo(std::move(snap_w), OpKind::Move,
                          std::move(hint_sel), hint_last);
            }
            recompute_dirty();
            invalidate_dirty_and_timestamp();
        }
        invalidate_waveform_area();
    };

    // Compute (delta_min, delta_max) scalar bounds for shifting the current
    // selection set by a uniform delta. Neighbors: for each selected marker,
    // the nearest non-selected marker on each side. Trim is purely cosmetic
    // and does not constrain edits. Returns (0, 0) if empty or time-0 marker
    // present (move forbidden).
    auto compute_selection_delta_bounds = [&](bool& ok)
        -> std::pair<double, double> {
        ok = false;
        const auto& mv = app.markers.markers();
        if (app.selected_markers.empty()) return {0.0, 0.0};
        const int sr = audio.sample_rate();
        if (sr <= 0) return {0.0, 0.0};
        for (int idx : app.selected_markers) {
            if (idx < 0 || idx >= static_cast<int>(mv.size())) return {0.0, 0.0};
            if (idx == 0 || mv[idx].time_seconds == 0.0) return {0.0, 0.0};
        }
        const double sr_d = static_cast<double>(sr);
        const double spp  = current_samples_per_pixel(app, audio);
        const double eps  = 3.0 * spp / sr_d;  // 3 pixels at current zoom

        double d_min = -std::numeric_limits<double>::infinity();
        double d_max =  std::numeric_limits<double>::infinity();
        for (int idx : app.selected_markers) {
            const double orig_t = mv[idx].time_seconds;
            int prev = idx - 1;
            while (prev >= 0 && app.selected_markers.count(prev)) --prev;
            if (prev >= 0) {
                const double lb = (mv[prev].time_seconds + eps) - orig_t;
                if (lb > d_min) d_min = lb;
            }
            int next = idx + 1;
            while (next < static_cast<int>(mv.size()) &&
                   app.selected_markers.count(next)) ++next;
            if (next < static_cast<int>(mv.size())) {
                const double ub = (mv[next].time_seconds - eps) - orig_t;
                if (ub < d_max) d_max = ub;
            }
        }
        ok = true;
        return {d_min, d_max};
    };

    // Shift every selected marker by the clamped delta. Returns whether any
    // marker actually moved.
    auto apply_selection_shift = [&](double raw_delta) -> bool {
        bool ok = false;
        auto [d_min, d_max] = compute_selection_delta_bounds(ok);
        if (!ok) return false;
        double delta = raw_delta;
        if (delta < d_min) delta = d_min;
        if (delta > d_max) delta = d_max;
        if (delta == 0.0) return false;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            m->time_seconds += delta;
        }
        return true;
    };

    // Nudge selected markers by +/- 1 pixel of source time at current zoom.
    // direction: -1 for earlier (up/left), +1 for later (down/right).
    auto nudge_selected_markers = [&](int direction) {
        if (app.loading || audio.total_frames() <= 0) return;
        // Nudges move the playhead (via sync_playhead_to_last_selected).
        // Stop playback first — covers both Ctrl+Left/Right keys and
        // Ctrl+wheel in one place.
        stop_playback_if_playing();
        if (app.selected_markers.empty()) return;
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const double spp = current_samples_per_pixel(app, audio);
        const double delta_s =
            static_cast<double>(direction) * spp / static_cast<double>(sr);
        if (delta_s == 0.0) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        if (apply_selection_shift(delta_s)) {
            push_undo(std::move(pre_state), OpKind::Move,
                      std::move(hint_sel), hint_last);
            sync_playhead_to_last_selected();
            recompute_dirty();
            invalidate_waveform_area();
            invalidate_dirty_and_timestamp();
        }
    };

    // `j`: move every selected marker so last_selected lands on the playhead.
    // All-or-nothing: if any resulting position would violate monotonicity
    // or trim, reject the whole operation with a stderr note.
    auto jump_selection_to_playhead = [&]() {
        if (app.selected_markers.empty()) return;
        if (app.last_selected_marker < 0) return;
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const auto& mv = app.markers.markers();
        if (app.last_selected_marker >= static_cast<int>(mv.size())) return;
        const double anchor_t = mv[app.last_selected_marker].time_seconds;
        const double ph_t =
            static_cast<double>(app.playhead_sample) /
            static_cast<double>(sr);
        const double delta = ph_t - anchor_t;
        if (delta == 0.0) return;

        bool ok = false;
        auto [d_min, d_max] = compute_selection_delta_bounds(ok);
        if (!ok || delta < d_min || delta > d_max) {
            std::fprintf(stderr,
                "warptempo_gui: jump rejected: would violate marker "
                "ordering\n");
            return;
        }
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            m->time_seconds += delta;
        }
        push_undo(std::move(pre_state), OpKind::Move,
                  std::move(hint_sel), hint_last);
        recompute_dirty();
        invalidate_waveform_area();
        invalidate_dirty_and_timestamp();
    };

    gui.set_on_key([&](KeySym keysym, unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        const bool ctrl  = (mods & ControlMask) != 0;
        const bool shift = (mods & ShiftMask)   != 0;
        const bool alt   = (mods & Mod1Mask)    != 0;

        // Unsaved-work dialog owns input while active. Only the dialog's
        // own keys (Tab / Shift+Tab / arrows / Enter / Esc) route through
        // dialog handling; everything else is swallowed so marker edits /
        // playback / viewport keys cannot sneak in while the modal is up.
        if (app.dialog.active) {
            if (keysym == XK_Escape) {
                // Cancel = the last button, by convention across all
                // dialog forms (Save/Discard/Cancel and Detect/Cancel).
                const int last = std::max(0,
                    static_cast<int>(app.dialog.button_labels.size()) - 1);
                dialog_activate_button(last);
                return;
            }
            if (keysym == XK_Return || keysym == XK_KP_Enter ||
                keysym == XK_space) {
                dialog_activate_button(app.dialog.focused_button);
                return;
            }
            const bool move_left =
                keysym == XK_Left ||
                keysym == XK_ISO_Left_Tab ||
                (keysym == XK_Tab && shift);
            const bool move_right =
                keysym == XK_Right ||
                (keysym == XK_Tab && !shift);
            const int n = static_cast<int>(app.dialog.button_labels.size());
            if (n > 0 && move_left) {
                app.dialog.focused_button =
                    (app.dialog.focused_button + n - 1) % n;
                invalidate_all();
                return;
            }
            if (n > 0 && move_right) {
                app.dialog.focused_button = (app.dialog.focused_button + 1) % n;
                invalidate_all();
                return;
            }
            // Other keys: swallow.
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
        if (gui_text_editor::is_active(app.top_flag_editor)) {
            (void)ctrl; (void)alt; // Modifiers swallowed except Shift→colon.
            const auto action = gui_text_editor::handle_key(
                app.top_flag_editor, keysym, mods);
            if (action == gui_text_editor::KeyAction::CommitRequested) {
                commit_top_flag_edit();
                return;
            }
            if (action == gui_text_editor::KeyAction::CancelRequested) {
                exit_top_flag_edit_no_commit();
                return;
            }
            if (action == gui_text_editor::KeyAction::Consumed) {
                invalidate_top_strip();
                return;
            }
            // NotConsumed: editor saw nothing useful; fall through is wrong
            // because the editor must own all keys while active. Treat as
            // consumed.
            return;
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

        // Escape during a Ctrl+drag cancels the drag.
        if (keysym == XK_Escape && app.drag.active) {
            cancel_drag();
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

        // Ctrl+Alt+R: in-process render. Synchronous — blocks the UI until
        // the pipeline finishes; all output goes to stderr (the GUI has no
        // progress UI for it yet). Silent no-op when no audio is loaded.
        if (ctrl && alt && !shift &&
            (keysym == XK_r || keysym == XK_R)) {
            if (!app.source_audio_path.empty()) {
                RenderRequest req;
                req.source_audio_path    = app.source_audio_path;
                req.markers              = app.markers.markers();
                req.settings_passthrough = app.settings_passthrough;
                // Active transients in source-frame domain. Disabled
                // entries are filtered; D entries surface their visible
                // (displaced) position via effective_frame(). The list is
                // already sorted by effective_frame() per the on-disk
                // invariant, so no re-sort is needed.
                for (const auto& m : app.transients.markers()) {
                    if (m.disabled) continue;
                    req.transient_frames.push_back(m.effective_frame());
                }
                do_render(req);
            }
            return;
        }

        // Ctrl+Alt+E: snapshot current authoring state into a per-source
        // render-queue entry. Pure snapshot — no render runs here.
        // Source-side authoring files (.warpmarkers / .transientmarkers /
        // .settings) are untouched; the entry captures live in-memory state.
        // Settings are not snapshotted in chunk U: render-all reads the
        // GUI's live settings_passthrough at render time.
        if (ctrl && alt && !shift &&
            (keysym == XK_e || keysym == XK_E)) {
            if (app.source_audio_path.empty()) return;

            std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path queue_root = src_parent / "renders";

            auto now = std::chrono::system_clock::now();
            std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm lt{};
            // localtime is single-threaded-safe enough for the GUI thread;
            // copy into our own tm so a later std::localtime call cannot
            // stomp the buffer mid-format.
            if (std::tm* p = std::localtime(&tt)) lt = *p;
            char ts_buf[32];
            std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d_%H%M%S", &lt);
            const std::string ts = ts_buf;

            // Same-second collision: append _2, _3, ... up to _100. The
            // 100 cap is paranoia; in practice two presses in the same
            // second is the typical worst case.
            std::filesystem::path entry_dir = queue_root / ts;
            int suffix = 2;
            while (std::filesystem::exists(entry_dir)) {
                if (suffix > 100) {
                    std::fprintf(stderr,
                        "warptempo_gui: queue error: too many same-second "
                        "entries under %s\n", queue_root.string().c_str());
                    return;
                }
                entry_dir = queue_root / (ts + "_" + std::to_string(suffix));
                ++suffix;
            }

            std::error_code ec;
            std::filesystem::create_directories(entry_dir, ec);
            if (ec) {
                std::fprintf(stderr,
                    "warptempo_gui: queue error: could not create '%s': %s\n",
                    entry_dir.string().c_str(), ec.message().c_str());
                return;
            }

            const std::string warp_path =
                (entry_dir / "warpmarkers").string();
            if (!app.markers.save(warp_path)) {
                std::fprintf(stderr,
                    "warptempo_gui: queue error: failed to write '%s'\n",
                    warp_path.c_str());
                return;
            }
            // Transients file is omitted entirely when the in-memory list
            // is empty — load_render_request_from_dir treats absence as
            // "no transients" without warning.
            if (!app.transients.markers().empty()) {
                const std::string tr_path =
                    (entry_dir / "transientmarkers").string();
                if (!app.transients.save(tr_path)) {
                    std::fprintf(stderr,
                        "warptempo_gui: queue error: failed to write '%s'\n",
                        tr_path.c_str());
                    return;
                }
            }

            std::fprintf(stderr,
                "warptempo_gui: queued render entry %s\n",
                entry_dir.string().c_str());
            return;
        }

        // Ctrl+Alt+Shift+E: walk this source's renders/ directory and
        // render every pending entry serially in oldest-first directory-
        // name order. Esc between entries cancels the remainder; mid-
        // engine Esc presses are queued and take effect after the
        // current entry finishes (no mid-engine cancellation in chunk U).
        if (ctrl && alt && shift &&
            (keysym == XK_e || keysym == XK_E)) {
            if (app.source_audio_path.empty()) return;

            std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path queue_root = src_parent / "renders";

            std::error_code ec;
            if (!std::filesystem::is_directory(queue_root, ec)) {
                std::fprintf(stderr,
                    "warptempo_gui: render-all: no pending entries\n");
                return;
            }

            // Pending = subdir contains warpmarkers but not output.wav.
            // Bad entries (no warpmarkers) are warned and skipped — they
            // remain on disk for manual cleanup.
            std::vector<std::filesystem::path> pending;
            for (const auto& de :
                 std::filesystem::directory_iterator(queue_root, ec)) {
                if (!de.is_directory()) continue;
                const auto p = de.path();
                const bool has_wm = std::filesystem::exists(p / "warpmarkers");
                const bool has_out =
                    std::filesystem::exists(p / "output.wav") ||
                    std::filesystem::exists(p / "output.mid");
                if (!has_wm) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-all: skipping malformed "
                        "entry (no warpmarkers): %s\n",
                        p.string().c_str());
                    continue;
                }
                if (has_out) continue;
                pending.push_back(p);
            }
            // Directory-name lex sort == chronological order because the
            // YYYY-MM-DD_HHMMSS prefix is fixed-width.
            std::sort(pending.begin(), pending.end());

            if (pending.empty()) {
                std::fprintf(stderr,
                    "warptempo_gui: render-all: no pending entries\n");
                return;
            }

            app.queue_cancel_requested = false;
            app.queue_running          = true;
            const int total = static_cast<int>(pending.size());
            int rendered = 0;
            bool cancelled = false;
            for (int i = 0; i < total; ++i) {
                const auto& entry = pending[i];
                std::fprintf(stderr,
                    "warptempo_gui: rendering entry %d of %d: %s\n",
                    i + 1, total, entry.filename().string().c_str());

                RenderRequest req;
                if (!load_render_request_from_dir(
                        app.source_audio_path,
                        entry.string(),
                        app.settings_passthrough,
                        req)) {
                    // load_render_request_from_dir already logged. The
                    // entry stays pending so a later run can retry it
                    // after manual fixup.
                    continue;
                }
                do_render(req);
                ++rendered;

                // Surface any X events queued during the render (Esc
                // presses, expose, etc.) so the cancel flag becomes
                // visible to the next iteration.
                gui.drain_events();
                if (app.queue_cancel_requested) {
                    const int remaining = total - (i + 1);
                    std::fprintf(stderr,
                        "warptempo_gui: render-all cancelled, %d entries "
                        "remaining\n", remaining);
                    cancelled = true;
                    break;
                }
            }
            app.queue_running          = false;
            app.queue_cancel_requested = false;

            if (cancelled) {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                    rendered, total);
            } else {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries\n",
                    rendered, total);
            }
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

        // `t` (no modifiers) toggles transient mode.
        if (keysym == XK_t && !ctrl && !shift && !alt) {
            toggle_active_mode();
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
        if (keysym == XK_i && !ctrl) {
            if (app.active_mode == 'T') {
                // Transient mode: only Shift+I (toggle disabled) is meaningful.
                // Plain `i` (toggle inherit) has no transient analogue.
                if (shift) toggle_transient_disabled();
                return;
            }
            if (shift) toggle_disabled();
            else       toggle_inherits();
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
            // Synchronous stop so the next tick doesn't snap the playhead
            // back to the audio cursor, overwriting the target tab's
            // stored playhead.
            stop_playback_if_playing();
            refresh_active_tab_from_app();
            app.active_tab = (app.active_tab == 'A') ? 'B' : 'A';
            const Tab& target = (app.active_tab == 'A') ? app.tab_a : app.tab_b;
            app.viewport_start_sample = target.viewport_start_sample;
            app.zoom_level            = target.zoom_level;
            app.playhead_sample       = target.playhead_sample;
            // Restore the active selection from the destination tab's
            // current-mode slot. Mode itself is per-AppState (not per-tab),
            // so the destination tab's other-mode slot stays warm for any
            // future `t` flip back inside that tab.
            if (app.active_mode == 'T') {
                app.selected_markers     = target.transient_selected;
                app.last_selected_marker = target.transient_last_selected;
            } else {
                app.selected_markers     = target.warp_selected;
                app.last_selected_marker = target.warp_last_selected;
            }
            clamp_viewport_start(app, audio);
            // invalidate_waveform_area covers the top strip + waveform
            // (including the playhead column inside it); the timestamp
            // area holds the letter and the ts text.
            invalidate_waveform_area();
            invalidate_timestamp_area();
            return;
        }

        if (keysym == XK_Tab && !shift) { select_next_marker(); return; }
        if (keysym == XK_Tab && shift)  { select_prev_marker(); return; }
        if (keysym == XK_ISO_Left_Tab)  { select_prev_marker(); return; }

        // Tempo nudge. Plain `=` and `-` only — no Shift / Ctrl variants yet.
        if (keysym == XK_equal && !shift && !ctrl) {
            adjust_tempo(+0.01); return;
        }
        if (keysym == XK_minus && !shift && !ctrl) {
            adjust_tempo(-0.01); return;
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

        switch (keysym) {
        case XK_Escape: /* top-level Escape is a no-op (chunk Q) */ break;
        case XK_Left:   stop_playback_if_playing();
                        move_playhead_pixels(-1);         break;
        case XK_Right:  stop_playback_if_playing();
                        move_playhead_pixels(+1);         break;
        case XK_Up:     zoom_in();                        break;
        case XK_Down:   zoom_out();                       break;
        case XK_f:      app.follow_mode = !app.follow_mode; break;
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
        // Dialog-modal input handling: only left-button clicks on a dialog
        // button do anything; everything else (including wheel) is swallowed.
        if (app.dialog.active) {
            if (button != 1) return;
            cairo_surface_t* s = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t* c = cairo_create(s);
            const DialogLayout L = compute_dialog_layout(
                c, app.width, app.height,
                app.dialog.prompt_text.c_str(),
                app.dialog.button_labels);
            cairo_destroy(c);
            cairo_surface_destroy(s);
            for (size_t i = 0; i < L.buttons.size(); ++i) {
                const DialogButtonRect& r = L.buttons[i];
                if (x >= r.x && x < r.x + r.w &&
                    y >= r.y && y < r.y + r.h) {
                    dialog_activate_button(static_cast<int>(i));
                    return;
                }
            }
            // Click outside any button: no-op per spec.
            return;
        }
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

        if (button == 1) {
            // Any button-1 press on the waveform / top strip stops
            // playback. Per Part 4 of chunk P patch 1: the user pressed
            // a mouse button, they want attention — even a Ctrl+press on
            // empty space (a no-op for the playhead) stops the audio.
            if (inside_waveform || inside_top) stop_playback_if_playing();

            // V.A1 editor: mouse handling.
            //   click inside top strip on the editing flag: no-op
            //   click inside top strip on a different flag: switch target
            //   click anywhere else: exit edit (no commit), then fall
            //     through so the click routes through normal handling.
            if (gui_text_editor::is_active(app.top_flag_editor)) {
                if (inside_top) {
                    const int hit_now = hit_test_flag(x, y);
                    if (hit_now == app.top_flag_editor.target) {
                        return; // no-op on same flag
                    }
                    if (hit_now >= 0 && app.active_mode != 'T') {
                        enter_top_flag_edit(hit_now);
                        return;
                    }
                    // Top strip click that isn't on a flag: exit and fall
                    // through to normal handling.
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
                            app.markers.markers()[hit].time_seconds *
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
                        sample = app.transients.markers()[hit].effective_frame();
                    } else {
                        sample = static_cast<int64_t>(std::llround(
                            app.markers.markers()[hit].time_seconds *
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
                        app.playhead_drag.last_added_marker    = -1;
                        app.playhead_drag.last_added_was_fresh = false;
                    } else {
                        // Shift+press on marker: selection otherwise preserved;
                        // add hit if not already present. Track whether this
                        // was a fresh add so subsequent snap-during-drag can
                        // un-toggle only fresh additions.
                        const bool was_in = app.selected_markers.count(hit) > 0;
                        if (!was_in) {
                            app.selected_markers.insert(hit);
                            invalidate_marker_column(hit);
                            invalidate_top_strip();
                        }
                        app.last_selected_marker = hit;
                        app.playhead_drag.last_added_marker    = hit;
                        app.playhead_drag.last_added_was_fresh = !was_in;
                    }
                    int64_t sample;
                    if (app.active_mode == 'T') {
                        sample = app.transients.markers()[hit].effective_frame();
                    } else {
                        sample = static_cast<int64_t>(std::llround(
                            app.markers.markers()[hit].time_seconds *
                            static_cast<double>(sr)));
                    }
                    move_playhead_to(sample);
                    app.playhead_drag.active         = true;
                    app.playhead_drag.shift_at_press = shift;
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
                    app.playhead_drag.active               = true;
                    app.playhead_drag.shift_at_press       = shift;
                    app.playhead_drag.last_added_marker    = -1;
                    app.playhead_drag.last_added_was_fresh = false;
                }
            }
        } else if (button == 4 || button == 5) {
            // Wheel in flag strip or window margins is ignored per spec.
            if (!inside_waveform) return;
            if (ctrl && alt) {
                // Ctrl+Alt+wheel fine-pan: 2% of visible viewport per tick.
                const int64_t step = std::max<int64_t>(
                    1, samples_visible(app, audio) / 50);
                scroll_viewport(button == 4 ? -step : +step);
                return;
            }
            if (ctrl) {
                // Ctrl+wheel nudges selected markers by 1 source pixel.
                // Up (button 4) = earlier, Down (button 5) = later, to
                // match the Ctrl+Left / Ctrl+Right convention.
                nudge_selected_markers(button == 4 ? -1 : +1);
                return;
            }
            if (alt) {
                const int64_t step = std::max<int64_t>(
                    1, samples_visible(app, audio) / 10);
                scroll_viewport(button == 4 ? -step : +step);
            } else {
                if (button == 4) zoom_out();
                else             zoom_in();
            }
        }
    });

    gui.set_on_button_release([&](unsigned int button, int, int, unsigned int) {
        if (app.dialog.active) return;
        if (button != 1) return;
        if (app.playhead_drag.active) {
            app.playhead_drag = PlayheadDragState{};
            return;
        }
        if (!app.drag.active) return;
        commit_drag();
    });

    gui.set_on_motion([&](int mouse_x, int /*mouse_y*/, unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        if (app.dialog.active) return;
        if (app.playhead_drag.active) {
            // Modifier changes mid-drag are ignored: only shift_at_press
            // governs gesture behavior. Left button must still be held; if
            // it's not, the release was lost — terminate the drag.
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
            const int hit = hit_test_marker_line(mouse_x);

            bool selection_changed = false;
            int64_t new_playhead;

            if (hit >= 0) {
                if (app.active_mode == 'T') {
                    new_playhead = app.transients.markers()[hit].effective_frame();
                } else {
                    new_playhead = static_cast<int64_t>(std::llround(
                        app.markers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
                }
                if (!app.playhead_drag.shift_at_press) {
                    // Plain drag: replace selection with {hit} unless it's
                    // already the sole selection.
                    const bool already_sole =
                        app.selected_markers.size() == 1 &&
                        app.selected_markers.count(hit) > 0;
                    if (!already_sole) {
                        set_single_selection(hit);
                        selection_changed = true;
                    }
                } else {
                    // Shift drag: if snapping to a different marker than the
                    // gesture's last-added, un-toggle the previous (only if
                    // fresh) and add the new.
                    if (app.playhead_drag.last_added_marker != hit) {
                        const int prev = app.playhead_drag.last_added_marker;
                        if (prev != -1 &&
                            app.playhead_drag.last_added_was_fresh) {
                            if (app.selected_markers.erase(prev) > 0) {
                                invalidate_marker_column(prev);
                                if (app.last_selected_marker == prev) {
                                    // repair_last_selected isn't in scope
                                    // here; match its behavior inline.
                                    if (app.selected_markers.empty()) {
                                        app.last_selected_marker = -1;
                                    } else {
                                        app.last_selected_marker =
                                            *app.selected_markers.rbegin();
                                    }
                                }
                                selection_changed = true;
                            }
                        }
                        const bool was_in = app.selected_markers.count(hit) > 0;
                        if (!was_in) {
                            app.selected_markers.insert(hit);
                            invalidate_marker_column(hit);
                            selection_changed = true;
                        }
                        app.last_selected_marker = hit;
                        app.playhead_drag.last_added_marker    = hit;
                        app.playhead_drag.last_added_was_fresh = !was_in;
                    }
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
            if (selection_changed) invalidate_top_strip();
            return;
        }
        if (!app.drag.active) return;
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
            ? static_cast<int>(app.transients.markers().size())
            : static_cast<int>(app.markers.markers().size());
        if (hit_idx >= 0 && hit_idx < n) {
            int64_t ph;
            if (transient_drag) {
                ph = app.transients.markers()[hit_idx].effective_frame();
            } else {
                ph = static_cast<int64_t>(std::llround(
                    app.markers.markers()[hit_idx].time_seconds * sr_d));
            }
            if (ph != app.playhead_sample) {
                const double old_px = playhead_pixel_x(app, audio);
                app.playhead_sample = ph;
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

        // Companion files: discover paths, create .warpmarkers if missing.
        // .settings is GUI-owned now — not pre-created on load; first save
        // materializes it.
        std::filesystem::path apath(path);
        std::filesystem::path parent = apath.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        const std::filesystem::path wm_path  = parent / ".warpmarkers";
        const std::filesystem::path tm_path  = parent / ".transientmarkers";
        const std::filesystem::path set_path = parent / ".settings";
        app.warpmarkers_path      = wm_path.string();
        app.transientmarkers_path = tm_path.string();
        app.settings_path         = set_path.string();
        app.source_audio_path     = path;

        create_if_missing(wm_path, "00:00.000|1.00\n");

        // Load the markers file. Parse failures are non-fatal: we log each
        // error to stderr and leave app.markers empty. The GUI still works
        // as a waveform viewer.
        app.markers.clear();
        app.transients.clear();
        app.selected_markers.clear();
        app.transient_selected_markers.clear();
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
        const bool markers_ok = app.markers.load(wm_path.string());
        if (!markers_ok) {
            for (const auto& err : app.markers.errors()) {
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
                         app.markers.markers().size(),
                         wm_path.string().c_str());
        }

        // Load .transientmarkers if present. Missing file is fine — the
        // transient list is just empty. Parse errors are logged to stderr;
        // the warp side stays usable regardless.
        if (std::filesystem::exists(tm_path)) {
            const bool tr_ok = app.transients.load(tm_path.string());
            if (!tr_ok) {
                for (const auto& err : app.transients.errors()) {
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
                             app.transients.markers().size(),
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
        Tab default_tab;
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
                             Tab& dst) {
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

    // Follow-mode scroll: when the playhead drifts off the visible viewport
    // during playback, advance the viewport by one pixel-page so the cursor
    // stays in view. Only the first move beyond vp_end triggers a scroll,
    // and it lands the playhead ~10% into the new viewport so there's room
    // to keep playing.
    auto follow_scroll_if_needed = [&]() {
        const int64_t visible = samples_visible(app, audio);
        if (visible <= 0) return;
        const int64_t vp_end = app.viewport_start_sample + visible;
        if (app.playhead_sample < app.viewport_start_sample ||
            app.playhead_sample >= vp_end) {
            const int64_t lead = visible / 10;
            const int64_t old_vp = app.viewport_start_sample;
            app.viewport_start_sample =
                std::max<int64_t>(0, app.playhead_sample - lead);
            clamp_viewport_start(app, audio);
            if (app.viewport_start_sample != old_vp) invalidate_waveform_area();
        }
    };

    // Tick: runs once per event-loop iteration. During playback, snapshots
    // the audio thread's cursor and mirrors it into the main-thread playhead,
    // invalidating just the columns and timestamp that changed. Also
    // detects natural end-of-playback via the atomic playing flag.
    gui.set_on_tick([&]() {
        // Blink the editor cursor independently of playback. Compare the
        // current visibility against the last painted state and invalidate
        // the top strip when it flips. Cheap: top_strip is small.
        if (gui_text_editor::is_active(app.top_flag_editor)) {
            const bool now_visible =
                gui_text_editor::cursor_visible_now(app.top_flag_editor);
            if (now_visible != app.top_flag_editor_blink_last) {
                app.top_flag_editor_blink_last = now_visible;
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
        if (app.is_playing || playback.is_playing()) return 16;
        // While the top-flag editor is active, wake periodically so the
        // cursor blink can flip. ~125ms gives ample resolution on a
        // 500ms half-period without burning CPU.
        if (gui_text_editor::is_active(app.top_flag_editor)) return 125;
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
