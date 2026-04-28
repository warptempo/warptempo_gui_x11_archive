#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Owns an audio file's sample buffer and a fixed-stride min/max peak pyramid
// (three int16 cache levels at strides 32, 1024, 32768). No knowledge of X11,
// Cairo, or progress UI. Synchronous loader with a progress callback that the
// caller wires up to its UI.
class GuiAudio {
public:
    using ProgressCallback = std::function<void(float)>;

    // Implementation detail of the peak cache. Public only so the cache
    // reader/writer free functions in gui_audio.cpp can name the type.
    // Each PyramidLevel holds a fixed stride, the number of (min,max) pairs
    // covering the source, and per-channel flat int16 storage interleaved as
    // (min0, max0, min1, max1, ...). Quantization: clamp(v,-1,1) * 32767.
    struct PyramidLevel {
        int32_t stride     = 0;
        int64_t pair_count = 0;
        std::vector<std::vector<int16_t>> pairs;  // pairs[ch][2*p..2*p+1]
    };

    // Opens `path` via libsndfile, reads all frames, and builds the pyramid.
    // Returns true on success. On failure, writes a diagnostic to stderr and
    // returns false. `on_progress` is invoked with a value in [0.0, 1.0]
    // periodically during pyramid construction; it may be empty.
    bool load(const std::string& path, const ProgressCallback& on_progress);

    int64_t total_frames()    const { return total_frames_; }
    int     sample_rate()     const { return sample_rate_; }
    int     channels()        const { return channels_; }
    int     render_channels() const { return render_channels_; }

    // Raw interleaved float32 sample buffer. The pointer is valid as long as
    // this GuiAudio instance is alive and no new load() has been called. The
    // playback engine reads samples off the audio callback thread; the caller
    // must orchestrate lifetime so the device is stopped before the buffer
    // goes away (see GuiPlayback::shutdown).
    const float* samples_ptr() const { return samples_.data(); }

    // Total number of pyramid levels, counting level 0 (raw samples).
    int num_levels() const;

    // Size of pyramid level L (in pairs). L == 0 returns total_frames_.
    int64_t level_size(int level) const;

    // Returns (min, max) over source-sample indices [start_sample, end_sample)
    // on `channel`, read at pyramid `level`. Level 0 is raw samples; levels
    // 1..3 select cached min/max pairs at strides 32, 1024, 32768
    // respectively. Levels above the deepest cached level clamp to it.
    // Inputs are clamped; an empty range returns (0, 0).
    std::pair<float,float> get_peak_range(int channel,
                                          int level,
                                          int64_t start_sample,
                                          int64_t end_sample) const;

private:
    std::vector<float> samples_;
    int64_t            total_frames_    = 0;
    int                sample_rate_     = 0;
    int                channels_        = 0;
    int                render_channels_ = 0;

    // Three fixed-stride cache levels (strides 32, 1024, 32768). Populated
    // either from the on-disk `<basename>.peaks` v2 sidecar or by streaming
    // over samples_ on cache miss.
    std::array<PyramidLevel, 3> levels_;
};

// Build a peak pyramid by streaming `wav_path` through libsndfile and write
// the resulting `<basename>.peaks` v2 sidecar atomically (the audio file's
// extension is replaced — for `song.wav` the sidecar is `song.peaks`).
// Allocates only a single 65536-frame chunk plus the int16 pyramid itself —
// no full samples buffer. Returns true on success. On any error, logs a
// single stderr line and returns false; the caller should ignore the return
// value.
bool write_peaks_cache_for_wav(const std::string& wav_path);
