#include "gui_audio.h"
#include "gui_markers.h"
#include "gui_playback.h"
#include "gui_render.h"
#include "gui_render_pipeline.h"
#include "gui_text_display.h"
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
constexpr double   kFlagFontSize      = 13.0;

// Half-width in pixels of the selection-hit window when clicking on a marker.
constexpr int kMarkerHitHalfPx = 3;

// Double-click detection thresholds. X11 doesn't synthesize DoubleClick; we
// roll it from ButtonPress timing + position deltas.
constexpr int kDoubleClickMs      = 300;
constexpr int kDoubleClickPixels  = 5;

// Time the cursor must dwell on a popup-eligible flag rect before the
// hover popup appears. Distinct from kDoubleClickMs (point-event window
// vs continuous-state duration) even though they currently share a value.
constexpr int kHoverDelayMs       = 500;

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

// Pixel gap between the popup's top edge and the flag rect's top edge
// (mirrors gui_text_display::kVerticalGapPx). Used for iteration popup
// hit-testing and vertical placement.
constexpr double kIterPopupVerticalGapPx = 4.0;
// V.B Addendum 2: extra inner padding on top/bottom of the iteration
// popup's bg rect (mirrors gui_render.cpp::kVPadExtraPx and
// gui_text_display.cpp::kVPadExtraPx). The flag hit-rect already grew by
// 2*kVPadExtraPx vertically, so the iter popup's hit_rect inherits that
// height and the gap to the flag rect is preserved automatically; the
// edit-state text baseline is shifted up by kIterPopupVPadExtraPx so the
// pending text clears the popup rect's bottom inner padding.
constexpr double kIterPopupVPadExtraPx = 1.0;

// V.B iteration mode: format the popup's bracket text for marker `m`.
// "[]" when both iter values are NaN; "[%+0.2f,%+0.2f]" when set.
inline std::string format_iter_bracket_text(const GuiMarker& m) {
    if (std::isnan(m.iter_start) || std::isnan(m.iter_end)) {
        return "[]";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%+0.2f,%+0.2f]",
                  m.iter_start, m.iter_end);
    return buf;
}

// V.B iteration mode: an owning marker (tempo_inherits=false AND no
// label_ref) gets a persistent iteration popup. Pass markers and
// label_ref markers are excluded; disabled status does not matter.
inline bool iter_popup_eligible_marker(const GuiMarker& m) {
    return !m.tempo_inherits && m.label_ref.empty();
}

// On-screen popup geometry for one iteration popup. `flag_rect` is the
// underlying flag's rect (used to anchor); `hit_rect` is the clickable
// region; `text` is the current popup text (for paint and seed-on-edit).
struct IterPopupHit {
    int          marker_index;
    GuiRect      flag_rect;
    GuiRect      hit_rect;
    std::string  text;
};

// Compute iteration popup hit-rects for visible owning markers in
// `top_strip_area`. Uses `compute_flag_hit_rects` for the underlying flag
// positions (so popups inherit the flag-strip greedy elision). Each
// popup sits kIterPopupVerticalGapPx above its flag's top edge. The
// hit_rect height matches the flag's height; width is the monospace
// extent of the popup's current text plus a small horizontal pad so
// edits with longer pending strings stay clickable.
inline std::vector<IterPopupHit> compute_iter_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<IterPopupHit> out;
    auto rects = compute_flag_hit_rects(
        cr, top_strip_area, markers,
        viewport_start_sample, viewport_end_sample,
        sample_rate, font_size);
    if (rects.empty()) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    // The widest possible iteration text drives a uniform hit-rect width
    // so popups don't visibly jiggle in size as values change. Matches
    // the [%+0.2f,%+0.2f] format with single-digit integer parts.
    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "[+0.00,+0.00]", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    for (const auto& r : rects) {
        const int idx = r.marker_index;
        if (idx < 0 || idx >= static_cast<int>(markers.size())) continue;
        if (!iter_popup_eligible_marker(markers[idx])) continue;
        IterPopupHit h;
        h.marker_index = idx;
        h.flag_rect.x = static_cast<int>(std::lround(r.x));
        h.flag_rect.y = static_cast<int>(std::lround(r.y));
        h.flag_rect.w = static_cast<int>(std::lround(r.w));
        h.flag_rect.h = static_cast<int>(std::lround(r.h));
        h.text = format_iter_bracket_text(markers[idx]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, h.text.c_str(), &ext);
        const int popup_w =
            static_cast<int>(std::ceil(uniform_ext.x_advance + 2 * hl_pad));
        const int popup_h = h.flag_rect.h;
        h.hit_rect.x = h.flag_rect.x;
        h.hit_rect.y = h.flag_rect.y -
            static_cast<int>(std::lround(kIterPopupVerticalGapPx)) -
            popup_h;
        h.hit_rect.w = popup_w;
        h.hit_rect.h = popup_h;
        out.push_back(h);
    }
    cairo_restore(cr);
    return out;
}

// V.A3b hover-popup text. Replicates parser.cpp's resolution math so the
// popup matches what the engine emits into the .timemap. Pass markers
// emit "= TEMPO" or "= TEMPO*SCALE" (single equals; resolved tempo of
// the nearest prior owning marker). Label_ref markers emit
// "~= BASE*COMBINED_SCALE" (tilde-equals; mirrors parser.cpp lines
// 612/651). BASE is rendered at 2 decimals; COMBINED_SCALE is
// `def_scale * multiplier` when the def has a typed scale, else just
// `multiplier`, rendered at 4 decimals. Returns "" when the marker
// doesn't qualify for a hover popup (owning, missing def, malformed).
inline std::string compute_hover_popup_text(
    const std::vector<GuiMarker>& mv, int idx, int sample_rate) {
    if (idx < 0 || idx >= static_cast<int>(mv.size())) return "";
    const GuiMarker& m = mv[idx];

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

        const GuiMarker& def = mv[def_idx];
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

        // settings.scale cancels in parser.cpp's line 648 expression:
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

// State for the plain/Shift left-button playhead-drag gesture. The drag
// only positions the playhead (with a 3px snap-to-marker magnet); selection
// is set at press time and never mutated by motion. The gesture ends on
// release (or on Escape, which ends at current position).
struct PlayheadDragState {
    bool active = false;
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
// applies to the unsaved-work prompts (CLOSE_WINDOW, REVERT_TO_BLANK);
// Detect/Cancel applies to the re-detect confirmation (DETECT_TRANSIENTS).
enum class DialogTrigger {
    CLOSE_WINDOW,
    REVERT_TO_BLANK,
    DETECT_TRANSIENTS,
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
    // slots live on ViewState and are saved/restored on mode/tab transitions.
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
    gui_text_editor::State top_flag_editor;
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

    // Chunk W: in-memory queue of pending renders. Ctrl+Alt+E pushes a
    // snapshot of the current authoring state onto the back of this list;
    // Ctrl+Alt+R consumes it, materializing one batch folder per execution
    // with one rendered output per queued entry. The list is session-only:
    // discarded on app close, never written to disk between sessions.
    // Settings are not snapshotted per-entry — all entries render against
    // the GUI's live `settings_passthrough` at execution time, mirroring
    // the chunk-U convention.
    struct QueuedRender {
        std::string                source_audio_path;
        std::vector<GuiMarker>     markers;
        std::vector<GuiTransient>  transients;
    };
    std::vector<QueuedRender> queued_renders;

    // V.B iteration mode. Toggled by plain `i` in warp mode (no-op in
    // transient mode). Session-only; survives mode-switches but is lost
    // on app close. When true, hover popups are suppressed and a
    // persistent iteration popup is rendered above every owning
    // marker's flag rect.
    bool iteration_mode_enabled = false;

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
    std::vector<GuiMarker>       render_view_markers;
    std::vector<GuiTransient>    render_view_transients;
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
    const int col = static_cast<int>(std::floor(px_x + 0.5));
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

GuiRect timestamp_invalidate_rect(int window_height, int window_width,
                                  bool wide_strip) {
    if (wide_strip) {
        return GuiRect{0, window_height - kTimestampRegionH,
                       window_width, kTimestampRegionH};
    }
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
    const ViewState& tab_a,
    const ViewState& tab_b,
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
    // Chunk W: parked source audio. Populated only while
    // app.render_view_enabled is true — std::move'd off `audio` on
    // toggle-in and std::move'd back on toggle-out so the source
    // doesn't have to be re-read from disk. Default-constructed
    // (empty / total_frames() == 0) when render-view is off.
    GuiAudio     source_audio_held;
    GuiPlayback  playback;
    GuiX11       gui;
    WaveformCache wf_cache;
    if (!gui.init(app.width, app.height, "Warptempo")) {
        return 1;
    }

    // -- Trim helpers --------------------------------------------------------

    auto trim_range = [&]() -> std::pair<int64_t, int64_t> {
        if (audio.total_frames() <= 0) return {0, 0};
        if (app.render_view_enabled) {
            return compute_trim_samples(
                app.render_view_markers, app.render_view_transients,
                audio.sample_rate(), audio.total_frames());
        }
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

    auto bottom_strip_wide = [&]() -> bool {
        return app.prompt.active || !app.queue_progress_text.empty();
    };

    auto invalidate_timestamp_area = [&]() {
        const GuiRect t = timestamp_invalidate_rect(
            app.height, app.width, bottom_strip_wide());
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
        // Brief E: playhead motion may flip a flag's outline on or off when
        // the playhead column matches a marker column. The flag rect can
        // extend well to the right of the playhead's 17-px invalidate band,
        // so the top strip must be invalidated separately to repaint the
        // affected flag(s) cleanly.
        const GuiRect ts = top_strip_area(app);
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h + 1);
    };

    // V.A3b Addendum 3: forward-declared so the viewport-mutator lambdas
    // below can invoke it. The body is assigned later (after hit_test_flag
    // and clear_hover_popup are in scope). Guarded with a truthiness check
    // because callbacks are wired after this assignment, so by the time
    // any viewport mutator fires from input, the body is in place.
    std::function<void()> recompute_hover_at_cursor;

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
        // V.A3b Addendum 3: viewport may have shifted (Home/End or any
        // playhead jump that pushed the viewport). Re-evaluate hover at
        // the cursor's last known coords.
        if (viewport_changed && recompute_hover_at_cursor) {
            recompute_hover_at_cursor();
        }
        if (playback.is_playing()) playback.resync_predictor();
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
        // Flags / hover popup live in the top strip — rect positions
        // change when the viewport scale changes.
        const GuiRect ts = top_strip_area(app);
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
        // V.A3b Addendum 3: rects shifted under the (possibly stationary)
        // cursor — re-evaluate hover.
        if (recompute_hover_at_cursor) recompute_hover_at_cursor();
        if (playback.is_playing()) playback.resync_predictor();
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
            // Flag positions move with the viewport; the hover popup rides
            // along, so the top strip must repaint too.
            const GuiRect ts = top_strip_area(app);
            gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
            // V.A3b Addendum 3: rects shifted under the (possibly
            // stationary) cursor — re-evaluate hover.
            if (recompute_hover_at_cursor) recompute_hover_at_cursor();
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
            const GuiRect ts = top_strip_area(app);
            gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
            // V.A3b Addendum 3: rects shifted under the (possibly
            // stationary) cursor — re-evaluate hover.
            if (recompute_hover_at_cursor) recompute_hover_at_cursor();
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

            // In render-view the audio buffer is already render-domain
            // (trim already baked in at render time, b=/e= flags stripped
            // from the .renderwarpmarkers/.rendertransientmarkers
            // sidecars). The
            // source's authoring markers carry b=/e= in source-frame
            // coordinates that don't map onto the rendered audio's
            // timeline, so feeding them to compute_trim_samples here
            // produces a patchy color split. Use the render-view markers
            // instead, which collapse to [0, total_frames] and dim
            // nothing.
            const auto trim = app.render_view_enabled
                ? compute_trim_samples(
                      app.render_view_markers, app.render_view_transients,
                      sr, audio.total_frames())
                : compute_trim_samples(
                      app.markers.markers(), app.transients.markers(),
                      sr, audio.total_frames());
            const int64_t trim_begin = trim.first;
            const int64_t trim_end   = trim.second;
            const TrimRange trim_struct{trim_begin, trim_end};

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
                                        kWaveform, dim(kWaveform));
                    } else if (rc >= 2) {
                        const int ch_h = (cache_area.h - kChannelGapPx) / 2;
                        const GuiRect ch0{0, 0, cache_area.w, ch_h};
                        const GuiRect ch1{0, ch_h + kChannelGapPx,
                                          cache_area.w, ch_h};
                        render_waveform(ccr, ch0, audio, 0,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveform, dim(kWaveform));
                        render_waveform(ccr, ch1, audio, 1,
                                        vp_start, vp_end,
                                        trim_begin, trim_end,
                                        kWaveform, dim(kWaveform));
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
                if (app.render_view_enabled) {
                    // Render-view: dark blue base, sky-tint when selected.
                    // The render's warpmarkers list is strict-monotonic on
                    // time_seconds (engine-written), so render_markers'
                    // usual ordering assumption holds. Selection is
                    // visual-only — it does not flow into commit.
                    // Brief F Section 3: when sub-mode is 'T', paint the
                    // render's transient list using the transient renderer
                    // (matches source-view's transient appearance).
                    if (app.active_mode == 'T') {
                        render_transient_markers(
                            cr, area, app.render_view_transients,
                            vp_start, vp_end, sr,
                            trim_struct);
                    } else {
                        render_markers(cr, area, app.render_view_markers,
                                       vp_start, vp_end, sr,
                                       trim_struct);
                    }
                } else if (app.active_mode == 'T') {
                    render_transient_markers(
                        cr, area, app.transients.markers(),
                        vp_start, vp_end, sr,
                        trim_struct);
                } else {
                    render_markers(cr, area, app.markers.markers(),
                                   vp_start, vp_end, sr,
                                   trim_struct);
                }
                const auto m1 = clock::now();
                t_markers_ms =
                    std::chrono::duration<double, std::milli>(m1 - m0).count();
            }

            // Brief E: precompute the playhead's pixel column so flag
            // renderers can light the outline of the marker the playhead
            // sits on. Same value is reused by render_playhead below.
            const double px_x = playhead_pixel_x(app, audio);

            // Flag annotations in the top strip.
            if (rects_intersect(exposed, top_strip)) {
                const auto f0 = clock::now();
                if (app.render_view_enabled) {
                    // Render-view: dark-blue flags, no editor overlay.
                    // Selection is visual-only (sky-tint on selected,
                    // dark-blue otherwise). Iteration popups are
                    // suppressed by the iteration_mode_enabled toggle
                    // being forced false on entry to render-view.
                    // Brief F Section 3: in transient sub-view there are
                    // no flags and no popup-eligible markers — skip both
                    // paints entirely.
                    if (app.active_mode != 'T') {
                    render_flags(cr, top_strip, app.render_view_markers,
                                 vp_start, vp_end, sr,
                                 kFlagFontSize,
                                 app.selected_markers,
                                 trim_struct,
                                 px_x,
                                 FlagEditorOverlay{});

                    // V.A3b hover popup paint, render-view variant.
                    // Mirrors the source-view branch below but reads
                    // from app.render_view_markers and uses the cached
                    // source sample rate (the render's audio sr is
                    // typically equal but the brief specifies source-
                    // axis presentation).
                    if (app.hover_popup.visible) {
                        const auto& mv = app.render_view_markers;
                        const int hidx = app.hover_popup.marker_index;
                        const bool eligible =
                            (hidx >= 0 &&
                             hidx < static_cast<int>(mv.size()) &&
                             (mv[hidx].tempo_inherits ||
                              !mv[hidx].label_ref.empty()) &&
                             !app.hover_popup.cached_text.empty());
                        if (eligible) {
                            auto rects = compute_flag_hit_rects(
                                cr, top_strip, mv,
                                vp_start, vp_end, sr, kFlagFontSize);
                            GuiRect anchor{0, 0, 0, 0};
                            for (const auto& r : rects) {
                                if (r.marker_index == hidx) {
                                    anchor.x = static_cast<int>(
                                        std::lround(r.x)) +
                                        static_cast<int>(kFlagInnerPadPx);
                                    anchor.y = static_cast<int>(
                                        std::lround(r.y));
                                    anchor.w = static_cast<int>(
                                        std::lround(r.w));
                                    anchor.h = static_cast<int>(
                                        std::lround(r.h));
                                    break;
                                }
                            }
                            if (anchor.w > 0 && anchor.h > 0) {
                                const int64_t pos = static_cast<int64_t>(
                                    mv[hidx].time_seconds *
                                    static_cast<double>(sr));
                                const bool oot =
                                    marker_out_of_trim(pos, trim_struct);
                                gui_text_display::State td;
                                td.anchor   = anchor;
                                td.content  = app.hover_popup.cached_text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    gui_text_display::Position::Top;
                                gui_text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }
                    } // end if (active_mode != 'T') — Brief F Section 3
                } else if (app.active_mode == 'T') {
                    render_transient_flags(
                        cr, top_strip, app.transients.markers(),
                        vp_start, vp_end, sr,
                        kFlagFontSize,
                        app.selected_markers,
                        trim_struct,
                        px_x);
                } else {
                    FlagEditorOverlay overlay;
                    // Only the V.A1 FlagPayload kind paints into the flag
                    // rect; the V.B IterationBracket kind owns the popup
                    // above the rect and leaves the flag's normal text
                    // alone. When the iter popup is the focused editor
                    // target, the flag rect below must suppress its
                    // last-selected highlight (V.B Addendum 2).
                    if (gui_text_editor::is_active(app.top_flag_editor) &&
                        app.top_flag_editor.kind ==
                            gui_text_editor::Kind::FlagPayload) {
                        overlay.marker_index   = app.top_flag_editor.target;
                        overlay.pending        = app.top_flag_editor.pending;
                        overlay.cursor_pos     = app.top_flag_editor.cursor_pos;
                        overlay.is_red         = app.top_flag_editor.red;
                        overlay.cursor_visible =
                            gui_text_editor::cursor_visible_now(
                                app.top_flag_editor);
                    } else if (gui_text_editor::is_active(app.top_flag_editor) &&
                               app.top_flag_editor.kind ==
                                   gui_text_editor::Kind::IterationBracket) {
                        overlay.iter_editor_target =
                            app.top_flag_editor.target;
                    }
                    render_flags(cr, top_strip, app.markers.markers(),
                                 vp_start, vp_end, sr,
                                 kFlagFontSize,
                                 app.selected_markers,
                                 trim_struct,
                                 px_x,
                                 overlay);

                    // V.A3b hover popup. Drawn on top of the flag strip,
                    // strictly after render_flags. Motion + tick already
                    // gate visibility; redraw just paints what state says.
                    // The popup text was precomputed at hover-entry into
                    // `app.hover_popup.cached_text` so this redraw branch
                    // doesn't have to repeat the parser-mirroring math.
                    if (app.hover_popup.visible &&
                        !app.iteration_mode_enabled) {
                        const auto& mv = app.markers.markers();
                        const int hidx = app.hover_popup.marker_index;
                        const bool eligible =
                            (hidx >= 0 &&
                             hidx < static_cast<int>(mv.size()) &&
                             (mv[hidx].tempo_inherits ||
                              !mv[hidx].label_ref.empty()) &&
                             !app.hover_popup.cached_text.empty());
                        if (eligible) {
                            auto rects = compute_flag_hit_rects(
                                cr, top_strip, mv,
                                vp_start, vp_end, sr, kFlagFontSize);
                            GuiRect anchor{0, 0, 0, 0};
                            for (const auto& r : rects) {
                                if (r.marker_index == hidx) {
                                    // anchor.x is the flag rect's text-origin
                                    // (rect's geometric x + render_flags' hl_pad
                                    // = kFlagInnerPadPx), so the popup's
                                    // leading character sits at the same column
                                    // as the flag's leading character.
                                    anchor.x = static_cast<int>(std::lround(r.x)) +
                                               static_cast<int>(kFlagInnerPadPx);
                                    anchor.y = static_cast<int>(std::lround(r.y));
                                    anchor.w = static_cast<int>(std::lround(r.w));
                                    anchor.h = static_cast<int>(std::lround(r.h));
                                    break;
                                }
                            }
                            if (anchor.w > 0 && anchor.h > 0) {
                                const int64_t pos = static_cast<int64_t>(
                                    mv[hidx].time_seconds *
                                    static_cast<double>(sr));
                                const bool oot =
                                    marker_out_of_trim(pos, trim_struct);
                                gui_text_display::State td;
                                td.anchor   = anchor;
                                td.content  = app.hover_popup.cached_text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    gui_text_display::Position::Top;
                                gui_text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }

                    // V.B iteration popups. Persistent per-flag annotations
                    // when iteration mode is on. Each owning marker gets a
                    // popup above its flag rect; pass markers and label_ref
                    // markers are excluded (no own tempo to vary). When the
                    // top_flag_editor is active in IterationBracket kind on
                    // marker `T`, popup `T` paints the editor's pending
                    // text (with the [] brackets visible during edit) and
                    // a 1-px cursor at cursor_pos; other popups paint
                    // their formatted iter text normally.
                    if (app.iteration_mode_enabled) {
                        const auto& mv = app.markers.markers();
                        auto hits = compute_iter_popup_hits(
                            cr, top_strip, mv,
                            vp_start, vp_end, sr, kFlagFontSize);
                        const bool editor_on_iter =
                            gui_text_editor::is_active(app.top_flag_editor) &&
                            app.top_flag_editor.kind ==
                                gui_text_editor::Kind::IterationBracket;
                        for (const auto& h : hits) {
                            // Anchor for gui_text_display: x at the flag's
                            // text-origin (flag.x + kFlagInnerPadPx, mirrors
                            // hover popup), y/w/h from the flag rect itself.
                            GuiRect anchor{
                                h.flag_rect.x +
                                    static_cast<int>(kFlagInnerPadPx),
                                h.flag_rect.y,
                                h.flag_rect.w,
                                h.flag_rect.h
                            };
                            const int64_t pos = static_cast<int64_t>(
                                mv[h.marker_index].time_seconds *
                                static_cast<double>(sr));
                            const bool oot =
                                marker_out_of_trim(pos, trim_struct);
                            if (editor_on_iter &&
                                app.top_flag_editor.target == h.marker_index) {
                                // Editor branch: state 2/3 of the three-state
                                // model. Background fills with kAccent on
                                // parse failure, otherwise kMarker; text and
                                // (blink-gated) cursor in kText. Out-of-trim
                                // wraps every color in dim() uniformly.
                                const std::string& pending =
                                    app.top_flag_editor.pending;
                                cairo_save(cr);
                                cairo_select_font_face(cr, "monospace",
                                    CAIRO_FONT_SLANT_NORMAL,
                                    CAIRO_FONT_WEIGHT_NORMAL);
                                cairo_set_font_size(cr, kFlagFontSize);
                                cairo_text_extents_t pext;
                                cairo_text_extents(cr, pending.c_str(), &pext);
                                cairo_text_extents_t uext;
                                cairo_text_extents(cr, "[+0.00,+0.00]", &uext);
                                const double hl_pad = kFlagInnerPadPx;
                                const double bg_w =
                                    pext.x_advance + 2.0 * hl_pad;
                                const double bg_x =
                                    static_cast<double>(anchor.x) - hl_pad;
                                const double bg_y =
                                    static_cast<double>(h.hit_rect.y);
                                const double bg_h =
                                    static_cast<double>(h.hit_rect.h);
                                GuiColor bg_col = app.top_flag_editor.red
                                    ? kAccent : kMarker;
                                if (oot) bg_col = dim(bg_col);
                                cairo_set_source_rgb(cr,
                                    bg_col.r, bg_col.g, bg_col.b);
                                const double sx = std::round(bg_x) + 0.5;
                                const double sy = std::round(bg_y) + 0.5;
                                const int sw = static_cast<int>(
                                    std::round(bg_w));
                                const int sh = static_cast<int>(
                                    std::round(bg_h));
                                cairo_set_line_width(cr, 1.0);
                                cairo_rectangle(cr, sx, sy,
                                    static_cast<double>(sw),
                                    static_cast<double>(sh));
                                cairo_stroke(cr);

                                const double baseline_y =
                                    static_cast<double>(anchor.y)
                                  - kIterPopupVerticalGapPx
                                  - kIterPopupVPadExtraPx
                                  - (uext.height + uext.y_bearing);
                                const GuiColor txt = oot ? dim(kText) : kText;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_move_to(cr,
                                    static_cast<double>(anchor.x), baseline_y);
                                cairo_show_text(cr, pending.c_str());

                                if (gui_text_editor::cursor_visible_now(
                                        app.top_flag_editor)) {
                                    std::string left = pending.substr(
                                        0, static_cast<size_t>(
                                            app.top_flag_editor.cursor_pos));
                                    cairo_text_extents_t lext;
                                    cairo_text_extents(cr, left.c_str(), &lext);
                                    const double cx =
                                        static_cast<double>(anchor.x) +
                                        lext.x_advance;
                                    cairo_set_source_rgb(cr,
                                        txt.r, txt.g, txt.b);
                                    cairo_set_line_width(cr, 1.0);
                                    cairo_move_to(cr, cx, bg_y);
                                    cairo_line_to(cr, cx, bg_y + bg_h);
                                    cairo_stroke(cr);
                                }
                                cairo_restore(cr);
                            } else {
                                gui_text_display::State td;
                                td.anchor   = anchor;
                                td.content  = h.text;
                                td.visible  = true;
                                td.color    = oot ? dim(kText) : kText;
                                td.position =
                                    gui_text_display::Position::Top;
                                gui_text_display::render(cr, td,
                                                         kFlagFontSize);
                            }
                        }
                    }
                }
                const auto f1 = clock::now();
                t_flags_ms =
                    std::chrono::duration<double, std::milli>(f1 - f0).count();
            }

            // Playhead drawn last so its stem and triangle paint over any
            // marker connector pixels they share a column with — the brief
            // mandates the playhead never be occluded by marker stems or
            // flag annotations. The triangle indicator lives in the top
            // strip, so render whenever either the waveform or top strip is
            // exposed; otherwise a flag-strip-only repaint would erase the
            // triangle.
            if (rects_intersect(exposed, area) ||
                rects_intersect(exposed, top_strip)) {
                const auto p0 = clock::now();
                render_playhead(cr, area, px_x, kPlayhead,
                                gui.playhead_triangle_surface());
                const auto p1 = clock::now();
                t_playhead_ms =
                    std::chrono::duration<double, std::milli>(p1 - p0).count();
            }

            // Bottom strip: either the prompt overlay (when active) or
            // the regular elements (timestamp / tab letter / dirty / render
            // -view filename). The prompt is modal — while active, it
            // owns the strip and the regular elements are not visible.
            const GuiRect ts = timestamp_invalidate_rect(
                app.height, app.width, bottom_strip_wide());
            if (rects_intersect(exposed, ts)) {
                const int baseline_y = app.height - kTimestampBaselineFromBottom;
                if (app.prompt.active) {
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_move_to(cr, kTimestampPadX, baseline_y);
                    cairo_show_text(cr, app.prompt.text.c_str());
                    cairo_text_extents_t pext;
                    cairo_text_extents(cr, app.prompt.text.c_str(), &pext);
                    const double label_gap = kTabLetterGapPx * 2.0;
                    double cursor_x = static_cast<double>(kTimestampPadX) +
                                      pext.x_advance + label_gap;
                    for (const auto& label : app.prompt.response_labels) {
                        cairo_move_to(cr, cursor_x, baseline_y);
                        cairo_show_text(cr, label.c_str());
                        cairo_text_extents_t lext;
                        cairo_text_extents(cr, label.c_str(), &lext);
                        cursor_x += lext.x_advance + label_gap;
                    }
                    cairo_restore(cr);
                } else if (!app.queue_progress_text.empty()) {
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_move_to(cr, kTimestampPadX, baseline_y);
                    cairo_show_text(cr, app.queue_progress_text.c_str());
                    cairo_restore(cr);
                } else {
                    // In source-view, sr is the loaded file's sample rate
                    // and playhead_sample is in source-frames. In render
                    // -view the active `audio` is the render, so its sr
                    // is what the engine wrote out — but the playhead is
                    // in render-frame coords. Render-view timestamp is
                    // render-domain (zero at render sample 0); source-time
                    // and render-time advance at different rates because
                    // of warping, so the same arithmetic suffices.
                    double seconds = 0.0;
                    if (sr > 0) {
                        seconds = static_cast<double>(app.playhead_sample) /
                                  static_cast<double>(sr);
                    }
                    {
                        const auto s0 = clock::now();
                        render_timestamp(cr, kTimestampPadX, baseline_y,
                                         seconds, kText);
                        const auto s1 = clock::now();
                        t_ts_ms =
                            std::chrono::duration<double, std::milli>(s1 - s0).count();
                    }

                    // A/B tab letter between timestamp and dirty indicator.
                    // Same font/size/color as the timestamp; no background.
                    // Suppressed in render-view since the Tab key is gated
                    // out there and the letter would carry no meaning.
                    const double tw = measure_timestamp_width(cr, seconds);
                    double right_after_letter =
                        static_cast<double>(kTimestampPadX) + tw;
                    if (!app.render_view_enabled) {
                        const double letter_x =
                            static_cast<double>(kTimestampPadX) + tw +
                            kTabLetterGapPx;
                        const char letter_buf[2] = { app.active_tab, '\0' };
                        cairo_save(cr);
                        cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
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
                        const double cx = right_after_letter + kTabLetterGapPx;
                        cairo_save(cr);
                        cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                        cairo_select_font_face(cr, "monospace",
                                               CAIRO_FONT_SLANT_NORMAL,
                                               CAIRO_FONT_WEIGHT_NORMAL);
                        cairo_set_font_size(cr, 14.0);
                        cairo_move_to(cr, cx, baseline_y);
                        cairo_show_text(cr, "*");
                        cairo_restore(cr);
                        const auto d1 = clock::now();
                        t_dirty_ms =
                            std::chrono::duration<double, std::milli>(d1 - d0).count();
                    }

                    // Chunk W: render-view filename. Right-aligned in the
                    // bottom strip so it doesn't conflict with the
                    // timestamp / tab letter / dirty indicator on the left.
                    if (app.render_view_enabled &&
                        app.render_view_index >= 0 &&
                        app.render_view_index <
                            static_cast<int>(app.render_view_list.size())) {
                        const auto& e =
                            app.render_view_list[app.render_view_index];
                        const std::string label =
                            e.batch_folder.filename().string() + "/" +
                            e.basename + ".wav";
                        cairo_save(cr);
                        cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                        cairo_select_font_face(cr, "monospace",
                                               CAIRO_FONT_SLANT_NORMAL,
                                               CAIRO_FONT_WEIGHT_NORMAL);
                        cairo_set_font_size(cr, 14.0);
                        cairo_text_extents_t ext;
                        cairo_text_extents(cr, label.c_str(), &ext);
                        const double rx = static_cast<double>(app.width) -
                                          static_cast<double>(kTimestampPadX) -
                                          ext.x_advance;
                        cairo_move_to(cr, rx, baseline_y);
                        cairo_show_text(cr, label.c_str());
                        cairo_restore(cr);
                    }
                }
            }
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
                if (playback.is_playing()) playback.resync_predictor();
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
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h + 1);
    };

    // V.A3b: a warp marker is hover-popup-eligible iff its rect doesn't
    // already display a numeric tempo: pass markers (with or without a
    // label_def) and label_ref markers. Owning markers display their tempo
    // in the rect, so no popup is needed. Transient mode has no pass
    // concept and is never eligible.
    //
    // V.B: when iteration mode is on, the hover popup for these same
    // marker types is suppressed entirely — the persistent iteration
    // popups occupy that visual space, and stacking a transient hover
    // hint on top would just clutter the strip.
    auto popup_eligible_marker = [&](int idx) -> bool {
        if (idx < 0) return false;
        if (app.render_view_enabled) {
            // In render-view, hover popups apply against the loaded
            // render's warpmarkers regardless of the pre-toggle mode.
            // Iteration-mode is forced off on toggle-in so its gate is
            // implicitly satisfied here too.
            const auto& mv = app.render_view_markers;
            if (idx >= static_cast<int>(mv.size())) return false;
            const auto& m = mv[idx];
            return m.tempo_inherits || !m.label_ref.empty();
        }
        if (app.active_mode != 'W') return false;
        if (app.iteration_mode_enabled) return false;
        const auto& mv = app.markers.markers();
        if (idx >= static_cast<int>(mv.size())) return false;
        const auto& m = mv[idx];
        return m.tempo_inherits || !m.label_ref.empty();
    };

    // Reset the hover popup state. If the popup was visible, invalidate the
    // top strip so the next paint erases it. Safe to call from any path.
    auto clear_hover_popup = [&]() {
        const bool was_visible = app.hover_popup.visible;
        app.hover_popup = HoverPopupState{};
        if (was_visible) invalidate_top_strip();
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
        const GuiRect t = timestamp_invalidate_rect(
            app.height, app.width, bottom_strip_wide());
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
        clear_hover_popup();
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
        clear_hover_popup();
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
        clear_hover_popup();
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
        clear_hover_popup();
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

        // Cache-free: typing `pass` writes inert defaults into
        // tempo_base/tempo_scale; typing an explicit tempo writes the
        // owned value. label_def is independent of tempo source —
        // `pass:LABEL` carries a def at this position while inheriting
        // the tempo from a prior owning marker.
        if (parsed.tempo_inherits) {
            m->tempo_inherits = true;
            m->tempo_base     = 1.0;
            m->tempo_scale    = "1.0000";
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

    // -- V.B iteration popup edit helpers -----------------------------------

    // Open an iteration popup edit on `idx`. The seed pending is the
    // current popup display ("[]" or "[+0.10,-0.05]") so the user can
    // backspace into a valid edit position. Reuses `top_flag_editor`
    // state but with Kind::IterationBracket so the editor's keyboard
    // vocabulary swaps to `[]+-,.` and digits.
    auto enter_iter_edit = [&](int idx) {
        if (idx < 0) return;
        if (!app.iteration_mode_enabled) return;
        const auto& mv = app.markers.markers();
        if (idx >= static_cast<int>(mv.size())) return;
        if (!iter_popup_eligible_marker(mv[idx])) return;
        if (gui_text_editor::is_active(app.top_flag_editor) &&
            app.top_flag_editor.target != idx) {
            gui_text_editor::deactivate(app.top_flag_editor);
        }
        gui_text_editor::enter(
            app.top_flag_editor, idx,
            /*locked_prefix=*/"",
            /*initial_pending=*/format_iter_bracket_text(mv[idx]),
            gui_text_editor::Kind::IterationBracket);
        clear_hover_popup();
        invalidate_top_strip();
    };

    // Commit the iteration popup's pending buffer. Four accepted forms:
    //   1. ""           → iter_start/iter_end := NaN (clear).
    //   2. "[]"         → iter_start/iter_end := NaN (clear).
    //   3. "[%+0.2f,%+0.2f]" with start <= end → set iter values.
    //   4. signed decimal "[+|-]NN[.NN]" → additive offset to tempo_base,
    //      clamped to [0.01, 9.99]; iter values cleared.
    // Anything else: red flash, stay in edit. Each accepted commit
    // pushes one undo entry. Only case 4 affects the on-disk dirty flag
    // (iter values are session-only and never serialized).
    auto commit_iter_edit = [&]() {
        if (!gui_text_editor::is_active(app.top_flag_editor)) return;
        if (app.top_flag_editor.kind !=
                gui_text_editor::Kind::IterationBracket) return;
        const int idx = app.top_flag_editor.target;
        const auto& mv_const = app.markers.markers();
        if (idx < 0 || idx >= static_cast<int>(mv_const.size())) {
            gui_text_editor::deactivate(app.top_flag_editor);
            invalidate_top_strip();
            return;
        }
        const std::string& s = app.top_flag_editor.pending;

        bool   clear_iter   = false;
        bool   set_iter     = false;
        double new_start    = 0.0;
        double new_end      = 0.0;
        bool   offset_tempo = false;
        double tempo_delta  = 0.0;

        if (s.empty() || s == "[]") {
            clear_iter = true;
        } else {
            // Case 3: bracketed pair. Strict format — sign, digits, '.',
            // exactly 2 digits — and start <= end.
            auto parse_signed_2dp = [](const std::string& v,
                                       double& out) -> bool {
                if (v.size() < 4) return false;
                if (v[0] != '+' && v[0] != '-') return false;
                const auto dot = v.find('.', 1);
                if (dot == std::string::npos) return false;
                if (dot == 1) return false;
                if (v.size() - dot - 1 != 2) return false;
                for (size_t i = 1; i < v.size(); ++i) {
                    if (i == dot) continue;
                    if (!std::isdigit(
                            static_cast<unsigned char>(v[i]))) return false;
                }
                try { out = std::stod(v); }
                catch (...) { return false; }
                return true;
            };
            bool tried_pair = false;
            if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
                const std::string inner = s.substr(1, s.size() - 2);
                const auto comma = inner.find(',');
                if (comma != std::string::npos) {
                    double pa, pb;
                    if (parse_signed_2dp(inner.substr(0, comma), pa) &&
                        parse_signed_2dp(inner.substr(comma + 1), pb) &&
                        pa <= pb) {
                        new_start = pa;
                        new_end   = pb;
                        set_iter  = true;
                    }
                    tried_pair = true;
                }
            }
            // Case 4: signed decimal additive offset to tempo_base.
            // Recognized only when bracket-pair parse failed; both cases
            // are mutually exclusive by syntax.
            if (!set_iter && !tried_pair) {
                if (s.size() >= 2 && (s[0] == '+' || s[0] == '-')) {
                    bool seen_dot = false;
                    int  digit_count = 0;
                    bool ok = true;
                    for (size_t i = 1; i < s.size(); ++i) {
                        const char c = s[i];
                        if (c == '.') {
                            if (seen_dot) { ok = false; break; }
                            seen_dot = true;
                            continue;
                        }
                        if (!std::isdigit(
                                static_cast<unsigned char>(c))) {
                            ok = false; break;
                        }
                        ++digit_count;
                    }
                    if (ok && digit_count > 0) {
                        try { tempo_delta = std::stod(s); offset_tempo = true; }
                        catch (...) { offset_tempo = false; }
                    }
                }
            }
        }

        if (!clear_iter && !set_iter && !offset_tempo) {
            app.top_flag_editor.red = true;
            invalidate_top_strip();
            std::fprintf(stderr,
                "warptempo_gui: iter edit rejected: invalid syntax: %s\n",
                s.c_str());
            return;
        }

        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;

        GuiMarker* m = app.markers.marker_mut(idx);
        if (!m) {
            gui_text_editor::deactivate(app.top_flag_editor);
            invalidate_top_strip();
            return;
        }

        bool tempo_changed = false;
        if (offset_tempo) {
            double new_tempo = m->tempo_base + tempo_delta;
            if (new_tempo < 0.01) new_tempo = 0.01;
            if (new_tempo > 9.99) new_tempo = 9.99;
            // Snap to 2 decimals to mirror the owning-marker precision
            // used elsewhere (drop-marker, nudge, etc.).
            new_tempo = std::round(new_tempo * 100.0) / 100.0;
            if (new_tempo != m->tempo_base) {
                m->tempo_base = new_tempo;
                tempo_changed = true;
            }
            m->iter_start = std::numeric_limits<double>::quiet_NaN();
            m->iter_end   = std::numeric_limits<double>::quiet_NaN();
        } else if (set_iter) {
            m->iter_start = new_start;
            m->iter_end   = new_end;
        } else if (clear_iter) {
            m->iter_start = std::numeric_limits<double>::quiet_NaN();
            m->iter_end   = std::numeric_limits<double>::quiet_NaN();
        }

        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        if (tempo_changed) {
            recompute_dirty();
            invalidate_waveform_area();
            invalidate_dirty_and_timestamp();
        }

        gui_text_editor::deactivate(app.top_flag_editor);
        invalidate_top_strip();
    };

    // Bulk-clear the session-only iter values across all warp markers.
    // Triggered by Shift+I while iteration mode is on. Single undo
    // entry. No-op when no marker carries iter values (avoids a noise
    // entry on the undo stack).
    auto bulk_clear_iter_values = [&]() {
        if (!app.iteration_mode_enabled) return;
        if (app.active_mode != 'W') return;
        auto& mv = app.markers.markers_mut();
        bool any = false;
        for (const auto& m : mv) {
            if (!std::isnan(m.iter_start) || !std::isnan(m.iter_end)) {
                any = true;
                break;
            }
        }
        if (!any) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        for (auto& m : mv) {
            m.iter_start = std::numeric_limits<double>::quiet_NaN();
            m.iter_end   = std::numeric_limits<double>::quiet_NaN();
        }
        push_undo(std::move(pre_state), OpKind::Other,
                  std::move(hint_sel), hint_last);
        invalidate_top_strip();
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
        if (playback.is_playing()) playback.resync_predictor();
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
        clear_hover_popup();
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
            ViewState& curtab = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
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
        clear_hover_popup();
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
            ViewState& curtab = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
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
        // pass markers carry inert defaults; their effective tempo is
        // resolved live from the marker list at every read site.
        if (inherit) {
            nm.tempo_base  = 1.0;
            nm.tempo_scale = "1.0000";
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

    // Shift+P: convert each selected marker's tempo source. Cache-free —
    // the only stored state on a pass marker is `tempo_inherits = true`
    // plus inert defaults. Three input cases per marker:
    //   - owning   → pass: inert defaults; label_def preserved.
    //   - pass     → owning: freeze the resolved tempo/scale at this moment;
    //                label_def preserved.
    //   - label_ref → pass: clear the ref; inert defaults.
    // The first marker is silently skipped (it must own its tempo).
    auto toggle_inherits = [&]() {
        if (app.selected_markers.empty()) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        const auto& mv_const = app.markers.markers();
        bool changed = false;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            if (idx == 0) continue;
            if (!m->label_ref.empty()) {
                m->label_ref.clear();
                m->tempo_inherits = true;
                m->tempo_base     = 1.0;
                m->tempo_scale    = "1.0000";
            } else if (m->tempo_inherits) {
                const double resolved_tempo =
                    resolve_inherited_tempo(mv_const, idx);
                const std::string resolved_scale =
                    resolve_inherited_tempo_scale(mv_const, idx);
                m->tempo_inherits = false;
                m->tempo_base     = resolved_tempo;
                m->tempo_scale    = resolved_scale;
            } else {
                m->tempo_inherits = true;
                m->tempo_base     = 1.0;
                m->tempo_scale    = "1.0000";
            }
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

    // Nudge every selected marker by `delta`. Label refs are silently
    // skipped (no tempo to nudge — convert via Shift+P first). Pass markers
    // resolve walk-backward to get their starting tempo/scale, then freeze
    // to owning at the nudged value. Owning markers nudge in place.
    // Clamps to [0.01, 9.99]. Only dirties / invalidates on real change.
    auto adjust_tempo = [&](double delta) {
        if (app.selected_markers.empty()) return;
        std::vector<GuiMarker> pre_state = app.markers.markers();
        std::set<int>          hint_sel  = app.selected_markers;
        const int              hint_last = app.last_selected_marker;
        const auto& mv_const = app.markers.markers();
        bool changed = false;
        for (int idx : app.selected_markers) {
            GuiMarker* m = app.markers.marker_mut(idx);
            if (!m) continue;
            if (!m->label_ref.empty()) continue;
            double      start_tempo;
            std::string start_scale;
            if (m->tempo_inherits) {
                start_tempo = resolve_inherited_tempo(mv_const, idx);
                start_scale = resolve_inherited_tempo_scale(mv_const, idx);
            } else {
                start_tempo = m->tempo_base;
                start_scale = m->tempo_scale;
            }
            double new_tempo = start_tempo + delta;
            if (new_tempo < 0.01) new_tempo = 0.01;
            if (new_tempo > 9.99) new_tempo = 9.99;
            if (!m->tempo_inherits && new_tempo == m->tempo_base) continue;
            m->tempo_inherits = false;
            m->tempo_base     = new_tempo;
            m->tempo_scale    = start_scale;
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
        ViewState& t = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
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

    // Brief J.2 Section 1: indirection that returns the currently
    // active ViewState — the slot that holds the inactive-mode
    // selection. Source-view: the active tab. Render-view: the
    // active render entry's `state`. Returns nullptr when no valid
    // active view-state is available; callers must handle nullptr
    // by no-op-ing rather than silently corrupting a fallback slot.
    auto active_view_state = [&]() -> ViewState* {
        if (app.render_view_enabled) {
            if (app.render_view_index >= 0 &&
                app.render_view_index <
                    static_cast<int>(app.render_view_list.size())) {
                return &app.render_view_list[app.render_view_index].state;
            }
            // Render-view enabled but no valid entry. Return null
            // rather than silently writing render-view indices into
            // a source tab slot.
            return nullptr;
        }
        return (app.active_tab == 'B') ? &app.tab_b : &app.tab_a;
    };

    // Brief J.2 Section 1: drop live-selection indices that are out
    // of range against whichever marker list `active_mode` points
    // at, and repair last_selected_marker to a valid member (or -1
    // if empty). Called after every mode-swap so a slot whose
    // indices may be stale becomes safe to use immediately.
    auto prune_live_selection = [&]() {
        int n = 0;
        if (app.render_view_enabled) {
            n = (app.active_mode == 'T')
                ? static_cast<int>(app.render_view_transients.size())
                : static_cast<int>(app.render_view_markers.size());
        } else {
            n = (app.active_mode == 'T')
                ? static_cast<int>(app.transients.markers().size())
                : static_cast<int>(app.markers.markers().size());
        }
        for (auto it = app.selected_markers.begin();
             it != app.selected_markers.end();) {
            if (*it < 0 || *it >= n) {
                it = app.selected_markers.erase(it);
            } else {
                ++it;
            }
        }
        if (app.last_selected_marker < 0 ||
            app.last_selected_marker >= n ||
            !app.selected_markers.count(app.last_selected_marker)) {
            app.last_selected_marker =
                app.selected_markers.empty()
                    ? -1
                    : *app.selected_markers.rbegin();
        }
    };

    // Toggle active editing mode between 'W' (warp) and 'T' (transient).
    // Saves the active selection into the leaving mode's per-tab slot,
    // then restores the destination mode's slot. Visible state (viewport /
    // zoom / playhead) is unaffected. Caller decides what invalidations to
    // run; this helper just shuffles the AppState fields.
    auto switch_active_mode_to = [&](char target_mode) {
        if (target_mode == app.active_mode) return;
        ViewState* vs = active_view_state();
        if (!vs) return;
        if (app.active_mode == 'T') {
            vs->transient_selected      = app.selected_markers;
            vs->transient_last_selected = app.last_selected_marker;
            app.selected_markers        = vs->warp_selected;
            app.last_selected_marker    = vs->warp_last_selected;
        } else {
            vs->warp_selected           = app.selected_markers;
            vs->warp_last_selected      = app.last_selected_marker;
            app.selected_markers        = vs->transient_selected;
            app.last_selected_marker    = vs->transient_last_selected;
        }
        app.active_mode = target_mode;
        prune_live_selection();
        clear_hover_popup();
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
        app.hover_popup   = HoverPopupState{};
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

        app.tab_a = ViewState{};
        app.tab_b = ViewState{};
        app.active_tab = 'A';

        invalidate_all();
    };

    // Forward decl: defined below so it can be captured by the prompt
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

    auto open_prompt_unsaved = [&](DialogTrigger t) {
        app.prompt.active          = true;
        app.prompt.text            = "Save unsaved changes?";
        app.prompt.response_keys   = {'s', 'd', 'c'};
        app.prompt.response_labels = {"[S]ave", "[D]iscard", "[C]ancel"};
        app.prompt.trigger         = t;
        clear_hover_popup();
        invalidate_all();
    };

    auto open_prompt_detect_confirm = [&]() {
        app.prompt.active          = true;
        app.prompt.text            =
            "Re-detect transients? Existing detection will be replaced.";
        app.prompt.response_keys   = {'d', 'c'};
        app.prompt.response_labels = {"[D]etect", "[C]ancel"};
        app.prompt.trigger         = DialogTrigger::DETECT_TRANSIENTS;
        clear_hover_popup();
        invalidate_all();
    };

    // Single-key response dispatch. The trigger captured at prompt-open
    // time selects which response set is in play; the key picks the
    // response. On a Save failure, the prompt mutates in place to a
    // retry/discard/cancel state — same trigger, new text and response
    // set — rather than dismissing.
    auto prompt_activate_response = [&](char k) {
        if (!app.prompt.active) return;
        const DialogTrigger trigger = app.prompt.trigger;

        if (trigger == DialogTrigger::CLOSE_WINDOW ||
            trigger == DialogTrigger::REVERT_TO_BLANK) {
            if (k == 's' || k == 'r') {
                const bool ok = save_markers();
                if (!ok) {
                    app.prompt.text            = "Save failed.";
                    app.prompt.response_keys   = {'r', 'd', 'c'};
                    app.prompt.response_labels =
                        {"[R]etry", "[D]iscard", "[C]ancel"};
                    invalidate_all();
                    return;
                }
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == 'd') {
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == 'c') {
                app.prompt.active = false;
                invalidate_all();
                return;
            }
            return;
        }

        if (trigger == DialogTrigger::DETECT_TRANSIENTS) {
            if (k == 'd') {
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == 'c') {
                app.prompt.active = false;
                invalidate_all();
                return;
            }
            return;
        }
    };

    // Route a close / revert gesture through the prompt when history is
    // dirty; otherwise proceed immediately. Centralizes the decision so
    // Ctrl+Q, Ctrl+W, and the WM-close callback share identical behavior.
    auto request_close_or_revert = [&](DialogTrigger t) {
        if (app.prompt.active) return; // already gated; ignore re-entry
        if (app.dirty) open_prompt_unsaved(t);
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
        if (app.prompt.active)             return;
        if (app.source_audio_path.empty()) return;
        if (audio.total_frames() <= 0)     return;

        bool has_prior_detection = false;
        for (const auto& m : app.transients.markers()) {
            if (!m.is_inserted) { has_prior_detection = true; break; }
        }
        if (has_prior_detection) {
            open_prompt_detect_confirm();
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
        // Speed change without resync would cause a backward cursor jump:
        // the predictor would retroactively apply the new speed to the
        // entire elapsed-since-anchor period.
        if (playback.is_playing()) playback.resync_predictor();
    };

    // Hit-test a marker line in the waveform area. Returns index or -1.
    // Mode-aware: iterates the active list (render-view markers when
    // render-view is on; otherwise warp markers or transients).
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
        const bool rv = app.render_view_enabled;
        // Brief F Section 3: in render-view, the visible sub-view's
        // list drives hit-testing. 'T' reads transient frames via
        // effective_frame() (matching source-view's transient branch).
        const bool rv_trans = rv && app.active_mode == 'T';
        const int n =
            rv_trans
                ? static_cast<int>(app.render_view_transients.size())
                : rv
                    ? static_cast<int>(app.render_view_markers.size())
                    : (app.active_mode == 'T')
                        ? static_cast<int>(app.transients.markers().size())
                        : static_cast<int>(app.markers.markers().size());
        for (int i = 0; i < n; ++i) {
            double ms;
            if (rv_trans) {
                ms = static_cast<double>(
                    app.render_view_transients[i].effective_frame());
            } else if (rv) {
                ms = app.render_view_markers[i].time_seconds *
                     static_cast<double>(sr);
            } else if (app.active_mode == 'T') {
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
        // Brief F Section 3: render-view's transient sub-view paints no
        // flags; short-circuit to no-hit so click and hover paths see a
        // bare top strip.
        if (app.render_view_enabled &&
            app.active_mode == 'T') {
            return -1;
        }
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
        if (app.render_view_enabled) {
            rects = compute_flag_hit_rects(
                scratch_cr, top, app.render_view_markers,
                vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
        } else if (app.active_mode == 'T') {
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

    // V.B: iteration popup hit-test. Returns the marker index whose
    // iteration popup contains (mouse_x, mouse_y), or -1. Always returns
    // -1 when iteration mode is off or in transient mode (no popups).
    auto hit_test_iter_popup = [&](int mouse_x, int mouse_y) -> int {
        if (!app.iteration_mode_enabled) return -1;
        if (app.active_mode != 'W') return -1;
        const GuiRect area = waveform_area(app);
        const GuiRect top  = top_strip_area(app);
        cairo_surface_t* scratch_s = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t* scratch_cr = cairo_create(scratch_s);
        const double spp = current_samples_per_pixel(app, audio);
        const int64_t vp_start = app.viewport_start_sample;
        const int64_t vp_end = vp_start +
            static_cast<int64_t>(std::llround(spp * area.w));
        auto hits = compute_iter_popup_hits(
            scratch_cr, top, app.markers.markers(),
            vp_start, vp_end, audio.sample_rate(), kFlagFontSize);
        cairo_destroy(scratch_cr);
        cairo_surface_destroy(scratch_s);
        for (const auto& h : hits) {
            if (mouse_x >= h.hit_rect.x &&
                mouse_x < h.hit_rect.x + h.hit_rect.w &&
                mouse_y >= h.hit_rect.y &&
                mouse_y < h.hit_rect.y + h.hit_rect.h) {
                return h.marker_index;
            }
        }
        return -1;
    };

    // V.A3b Addendum 3: re-evaluate hover at the cursor's last on_motion
    // coordinates. Called after viewport mutations (zoom, scroll, center,
    // playhead-driven viewport shift) so a stationary cursor's hover state
    // tracks the rects that just slid under it. Mirrors the on_motion
    // hover-detection branch: same gating, same hit-test, same state
    // transitions; the tick handler still drives the dwell-to-visible flip.
    recompute_hover_at_cursor = [&]() {
        if (app.last_mouse_x < 0 || app.last_mouse_y < 0) return;
        // Dialog / drag / editor / queue still suppress hover in either
        // view. Source-view also requires warp mode + iter mode off;
        // render-view bypasses the mode checks because hover always
        // applies against the loaded render's warpmarkers.
        if (app.prompt.active ||
            app.drag.active ||
            app.playhead_drag.active ||
            gui_text_editor::is_active(app.top_flag_editor) ||
            app.queue_running) {
            clear_hover_popup();
            return;
        }
        if (!app.render_view_enabled &&
            (app.active_mode != 'W' || app.iteration_mode_enabled)) {
            clear_hover_popup();
            return;
        }
        const int hit = hit_test_flag(app.last_mouse_x, app.last_mouse_y);
        if (hit != app.hover_popup.marker_index) {
            if (app.hover_popup.visible) invalidate_top_strip();
            app.hover_popup.marker_index = hit;
            app.hover_popup.visible      = false;
            app.hover_popup.entry_time   =
                std::chrono::steady_clock::now();
            // Precompute the popup's display text at rect-entry so the
            // delay-completion paint doesn't have to recompute it. Empty
            // when `hit` is not popup-eligible (the redraw branch then
            // skips paint and keeps the strip clean).
            if (app.render_view_enabled) {
                app.hover_popup.cached_text =
                    popup_eligible_marker(hit)
                        ? compute_hover_popup_text(
                              app.render_view_markers, hit,
                              app.render_view_src_sr)
                        : std::string();
            } else {
                app.hover_popup.cached_text =
                    popup_eligible_marker(hit)
                        ? compute_hover_popup_text(
                              app.markers.markers(), hit,
                              audio.sample_rate())
                        : std::string();
            }
        }
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
        clear_hover_popup();
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
        clear_hover_popup();
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

    // -- Chunk W: render-view helpers ---------------------------------------

    // Enumerate <source_parent>/renders/<i>_<tag>/<NN>.wav into a flat
    // ordered list. Sort key is (batch_index ascending, basename_index
    // ascending). Empty result when the renders folder doesn't exist or
    // contains no valid entries.
    auto enumerate_render_view_list =
            [&]() -> std::vector<AppState::RenderViewEntry> {
        std::vector<AppState::RenderViewEntry> out;
        if (app.source_audio_path.empty()) return out;
        std::filesystem::path src(app.source_audio_path);
        std::filesystem::path src_parent = src.parent_path();
        if (src_parent.empty()) src_parent = std::filesystem::path(".");
        const std::filesystem::path renders_root = src_parent / "renders";
        std::error_code ec;
        if (!std::filesystem::is_directory(renders_root, ec)) return out;

        auto leading_int = [](const std::string& s, size_t& end_out) -> int {
            int v = 0;
            size_t i = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                v = v * 10 + (s[i] - '0');
                ++i;
            }
            end_out = i;
            return v;
        };

        struct BatchSlot { int idx; std::filesystem::path path; };
        std::vector<BatchSlot> batches;
        for (const auto& de :
             std::filesystem::directory_iterator(renders_root, ec)) {
            if (!de.is_directory()) continue;
            const std::string name = de.path().filename().string();
            size_t end = 0;
            const int v = leading_int(name, end);
            if (end == 0 || end >= name.size() || name[end] != '_') continue;
            batches.push_back({v, de.path()});
        }
        std::sort(batches.begin(), batches.end(),
                  [](const BatchSlot& a, const BatchSlot& b) {
                      return a.idx < b.idx;
                  });

        for (const auto& b : batches) {
            struct WavSlot {
                int idx;
                std::filesystem::path path;
                std::string basename;
            };
            std::vector<WavSlot> wavs;
            for (const auto& fe :
                 std::filesystem::directory_iterator(b.path, ec)) {
                if (!fe.is_regular_file()) continue;
                if (fe.path().extension() != ".wav") continue;
                const std::string stem = fe.path().stem().string();
                size_t end = 0;
                const int v = leading_int(stem, end);
                if (end != stem.size()) continue;
                wavs.push_back({v, fe.path(), stem});
            }
            std::sort(wavs.begin(), wavs.end(),
                      [](const WavSlot& a, const WavSlot& b) {
                          return a.idx < b.idx;
                      });
            for (auto& w : wavs) {
                AppState::RenderViewEntry e;
                e.batch_folder = b.path;
                e.basename     = std::move(w.basename);
                e.wav_path     = std::move(w.path);
                out.push_back(std::move(e));
            }
        }
        return out;
    };

    // -- Chunk W Addendum 5: <basename>.rendersettings sidecar -------------
    //
    // Per-render zoom/viewport/playhead persistence. Captures the live
    // render-view state at navigation/exit boundaries; applied on entry
    // / arrival. Source-domain authoring is unaffected — these helpers
    // run only against render-view AppState fields.

    auto rendersettings_path =
            [&](const AppState::RenderViewEntry& e) -> std::filesystem::path {
        return e.batch_folder / (e.basename + ".rendersettings");
    };

    // Atomic save of the live render-view zoom/viewport/playhead. Same
    // <path>.tmp + fsync + rename scheme as the warpmarkers writer.
    // Failures are non-fatal — logged once and discarded.
    auto write_rendersettings_for =
            [&](const AppState::RenderViewEntry& e) {
        const std::filesystem::path path = rendersettings_path(e);
        char buf[256];
        const int len = std::snprintf(buf, sizeof(buf),
            "render_viewport_start=%lld\n"
            "render_zoom=%d\n"
            "render_playhead=%lld\n",
            static_cast<long long>(app.viewport_start_sample),
            app.zoom_level,
            static_cast<long long>(app.playhead_sample));
        if (len <= 0 || len >= static_cast<int>(sizeof(buf))) {
            std::fprintf(stderr,
                "warptempo_gui: render-view: rendersettings format failed\n");
            return;
        }
        const std::string tmp_path = path.string() + ".tmp";
        int fd = ::open(tmp_path.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::fprintf(stderr,
                "warptempo_gui: render-view: failed to open %s: %s\n",
                tmp_path.c_str(), std::strerror(errno));
            return;
        }
        ssize_t off = 0;
        while (off < len) {
            const ssize_t n = ::write(fd, buf + off, len - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                ::unlink(tmp_path.c_str());
                std::fprintf(stderr,
                    "warptempo_gui: render-view: write %s failed: %s\n",
                    tmp_path.c_str(), std::strerror(errno));
                return;
            }
            off += n;
        }
        if (::fsync(fd) != 0 || ::close(fd) != 0) {
            ::unlink(tmp_path.c_str());
            std::fprintf(stderr,
                "warptempo_gui: render-view: fsync/close %s failed\n",
                tmp_path.c_str());
            return;
        }
        if (::rename(tmp_path.c_str(), path.string().c_str()) != 0) {
            ::unlink(tmp_path.c_str());
            std::fprintf(stderr,
                "warptempo_gui: render-view: rename %s failed: %s\n",
                tmp_path.c_str(), std::strerror(errno));
        }
    };

    // Tolerant parser. Missing / malformed file applies fit-file zoom
    // and zeroed viewport/playhead. Apply order: zoom → viewport →
    // playhead → clamp_viewport_start (zoom drives the spp used by
    // clamp).
    auto apply_rendersettings_for =
            [&](const AppState::RenderViewEntry& e) {
        int     z   = kFitFileLevel;
        int64_t vp  = 0;
        int64_t ph  = 0;
        const std::filesystem::path path = rendersettings_path(e);
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: malformed line in "
                        "%s: %s\n", path.string().c_str(), line.c_str());
                    continue;
                }
                const std::string key = line.substr(0, eq);
                const std::string val = line.substr(eq + 1);
                try {
                    if (key == "render_zoom") {
                        z = std::stoi(val);
                    } else if (key == "render_viewport_start") {
                        vp = static_cast<int64_t>(std::stoll(val));
                    } else if (key == "render_playhead") {
                        ph = static_cast<int64_t>(std::stoll(val));
                    }
                    // Unknown keys ignored.
                } catch (...) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: bad value in "
                        "%s: %s\n", path.string().c_str(), line.c_str());
                }
            }
        }
        // Sanitize zoom — accept only kFitFileLevel or 0..kNumZoomLevels-1.
        if (z != kFitFileLevel && (z < 0 || z >= kNumZoomLevels)) {
            z = kFitFileLevel;
        }
        app.zoom_level            = z;
        app.viewport_start_sample = vp;
        app.playhead_sample       = ph;
        clamp_viewport_start(app, audio);
    };

    // Brief F Section 4: capture (size, mtime_seconds) for a wav path.
    // Errors → (0, 0), interpreted as "no valid stat tuple" by callers
    // (forces a mismatch on compare). Uses stat() directly because
    // C++17's std::filesystem::file_time_type isn't portably
    // convertible to system_clock; stat's st_mtime is seconds-since-
    // epoch, which is what the persisted field stores.
    auto wav_stat_tuple =
        [&](const std::filesystem::path& p) -> std::pair<uintmax_t, int64_t> {
        struct stat st{};
        if (::stat(p.c_str(), &st) != 0) return {0, 0};
        return {static_cast<uintmax_t>(st.st_size),
                static_cast<int64_t>(st.st_mtime)};
    };

    // Brief J.2 Section 4: stash the live selection into the active
    // RenderViewEntry's matching-mode slot, along with the wav's
    // current stat tuple. No-op when no entry is active. Called from
    // the render-view exit path and from the batch-nav path
    // (Shift+Left/Right) before the destination is loaded. The
    // OTHER-mode slot was last written when active_mode flipped
    // away from it via switch_active_mode_to (or never written if
    // the user has not flipped mode in this render-view session);
    // either way it is current at stash time.
    auto stash_render_view_selection_to_active_entry = [&]() {
        if (app.render_view_index < 0 ||
            app.render_view_index >=
                static_cast<int>(app.render_view_list.size())) {
            return;
        }
        auto& e = app.render_view_list[app.render_view_index];
        if (app.active_mode == 'T') {
            e.state.transient_selected      = app.selected_markers;
            e.state.transient_last_selected = app.last_selected_marker;
        } else {
            e.state.warp_selected           = app.selected_markers;
            e.state.warp_last_selected      = app.last_selected_marker;
        }
        const auto stat = wav_stat_tuple(e.wav_path);
        e.persisted_size  = stat.first;
        e.persisted_mtime = stat.second;
    };

    // Loads the render at app.render_view_list[index] into the active
    // `audio`, parking the source audio on first entry. Parses sibling
    // <basename>.warpmarkers and <basename>.transientmarkers into
    // app.render_view_markers/transients and computes F_begin/F_end
    // against the cached source sr/total. Stops playback before the
    // swap and re-binds the playback device. Returns true on success;
    // on failure logs to stderr and the prior state is preserved.
    //
    // Brief F Section 4: when the destination entry's persisted stat
    // tuple matches the wav's current stat, restores the persisted
    // selection. Mismatch leaves the live selection empty.
    auto load_render_view_at = [&](int index) -> bool {
        if (index < 0 ||
            index >= static_cast<int>(app.render_view_list.size())) {
            return false;
        }
        auto& e = app.render_view_list[index];

        GuiAudio next;
        if (!next.load(e.wav_path.string(), {})) {
            std::fprintf(stderr,
                "warptempo_gui: render-view: failed to load %s\n",
                e.wav_path.string().c_str());
            return false;
        }

        // Render-view consumes render-domain sidecars
        // (.renderwarpmarkers / .rendertransientmarkers) so visible marker
        // positions match the rendered audio's time axis. The source-domain
        // pair (.warpmarkers / .transientmarkers) is what Ctrl+Alt+C commit
        // reloads when promoting a render's markers into authoring memory.
        std::vector<GuiMarker>     loaded_warp;
        std::vector<GuiTransient>  loaded_trans;
        {
            const std::filesystem::path wmd =
                e.batch_folder / (e.basename + ".renderwarpmarkers");
            std::error_code ec;
            if (std::filesystem::exists(wmd, ec)) {
                GuiMarkers m;
                m.load(wmd.string());
                loaded_warp = m.markers();
            } else {
                std::fprintf(stderr,
                    "warptempo_gui: render-view: %s missing — markers will "
                    "not be displayed for this render\n",
                    wmd.string().c_str());
            }
        }
        {
            const std::filesystem::path tmd =
                e.batch_folder / (e.basename + ".rendertransientmarkers");
            std::error_code ec;
            if (std::filesystem::exists(tmd, ec)) {
                GuiTransients t;
                t.load(tmd.string());
                loaded_trans = t.markers();
            }
        }
        playback.stop();
        playback.shutdown();
        app.is_playing      = false;
        app.playback_cursor = 0;
        clear_hover_popup();

        // Snapshot the live authoring playhead/viewport/zoom into the
        // active tab's slot before we overwrite `audio`. restore_source_audio
        // reads it back on render-view exit so the user lands where they
        // left the source view rather than at sample 0.
        if (source_audio_held.total_frames() == 0) {
            refresh_active_tab_from_app();
            source_audio_held = std::move(audio);
        }
        audio = std::move(next);
        app.audio_generation++;

        const auto trim = compute_trim_samples(
            loaded_warp, loaded_trans,
            app.render_view_src_sr, app.render_view_src_total);
        app.render_view_src_F_begin = trim.first;
        app.render_view_src_F_end   = trim.second;

        app.render_view_markers           = std::move(loaded_warp);
        app.render_view_transients        = std::move(loaded_trans);
        app.render_view_index             = index;
        app.last_render_view_path         = e.wav_path.string();

        // Brief F Section 4: stat-tuple-gated selection restore. A
        // matching persisted tuple (non-zero, equal to current) means
        // the wav hasn't changed since stash; replay the persisted
        // selection. Mismatch (or never-stashed defaults) drops to
        // empty selection — the destination entry has no remembered
        // session for this file.
        const auto cur_stat = wav_stat_tuple(e.wav_path);
        const bool stat_match =
            cur_stat.first  != 0 &&
            cur_stat.second != 0 &&
            cur_stat.first  == e.persisted_size &&
            cur_stat.second == e.persisted_mtime;
        if (stat_match) {
            // Brief J.2 Section 4: load only the matching-mode slot
            // into the live pair. The OTHER-mode slot stays on state
            // and gets swapped in if mode flips during this render-
            // view session via switch_active_mode_to.
            if (app.active_mode == 'T') {
                app.selected_markers     = e.state.transient_selected;
                app.last_selected_marker = e.state.transient_last_selected;
            } else {
                app.selected_markers     = e.state.warp_selected;
                app.last_selected_marker = e.state.warp_last_selected;
            }
            prune_live_selection();
        } else {
            // Stat mismatch invalidates BOTH slots — the wav has
            // changed, so any stashed indices for either mode could
            // be stale. Symmetric with stat-match's "live pair gets
            // matching slot, OTHER stays on state": when we don't
            // trust state, clear both the live pair AND the OTHER
            // slot on state so a later mode-flip doesn't pull in
            // stale data.
            app.selected_markers.clear();
            app.last_selected_marker = -1;
            e.state.warp_selected.clear();
            e.state.warp_last_selected      = -1;
            e.state.transient_selected.clear();
            e.state.transient_last_selected = -1;
        }

        // Apply this render's persisted zoom/viewport/playhead (or
        // fit-file defaults when no .rendersettings sidecar exists).
        // Order matters: apply_rendersettings_for sets zoom first
        // (clamp depends on it) and runs clamp at the end.
        apply_rendersettings_for(e);

        if (!playback.init(audio.sample_rate(), audio.channels(),
                           audio.samples_ptr(), audio.total_frames())) {
            std::fprintf(stderr,
                "warptempo_gui: playback disabled in render-view\n");
        }
        gui.invalidate_region(0, 0, app.width, app.height);
        return true;
    };

    // Restores source audio from the parked source_audio_held. Inverse
    // of the load_render_view_at entry path. No-op when
    // source_audio_held is empty (nothing to restore).
    auto restore_source_audio = [&]() {
        if (source_audio_held.total_frames() == 0) return;
        playback.stop();
        playback.shutdown();
        app.is_playing      = false;
        app.playback_cursor = 0;
        clear_hover_popup();

        audio = std::move(source_audio_held);
        source_audio_held = GuiAudio{};
        app.audio_generation++;

        // Read back the active tab's snapshot saved when render-view was
        // first entered. The Tab key is gated out of render-view's input
        // allowlist, so app.active_tab is the same letter the snapshot
        // was written under.
        const ViewState& t = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
        app.viewport_start_sample = t.viewport_start_sample;
        app.zoom_level            = t.zoom_level;
        app.playhead_sample       = t.playhead_sample;
        // Brief J.2 Section 4: load the matching-mode slot into the
        // live pair. Live pair held render-view selection while
        // render-view was active; restoring source-view requires
        // pulling the source tab's matching-mode slot back in.
        if (app.active_mode == 'T') {
            app.selected_markers     = t.transient_selected;
            app.last_selected_marker = t.transient_last_selected;
        } else {
            app.selected_markers     = t.warp_selected;
            app.last_selected_marker = t.warp_last_selected;
        }
        clamp_viewport_start(app, audio);
        prune_live_selection();

        if (!playback.init(audio.sample_rate(), audio.channels(),
                           audio.samples_ptr(), audio.total_frames())) {
            std::fprintf(stderr, "warptempo_gui: playback disabled\n");
        }
        gui.invalidate_region(0, 0, app.width, app.height);
    };

    // Shared wheel handler covering source-view and render-view. Ctrl+Alt =
    // fine-pan (2% of viewport), Alt = coarse-pan (10%), plain = zoom.
    // Ctrl+wheel nudges selected markers in source-view; render-view is
    // read-only so it passes allow_nudge=false and Ctrl+wheel becomes a
    // silent no-op there.
    auto handle_wheel = [&](unsigned int button,
                            bool ctrl, bool alt,
                            bool inside_waveform, bool inside_top,
                            bool allow_nudge) {
        if (!inside_waveform && !inside_top) return;
        if (ctrl && alt) {
            const int64_t step = std::max<int64_t>(
                1, samples_visible(app, audio) / 50);
            scroll_viewport(button == 4 ? -step : +step);
            return;
        }
        if (ctrl) {
            if (allow_nudge) {
                nudge_selected_markers(button == 4 ? -1 : +1);
            }
            return;
        }
        if (alt) {
            const int64_t step = std::max<int64_t>(
                1, samples_visible(app, audio) / 10);
            scroll_viewport(button == 4 ? -step : +step);
            return;
        }
        if (button == 4) zoom_out();
        else             zoom_in();
    };

    // Multi-render queue runner. Owns the queue_running / cancel-flag
    // bookkeeping and per-entry progress display; the caller owns batch
    // folder creation, RenderRequest construction, and the post-summary
    // log. Returns rendered count and whether Esc cut the run short.
    struct RenderBatchResult {
        int  rendered  = 0;
        bool cancelled = false;
    };
    auto run_render_batch =
        [&](const std::vector<RenderRequest>& reqs,
            const std::string& batch_label) -> RenderBatchResult {
        RenderBatchResult result;
        if (reqs.empty()) return result;

        const int total = static_cast<int>(reqs.size());

        app.queue_cancel_requested = false;
        app.queue_running          = true;
        clear_hover_popup();

        for (int i = 0; i < total; ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "%s: rendering %d of %d...",
                          batch_label.c_str(), i + 1, total);
            app.queue_progress_text = buf;
            invalidate_dirty_and_timestamp();
            // First drain surfaces the progress-text paint before the
            // engine starts; otherwise the new "rendering K of N" only
            // appears after the entry completes.
            gui.drain_events();

            if (do_render(reqs[i])) ++result.rendered;

            // Second drain surfaces X events queued during the render —
            // Esc presses, expose events. The cancel flag becomes
            // visible to the next iteration through this drain.
            gui.drain_events();
            if (app.queue_cancel_requested) {
                result.cancelled = true;
                break;
            }
        }

        app.queue_running          = false;
        app.queue_cancel_requested = false;
        app.queue_progress_text.clear();
        invalidate_dirty_and_timestamp();

        return result;
    };

    gui.set_on_key([&](KeySym keysym, unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        const bool ctrl  = (mods & ControlMask) != 0;
        const bool shift = (mods & ShiftMask)   != 0;
        const bool alt   = (mods & Mod1Mask)    != 0;

        // Bottom-strip prompt owns input while active. Only the prompt's
        // own response keys (case-insensitive) and Esc (rightmost
        // response = Cancel by convention) do anything; everything else
        // is swallowed so marker edits / playback / viewport keys cannot
        // sneak in while the prompt is up.
        if (app.prompt.active) {
            char k = 0;
            if (keysym >= XK_a && keysym <= XK_z) {
                k = static_cast<char>('a' + (keysym - XK_a));
            } else if (keysym >= XK_A && keysym <= XK_Z) {
                k = static_cast<char>('a' + (keysym - XK_A));
            }
            if (keysym == XK_Escape) {
                if (!app.prompt.response_keys.empty()) {
                    prompt_activate_response(app.prompt.response_keys.back());
                }
                return;
            }
            if (k != 0) {
                for (char rk : app.prompt.response_keys) {
                    if (k == rk) {
                        prompt_activate_response(rk);
                        return;
                    }
                }
            }
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
                if (app.top_flag_editor.kind ==
                        gui_text_editor::Kind::IterationBracket) {
                    commit_iter_edit();
                } else {
                    commit_top_flag_edit();
                }
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

        // Chunk W: render-view input gate. While render-view is active
        // only keys driving navigation / playback / exit / commit are
        // honored; every authoring key is silently dropped so a stray
        // press can't mutate state through a swapped-out view.
        // Allowlist:
        //   - r (no mods)            → toggle render-view off
        //   - Shift+Left/Right       → previous/next render
        //   - Ctrl+Alt+C             → commit displayed render's markers
        //   - Space                  → playback toggle
        //   - Left/Right (no mods)   → playhead-by-pixel scrub
        //   - Home/End (no mods)     → playhead to trim begin/end
        //   - Esc                    → top-level no-op (chunk Q)
        //   - t (no mods)            → toggle warp/transient sub-view (Brief F)
        //   - Ctrl+Q / Ctrl+W        → close-prompt routing (Brief F)
        if (app.render_view_enabled) {
            const bool is_r =
                (keysym == XK_r && !ctrl && !shift && !alt);
            const bool is_nav =
                ((keysym == XK_Left || keysym == XK_Right) &&
                 shift && !ctrl && !alt);
            const bool is_commit =
                (ctrl && alt && !shift &&
                 (keysym == XK_c || keysym == XK_C));
            const bool is_playback = (keysym == XK_space);
            const bool is_scrub =
                ((keysym == XK_Left || keysym == XK_Right) &&
                 !ctrl && !shift && !alt);
            const bool is_jump =
                ((keysym == XK_Home || keysym == XK_End) &&
                 !ctrl && !shift && !alt);
            const bool is_esc = (keysym == XK_Escape);
            const bool is_sub_view_toggle =
                (keysym == XK_t && !ctrl && !shift && !alt);
            const bool is_ctrl_q =
                (ctrl && !shift && !alt && keysym == XK_q);
            const bool is_ctrl_w =
                (ctrl && !shift && !alt && keysym == XK_w);
            if (!(is_r || is_nav || is_commit || is_playback ||
                  is_scrub || is_jump || is_esc ||
                  is_sub_view_toggle || is_ctrl_q || is_ctrl_w)) {
                return;
            }
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

        // Ctrl+Alt+E: snapshot current authoring state into the in-memory
        // render queue. No disk writes; on-disk authoring files are untouched.
        // Settings are not snapshotted per-entry — the queue walker uses
        // the live settings_passthrough at execution time, mirroring the
        // chunk-U convention. (Chunk W: snapshots moved from disk to memory.)
        if (ctrl && alt && !shift &&
            (keysym == XK_e || keysym == XK_E)) {
            if (app.source_audio_path.empty()) return;
            AppState::QueuedRender q;
            q.source_audio_path = app.source_audio_path;
            q.markers           = app.markers.markers();
            q.transients        = app.transients.markers();
            app.queued_renders.push_back(std::move(q));
            std::fprintf(stderr,
                "warptempo_gui: queued render (%zu in queue)\n",
                app.queued_renders.size());
            return;
        }

        // Ctrl+Alt+R: single render into the source directory using `title`
        // from settings. Mirrors the pre-Chunk-W non-batch path inside
        // do_render: empty batch_folder/batch_basename triggers the
        // engine/limiter-prefix naming. Title-not-set is a hard error
        // surfaced from do_render. Does not consult the in-memory queue and
        // does not write any sidecars beyond the .peaks pyramid that
        // do_render already deposits.
        if (ctrl && alt && !shift &&
            (keysym == XK_r || keysym == XK_R)) {
            if (app.source_audio_path.empty()) return;

            RenderRequest req;
            req.source_audio_path    = app.source_audio_path;
            req.markers              = app.markers.markers();
            req.transients           = app.transients.markers();
            req.settings_passthrough = app.settings_passthrough;
            for (const auto& m : app.transients.markers()) {
                if (m.disabled) continue;
                req.transient_frames.push_back(m.effective_frame());
            }
            // Empty batch_folder/basename selects the source-dir naming
            // convention inside do_render.
            do_render(req);
            return;
        }

        // Ctrl+Alt+Shift+E: render the in-memory queue as one batch. Each
        // queued entry produces a sibling .wav (+ .warpmarkers /
        // .transientmarkers when non-empty / .peaks sidecars) inside a fresh
        // batch folder `<source_parent>/renders/<index>_render_all_in_queue/`.
        // The index is one greater than the highest pre-existing batch index
        // in that renders folder (regardless of command tag). Filenames
        // inside the batch are the entry position zero-padded to fit the
        // queue size: 01..10 for 10 entries, 1..7 for 7, 001..100 for 100.
        //
        // Empty queue is a silent no-op (no implicit-batch fallback —
        // single-shot rendering belongs on Ctrl+Alt+R now).
        //
        // Esc between entries drops the remainder. The current render
        // cannot be interrupted (no mid-engine cancellation); its sidecars
        // are written if it succeeds, then the loop exits and the rest of
        // the queue is discarded. The batch folder is left as-is on disk —
        // partial batches just contain fewer files than the queue had.
        // The in-memory queue is cleared after execution whether all
        // entries ran or Esc cut it short.
        if (ctrl && alt && shift &&
            (keysym == XK_e || keysym == XK_E)) {
            if (app.source_audio_path.empty()) return;
            if (app.queued_renders.empty()) return;

            std::vector<AppState::QueuedRender> entries =
                std::move(app.queued_renders);
            app.queued_renders.clear();

            std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path queue_root = src_parent / "renders";

            // Resolve the next batch index: max+1 over directory entries
            // matching `<digits>_<anything>`. Empty / missing renders/
            // folder seeds index 1.
            int next_index = 1;
            std::error_code ec;
            if (std::filesystem::is_directory(queue_root, ec)) {
                int max_idx = 0;
                for (const auto& de :
                     std::filesystem::directory_iterator(queue_root, ec)) {
                    if (!de.is_directory()) continue;
                    const std::string name = de.path().filename().string();
                    int v = 0;
                    size_t i = 0;
                    while (i < name.size() &&
                           name[i] >= '0' && name[i] <= '9') {
                        v = v * 10 + (name[i] - '0');
                        ++i;
                    }
                    if (i == 0 || i >= name.size() || name[i] != '_') continue;
                    if (v > max_idx) max_idx = v;
                }
                next_index = max_idx + 1;
            }

            const std::string command_tag = "render_all_in_queue";
            const std::filesystem::path batch_folder =
                queue_root /
                (std::to_string(next_index) + "_" + command_tag);
            std::filesystem::create_directories(batch_folder, ec);
            if (ec) {
                std::fprintf(stderr,
                    "warptempo_gui: render-all: could not create '%s': %s\n",
                    batch_folder.string().c_str(), ec.message().c_str());
                return;
            }

            // Width-to-fit zero-padding for filename indices. pad_width is
            // computed from the queue size and clamped to a sane upper
            // bound so the snprintf below has a known-bounded output.
            const int total = static_cast<int>(entries.size());
            int pad_width = 1;
            for (int n = total; n >= 10; n /= 10) ++pad_width;
            if (pad_width > 9) pad_width = 9;

            std::vector<RenderRequest> reqs;
            reqs.reserve(total);
            for (int i = 0; i < total; ++i) {
                const auto& q = entries[i];
                char num_buf[16];
                std::snprintf(num_buf, sizeof(num_buf),
                              "%0*d", pad_width, i + 1);
                std::fprintf(stderr,
                    "warptempo_gui: rendering entry %d of %d: %s/%s.wav\n",
                    i + 1, total,
                    batch_folder.filename().string().c_str(), num_buf);

                RenderRequest req;
                req.source_audio_path    = q.source_audio_path;
                req.markers              = q.markers;
                req.transients           = q.transients;
                req.settings_passthrough = app.settings_passthrough;
                for (const auto& m : q.transients) {
                    if (m.disabled) continue;
                    req.transient_frames.push_back(m.effective_frame());
                }
                req.batch_folder   = batch_folder.string();
                req.batch_basename = num_buf;
                reqs.push_back(std::move(req));
            }

            const auto result = run_render_batch(reqs, "render queue");
            if (result.cancelled) {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                    result.rendered, total);
            } else {
                std::fprintf(stderr,
                    "warptempo_gui: rendered %d of %d entries into %s\n",
                    result.rendered, total,
                    batch_folder.filename().string().c_str());
            }
            return;
        }

        // Chunk W: Ctrl+Alt+C commits the displayed render's markers
        // and transients into authoring memory. Single cross-file undo
        // entry; both warp_dirty and transient_dirty are recomputed.
        // After the commit succeeds: render-view exits, the parked
        // source audio is restored, and <source_parent>/renders/ is
        // recursively wiped — by definition the user has chosen one
        // render's parameters as the new baseline, so the prior batch
        // outputs are stale and shouldn't accumulate. Silent no-op
        // outside render-view.
        if (ctrl && alt && !shift &&
            (keysym == XK_c || keysym == XK_C)) {
            if (!app.render_view_enabled) return;
            if (app.render_view_index < 0) return;

            // Addendum 3: app.render_view_markers / _transients are now
            // render-domain (loaded from .renderwarpmarkers /
            // .rendertransientmarkers for display). The commit promotes
            // the render's *source-domain*
            // markers into authoring memory, so reload them from the
            // adjacent .warpmarkers / .transientmarkers sidecars at commit
            // time. Failure to read the source-domain warpmarkers aborts —
            // committing render-domain values into authoring would corrupt
            // the source coordinate system.
            const auto& cur_e =
                app.render_view_list[app.render_view_index];
            std::vector<GuiMarker>    src_warp;
            std::vector<GuiTransient> src_trans;
            {
                const std::filesystem::path wm =
                    cur_e.batch_folder / (cur_e.basename + ".warpmarkers");
                GuiMarkers m;
                if (!m.load(wm.string())) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: commit aborted, failed "
                        "to load %s\n", wm.string().c_str());
                    return;
                }
                src_warp = m.markers();
            }
            {
                const std::filesystem::path tm = cur_e.batch_folder /
                    (cur_e.basename + ".transientmarkers");
                std::error_code ec;
                if (std::filesystem::exists(tm, ec)) {
                    GuiTransients t;
                    if (t.load(tm.string())) {
                        src_trans = t.markers();
                    }
                    // Load failure: treat as empty transients (the
                    // load() call already logged its own diagnostics).
                }
            }

            std::vector<GuiMarker>    warp_pre  = app.markers.markers();
            std::vector<GuiTransient> trans_pre = app.transients.markers();
            std::set<int>             hint_sel  = app.selected_markers;
            const int                 hint_last = app.last_selected_marker;

            app.markers.markers_mut()    = std::move(src_warp);
            app.transients.markers_mut() = std::move(src_trans);
            app.selected_markers.clear();
            app.transient_selected_markers.clear();
            app.last_selected_marker = -1;
            // Brief J.2 Section 4: the active tab's per-mode slots
            // referenced the OLD app.markers/transients we just
            // replaced. Clear them so restore_source_audio loads
            // empty into the live pair (and so a later mode flip
            // doesn't surface stale indices).
            {
                ViewState& t = (app.active_tab == 'B') ? app.tab_b
                                                       : app.tab_a;
                t.warp_selected.clear();
                t.warp_last_selected      = -1;
                t.transient_selected.clear();
                t.transient_last_selected = -1;
            }

            push_undo_both(std::move(warp_pre), std::move(trans_pre),
                           'W', OpKind::Other,
                           std::move(hint_sel), hint_last);
            recompute_dirty();

            const std::filesystem::path src(app.source_audio_path);
            std::filesystem::path src_parent = src.parent_path();
            if (src_parent.empty()) src_parent = std::filesystem::path(".");
            const std::filesystem::path renders_root =
                src_parent / "renders";

            restore_source_audio();
            app.render_view_enabled = false;
            app.render_view_list.clear();
            app.render_view_markers.clear();
            app.render_view_transients.clear();
            app.render_view_index             = -1;
            app.render_view_src_F_begin       = 0;
            app.render_view_src_F_end         = 0;
            app.last_render_view_path.clear();

            std::error_code ec;
            if (std::filesystem::is_directory(renders_root, ec)) {
                std::filesystem::remove_all(renders_root, ec);
                if (ec) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: failed to wipe "
                        "%s: %s\n",
                        renders_root.string().c_str(),
                        ec.message().c_str());
                }
            }

            std::fprintf(stderr,
                "warptempo_gui: render-view: committed render and wiped "
                "renders/\n");
            gui.invalidate_region(0, 0, app.width, app.height);
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

        // `t` (no modifiers) toggles transient mode globally. Brief
        // J.2: render-view shares the global active_mode flag, so a
        // single handler serves both views. Render-view inherits the
        // engine / transients_enabled precondition checks from
        // toggle_active_mode.
        if (keysym == XK_t && !ctrl && !shift && !alt) {
            toggle_active_mode();
            return;
        }

        // V.B `i` (no modifiers) toggles iteration mode in warp. Silent
        // no-op in transient mode (transient flags carry no tempo to
        // iterate). The editor-active branch above already swallows any
        // keystroke while a popup edit is in flight, so this code only
        // runs with no active editor. Toggling repaints the top strip
        // so iteration popups appear or vanish in one frame.
        if (keysym == XK_i && !ctrl && !shift && !alt) {
            if (app.active_mode == 'W') {
                app.iteration_mode_enabled = !app.iteration_mode_enabled;
                clear_hover_popup();
                invalidate_top_strip();
            }
            return;
        }
        // V.B Shift+I bulk-clears every marker's iter values. Only fires
        // while iteration mode is on; otherwise silent no-op.
        if (keysym == XK_i && !ctrl && shift && !alt) {
            if (app.active_mode == 'W' && app.iteration_mode_enabled) {
                bulk_clear_iter_values();
            }
            return;
        }

        // Chunk W: plain `r` toggles render analysis mode. Source audio
        // must be loaded; otherwise silent no-op (nothing to base the
        // renders folder lookup on). Toggle-on enumerates the renders
        // folder and loads either the last-displayed render (if its
        // path is still in the list) or the first entry; an empty
        // enumeration aborts the toggle. Iteration mode is forcibly
        // disabled on entry per the chunk W brief; the prior value is
        // not restored on toggle-off — the user re-enables it
        // explicitly if desired.
        if (keysym == XK_r && !ctrl && !shift && !alt) {
            if (app.source_audio_path.empty()) return;
            if (app.loading) return;
            if (!app.render_view_enabled) {
                std::vector<AppState::RenderViewEntry> list =
                    enumerate_render_view_list();
                if (list.empty()) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: no renders found "
                        "under %s/renders/\n",
                        std::filesystem::path(app.source_audio_path)
                            .parent_path().string().c_str());
                    return;
                }
                // Brief F Section 4: migrate persisted selection from
                // the prior render-view session (still on the old
                // app.render_view_list) into the freshly enumerated
                // list, keyed by wav_path. Entries that disappeared
                // since last session simply lose their persisted state;
                // newly added entries start with default-empty
                // persistence (no match → load_render_view_at clears).
                if (!app.render_view_list.empty()) {
                    std::map<std::string,
                        AppState::RenderViewEntry*> prior;
                    for (auto& pe : app.render_view_list) {
                        prior[pe.wav_path.string()] = &pe;
                    }
                    for (auto& ne : list) {
                        auto it = prior.find(ne.wav_path.string());
                        if (it == prior.end()) continue;
                        const auto& src = *it->second;
                        ne.state           = src.state;
                        ne.persisted_size  = src.persisted_size;
                        ne.persisted_mtime = src.persisted_mtime;
                    }
                }
                int target = 0;
                if (!app.last_render_view_path.empty()) {
                    for (size_t i = 0; i < list.size(); ++i) {
                        if (list[i].wav_path.string() ==
                            app.last_render_view_path) {
                            target = static_cast<int>(i);
                            break;
                        }
                    }
                }
                app.render_view_src_sr    = audio.sample_rate();
                app.render_view_src_total = audio.total_frames();
                app.render_view_list      = std::move(list);
                app.iteration_mode_enabled = false;
                app.render_view_enabled    = true;
                // Brief J.2: render-view shares the global active_mode
                // flag, so the user's chosen mode carries across the
                // view-domain transition without per-entry restore.
                if (!load_render_view_at(target)) {
                    app.render_view_enabled = false;
                    app.render_view_list.clear();
                }
            } else {
                // Capture the just-viewed render's zoom/viewport/playhead
                // before restoring source-audio state. Not done on the
                // Ctrl+Alt+C commit path — the renders folder is wiped
                // immediately after commit, so the write would be lost.
                if (app.render_view_index >= 0 &&
                    app.render_view_index <
                        static_cast<int>(app.render_view_list.size())) {
                    write_rendersettings_for(
                        app.render_view_list[app.render_view_index]);
                }
                // Brief F Section 4: stash the live selection onto
                // the active entry so the next toggle-on can restore
                // it (gated by the wav's stat tuple still matching).
                // render_view_list is intentionally NOT cleared here
                // — re-entry migrates its persisted_* fields into the
                // freshly enumerated list.
                stash_render_view_selection_to_active_entry();
                restore_source_audio();
                app.render_view_enabled = false;
                app.render_view_markers.clear();
                app.render_view_transients.clear();
                app.render_view_index             = -1;
                app.render_view_src_F_begin       = 0;
                app.render_view_src_F_end         = 0;
            }
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
        // Shift+P: toggle inherit (warp only). Plain `p` is unbound.
        if (keysym == XK_p && !ctrl && !alt && shift) {
            if (app.active_mode == 'T') return;
            toggle_inherits();
            return;
        }
        // Shift+D: toggle disabled (warp + transient). Plain `d` is unbound.
        if (keysym == XK_d && !ctrl && !alt && shift) {
            if (app.active_mode == 'T') toggle_transient_disabled();
            else                        toggle_disabled();
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
            clear_hover_popup();
            refresh_active_tab_from_app();
            app.active_tab = (app.active_tab == 'A') ? 'B' : 'A';
            const ViewState& target = (app.active_tab == 'A') ? app.tab_a : app.tab_b;
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

        // Chunk W: Shift+Left / Shift+Right navigates the render-view
        // list with wraparound. Active only when render_view_enabled is
        // true; in source-view these chords fall through to the normal
        // playhead-by-pixel handler in the switch below. Wraparound
        // mirrors the brief: Shift+Right past the end loops to index 0,
        // Shift+Left before index 0 loops to the last entry.
        if (app.render_view_enabled && shift && !ctrl && !alt &&
            (keysym == XK_Left || keysym == XK_Right)) {
            const int n = static_cast<int>(app.render_view_list.size());
            if (n <= 0) return;
            int next = app.render_view_index;
            if (keysym == XK_Left)  next = (next - 1 + n) % n;
            else                    next = (next + 1) % n;
            // Capture the outgoing render's live zoom/viewport/playhead
            // before swapping.
            if (app.render_view_index >= 0 &&
                app.render_view_index <
                    static_cast<int>(app.render_view_list.size())) {
                write_rendersettings_for(
                    app.render_view_list[app.render_view_index]);
            }
            // Brief F Section 4: stash the outgoing entry's
            // selection so re-navigating back later (in the same
            // session) restores it. load_render_view_at then loads
            // the destination's own persisted state if its stat tuple
            // still matches; otherwise leaves selection empty.
            stash_render_view_selection_to_active_entry();
            load_render_view_at(next);
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

        // Bare-key dispatch. Every modifier-gated handler above this point
        // returns on match, so by the time we reach here, any modifier being
        // held means the chord had no binding and should be a silent no-op
        // — never fall through into a bare binding (e.g. Ctrl+Shift+Alt+E
        // must not toggle end-time via XK_e).
        if (!ctrl && !shift && !alt) {
            switch (keysym) {
            case XK_Escape: /* top-level Escape is a no-op (chunk Q) */ break;
            case XK_Left:   stop_playback_if_playing();
                            move_playhead_pixels(-1);         break;
            case XK_Right:  stop_playback_if_playing();
                            move_playhead_pixels(+1);         break;
            case XK_Up:     zoom_in();                        break;
            case XK_Down:   zoom_out();                       break;
            case XK_f: {
                const bool was_off = !app.follow_mode;
                app.follow_mode = !app.follow_mode;
                if (was_off && app.follow_mode &&
                    playback.is_playing()) {
                    playback.resync_predictor();
                }
                break;
            }
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
        // Prompt-modal input handling: while the bottom-strip prompt is
        // active, all mouse events are swallowed. Responses go through
        // the keyboard.
        if (app.prompt.active) return;
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

        // Chunk W: render-view mouse gate. Left-click on a marker line
        // (in the waveform area) or a flag rect (in the top strip)
        // toggles selection and jumps the playhead to the marker;
        // left-click elsewhere in the waveform area positions the
        // playhead (with playback stop) and clears the selection unless
        // Shift is held. Wheel zoom and Alt/Ctrl+Alt+wheel scroll are
        // pure viewport ops and pass through; Ctrl+wheel marker-nudge
        // is gated out (mutates markers). Drag-create and top-strip
        // playhead movement are silent no-ops so the read-only
        // invariant on marker state is preserved. Hover-popup motion
        // still runs in the motion handler against render_view_markers.
        if (app.render_view_enabled) {
            if (button == 4 || button == 5) {
                handle_wheel(button, ctrl, alt,
                             inside_waveform, inside_top,
                             /*allow_nudge=*/false);
                return;
            }
            if (button != 1) return;
            // Brief F Section 3: in transient sub-view, top-strip clicks
            // are silent no-ops (transients have no flag rects). Bail
            // before hit-testing so we don't attempt selection bookkeeping
            // on a non-existent flag pack.
            if (app.active_mode == 'T' && inside_top) return;
            int hit = -1;
            if (inside_waveform)  hit = hit_test_marker_line(x);
            else if (inside_top)  hit = hit_test_flag(x, y);
            else                  return;
            // Brief J.2 Section 3: live selection lives in the global
            // pair regardless of view domain. active_mode tells us
            // which marker list the indices map to.
            const bool sub_t = (app.active_mode == 'T');
            std::set<int>& sel = app.selected_markers;
            int& last_sel      = app.last_selected_marker;
            const int n = sub_t
                ? static_cast<int>(app.render_view_transients.size())
                : static_cast<int>(app.render_view_markers.size());
            if (hit >= 0 && hit < n) {
                if (shift) {
                    auto it = sel.find(hit);
                    if (it == sel.end()) {
                        sel.insert(hit);
                        last_sel = hit;
                    } else {
                        sel.erase(it);
                        if (last_sel == hit) {
                            last_sel = sel.empty()
                                ? -1
                                : *sel.rbegin();
                        }
                    }
                } else {
                    sel.clear();
                    sel.insert(hit);
                    last_sel = hit;
                }
                gui.invalidate_region(0, 0, app.width, app.height);
                const int sr = audio.sample_rate();
                int64_t sample;
                if (sub_t) {
                    sample = app.render_view_transients[hit].effective_frame();
                } else {
                    sample = static_cast<int64_t>(std::llround(
                        app.render_view_markers[hit].time_seconds *
                        static_cast<double>(sr)));
                }
                move_playhead_to(sample);
                // Brief F Section 2: any waveform-area press starts a
                // playhead-drag gesture. Top-strip flag-click does not.
                if (inside_waveform) {
                    app.playhead_drag.active = true;
                }
                return;
            }
            // Empty-space click in the waveform area: clear the active
            // sub-view's selection (unless Shift) and move the playhead.
            // Brief F Section 2: also start a playhead-drag gesture so
            // the motion handler's snap logic kicks in.
            if (inside_waveform) {
                if (!shift &&
                    (!sel.empty() || last_sel != -1)) {
                    sel.clear();
                    last_sel = -1;
                    gui.invalidate_region(0, 0, app.width, app.height);
                }
                stop_playback_if_playing();
                const double spp = current_samples_per_pixel(app, audio);
                int rel = x - area.x;
                if (rel < 0) rel = 0;
                if (rel >= area.w) rel = area.w - 1;
                const int64_t sample =
                    app.viewport_start_sample +
                    static_cast<int64_t>(std::llround(rel * spp));
                move_playhead_to(sample);
                app.playhead_drag.active = true;
            }
            return;
        }

        if (button == 1) {
            // Any button-1 press on the waveform / top strip stops
            // playback. Per Part 4 of chunk P patch 1: the user pressed
            // a mouse button, they want attention — even a Ctrl+press on
            // empty space (a no-op for the playhead) stops the audio.
            if (inside_waveform || inside_top) stop_playback_if_playing();

            // V.A1 / V.B editor: mouse handling.
            //   click inside top strip on the editing target: no-op
            //   click inside top strip on a different popup/flag: switch
            //     target (iter popup wins over the flag below it when
            //     iteration mode is on)
            //   click anywhere else: exit edit (no commit), then fall
            //     through so the click routes through normal handling.
            if (gui_text_editor::is_active(app.top_flag_editor)) {
                if (inside_top) {
                    const int iter_hit = hit_test_iter_popup(x, y);
                    if (iter_hit >= 0) {
                        if (app.top_flag_editor.kind ==
                                gui_text_editor::Kind::IterationBracket &&
                            iter_hit == app.top_flag_editor.target) {
                            return; // no-op on same popup
                        }
                        enter_iter_edit(iter_hit);
                        return;
                    }
                    const int hit_now = hit_test_flag(x, y);
                    if (app.top_flag_editor.kind ==
                            gui_text_editor::Kind::FlagPayload &&
                        hit_now == app.top_flag_editor.target) {
                        return; // no-op on same flag
                    }
                    if (hit_now >= 0 && app.active_mode != 'T') {
                        enter_top_flag_edit(hit_now);
                        return;
                    }
                    // Top strip click that isn't on a popup or flag: exit
                    // and fall through to normal handling.
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

            // Iteration popup click takes priority over flag click when
            // iteration mode is on. The popup sits above the flag rect so
            // their hit zones don't overlap, but checking the popup first
            // makes the intent unambiguous when the flag-strip extents
            // change shape.
            if (inside_top && !ctrl) {
                const int iter_hit = hit_test_iter_popup(x, y);
                if (iter_hit >= 0) {
                    enter_iter_edit(iter_hit);
                    return;
                }
            }

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
                    } else {
                        // Shift+press on marker: selection otherwise preserved;
                        // add hit if not already present.
                        const bool was_in = app.selected_markers.count(hit) > 0;
                        if (!was_in) {
                            app.selected_markers.insert(hit);
                            invalidate_marker_column(hit);
                            invalidate_top_strip();
                        }
                        app.last_selected_marker = hit;
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
                    app.playhead_drag.active = true;
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
                    app.playhead_drag.active = true;
                }
            }
        } else if (button == 4 || button == 5) {
            handle_wheel(button, ctrl, alt,
                         inside_waveform, inside_top,
                         /*allow_nudge=*/true);
        }
    });

    gui.set_on_button_release([&](unsigned int button, int, int,
                                  unsigned int mods) {
        if (app.prompt.active) return;
        if (button != 1) return;
        if (app.playhead_drag.active) {
            // Brief F Section 1: if the playhead snapped onto a marker
            // during the drag, commit selection on release. Plain release
            // sets the snapped marker as the single selection; Shift
            // release adds it to the existing set without removing
            // anything. Off-marker release leaves selection alone.
            const bool shift = (mods & ShiftMask) != 0;
            const int  sr    = audio.sample_rate();
            int snapped = -1;
            if (sr > 0) {
                const int64_t ph = app.playhead_sample;
                if (app.render_view_enabled) {
                    if (app.active_mode == 'T') {
                        const auto& mv = app.render_view_transients;
                        for (size_t i = 0; i < mv.size(); ++i) {
                            if (mv[i].effective_frame() == ph) {
                                snapped = static_cast<int>(i);
                                break;
                            }
                        }
                    } else {
                        const auto& mv = app.render_view_markers;
                        for (size_t i = 0; i < mv.size(); ++i) {
                            const int64_t s = static_cast<int64_t>(
                                std::llround(mv[i].time_seconds *
                                             static_cast<double>(sr)));
                            if (s == ph) {
                                snapped = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                } else if (app.active_mode == 'T') {
                    const auto& mv = app.transients.markers();
                    for (size_t i = 0; i < mv.size(); ++i) {
                        if (mv[i].effective_frame() == ph) {
                            snapped = static_cast<int>(i);
                            break;
                        }
                    }
                } else {
                    const auto& mv = app.markers.markers();
                    for (size_t i = 0; i < mv.size(); ++i) {
                        const int64_t s = static_cast<int64_t>(
                            std::llround(mv[i].time_seconds *
                                         static_cast<double>(sr)));
                        if (s == ph) {
                            snapped = static_cast<int>(i);
                            break;
                        }
                    }
                }
            }
            if (snapped >= 0) {
                if (app.render_view_enabled) {
                    // Brief J.2 Section 3: render-view writes the
                    // global live pair. active_mode tells us which
                    // marker list the indices map to.
                    if (shift) {
                        app.selected_markers.insert(snapped);
                        app.last_selected_marker = snapped;
                    } else {
                        app.selected_markers.clear();
                        app.selected_markers.insert(snapped);
                        app.last_selected_marker = snapped;
                    }
                    gui.invalidate_region(0, 0, app.width, app.height);
                } else if (shift) {
                    app.selected_markers.insert(snapped);
                    app.last_selected_marker = snapped;
                    invalidate_marker_column(snapped);
                    invalidate_top_strip();
                } else {
                    set_single_selection(snapped);
                }
            }
            app.playhead_drag = PlayheadDragState{};
            return;
        }
        if (!app.drag.active) return;
        commit_drag();
    });

    gui.set_on_motion([&](int mouse_x, int mouse_y, unsigned int mods) {
        if constexpr (kDebugPerf) {
            app.last_input_event_time = std::chrono::steady_clock::now();
        }
        // V.A3b Addendum 3: record latest cursor coords so viewport
        // mutators can re-evaluate hover at the cursor's last position.
        app.last_mouse_x = mouse_x;
        app.last_mouse_y = mouse_y;
        if (app.prompt.active) {
            clear_hover_popup();
            return;
        }
        // Chunk W: render-view motion handler. Brief F Section 2 adds
        // playhead-drag snap support: when a drag is in flight, snap the
        // playhead to the visible sub-view's markers (3px epsilon),
        // matching source-view's gesture. Otherwise run hover popup
        // detection against render_view_markers (suppressed in transient
        // sub-view because hit_test_flag short-circuits to -1).
        if (app.render_view_enabled) {
            if (app.playhead_drag.active) {
                clear_hover_popup();
                if ((mods & Button1Mask) == 0) {
                    app.playhead_drag = PlayheadDragState{};
                    return;
                }
                const int sr = audio.sample_rate();
                if (sr <= 0) return;
                const GuiRect area = waveform_area(app);
                const double spp = current_samples_per_pixel(app, audio);
                if (spp <= 0.0) return;
                const int hit = hit_test_marker_line(mouse_x);
                int64_t new_playhead;
                if (hit >= 0) {
                    if (app.active_mode == 'T') {
                        new_playhead =
                            app.render_view_transients[hit].effective_frame();
                    } else {
                        new_playhead = static_cast<int64_t>(std::llround(
                            app.render_view_markers[hit].time_seconds *
                            static_cast<double>(sr)));
                    }
                } else {
                    int rel = mouse_x - area.x;
                    if (rel < 0) rel = 0;
                    if (rel >= area.w) rel = area.w - 1;
                    new_playhead = app.viewport_start_sample +
                        static_cast<int64_t>(std::llround(rel * spp));
                }
                if (new_playhead != app.playhead_sample) {
                    move_playhead_to(new_playhead);
                }
                return;
            }
            const int hit = hit_test_flag(mouse_x, mouse_y);
            if (hit != app.hover_popup.marker_index) {
                if (app.hover_popup.visible) invalidate_top_strip();
                app.hover_popup.marker_index = hit;
                app.hover_popup.visible      = false;
                app.hover_popup.entry_time   =
                    std::chrono::steady_clock::now();
                app.hover_popup.cached_text =
                    popup_eligible_marker(hit)
                        ? compute_hover_popup_text(
                              app.render_view_markers, hit,
                              app.render_view_src_sr)
                        : std::string();
            }
            return;
        }
        if (app.playhead_drag.active) {
            clear_hover_popup();
            // Left button must still be held; if not, the release was lost —
            // terminate the drag. Modifier changes mid-drag are ignored.
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
            // Selection is fixed at press time and is NOT mutated here; the
            // snap is purely a playhead-positioning magnet.
            const int hit = hit_test_marker_line(mouse_x);
            int64_t new_playhead;
            if (hit >= 0) {
                if (app.active_mode == 'T') {
                    new_playhead = app.transients.markers()[hit].effective_frame();
                } else {
                    new_playhead = static_cast<int64_t>(std::llround(
                        app.markers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
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
            return;
        }
        if (!app.drag.active) {
            // No active gesture: run hover-popup detection. Only in warp
            // mode, with no editor, no dialog (already returned), no drag,
            // and not while iteration mode owns the popup space.
            // The dwell timer is started/restarted on every transition into
            // an eligible rect; the on_tick handler flips visibility.
            if (app.active_mode == 'W' &&
                !app.iteration_mode_enabled &&
                !gui_text_editor::is_active(app.top_flag_editor) &&
                !app.queue_running) {
                const int hit = hit_test_flag(mouse_x, mouse_y);
                if (hit != app.hover_popup.marker_index) {
                    if (app.hover_popup.visible) invalidate_top_strip();
                    app.hover_popup.marker_index = hit;
                    app.hover_popup.visible      = false;
                    app.hover_popup.entry_time   =
                        std::chrono::steady_clock::now();
                    // Precompute popup text at rect-entry so the
                    // delay-completion paint doesn't repeat the math.
                    app.hover_popup.cached_text =
                        popup_eligible_marker(hit)
                            ? compute_hover_popup_text(
                                  app.markers.markers(), hit,
                                  audio.sample_rate())
                            : std::string();
                }
            } else {
                clear_hover_popup();
            }
            return;
        }
        // A drag is active — drop any pending popup.
        clear_hover_popup();
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
                if (playback.is_playing()) playback.resync_predictor();
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
        app.hover_popup    = HoverPopupState{};

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

        // Companion files: discover paths, create <basename>.warpmarkers if
        // missing. `.settings` is GUI-owned now — not pre-created on load;
        // first save materializes it. Companion file convention is
        // <source_dir>/<source_basename>.<ext> (sibling, basename-prefixed),
        // not the legacy hidden `./.warpmarkers` form.
        std::filesystem::path apath(path);
        std::filesystem::path parent = apath.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        const std::string stem = apath.stem().string();
        const std::filesystem::path wm_path  = parent / (stem + ".warpmarkers");
        const std::filesystem::path tm_path  = parent / (stem + ".transientmarkers");
        const std::filesystem::path set_path = parent / (stem + ".settings");
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
        ViewState default_tab;
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
                             ViewState& dst) {
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
            if (app.viewport_start_sample != old_vp) {
                invalidate_waveform_area();
                if (playback.is_playing()) playback.resync_predictor();
            }
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

        // V.A3b: dwell-driven popup show. The motion handler already gates
        // on warp mode + no editor + no drag + no dialog and clears
        // hover_popup when those conditions break, so here it's enough to
        // check the elapsed time and re-validate eligibility.
        if (!app.hover_popup.visible &&
            app.hover_popup.marker_index >= 0 &&
            popup_eligible_marker(app.hover_popup.marker_index)) {
            const auto now = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - app.hover_popup.entry_time).count();
            if (ms >= kHoverDelayMs) {
                app.hover_popup.visible = true;
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
        if (app.is_playing || playback.is_playing()) return gui.playback_tick_ms();
        // While the top-flag editor is active, wake periodically so the
        // cursor blink can flip. ~125ms gives ample resolution on a
        // 500ms half-period without burning CPU.
        if (gui_text_editor::is_active(app.top_flag_editor)) return 125;
        // V.A3b: while a hover is pending (dwell timer running but popup
        // not yet visible), wake periodically so the tick-driven check
        // can flip visibility on time. Once visible, no extra wake is
        // needed — input events drive any state change.
        if (!app.hover_popup.visible && app.hover_popup.marker_index >= 0) {
            return 125;
        }
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
