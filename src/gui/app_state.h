#pragma once

#include "render.h"
#include "text_editor.h"
#include "transientmarkers.h"
#include "warpmarkers.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

class GuiAudio;

// Number of numeric zoom levels in the table (defined in main.cpp). Used by
// max_valid_numeric_level and by zoom_in / zoom_out for sentinel comparisons.
constexpr int kNumZoomLevels = 8;

// Sentinel for the fit-file level ("whole file visible"). Computed at zoom /
// resize time, not stored as a fixed ms/pixel.
constexpr int kFitFileLevel = kNumZoomLevels;

// X.7.8b-2: hoisted from main.cpp's anonymous namespace so the hit_test_*
// free functions (in app_state.cpp) and the GuiInputHandler mouse handler
// (in input_handler.cpp) can reach them.
constexpr int kMarkerHitHalfPx    = 3;
constexpr int kDoubleClickMs      = 300;
constexpr int kDoubleClickPixels  = 5;

// Op-kind tag carried on every undo/redo entry. Marker selection collapses
// per kind on undo: Create restores selection to the just-created marker,
// Destroy restores to the hint-last-selected captured pre-op so the user
// gets back the focus they had, Move/Other use the snapshot's last_selected
// to reproduce playhead behavior; count-preserving ops split Move vs Other
// so Move can restore the "what just moved" selection.
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
    std::vector<GuiWarpMarker>      snapshot;
    std::vector<GuiTransientMarker> transient_snapshot;
    char                      op_mode              = 'W';
    OpKind                    op_kind              = OpKind::Other;
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
    // can push it onto the undo stack when motion landed; discarded on
    // commit when no motion occurred (DragState is reset wholesale there).
    std::vector<GuiWarpMarker>      pre_drag_snapshot;
    std::vector<GuiTransientMarker> pre_drag_transient_snapshot;
    // Pre-drag last_selected for the undo hint; carried onto the entry at commit.
    int                    pre_drag_last_selected = -1;
    // Index of the marker that was clicked to start the drag. Used to track
    // the playhead during motion so the audio cursor follows the grabbed
    // marker as it moves.
    int                    hit_marker           = -1;
    // True when begin_drag found the hit marker outside the current
    // selection. The selection-collapse-to-{hit} mutation is deferred until
    // motion is actually observed, so a Ctrl+click without drag leaves the
    // selection untouched. Cleared on the first moved transition.
    bool                   pending_collapse_to_hit = false;
    // Which list this drag operates on (chunk S.2.2). The motion / commit
    // handlers dispatch on this so a drag started in transient mode
    // mutates the transient list.
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

// State for the plain/Shift left-button playhead-drag gesture. The drag
// only positions the playhead (with a 3px snap-to-marker magnet); selection
// is set at press time and never mutated by motion. The gesture ends on
// release (or on Escape, which ends at current position).
//
// Brief six: a click that reseats the playhead in the waveform area keeps
// playback alive (the press site reseeks the audio device to the new
// position). A drag — i.e. button-1 motion observed while the gesture is
// active — converts the gesture into a scrub and stops playback the moment
// motion is detected. `was_playing_at_press` carries the press-time playback
// state into the motion handler; `drag_motion_stopped_audio` is the one-shot
// guard so the stop fires at most once per drag.
struct PlayheadDragState {
    bool active                    = false;
    bool was_playing_at_press      = false;
    bool drag_motion_stopped_audio = false;
    // Marker index the press landed on, or -1 if pressed on empty space;
    // release uses it to suppress the snap-action when no actual drag
    // occurred.
    int  press_marker_idx          = -1;
};

// Cross-file flag scan result. `valid` is false when the requested flag
// (b= or e=) is not set on any marker. Returned by find_flag, which scans
// the warp list first then the transient list — `transient` distinguishes
// which list `idx` indexes into. `frame` is the marker's effective sample
// frame (computed at lookup time so callers don't need a sample-rate).
struct FlagLoc {
    bool    valid     = false;
    bool    transient = false;
    int     idx       = -1;
    int64_t frame     = 0;
};

// V.A3b hover popup state. A popup-eligible warp marker (pass marker or
// label_ref) under the cursor for kHoverDelayMs becomes a tooltip showing
// the resolved tempo. The motion handler sets `marker_index` + `entry_time`
// when the cursor first lands on an eligible rect; the tick handler flips
// `visible` when the dwell threshold is crossed; mutation paths /
// dismiss conditions clear the whole struct.
//
// `cached_text` is the popup's content string, computed on rect-entry
// (when the dwell timer starts) and rendered at delay-completion.
// Precomputing during the otherwise-idle delay window hides the
// label_ref math (def lookup, frame-distance ratio) behind time the
// user is already waiting through, so the popup-show frame doesn't
// stutter. Discarded with the rest of the struct on rect-exit.
struct HoverPopupState {
    int         marker_index = -1;
    bool        visible      = false;
    std::string cached_text;
    std::chrono::steady_clock::time_point entry_time{};
};

// What action triggered the modal prompt; the activate-response dispatch
// switches on this together with the response key. Save/Discard/Cancel
// applies to the unsaved-work prompts (CLOSE_WINDOW, REVERT_TO_BLANK).
enum class DialogTrigger {
    CLOSE_WINDOW,
    REVERT_TO_BLANK,
};

// In-window modal prompt state. When `active` is true, the bottom strip
// overlays the prompt's text and response options in place of the
// timestamp / tab letter / dirty indicator / render-view filename.
// Input is owned by the prompt: only the response keys (and Esc, which
// activates the rightmost response) do anything; everything else is
// swallowed. `response_keys` holds lowercase letters; the activator
// lowercases incoming keypresses before comparing.
struct PromptState {
    bool                     active = false;
    std::string              text;
    std::vector<char>        response_keys;     // lowercase
    std::vector<std::string> response_labels;   // e.g. "[S]ave"
    DialogTrigger            trigger = DialogTrigger::CLOSE_WINDOW;
};

// Navigational bookmark. Holds a snapshot of the three fields that define
// what the user sees and where playback would start. Not in the undo domain.
//
// Chunk S.2.2: each tab also carries per-mode selection slots so switching
// tabs (Ctrl+Tab) and switching modes (`t`) both restore the right
// selection set for the destination cell. The active selection lives in
// AppState; these slots are the persistent snapshots.
//
// Brief J.1: the same struct is reused by RenderViewEntry::state to carry
// per-render persisted view-state across render-view exit/enter and
// batch-nav. Render-view entries leave the viewport/zoom/playhead fields
// at default in J.1 (those still flow through the live AppState fields
// and the .rendersettings sidecar; J.2 will reroute them through `state`).
struct ViewState {
    int64_t viewport_start_sample = 0;
    int     zoom_level            = 0;
    int64_t playhead_sample       = 0;

    std::set<int> warp_selected;
    int           warp_last_selected      = -1;
    std::set<int> transient_selected;
    int           transient_last_selected = -1;
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
    GuiWarpMarkers  warpmarkers;

    // Parsed transient markers (chunk S.2.2). Authored by the GUI but not
    // yet consumed by the render pipeline (S.3 will wire that up).
    GuiTransientMarkers transientmarkers;

    // Multi-selection set + focus. `last_selected_marker` is either -1 or
    // a member of `selected_markers`; keyed operations (Tab cycling, `j`)
    // anchor on it.
    //
    // Chunk S.2.2: this pair holds the *active* selection — i.e. for the
    // current tab + current `active_mode`. The persistent per-tab per-mode
    // slots live on ViewState and are saved/restored on mode/tab transitions.
    std::set<int> selected_markers;
    int           last_selected_marker = -1;

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

    // V.A3b hover-popup state. See HoverPopupState above.
    HoverPopupState   hover_popup;

    // V.A3b Addendum 3: cursor screen position from the last on_motion
    // event. Used by recompute_hover_at_cursor() to re-evaluate hover
    // after a viewport mutation (when the cursor is stationary but rects
    // have shifted). -1 means "no motion seen yet".
    int               last_mouse_x = -1;
    int               last_mouse_y = -1;

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
    ViewState tab_a;
    ViewState tab_b;
    char active_tab = 'A';

    // Pass-through entries read from .settings on load and re-emitted
    // verbatim on save. Preserves original order. Never interpreted by
    // the GUI; exists so the wrapper script's keys survive a round-trip.
    std::vector<std::pair<std::string, std::string>> settings_passthrough;

    // Bottom-strip command prompt. Active only when a close / revert /
    // re-detect gesture fires while a confirmation is required. See Part
    // 2 of chunk Q (originally a centered modal dialog; brief H.5 moves
    // the same modal semantics into the bottom strip).
    PromptState prompt;

    // Top-flag text editor (V.A1). Active only when editing a flag rect
    // in warp mode. The editor owns the keyboard while active and
    // overlays a custom rect + cursor on top of render_flags.
    text_editor::State top_flag_editor;
    // Last-painted cursor visibility, so the tick can detect a flip and
    // invalidate the top strip without redundant repaints.
    bool top_flag_editor_blink_last = false;

    // Render-queue state (chunk U). `queue_running` is true only inside the
    // Ctrl+Alt+R queue walker. The Esc handler checks it to scope the
    // cancel binding away from normal interaction. `queue_cancel_requested`
    // is set by Esc during a queue run and read between entries.
    bool queue_running           = false;
    bool queue_cancel_requested  = false;

    // Non-interactive bottom-strip status text. Set by long-running
    // operations (currently only the multi-render queue runner) so the
    // user has visual feedback while no other UI is updating. Empty
    // means "no status — render the timestamp normally." Mutually
    // exclusive with prompt.active in practice (the queue runner can't
    // fire while a prompt is up).
    std::string queue_progress_text;

    // Chunk W: in-memory queue of pending renders. Ctrl+E pushes a
    // snapshot of the current authoring state onto the back of this list;
    // Ctrl+Alt+E consumes it, materializing one batch folder per execution
    // with one rendered output per queued entry. The list is session-only:
    // discarded on app close, never written to disk between sessions.
    // Settings are not snapshotted per-entry — all entries render against
    // the GUI's live `settings_passthrough` at execution time, mirroring
    // the chunk-U convention.
    struct QueuedRender {
        std::string                source_audio_path;
        std::vector<GuiWarpMarker>     markers;
        std::vector<GuiTransientMarker>  transients;
    };
    std::vector<QueuedRender> queued_renders;

    // V.B iteration mode. Toggled by plain `i` in warp mode (no-op in
    // transient mode). Session-only; survives mode-switches but is lost
    // on app close. When true, hover popups are suppressed and a
    // persistent iteration popup is rendered above every owning
    // marker's flag rect.
    bool iteration_mode_enabled = false;

    // Brief X.2 BPM mode. Toggled by plain `m` in warp mode. Mutually
    // exclusive with iteration_mode_enabled (toggling one ON forces the
    // other OFF). Session-only. The popup owner is identified at runtime
    // by walking markers for bpm_is_popup_owner=true; at most one marker holds
    // the flag at a time, maintained as an invariant by the toggle.
    bool bpm_mode_enabled = false;

    // Chunk W: render analysis mode. Plain `r` toggles between source-view
    // (authoring) and render-view (read-only auditioning of rendered
    // outputs from <source_parent>/renders/). All authoring state above
    // is preserved untouched while render-view is active; this struct
    // holds the parallel context that drives render-view's display.
    bool render_view_enabled = false;
    // Path to the last-displayed render's .wav, persisted across toggle
    // off/on cycles within a session. Empty before the first entry; reset
    // to whatever path was active when the previous toggle-off fired.
    std::string last_render_view_path;
    // One entry in the flat list of valid renders enumerated on toggle-in.
    struct RenderViewEntry {
        std::filesystem::path batch_folder;     // <source_parent>/renders/<i>_<tag>
        std::string           basename;         // e.g. "01" (no extension)
        std::filesystem::path wav_path;         // batch_folder / (basename + ".wav")

        // Brief J.1: per-entry persisted view-state across render-view
        // exit/enter and batch-nav. Selection + sub-mode are valid only
        // when the wav still has the same (size, mtime) as when stashed;
        // mismatch on reload drops them silently. The viewport/zoom/
        // playhead fields on `state` are unused in J.1 — render-view's
        // viewport state continues to flow through the live AppState
        // fields and the .rendersettings sidecar; J.2 will reroute them
        // through `state`.
        ViewState state;

        // Stat-tuple key for selection validity. Captured when stashed,
        // compared against the current file's stat on re-load.
        uintmax_t     persisted_size             = 0;
        int64_t       persisted_mtime            = 0;
    };
    std::vector<RenderViewEntry> render_view_list;
    int                          render_view_index = -1;     // -1 = unset
    // The current render's loaded markers + transients, parsed from
    // sibling `<basename>.renderwarpmarkers` /
    // `<basename>.rendertransientmarkers`.
    std::vector<GuiWarpMarker>       render_view_markers;
    std::vector<GuiTransientMarker>    render_view_transients;
    // Source-frame mapping of the current render: F_begin..F_end (source
    // sample-rate frames) is what the render's full audio covers. When the
    // render's warpmarkers carry no `b=` flag, F_begin is 0; when it carries
    // no `e=` flag, F_end is the source's total_frames. Used by the render
    // -view waveform mapping and the timestamp readout.
    int64_t                      render_view_src_F_begin = 0;
    int64_t                      render_view_src_F_end   = 0;
    // Source audio's sample rate / total frames at the time render-view
    // was entered. Cached so timestamp computation and trim resolution
    // don't have to peek at the swapped-out source GuiAudio.
    int                          render_view_src_sr      = 0;
    int64_t                      render_view_src_total   = 0;
};

// Geometry helpers — definitions live at file scope in main.cpp. Declared
// here so viewport.cpp can call them.
GuiRect waveform_area(const AppState& a);
GuiRect top_strip_area(const AppState& a);
int64_t samples_visible(const AppState& a, const GuiAudio& audio);
double  current_samples_per_pixel(const AppState& a, const GuiAudio& audio);
void    clamp_viewport_start(AppState& a, const GuiAudio& audio);
double  playhead_pixel_x(const AppState& a, const GuiAudio& audio);
int     max_valid_numeric_level(int waveform_width_px,
                                int64_t total_frames,
                                int sample_rate);
std::pair<long long, long long> compute_trim_samples(
    const std::vector<GuiWarpMarker>& warp_markers,
    const std::vector<GuiTransientMarker>& transients,
    int sample_rate, long long total_frames);
GuiRect timestamp_invalidate_rect(int window_height, int window_width,
                                  bool wide_strip);
GuiRect playhead_invalidate_rect(const GuiRect& area, double px_x);
bool    rects_intersect(GuiRect a, GuiRect b);
GuiRect union_rect(GuiRect a, GuiRect b);

// X.7.7: promoted from a lambda in main(). Looks up `key` in
// app.settings_passthrough and returns its value, or `dflt` if the key
// is not present. Used by the `t`-mode entry path in GuiTabMode to gate
// on engine= without a typed parser.
std::string settings_get(const AppState& app, const std::string& key,
                         const std::string& dflt);

// X.7.8a: promoted from a lambda in main(). True iff the bottom strip
// must paint full-width — when the prompt overlay is active or when a
// queue progress message is showing. Drives timestamp_invalidate_rect
// width selection in the redraw path. Pure read against AppState.
bool bottom_strip_wide(const AppState& app);

// X.7.8b-2: promoted from lambdas in main(). Mode-aware hit-tests against
// the visible marker / flag / popup geometry. Bodies live in app_state.cpp
// and pull in cairo + paint_handler.h for the popup-rect math; the
// signatures stay free of cairo so the header keeps a clean include list.
//
// hit_test_marker_line: scan the active list (render-view markers in
// render-view; transients in 'T' mode; warp markers otherwise) and return
// the index whose pixel column is within kMarkerHitHalfPx of `mouse_x`,
// or -1 if no marker line is within reach.
int hit_test_marker_line(const AppState& app, const GuiAudio& audio,
                         int mouse_x);

// hit_test_flag: scan the active flag-pack rects in the top strip and
// return the marker index under (mouse_x, mouse_y), or -1. Returns -1
// in render-view's transient sub-view (no flag rects there).
int hit_test_flag(const AppState& app, const GuiAudio& audio,
                  int mouse_x, int mouse_y);

// hit_test_iter_popup: V.B iteration-popup hit-test. Returns the marker
// index whose iteration popup contains (mouse_x, mouse_y), or -1. Always
// -1 when iteration mode is off, in transient mode, or in render-view.
int hit_test_iter_popup(const AppState& app, const GuiAudio& audio,
                        int mouse_x, int mouse_y,
                        double* out_text_left_x = nullptr);

// hit_test_bpm_popup: brief X.2 BPM-popup hit-test. Mirrors
// hit_test_iter_popup. Iter and BPM modes are mutually exclusive so at
// most one of these returns >= 0 for a given (x, y).
int hit_test_bpm_popup(const AppState& app, const GuiAudio& audio,
                       int mouse_x, int mouse_y,
                       double* out_text_left_x = nullptr);

// X.7.8b-3: promoted from a lambda in main(). True iff the warp marker
// at `idx` is hover-popup-eligible — i.e. its rect doesn't already
// display a numeric tempo (pass markers and label_ref markers qualify;
// owning markers don't). Render-view honors the loaded render's
// markers regardless of the pre-toggle mode; source-view requires warp
// mode with iteration mode off. Always false in transient mode (no
// pass concept).
bool popup_eligible_marker(const AppState& app, int idx);
