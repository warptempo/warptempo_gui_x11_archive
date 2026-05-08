#pragma once

#include "app_state.h"
#include "selection.h"
#include "undo.h"
#include "viewport.h"

#include <functional>
#include <utility>

class GuiAudio;
class GuiX11;

// X.7.5a: warp-authoring cluster, extracted from main.cpp's inline
// lambdas. Covers the basic authoring lambdas (drop / delete / toggle /
// adjust / clear), the drag cluster (begin / apply / commit, mode-aware
// across warp and transient lists), and the selection-shift cluster
// (nudge / jump-to-playhead and their shared bounds helper). The struct
// holds references to the long-lived state and the std::function lambdas
// the methods read and write; bodies are byte-identical to the originals
// modulo `this->` access on the captured references and the lambda-call
// rewriting documented in warpmarkers_ops.cpp.
//
// The drag methods stay mode-aware: warp drag is the dominant case and
// transient drag was bolted on later. apply_drag_motion's transient
// branch reaches the free-function apply_transient_position_delta
// (declared in transientmarkers_ops.h). The GuiX11 reference is for
// apply_drag_motion's gui.invalidate_region calls during drag.
struct GuiWarpMarkersOps {
    AppState&                                     app;
    const GuiAudio&                               audio;
    GuiX11&                                       gui;
    Viewport&                                     viewport;
    Selection&                                    selection;
    Undo&                                         undo;
    std::function<void()>&                        clear_hover_popup;
    std::function<void()>&                        stop_playback_if_playing;
    std::function<FlagLoc(bool, int)>&            find_flag;

    GuiWarpMarkersOps(AppState&                                     app_,
                      const GuiAudio&                               audio_,
                      GuiX11&                                       gui_,
                      Viewport&                                     viewport_,
                      Selection&                                    selection_,
                      Undo&                                         undo_,
                      std::function<void()>&                        clear_hover_popup_,
                      std::function<void()>&                        stop_playback_if_playing_,
                      std::function<FlagLoc(bool, int)>&            find_flag_)
        : app(app_),
          audio(audio_),
          gui(gui_),
          viewport(viewport_),
          selection(selection_),
          undo(undo_),
          clear_hover_popup(clear_hover_popup_),
          stop_playback_if_playing(stop_playback_if_playing_),
          find_flag(find_flag_) {}

    void drop_marker(double time_seconds, bool inherit);
    void drop_marker_at_playhead();
    void drop_inherit_marker_at_playhead();
    void delete_selected_marker();
    void force_delete_selected_marker();
    void toggle_inherits();
    void toggle_disabled();
    void toggle_begin_time();
    void toggle_end_time();
    void adjust_tempo(double delta);
    void clear_trim();
    bool begin_drag(int hit, int mouse_x);
    void apply_drag_motion(double raw_delta);
    void commit_drag();
    std::pair<double, double> compute_selection_delta_bounds(bool& ok);
    bool apply_selection_shift(double raw_delta);
    void nudge_selected_markers(int direction);
    void jump_selection_to_playhead();
};
