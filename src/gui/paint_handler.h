#pragma once

#include "app_state.h"
#include "audio.h"
#include "playback.h"
#include "render.h"
#include "warpmarkers.h"
#include "platform_x11.h"

#include <cairo/cairo.h>
#include <string>
#include <vector>

// X.7.8a: paint handler cluster. Owns the on_redraw and on_resize callback
// bodies, extracted verbatim from main.cpp's lambdas. Bodies use the
// reference members below in place of the original lambda captures; the
// only behavior change is the indirection.
//
// Construction site: main.cpp, after AppState / GuiAudio / GuiPlayback /
// GuiPlatform / WaveformCache exist. Lifetime is the same scope as the other
// operation structs (Undo, Selection, GuiTabMode, etc.).
//
// Reference list deviates from the original brief:
//   - The brief listed Viewport& and std::function<bool(int)>&
//     popup_eligible_marker, but a verbatim body copy reveals that paint
//     never calls a Viewport method (geometry queries go through free
//     functions waveform_area / top_strip_area / current_samples_per_pixel
//     declared in app_state.h) and never calls popup_eligible_marker (the
//     eligibility check is inlined as `tempo_inherits || !label_ref.empty()`
//     at each hover-popup paint site). Both omitted to avoid dead weight.
//   - GuiPlatform& is added because paint calls gui.playhead_triangle_surface()
//     for the playhead's triangle indicator.
//   - GuiPlayback& is non-const because on_resize calls
//     playback.resync_predictor(), which mutates atomic predictor state.

// -- Constants used by paint code ----------------------------------------
//
// X.7.8a: hoisted from main.cpp's anonymous namespace so paint_handler.cpp
// can reach them. Other constants (kHoverDelayMs, kMarkerHitHalfPx,
// kDoubleClickMs, kDoubleClickPixels, kPlayheadHalfPx, kTimestampRegionW,
// kTimestampRegionH, kDirtyGapPx, kZoomMsPerPixel) are paint-handler-
// independent and stay in main.cpp's anonymous namespace.

constexpr int      kProgressBarHeight        = 4;
constexpr int      kChannelGapPx             = 2;
// kFlagFontSize lives in render.h so render.cpp can reach it without
// pulling paint_handler.h into the lower-layer include graph.

// Timestamp text layout (bottom-left of the status strip).
constexpr int      kTimestampPadX            = 8;
constexpr int      kTimestampBaselineFromBottom = 12;
constexpr double   kTabLetterGapPx           = 10.0;

// V.B / V.B Addendum 2: iteration popup vertical geometry. The popup sits
// kIterPopupVerticalGapPx above its flag's top edge; the edit-state text
// baseline is shifted up by kIterPopupVPadExtraPx so the pending text
// clears the popup rect's bottom inner padding.
constexpr double   kIterPopupVerticalGapPx   = 4.0;
constexpr double   kIterPopupVPadExtraPx     = 1.0;

// -- Off-screen pixel cache for the waveform subsystem -------------------
//
// Lives for the life of main(); recreated when the waveform area is
// resized; re-rendered when any input to render_waveform has changed.
// The redraw path blits this surface onto the pixmap and paints markers
// / flags / playhead / timestamp on top. No implicit Cairo state from
// the main pixmap context leaks in — render_waveform does its own
// save/restore and does not depend on the caller's transform.
struct WaveformCache {
    cairo_surface_t* surface = nullptr;
    int              width   = 0;     // surface width  (== area.w when valid)
    int              height  = 0;     // surface height (== area.h when valid)

    // Fingerprint of the last successful render. Compared against the
    // current redraw's inputs to decide whether to re-render.
    int64_t   fp_vp_start   = 0;
    int64_t   fp_vp_end     = 0;
    int64_t   fp_trim_begin = 0;
    int64_t   fp_trim_end   = 0;
    int       fp_area_w     = 0;
    int       fp_area_h     = 0;
    long long fp_audio_gen  = -1;     // -1 = never rendered

    bool dirty = true;

    void destroy_surface() {
        if (surface) {
            cairo_surface_destroy(surface);
            surface = nullptr;
        }
        width  = 0;
        height = 0;
        dirty  = true;
        fp_audio_gen = -1;
    }

    ~WaveformCache() { destroy_surface(); }
};

// -- Iteration popup geometry --------------------------------------------
//
// On-screen geometry for one iteration popup. `flag_rect` is the
// underlying flag's rect (used to anchor); `hit_rect` is the clickable
// region; `text` is the current popup text (for paint and seed-on-edit).
struct IterPopupHit {
    int          marker_index;
    GuiRect      flag_rect;
    GuiRect      hit_rect;
    std::string  text;
};

// Compute iteration popup hit-rects for visible owning markers in
// `top_strip_area`. Uses `compute_flag_hit_rects` for the underlying flag
// positions (so popups inherit the flag-strip greedy elision). Each
// popup sits kIterPopupVerticalGapPx above its flag's top edge. The
// hit_rect height matches the flag's height; width is the monospace
// extent of the popup's current text plus a small horizontal pad so
// edits with longer pending strings stay clickable.
std::vector<IterPopupHit> compute_iter_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size);

// -- BPM popup geometry --------------------------------------------------
//
// Brief X.2 BPM popup geometry. Mirrors IterPopupHit.
struct BpmPopupHit {
    int          marker_index;
    GuiRect      flag_rect;
    GuiRect      hit_rect;
    std::string  text;
};

// Brief X.2: compute BPM popup hit-rects for visible owning markers that
// carry a stored BPM value. Mirrors compute_iter_popup_hits in shape:
// uniform hit_rect.w sized to "99@[999,999]" so click targets are stable
// as values change; pack collision uses the painted-text width so static
// states pixel-match neighbors.
std::vector<BpmPopupHit> compute_bpm_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size);

// -- GuiPaintHandler -----------------------------------------------------
//
// X.7.8a: extracted from main.cpp's set_on_redraw / set_on_resize lambdas.
// Reference members map to the long-lived state the paint code reads.
// The struct is constructed once, then the original lambda registrations
// become one-line calls into these methods.
struct GuiPaintHandler {
    AppState&          app;
    const GuiAudio&    audio;
    GuiPlayback&       playback;
    WaveformCache&     wf_cache;
    const GuiPlatform&      gui;

    GuiPaintHandler(AppState&          app_,
                    const GuiAudio&    audio_,
                    GuiPlayback&       playback_,
                    WaveformCache&     wf_cache_,
                    const GuiPlatform&      gui_)
        : app(app_),
          audio(audio_),
          playback(playback_),
          wf_cache(wf_cache_),
          gui(gui_) {}

    void on_redraw(cairo_t* cr, int x, int y, int w, int h);
    void on_resize(int w, int h);
};
