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
