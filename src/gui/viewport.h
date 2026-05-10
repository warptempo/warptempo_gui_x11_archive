#pragma once

#include "app_state.h"

#include <cstdint>
#include <functional>
#include <utility>

class GuiAudio;
class GuiPlatform;
class GuiPlayback;

// X.7.1: viewport mutators and invalidation helpers, extracted from main.cpp's
// inline lambdas. The struct holds references to the long-lived state the
// methods read and write; bodies are byte-identical to the originals modulo
// `this->` access on the captured references.
struct Viewport {
    AppState&                       app;
    const GuiAudio&                 audio;
    GuiPlatform&                         gui;
    GuiPlayback&                    playback;
    std::function<void()>&          recompute_hover_at_cursor;

    Viewport(AppState&                       app_,
             const GuiAudio&                 audio_,
             GuiPlatform&                         gui_,
             GuiPlayback&                    playback_,
             std::function<void()>&          recompute_hover_at_cursor_)
        : app(app_),
          audio(audio_),
          gui(gui_),
          playback(playback_),
          recompute_hover_at_cursor(recompute_hover_at_cursor_) {}

    // Trim helpers.
    std::pair<int64_t, int64_t> trim_range() const;
    int64_t                     trim_begin_sample() const;
    int64_t                     trim_end_sample() const;

    // Viewport mutators.
    void move_playhead_to(int64_t new_sample);
    void move_playhead_pixels(int delta_px);
    void apply_zoom_change(int new_zoom_level);
    void zoom_in();
    void zoom_out();
    void scroll_viewport(int64_t delta_samples);
    void center_viewport_on_playhead();
    void follow_scroll_if_needed();

    // Invalidation.
    void invalidate_waveform_area();
    void invalidate_timestamp_area();
    void invalidate_playhead_columns(double old_px, double new_px);
    void invalidate_top_strip();
    void invalidate_all();
};
