#pragma once

#include "app_state.h"
#include "undo.h"
#include "viewport.h"

// Copy/paste operations for the W-mode phase reset propagate feature.
// Both methods operate on warp-marker selection in W-mode and mutate
// the phase reset list as a side effect (paste only). Mode/selection-
// count gating is the caller's responsibility, except for the
// empty-clipboard silent no-op which lives inside paste_apply.

struct PhaseResetPropagate {
    AppState& app;
    Viewport& viewport;
    Undo&     undo;

    PhaseResetPropagate(AppState& app_, Viewport& viewport_, Undo& undo_)
        : app(app_), viewport(viewport_), undo(undo_) {}

    // Ctrl+T copy. Caller has already verified W-mode + exactly two
    // warp markers selected. Replaces the clipboard with the named
    // blocks and fractional placements derived from the half-open
    // [first, last) source range. Non-mutating beyond clipboard
    // state — no undo entry, no marker changes.
    void copy_from_selection();

    // Build the named-block list for the paste confirmation prompt
    // and stash the anchor on AppState. Caller has verified W-mode +
    // exactly one warp marker selected and a non-empty clipboard.
    void open_paste_confirmation();

    // Materialize the paste against the destination anchor stashed in
    // AppState::pending_paste_anchor. Walks the destination block list
    // in lockstep with the clipboard, stops on the first name divergence,
    // and produces a single undo entry covering all materialized blocks.
    void paste_apply();
};
