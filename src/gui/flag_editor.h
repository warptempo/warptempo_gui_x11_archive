#pragma once

#include "app_state.h"
#include "undo.h"
#include "viewport.h"
#include "warpmarkers.h"

#include <functional>
#include <string>

// X.7.5b: flag-editor cluster, extracted from main.cpp's inline lambdas.
// Covers the top-flag canonical-line editor (V.A1), the V.B iteration
// popup editor, the Brief X.2 BPM popup editor, the Shift+I/Shift+M bulk
// clears, and the BPM-mode enter/exit transitions. The struct holds
// references to the long-lived state and the std::function lambdas the
// methods read and write; bodies are byte-identical to the originals
// modulo `this->` access on the captured references and the lambda-call
// rewriting documented in flag_editor.cpp.
//
// Smaller reference-member set than GuiWarpMarkersOps: the editors don't
// touch sample_rate, selection methods, the platform layer, or playback.
// They operate on the text editor state machine and write parsed values
// into the warp marker container directly via app.warpmarkers.marker_mut.
struct GuiFlagEditor {
    AppState&              app;
    Viewport&              viewport;
    Undo&                  undo;
    std::function<void()>& clear_hover_popup;

    GuiFlagEditor(AppState&              app_,
                  Viewport&              viewport_,
                  Undo&                  undo_,
                  std::function<void()>& clear_hover_popup_)
        : app(app_),
          viewport(viewport_),
          undo(undo_),
          clear_hover_popup(clear_hover_popup_) {}

    std::string build_locked_prefix(const GuiWarpMarker& m);
    void exit_top_flag_edit_no_commit();
    void enter_top_flag_edit(int idx);
    void commit_top_flag_edit();
    void enter_iter_edit(int idx);
    void commit_iter_edit();
    void bulk_clear_iter_values();
    void enter_bpm_edit(int idx);
    void commit_bpm_edit();
    void bulk_clear_bpm_values();
    void enter_bpm_mode();
    void exit_bpm_mode();
};
