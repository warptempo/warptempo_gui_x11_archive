#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

// Audio playback engine. Owns a miniaudio device and drives it from an
// internal audio callback thread. The sample buffer is borrowed, not owned:
// the caller must keep the pointer passed to init() valid until shutdown()
// returns, and must call stop()+shutdown() before tearing down the source.
//
// Thread model:
//   - Audio thread (miniaudio-owned): reads cursor_/speed_, writes cursor_
//     and is_playing_ via relaxed atomics. No allocation, no I/O, no locks.
//   - Main thread: calls init/play/stop/set_speed/shutdown; snapshots
//     cursor() and is_playing() per redraw.
//
// Playback is naive sample-rate rescaling: speed 0.7 reads every 0.7th
// source sample, which shifts pitch along with tempo. This matches the
// user's Reaper-style workflow expectation.
//
// Predictor design note (Brief R)
// -------------------------------
// The cursor predictor is a free-running linear extrapolator anchored at
// (anchor_sample, anchor_ns) and re-anchored only at events of acceptable
// visible discontinuity, never inside the audio callback. The set of
// resync events is fixed by Brief N's mechanism, mapped onto the post-Q
// topology: playhead jumps via move_playhead, zoom in/out via the shared
// apply_zoom_change helper, the resize fit-file fallback,
// sync_playhead_to_last_selected (live nudges and undo/redo restores),
// set_playback_speed, follow-mode off-to-on, marker-drag hover, and
// follow-scroll auto-shift.
//
// Two alternatives were considered and rejected. A free-running predictor
// with no resync (Brief O state) is insufficient for medium-zoom playback
// over windows long enough for steady_clock vs sample-clock skew to
// accumulate to visible drift. A continuous audio-thread timestamp publish
// with main-thread extrapolation against the latest publish (Brief M) is
// rejected on perceptual grounds: a 100 Hz resync cadence at audio-buffer
// rate produces a periodic high-frequency signal that the user is
// sensitive to, even at sub-sample per-resync amplitudes.
//
// The masking criterion for the chosen design is single-frame: each
// resync's discontinuity (bounded by one audio buffer's worth of samples)
// must land in the same monitor frame as the viewport reflow it is
// co-located with. Future predictor work must preserve this constraint
// or argue explicitly to overturn it.
class GuiPlayback {
public:
    GuiPlayback();
    ~GuiPlayback();

    GuiPlayback(const GuiPlayback&)            = delete;
    GuiPlayback& operator=(const GuiPlayback&) = delete;

    // Bring up the audio device at the given source sample rate and channel
    // count and bind it to `samples` / `total_frames`. Returns true on
    // success. On failure, logs to stderr and leaves the object in an
    // un-initialised state where play() is a silent no-op.
    bool init(int sample_rate, int channels, const float* samples,
              int64_t total_frames);

    // Begin playback at `start_sample`, stopping when the source cursor
    // reaches or passes `end_sample` (exclusive). Safe to call while
    // already playing — the previous run is torn down cleanly first.
    void play(int64_t start_sample, int64_t end_sample);

    // Stop playback immediately. The cursor retains its last value so the
    // main thread can snapshot where it stopped.
    void stop();

    // Clamp to [0.10, 1.00] and publish to the audio thread. The change
    // takes effect on the next callback buffer.
    void set_speed(float speed);

    // Re-anchor the free-running cursor predictor at the audio thread's
    // current cursor and the current steady_clock time. Call from the main
    // thread at events where a small visible discontinuity is acceptable
    // (jumps, viewport reflows, speed changes) so the predictor remains a
    // smooth linear function of wall-clock between resyncs. Safe to call
    // when not playing — the next play() will overwrite the anchor.
    void resync_predictor();

    // Snapshot accessors. Safe from the main thread.
    bool    is_playing() const;
    int64_t cursor()     const;

    // Tear down the device. Blocks until the audio callback has drained.
    // Call before the sample buffer dies (reload, shutdown).
    void shutdown();

    // Opaque to consumers, but public so the audio callback in
    // gui_playback.cpp (a free function outside the class) can reach it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
