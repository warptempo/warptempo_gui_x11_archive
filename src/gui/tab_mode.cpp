#include "tab_mode.h"

#include <cstdio>
#include <string>

// X.7.7: mode/tab management cluster. Method bodies are byte-identical to
// the lambdas they replaced in main.cpp, with these mechanical rewrites:
//
//   active_view_state,
//   refresh_active_tab_from_app,
//   switch_active_mode_to,
//   switch_active_tab_to            → this->method_name (intra-cluster calls)
//   prune_live_selection            → selection.prune_live_selection
//   invalidate_waveform_area        → viewport.invalidate_waveform_area
//   invalidate_timestamp_area       → viewport.invalidate_timestamp_area
//   clear_hover_popup               → std::function ref (called as f())
//   stop_playback_if_playing        → std::function ref (called as f())
//   settings_get                    → free function, takes app explicitly
//
// Free function calls (clamp_viewport_start) keep their original spelling —
// declared at file scope in app_state.h.

// Overwrite the active tab's snapshot with the live AppState viewport /
// zoom / playhead. Shared by Ctrl+Tab (pre-flip) and Ctrl+S (pre-write)
// so "remembered spot" semantics stay consistent between the two paths.
// Also stashes the active selection into the per-mode slot so a tab
// flip + mode flip can restore the right pair on return.
void GuiTabMode::refresh_active_tab_from_app() {
    ViewState& t = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
    t.viewport_start_sample = app.viewport_start_sample;
    t.zoom_level            = app.zoom_level;
    t.playhead_sample       = app.playhead_sample;
    if (app.active_mode == 'P') {
        t.phase_reset_selected      = app.selected_markers;
        t.phase_reset_last_selected = app.last_selected_marker;
    } else {
        t.warp_selected           = app.selected_markers;
        t.warp_last_selected      = app.last_selected_marker;
    }
}

// Brief J.2 Section 1: indirection that returns the currently
// active ViewState — the slot that holds the inactive-mode
// selection. Source-view: the active tab. Render-view: the
// active render entry's `state`. Returns nullptr when no valid
// active view-state is available; callers must handle nullptr
// by no-op-ing rather than silently corrupting a fallback slot.
ViewState* GuiTabMode::active_view_state() {
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
}

// Toggle active editing mode between 'W' (warp) and 'P' (phase reset).
// Saves the active selection into the leaving mode's per-tab slot,
// then restores the destination mode's slot. Visible state (viewport /
// zoom / playhead) is unaffected. Caller decides what invalidations to
// run; this helper just shuffles the AppState fields.
void GuiTabMode::switch_active_mode_to(char target_mode) {
    if (target_mode == app.active_mode) return;
    ViewState* vs = this->active_view_state();
    if (!vs) return;
    if (app.active_mode == 'P') {
        vs->phase_reset_selected      = app.selected_markers;
        vs->phase_reset_last_selected = app.last_selected_marker;
        app.selected_markers        = vs->warp_selected;
        app.last_selected_marker    = vs->warp_last_selected;
    } else {
        vs->warp_selected           = app.selected_markers;
        vs->warp_last_selected      = app.last_selected_marker;
        app.selected_markers        = vs->phase_reset_selected;
        app.last_selected_marker    = vs->phase_reset_last_selected;
    }
    app.active_mode = target_mode;
    selection.prune_live_selection();
    clear_hover_popup();
}

// Ctrl+Tab toggles A/B navigational tabs. Stops playback, saves
// current viewport/zoom/playhead to the leaving tab, restores the
// target tab. Does not mark the document dirty.
void GuiTabMode::switch_active_tab_to(char target_tab) {
    // Synchronous stop so the next tick doesn't snap the playhead
    // back to the audio cursor, overwriting the target tab's
    // stored playhead.
    stop_playback_if_playing();
    clear_hover_popup();
    this->refresh_active_tab_from_app();
    app.active_tab = target_tab;
    const ViewState& target = (app.active_tab == 'A') ? app.tab_a : app.tab_b;
    app.viewport_start_sample = target.viewport_start_sample;
    app.zoom_level            = target.zoom_level;
    app.playhead_sample       = target.playhead_sample;
    // Restore the active selection from the destination tab's
    // current-mode slot. Mode itself is per-AppState (not per-tab),
    // so the destination tab's other-mode slot stays warm for any
    // future `p` flip back inside that tab.
    if (app.active_mode == 'P') {
        app.selected_markers     = target.phase_reset_selected;
        app.last_selected_marker = target.phase_reset_last_selected;
    } else {
        app.selected_markers     = target.warp_selected;
        app.last_selected_marker = target.warp_last_selected;
    }
    clamp_viewport_start(app, audio);
    // invalidate_waveform_area covers the top strip + waveform
    // (including the playhead column inside it); the timestamp
    // area holds the letter and the ts text.
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// `p` key: toggle into/out of phase reset mode. Entry preconditions
// (only when going W → P): engine setting must be `warptempo`. Exit
// (P → W) is unconditional.
void GuiTabMode::toggle_active_mode() {
    if (app.active_mode == 'P') {
        this->switch_active_mode_to('W');
    } else {
        const std::string engine =
            settings_get(app, "engine", "warptempo");
        if (engine != "warptempo") {
            std::fprintf(stderr,
                "warptempo_gui: phase_reset mode unavailable: "
                "engine=%s\n", engine.c_str());
            return;
        }
        this->switch_active_mode_to('P');
    }
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}
