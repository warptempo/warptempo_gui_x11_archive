#pragma once

#include "app_state.h"
#include "audio.h"
#include "selection.h"
#include "viewport.h"

#include <functional>

// X.7.7: mode/tab management cluster, extracted from main.cpp's inline
// lambdas and the inline Ctrl+Tab block in the keyboard handler. Owns the
// active-tab snapshot push (refresh_active_tab_from_app), the per-mode
// selection-slot resolver (active_view_state), the W/T mode swap
// (switch_active_mode_to), the `t`-keypress entry path with engine /
// transients_enabled gating (toggle_active_mode), and the Ctrl+Tab
// flip (switch_active_tab_to).
//
// The struct holds references to long-lived AppState/Viewport/Selection
// and to the std::function callables the methods need. Bodies are
// byte-identical to the originals modulo the lambda-call rewriting
// documented in tab_mode.cpp.
struct GuiTabMode {
    AppState&              app;
    const GuiAudio&        audio;
    Viewport&              viewport;
    Selection&             selection;
    std::function<void()>& clear_hover_popup;
    std::function<void()>& stop_playback_if_playing;

    GuiTabMode(AppState&              app_,
               const GuiAudio&        audio_,
               Viewport&              viewport_,
               Selection&             selection_,
               std::function<void()>& clear_hover_popup_,
               std::function<void()>& stop_playback_if_playing_)
        : app(app_),
          audio(audio_),
          viewport(viewport_),
          selection(selection_),
          clear_hover_popup(clear_hover_popup_),
          stop_playback_if_playing(stop_playback_if_playing_) {}

    void       refresh_active_tab_from_app();
    ViewState* active_view_state();
    void       switch_active_mode_to(char target_mode);
    void       switch_active_tab_to(char target_tab);
    void       toggle_active_mode();
};
