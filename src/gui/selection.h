#pragma once

#include "app_state.h"
#include "viewport.h"

#include <set>

class GuiAudio;
class GuiPlayback;

// X.7.2: selection-cluster operations, extracted from main.cpp's inline
// lambdas. The struct holds references to the long-lived state the methods
// read and write; bodies are byte-identical to the originals modulo `this->`
// access on the captured references.
struct Selection {
    AppState&       app;
    const GuiAudio& audio;
    Viewport&       viewport;
    GuiPlayback&    playback;

    Selection(AppState&       app_,
              const GuiAudio& audio_,
              Viewport&       viewport_,
              GuiPlayback&    playback_)
        : app(app_),
          audio(audio_),
          viewport(viewport_),
          playback(playback_) {}

    void repair_last_selected();
    void set_single_selection(int idx);
    void clear_selection();
    bool toggle_selection_membership(int idx);
    void sanitize_selection_after_restore(int n);
    void cycle_selection(bool forward);
    void select_next_marker();
    void select_prev_marker();
    void prune_live_selection();
    void sync_playhead_to_last_selected();
    void jump_playhead_to(int64_t target_sample);
};
