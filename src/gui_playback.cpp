#include "gui_playback.h"

#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Implementation notes
// --------------------
// The audio callback runs on miniaudio's internal thread. Its only contract
// with the main thread is through three relaxed atomics: cursor_, speed_,
// playing_. Relaxed ordering is enough because none of the atomics guards
// non-atomic data — they are plain scalar reads whose value we want to see
// "eventually". The sample buffer is read-only from the audio thread's
// point of view, and its address lives across the device's entire life.
//
// Speed changes are applied at buffer granularity. A set_speed() call
// between callback invocations is picked up on the next fill; the speed
// stays constant within one fill, avoiding mid-buffer rate artefacts.

struct GuiPlayback::Impl {
    ma_device device{};
    bool      device_inited = false;

    // Borrowed source buffer.
    const float* samples       = nullptr;
    int64_t      total_frames  = 0;
    int          channels      = 0;
    int          source_rate   = 0;

    // Current range. Updated from the main thread, read from the audio
    // thread. end_sample is exclusive.
    std::atomic<int64_t> end_sample{0};

    // Mutable playback state.
    std::atomic<int64_t> cursor{0};
    std::atomic<int32_t> speed_x1000{1000};  // speed * 1000, so we can store in int
    std::atomic<bool>    playing{false};

    // Audio-thread-only fractional source cursor. Tracking the fractional
    // position across buffer boundaries is what prevents per-buffer floor()
    // rounding from compounding into audible drift between audio and visual
    // playhead over long playback. The integer `cursor` is snapshotted from
    // this each buffer for the main thread to read.
    double fractional_cursor = 0.0;

    // Main thread sets a pending restart position via play(); the audio
    // thread picks it up at the top of its next fill to reseat
    // fractional_cursor without a lock. -1 sentinel means "no pending".
    std::atomic<int64_t> pending_start{-1};
};

namespace {

// Copy N output frames at the current speed, advancing the cursor. Stops
// early and fills the remainder with silence if the cursor would pass
// end_sample. Writes the final source-cursor back to impl->cursor before
// returning; on natural end, also clears impl->playing.
void fill_output(GuiPlayback::Impl& impl,
                 float* out_interleaved,
                 ma_uint32 frame_count,
                 int out_channels) {
    const int    src_channels = impl.channels;
    const double speed        = static_cast<double>(
                                    impl.speed_x1000.load(std::memory_order_relaxed))
                                / 1000.0;
    const int64_t end         = impl.end_sample.load(std::memory_order_relaxed);
    const int64_t total       = impl.total_frames;

    // Pick up any pending restart position published by play(). Clearing it
    // atomically lets us idempotently absorb the latest restart without a
    // lock. See the class doc on pending_start.
    const int64_t pending =
        impl.pending_start.exchange(-1, std::memory_order_relaxed);
    if (pending >= 0) {
        impl.fractional_cursor = static_cast<double>(pending);
    }

    const int copy_channels = std::min(src_channels, out_channels);
    bool natural_end = false;
    const double base = impl.fractional_cursor;

    for (ma_uint32 n = 0; n < frame_count; ++n) {
        const double  frac_src_pos = base + static_cast<double>(n) * speed;
        const double  floor_pos    = std::floor(frac_src_pos);
        const int64_t src_floor    = static_cast<int64_t>(floor_pos);
        const int64_t src_ceil     = src_floor + 1;
        const double  frac         = frac_src_pos - floor_pos;

        if (src_floor >= end || src_floor >= total) {
            // Fill remainder with silence.
            std::memset(out_interleaved + static_cast<size_t>(n) * out_channels,
                        0,
                        sizeof(float) * (frame_count - n) * out_channels);
            natural_end = true;
            break;
        }

        const float* sp_floor = impl.samples +
                                static_cast<size_t>(src_floor) * src_channels;
        const bool ceil_ok = (src_ceil < end && src_ceil < total);
        const float* sp_ceil = ceil_ok
            ? impl.samples + static_cast<size_t>(src_ceil) * src_channels
            : sp_floor;  // last-sample fallback
        float* op = out_interleaved + static_cast<size_t>(n) * out_channels;

        for (int c = 0; c < copy_channels; ++c) {
            const double a = sp_floor[c];
            const double b = sp_ceil[c];
            op[c] = static_cast<float>((1.0 - frac) * a + frac * b);
        }
        // If the device has more output channels than the source (e.g. mono
        // source on a stereo device) duplicate channel 0 across the rest;
        // miniaudio's default data converter otherwise handles fewer output
        // channels for us.
        if (out_channels > copy_channels) {
            const float mono = op[0];
            for (int c = copy_channels; c < out_channels; ++c) op[c] = mono;
        }
    }

    // Advance the fractional cursor by the exact float amount consumed.
    // Drift-free across buffer boundaries. The integer atomic is the
    // main-thread snapshot.
    impl.fractional_cursor += static_cast<double>(frame_count) * speed;
    int64_t new_cur = static_cast<int64_t>(std::floor(impl.fractional_cursor));
    if (new_cur > end)   new_cur = end;
    if (new_cur > total) new_cur = total;
    impl.cursor.store(new_cur, std::memory_order_relaxed);
    if (natural_end) {
        impl.playing.store(false, std::memory_order_relaxed);
    }
}

void data_callback(ma_device* pDevice, void* pOutput, const void* /*pInput*/,
                   ma_uint32 frameCount) {
    auto* impl = static_cast<GuiPlayback::Impl*>(pDevice->pUserData);
    if (!impl) {
        std::memset(pOutput, 0,
                    sizeof(float) * frameCount * pDevice->playback.channels);
        return;
    }

    if (!impl->playing.load(std::memory_order_relaxed)) {
        std::memset(pOutput, 0,
                    sizeof(float) * frameCount * pDevice->playback.channels);
        return;
    }

    fill_output(*impl,
                static_cast<float*>(pOutput),
                frameCount,
                static_cast<int>(pDevice->playback.channels));
}

} // namespace

GuiPlayback::GuiPlayback() : impl_(std::make_unique<Impl>()) {}
GuiPlayback::~GuiPlayback() { shutdown(); }

bool GuiPlayback::init(int sample_rate, int channels, const float* samples,
                       int64_t total_frames) {
    shutdown(); // idempotent

    impl_->samples      = samples;
    impl_->total_frames = total_frames;
    impl_->channels     = channels;
    impl_->source_rate  = sample_rate;
    impl_->cursor.store(0, std::memory_order_relaxed);
    impl_->end_sample.store(0, std::memory_order_relaxed);
    impl_->speed_x1000.store(1000, std::memory_order_relaxed);
    impl_->playing.store(false, std::memory_order_relaxed);
    impl_->pending_start.store(-1, std::memory_order_relaxed);
    impl_->fractional_cursor = 0.0;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format  = ma_format_f32;
    cfg.playback.channels = static_cast<ma_uint32>(channels);
    cfg.sampleRate        = static_cast<ma_uint32>(sample_rate);
    cfg.dataCallback      = data_callback;
    cfg.pUserData         = impl_.get();

    const ma_result r = ma_device_init(nullptr, &cfg, &impl_->device);
    if (r != MA_SUCCESS) {
        std::fprintf(stderr,
            "warptempo_gui: audio device init failed (ma_result=%d); "
            "playback disabled\n", static_cast<int>(r));
        impl_->samples      = nullptr;
        impl_->total_frames = 0;
        return false;
    }
    impl_->device_inited = true;

    const ma_result s = ma_device_start(&impl_->device);
    if (s != MA_SUCCESS) {
        std::fprintf(stderr,
            "warptempo_gui: audio device start failed (ma_result=%d); "
            "playback disabled\n", static_cast<int>(s));
        ma_device_uninit(&impl_->device);
        impl_->device_inited = false;
        impl_->samples      = nullptr;
        impl_->total_frames = 0;
        return false;
    }
    return true;
}

void GuiPlayback::play(int64_t start_sample, int64_t end_sample) {
    if (!impl_->device_inited) return;
    if (!impl_->samples || impl_->total_frames <= 0) return;
    if (start_sample < 0) start_sample = 0;
    if (start_sample >= impl_->total_frames) return;
    if (end_sample > impl_->total_frames) end_sample = impl_->total_frames;
    if (end_sample <= start_sample) return;

    // Publish the new range before flipping playing to true, so the audio
    // callback sees a consistent view. Relaxed is fine — the playing flag is
    // the only thing the callback guards on, and we set it last.
    //
    // `pending_start` hands off the restart position to the audio thread,
    // which reseats its private `fractional_cursor` at the top of the next
    // fill. The integer `cursor` atomic is set here so the main thread sees
    // a consistent snapshot immediately (before the next buffer runs).
    impl_->cursor.store(start_sample, std::memory_order_relaxed);
    impl_->end_sample.store(end_sample, std::memory_order_relaxed);
    impl_->pending_start.store(start_sample, std::memory_order_relaxed);
    impl_->playing.store(true, std::memory_order_relaxed);
}

void GuiPlayback::stop() {
    if (!impl_->device_inited) return;
    impl_->playing.store(false, std::memory_order_relaxed);
}

void GuiPlayback::set_speed(float speed) {
    if (speed < 0.10f) speed = 0.10f;
    if (speed > 1.00f) speed = 1.00f;
    const int32_t v = static_cast<int32_t>(std::lround(speed * 1000.0f));
    impl_->speed_x1000.store(v, std::memory_order_relaxed);
}

bool GuiPlayback::is_playing() const {
    return impl_->playing.load(std::memory_order_relaxed);
}

int64_t GuiPlayback::cursor() const {
    return impl_->cursor.load(std::memory_order_relaxed);
}

void GuiPlayback::shutdown() {
    if (!impl_) return;
    if (impl_->device_inited) {
        impl_->playing.store(false, std::memory_order_relaxed);
        // ma_device_uninit stops the device and waits for the last callback
        // invocation to drain, so it is safe for the sample buffer to die
        // immediately after this returns.
        ma_device_uninit(&impl_->device);
        impl_->device_inited = false;
    }
    impl_->samples      = nullptr;
    impl_->total_frames = 0;
}
