#pragma once

#include "app_state.h"
#include "selection.h"
#include "undo.h"
#include "viewport.h"

#include <cstdint>
#include <functional>

class GuiAudio;

// Free function: apply a position delta to one transient's effective frame.
// Defined at file scope in main.cpp; called from Transients's methods (this
// TU) and from the warp drag handler (in main.cpp). Promoted from a
// captureless lambda in X.7.4 so the cluster could be extracted without
// dragging the lambda along by reference.
void apply_transient_position_delta(GuiTransientMarker& m, int64_t delta);

// X.7.4: transient-authoring cluster, extracted from main.cpp's inline
// lambdas. The struct holds references to the long-lived state and the
// std::function lambdas the methods read and write; bodies are byte-
// identical to the originals modulo `this->` access on the captured
// references and the lambda-call rewriting documented in transients.cpp.
struct Transients {
    AppState&                                     app;
    const GuiAudio&                               audio;
    Viewport&                                     viewport;
    Selection&                                    selection;
    Undo&                                         undo;
    std::function<void()>&                        clear_hover_popup;
    std::function<void()>&                        stop_playback_if_playing;
    std::function<FlagLoc(bool, bool, int)>&      find_flag;
    std::function<void()>&                        open_prompt_detect_confirm;

    Transients(AppState&                                     app_,
               const GuiAudio&                               audio_,
               Viewport&                                     viewport_,
               Selection&                                    selection_,
               Undo&                                         undo_,
               std::function<void()>&                        clear_hover_popup_,
               std::function<void()>&                        stop_playback_if_playing_,
               std::function<FlagLoc(bool, bool, int)>&      find_flag_,
               std::function<void()>&                        open_prompt_detect_confirm_)
        : app(app_),
          audio(audio_),
          viewport(viewport_),
          selection(selection_),
          undo(undo_),
          clear_hover_popup(clear_hover_popup_),
          stop_playback_if_playing(stop_playback_if_playing_),
          find_flag(find_flag_),
          open_prompt_detect_confirm(open_prompt_detect_confirm_) {}

    void drop_transient_at_position(double time_seconds);
    void drop_transient_at_playhead();
    void delete_selected_transient();
    void toggle_transient_disabled();
    std::pair<int64_t, int64_t> compute_transient_delta_bounds(bool& ok);
    void nudge_selected_transients(int direction);
    void jump_transient_selection_to_playhead();
    void toggle_transient_begin_time();
    void toggle_transient_end_time();
    void merge_detection(const std::vector<int64_t>& fresh_src_frames);
    void run_detect_now();
    void detect_transients();
    void clear_all_transients();
};
