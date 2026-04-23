#include "gui_audio.h"

#include <sndfile.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

// A level is not built if it would contain fewer pairs than this; beyond that
// point a single pair already covers essentially the whole file.
constexpr size_t kMinPairsPerLevel = 8;

// Progress is reported every this many level-1 pairs. Power of two so the
// mask test is cheap.
constexpr int64_t kProgressStride = 1 << 16;

// Reserve the last 5% of the progress budget for the tail (higher levels +
// cleanup). Level 1 is the dominant pass.
constexpr float kLevel1Share = 0.95f;

} // namespace

bool GuiAudio::load(const std::string& path, const ProgressCallback& on_progress) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));

    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        std::fprintf(stderr,
                     "warptempo_gui: could not open '%s': %s\n",
                     path.c_str(), sf_strerror(nullptr));
        return false;
    }

    channels_        = info.channels;
    sample_rate_     = info.samplerate;
    render_channels_ = std::min(channels_, 2);

    if (channels_ > 2) {
        std::fprintf(stderr,
                     "warptempo_gui: %d-channel file '%s'; only the first 2 "
                     "channels will be displayed\n",
                     channels_, path.c_str());
    }

    const int64_t claimed = info.frames;
    samples_.assign(static_cast<size_t>(claimed) * channels_, 0.0f);

    const sf_count_t got = sf_readf_float(snd, samples_.data(), claimed);
    sf_close(snd);

    if (got <= 0) {
        std::fprintf(stderr,
                     "warptempo_gui: no audio frames read from '%s'\n",
                     path.c_str());
        return false;
    }
    if (got != claimed) {
        samples_.resize(static_cast<size_t>(got) * channels_);
    }
    total_frames_ = got;

    if (on_progress) on_progress(0.0f);

    // Level 1.
    pyramid_.assign(render_channels_, {});
    const int64_t level1_size = (total_frames_ + 1) / 2;
    for (int ch = 0; ch < render_channels_; ch++) {
        pyramid_[ch].emplace_back(static_cast<size_t>(level1_size));
    }

    const int64_t progress_denom = level1_size > 0 ? level1_size : 1;
    for (int64_t i = 0; i < level1_size; i++) {
        const int64_t f0 = 2 * i;
        const int64_t f1 = 2 * i + 1;
        for (int ch = 0; ch < render_channels_; ch++) {
            const float v0 = samples_[f0 * channels_ + ch];
            const float v1 = (f1 < total_frames_) ? samples_[f1 * channels_ + ch] : v0;
            pyramid_[ch][0][static_cast<size_t>(i)] =
                { std::min(v0, v1), std::max(v0, v1) };
        }
        if (on_progress && (i & (kProgressStride - 1)) == 0) {
            const float p = kLevel1Share *
                (static_cast<float>(i) / static_cast<float>(progress_denom));
            on_progress(p);
        }
    }

    // Levels 2..N: each consumes its predecessor.
    for (int ch = 0; ch < render_channels_; ch++) {
        while (true) {
            const auto& prev = pyramid_[ch].back();
            const size_t new_size = (prev.size() + 1) / 2;
            if (new_size < kMinPairsPerLevel) break;

            std::vector<std::pair<float,float>> next(new_size);
            for (size_t i = 0; i < new_size; i++) {
                const size_t a = 2 * i;
                const size_t b = 2 * i + 1;
                const auto& pa = prev[a];
                const auto  pb = (b < prev.size()) ? prev[b] : pa;
                next[i] = { std::min(pa.first,  pb.first),
                            std::max(pa.second, pb.second) };
            }
            pyramid_[ch].push_back(std::move(next));
        }
    }

    if (on_progress) on_progress(1.0f);
    return true;
}

int GuiAudio::num_levels() const {
    if (total_frames_ <= 0) return 0;
    if (pyramid_.empty())   return 1;
    return 1 + static_cast<int>(pyramid_[0].size());
}

int64_t GuiAudio::level_size(int level) const {
    if (level <= 0) return total_frames_;
    if (pyramid_.empty()) return 0;
    const int idx = level - 1;
    if (idx >= static_cast<int>(pyramid_[0].size())) return 0;
    return static_cast<int64_t>(pyramid_[0][idx].size());
}

std::pair<float,float> GuiAudio::get_peak_range(int channel,
                                                int level,
                                                int64_t start_sample,
                                                int64_t end_sample) const {
    const std::pair<float,float> empty{0.0f, 0.0f};

    if (channel < 0 || channel >= render_channels_) return empty;
    if (start_sample < 0) start_sample = 0;
    if (end_sample > total_frames_) end_sample = total_frames_;
    if (end_sample <= start_sample) return empty;

    float lo =  std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();

    if (level <= 0) {
        for (int64_t s = start_sample; s < end_sample; s++) {
            const float v = samples_[s * channels_ + channel];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    } else {
        const int top = static_cast<int>(pyramid_[channel].size());
        const int L   = std::min(level, top);
        if (L <= 0) return empty;
        const auto& pairs = pyramid_[channel][L - 1];
        const int64_t stride = int64_t{1} << L;
        int64_t i0 = start_sample / stride;
        int64_t i1 = (end_sample + stride - 1) / stride;
        if (i1 > static_cast<int64_t>(pairs.size())) i1 = pairs.size();
        for (int64_t i = i0; i < i1; i++) {
            const auto& p = pairs[static_cast<size_t>(i)];
            if (p.first  < lo) lo = p.first;
            if (p.second > hi) hi = p.second;
        }
    }

    if (lo == std::numeric_limits<float>::infinity()) return empty;
    return {lo, hi};
}
