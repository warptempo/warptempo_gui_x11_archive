#pragma once

#include "app_state.h"
#include "audio.h"
#include "playback.h"
#include "selection.h"
#include "platform_x11.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

// X.7.6: render-view cluster, extracted from main.cpp's inline lambdas.
// Covers the directory enumeration of <source_parent>/renders/<batch>/<basename>.wav,
// the .rendersettings sidecar (per-render zoom/viewport/playhead persistence),
// the per-entry selection stash with stat-tuple gating, the load-into-active-audio
// path that parks the source GuiAudio onto an owned member, and the inverse
// restore. The struct holds references to the long-lived state and std::function
// callables the methods read and write; bodies are byte-identical to the
// originals modulo `this->` access on the captured references and the
// lambda-call rewriting documented in render_view.cpp.
//
// `source_audio_held` is owned (not a reference): it was a local in main()
// before extraction and is used only by load_render_view_at /
// restore_source_audio. Promoting it to a member keeps lifetime tied to
// the struct rather than to main's stack frame.
struct GuiRenderView {
    AppState&              app;
    GuiAudio&              audio;
    GuiPlayback&           playback;
    GuiPlatform&                gui;
    Selection&             selection;
    std::function<void()>& clear_hover_popup;
    std::function<void()>& refresh_active_tab_from_app;

    // Chunk W: parked source audio. Populated only while
    // app.render_view_enabled is true — std::move'd off `audio` on
    // toggle-in and std::move'd back on toggle-out so the source
    // doesn't have to be re-read from disk. Default-constructed
    // (empty / total_frames() == 0) when render-view is off.
    GuiAudio source_audio_held;

    GuiRenderView(AppState&              app_,
                  GuiAudio&              audio_,
                  GuiPlayback&           playback_,
                  GuiPlatform&                gui_,
                  Selection&             selection_,
                  std::function<void()>& clear_hover_popup_,
                  std::function<void()>& refresh_active_tab_from_app_)
        : app(app_),
          audio(audio_),
          playback(playback_),
          gui(gui_),
          selection(selection_),
          clear_hover_popup(clear_hover_popup_),
          refresh_active_tab_from_app(refresh_active_tab_from_app_) {}

    std::vector<AppState::RenderViewEntry> enumerate_render_view_list();
    std::filesystem::path rendersettings_path(
        const AppState::RenderViewEntry& e);
    void write_rendersettings_for(const AppState::RenderViewEntry& e);
    void apply_rendersettings_for(const AppState::RenderViewEntry& e);
    std::pair<uintmax_t, int64_t> wav_stat_tuple(
        const std::filesystem::path& p);
    void stash_render_view_selection_to_active_entry();
    bool load_render_view_at(int index);
    void restore_source_audio();
};
