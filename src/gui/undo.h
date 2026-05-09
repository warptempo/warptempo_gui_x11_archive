#pragma once

#include "app_state.h"
#include "selection.h"
#include "viewport.h"

#include <functional>
#include <vector>

// X.7.3: undo-cluster operations, extracted from main.cpp's inline lambdas.
// The struct holds references to the long-lived state the methods read and
// write; bodies are byte-identical to the originals modulo `this->` access
// on the captured references. The two std::function references
// (clear_hover_popup, stop_playback_if_playing) belong to lambdas that stay
// in main.cpp because they have many callers outside the undo cluster —
// same pattern as recompute_hover_at_cursor on Viewport.
struct Undo {
    AppState&              app;
    Viewport&              viewport;
    Selection&             selection;
    std::function<void()>& clear_hover_popup;
    std::function<void()>& stop_playback_if_playing;

    Undo(AppState&              app_,
         Viewport&              viewport_,
         Selection&             selection_,
         std::function<void()>& clear_hover_popup_,
         std::function<void()>& stop_playback_if_playing_)
        : app(app_),
          viewport(viewport_),
          selection(selection_),
          clear_hover_popup(clear_hover_popup_),
          stop_playback_if_playing(stop_playback_if_playing_) {}

    void recompute_dirty();
    void push_undo(std::vector<GuiWarpMarker> pre_state, OpKind op_kind,
                   int hint_last);
    void push_undo_phase_reset(std::vector<GuiPhaseResetMarker> pre_state,
                             OpKind op_kind, int hint_last);
    void push_undo_both(std::vector<GuiWarpMarker> warp_pre,
                        std::vector<GuiPhaseResetMarker> trans_pre,
                        char op_mode, OpKind op_kind, int hint_last);
    void apply_post_restore_rules_warp(const UndoEntry& entry,
                                       const std::vector<GuiWarpMarker>& before);
    void apply_post_restore_rules_phase_reset(const UndoEntry& entry,
                                            const std::vector<GuiPhaseResetMarker>& before);
    void do_undo();
    void do_redo();
};
