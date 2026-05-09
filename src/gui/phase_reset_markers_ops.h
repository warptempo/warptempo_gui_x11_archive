#pragma once

#include "app_state.h"
#include "selection.h"
#include "undo.h"
#include "viewport.h"

#include <cstdint>
#include <functional>

class GuiAudio;

// Free function: apply a position delta (in seconds) to one phase reset's
// time_seconds. Defined at file scope in main.cpp; called from
// GuiPhaseResetMarkersOps's methods (this TU) and from the warp drag
// handler (in main.cpp). Promoted from a captureless lambda in X.7.4 so
// the cluster could be extracted without dragging the lambda along by
// reference.
void apply_phase_reset_position_delta(GuiPhaseResetMarker& m, double delta_seconds);

// X.7.4: phase reset authoring cluster, extracted from main.cpp's inline
// lambdas. The struct holds references to the long-lived state and the
// std::function lambdas the methods read and write; bodies are byte-
// identical to the originals modulo `this->` access on the captured
// references and the lambda-call rewriting documented in
// phase_reset_markers_ops.cpp.
struct GuiPhaseResetMarkersOps {
    AppState&                                     app;
    const GuiAudio&                               audio;
    Viewport&                                     viewport;
    Selection&                                    selection;
    Undo&                                         undo;
    std::function<void()>&                        clear_hover_popup;
    std::function<void()>&                        stop_playback_if_playing;
    std::function<FlagLoc(bool, int)>&            find_flag;

    GuiPhaseResetMarkersOps(AppState&                                     app_,
                           const GuiAudio&                               audio_,
                           Viewport&                                     viewport_,
                           Selection&                                    selection_,
                           Undo&                                         undo_,
                           std::function<void()>&                        clear_hover_popup_,
                           std::function<void()>&                        stop_playback_if_playing_,
                           std::function<FlagLoc(bool, int)>&            find_flag_)
        : app(app_),
          audio(audio_),
          viewport(viewport_),
          selection(selection_),
          undo(undo_),
          clear_hover_popup(clear_hover_popup_),
          stop_playback_if_playing(stop_playback_if_playing_),
          find_flag(find_flag_) {}

    void drop_phase_reset_at_position(double time_seconds);
    void drop_phase_reset_at_playhead();
    void delete_selected_phase_reset();
    void toggle_phase_reset_disabled();
    std::pair<double, double> compute_phase_reset_delta_bounds(bool& ok);
    void nudge_selected_phase_resets(int direction);
    void jump_phase_reset_selection_to_playhead();
};
