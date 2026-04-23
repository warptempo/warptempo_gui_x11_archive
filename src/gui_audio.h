#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Owns an audio file's sample buffer and a multi-resolution min/max peak
// pyramid. No knowledge of X11, Cairo, or progress UI. Synchronous loader with
// a progress callback that the caller wires up to its UI.
class GuiAudio {
public:
    using ProgressCallback = std::function<void(float)>;

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
    // on `channel`, read at pyramid `level`. Level 0 is raw samples; level L>0
    // uses stored min/max pairs each covering 2^L source samples. If `level`
    // exceeds the top built level, reads from the top level. Inputs are
    // clamped; an empty range returns (0, 0).
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

    // pyramid_[ch][L-1] holds level L's (min,max) pairs. Level 0 lives in
    // samples_ directly; there is no corresponding vector here.
    std::vector<std::vector<std::vector<std::pair<float,float>>>> pyramid_;
};
