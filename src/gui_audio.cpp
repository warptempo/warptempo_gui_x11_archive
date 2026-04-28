#include "gui_audio.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// `.peaks` v2 format -------------------------------------------------------
//
// 32-byte preamble:
//   off 0  | 8  | magic = "WTPEAKS\0"
//   off 8  | 2  | version (uint16, currently 2)
//   off 10 | 2  | flags   (uint16, written 0)
//   off 12 | 8  | source_size  (int64, bytes)
//   off 20 | 8  | source_mtime (int64, seconds)
//   off 28 | 4  | sample_rate  (int32)
//
// Body header (16 bytes):
//   8 | total_frames    (int64)
//   1 | render_channels (uint8, 1 or 2)
//   1 | num_levels      (uint8, 3)
//   6 | reserved (zero)
//
// Then for each level in ascending stride order:
//   4 | stride     (int32)
//   8 | pair_count (int64)
//   ? | int16 data, channel-major: all of channel 0's pairs first
//       (pair_count * 4 bytes), then channel 1 if render_channels == 2.
//       Each pair is (min_int16, max_int16).
//
// Quantization: float v in [-1, 1] -> int16 round(clamp(v) * 32767).
// Out-of-range source peaks clip at the boundary.

constexpr char     kCacheMagic[8]         = "WTPEAKS";
constexpr uint16_t kCacheVersion          = 2;
constexpr int      kStreamFramesPerChunk  = 65536;
constexpr int      kNumLevels             = 3;
constexpr int32_t  kStrides[kNumLevels]   = { 32, 1024, 32768 };
constexpr int      kReductionFactor       = 32;  // 1024/32 = 32; 32768/1024 = 32.
constexpr float    kQuantScale            = 32767.0f;

// Reserve the first 95% of the progress budget for the dominant level-1 pass;
// levels 2 and 3 fold from in-memory int16 buffers and finish in microseconds.
constexpr float    kLevel1Share           = 0.95f;

inline int16_t quantize_f32(float v) {
    if (v < -1.0f) v = -1.0f;
    if (v >  1.0f) v =  1.0f;
    return static_cast<int16_t>(std::lround(v * kQuantScale));
}
inline float dequantize_i16(int16_t q) {
    return static_cast<float>(q) / kQuantScale;
}

std::string cache_path_for(const std::string& source) {
    // Sibling of the audio file with the source's extension swapped to
    // `.peaks`. For `song.wav` this is `song.peaks` (NOT `song.wav.peaks`
    // and NOT `song.wav.wtpeaks` — both legacy forms are obsolete and any
    // such files left on disk are disposable).
    std::filesystem::path p(source);
    p.replace_extension(".peaks");
    return p.string();
}

bool stat_size_mtime(const std::string& path, int64_t& size, int64_t& mtime) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    size  = static_cast<int64_t>(st.st_size);
    mtime = static_cast<int64_t>(st.st_mtime);
    return true;
}

void reset_levels(std::array<GuiAudio::PyramidLevel, 3>& levels) {
    for (auto& L : levels) {
        L.stride     = 0;
        L.pair_count = 0;
        L.pairs.clear();
        L.pairs.shrink_to_fit();
    }
}

// Streaming pyramid build. Pulls source samples through `source` in
// kStreamFramesPerChunk-frame chunks, accumulating level-1 (stride 32) pairs
// directly. Levels 2 and 3 fold from level 1 in memory. Buffers are sized
// from `total_frames`; if `source` returns 0 early the trailing buffer is
// truncated so pair_count reflects the actual data.
//
// `channels` is the source's interleaved channel count; `render_channels` is
// the number of output channels in the cache (1 or 2; <= channels).
using FrameIterator = std::function<int64_t(float*, int64_t)>;

void build_pyramid_streaming(const FrameIterator& source,
                             int64_t total_frames,
                             int channels,
                             int render_channels,
                             const GuiAudio::ProgressCallback& on_progress,
                             std::array<GuiAudio::PyramidLevel, 3>& out) {
    reset_levels(out);

    const int64_t pc1_hint = (total_frames + kStrides[0] - 1) / kStrides[0];
    out[0].stride = kStrides[0];
    out[0].pairs.assign(static_cast<size_t>(render_channels), {});
    for (int ch = 0; ch < render_channels; ch++) {
        out[0].pairs[ch].reserve(static_cast<size_t>(2 * pc1_hint));
    }

    std::vector<float> chunk(static_cast<size_t>(kStreamFramesPerChunk) * channels);
    std::vector<float> cur_min(static_cast<size_t>(render_channels));
    std::vector<float> cur_max(static_cast<size_t>(render_channels));
    int64_t in_window = 0;

    auto flush_window = [&]() {
        if (in_window > 0) {
            for (int ch = 0; ch < render_channels; ch++) {
                out[0].pairs[ch].push_back(quantize_f32(cur_min[ch]));
                out[0].pairs[ch].push_back(quantize_f32(cur_max[ch]));
            }
            in_window = 0;
        }
    };

    int64_t frames_done = 0;
    const int64_t prog_denom = total_frames > 0 ? total_frames : 1;
    while (frames_done < total_frames) {
        const int64_t want =
            std::min<int64_t>(kStreamFramesPerChunk, total_frames - frames_done);
        const int64_t got = source(chunk.data(), want);
        if (got <= 0) break;

        for (int64_t f = 0; f < got; f++) {
            const float* p = &chunk[f * channels];
            if (in_window == 0) {
                for (int ch = 0; ch < render_channels; ch++) {
                    cur_min[ch] = p[ch];
                    cur_max[ch] = p[ch];
                }
            } else {
                for (int ch = 0; ch < render_channels; ch++) {
                    const float v = p[ch];
                    if (v < cur_min[ch]) cur_min[ch] = v;
                    if (v > cur_max[ch]) cur_max[ch] = v;
                }
            }
            in_window++;
            if (in_window == kStrides[0]) flush_window();
        }
        frames_done += got;

        if (on_progress) {
            const float p = kLevel1Share *
                static_cast<float>(frames_done) / static_cast<float>(prog_denom);
            on_progress(p);
        }
    }
    flush_window();  // tail (may cover < kStrides[0] frames)

    out[0].pair_count = render_channels > 0
        ? static_cast<int64_t>(out[0].pairs[0].size() / 2)
        : 0;
    for (int ch = 0; ch < render_channels; ch++) {
        out[0].pairs[ch].shrink_to_fit();
    }

    // Levels 2 and 3 fold by min-of-mins / max-of-maxes over kReductionFactor
    // adjacent pairs of the previous level. Already-quantized int16 in, no
    // requantization.
    for (int L = 1; L < kNumLevels; L++) {
        const int64_t prev_pc = out[L - 1].pair_count;
        const int64_t cur_pc  = (prev_pc + kReductionFactor - 1) / kReductionFactor;
        out[L].stride     = kStrides[L];
        out[L].pair_count = cur_pc;
        out[L].pairs.assign(static_cast<size_t>(render_channels),
                            std::vector<int16_t>(static_cast<size_t>(2 * cur_pc)));

        for (int ch = 0; ch < render_channels; ch++) {
            const auto& src = out[L - 1].pairs[ch];
            auto&       dst = out[L].pairs[ch];
            for (int64_t q = 0; q < cur_pc; q++) {
                const int64_t i0 = q * kReductionFactor;
                const int64_t i1 = std::min<int64_t>(i0 + kReductionFactor, prev_pc);
                int16_t lo = src[static_cast<size_t>(2 * i0)];
                int16_t hi = src[static_cast<size_t>(2 * i0 + 1)];
                for (int64_t i = i0 + 1; i < i1; i++) {
                    const int16_t a = src[static_cast<size_t>(2 * i)];
                    const int16_t b = src[static_cast<size_t>(2 * i + 1)];
                    if (a < lo) lo = a;
                    if (b > hi) hi = b;
                }
                dst[static_cast<size_t>(2 * q)]     = lo;
                dst[static_cast<size_t>(2 * q + 1)] = hi;
            }
        }
    }

    if (on_progress) on_progress(1.0f);
}

// Try to populate `levels` from the disk cache. Returns true on a clean hit.
// On version/header mismatch ("stale") logs and returns false leaving
// `levels` untouched. On corruption (short read, internal mismatch) clears
// `levels` so the caller's rebuild path starts fresh, logs, returns false.
bool try_load_cache(const std::string& source_path,
                    int64_t total_frames,
                    int     render_channels,
                    int     sample_rate,
                    std::array<GuiAudio::PyramidLevel, 3>& levels) {

    const std::string cpath = cache_path_for(source_path);
    FILE* f = std::fopen(cpath.c_str(), "rb");
    if (!f) return false;

    int64_t src_size = 0, src_mtime = 0;
    if (!stat_size_mtime(source_path, src_size, src_mtime)) {
        std::fclose(f);
        return false;
    }

    auto stale = [&]() {
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache stale or invalid for %s; rebuilding\n",
            source_path.c_str());
        std::fclose(f);
    };
    auto corrupt = [&]() {
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache corrupt for %s; rebuilding\n",
            source_path.c_str());
        reset_levels(levels);
        std::fclose(f);
    };

    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 ||
        std::memcmp(magic, kCacheMagic, 8) != 0) {
        stale(); return false;
    }
    uint16_t version = 0, flags = 0;
    if (std::fread(&version, sizeof(version), 1, f) != 1 ||
        version != kCacheVersion) {
        stale(); return false;
    }
    if (std::fread(&flags, sizeof(flags), 1, f) != 1) { stale(); return false; }
    int64_t hdr_size = 0, hdr_mtime = 0;
    if (std::fread(&hdr_size, sizeof(hdr_size), 1, f) != 1 ||
        hdr_size != src_size) {
        stale(); return false;
    }
    if (std::fread(&hdr_mtime, sizeof(hdr_mtime), 1, f) != 1 ||
        hdr_mtime != src_mtime) {
        stale(); return false;
    }
    int32_t hdr_sr = 0;
    if (std::fread(&hdr_sr, sizeof(hdr_sr), 1, f) != 1 ||
        hdr_sr != sample_rate) {
        stale(); return false;
    }

    int64_t hdr_total_frames = 0;
    uint8_t hdr_rc = 0, hdr_nl = 0;
    char    reserved[6];
    if (std::fread(&hdr_total_frames, sizeof(hdr_total_frames), 1, f) != 1 ||
        hdr_total_frames != total_frames) {
        stale(); return false;
    }
    if (std::fread(&hdr_rc, sizeof(hdr_rc), 1, f) != 1 ||
        hdr_rc != static_cast<uint8_t>(render_channels)) {
        stale(); return false;
    }
    if (std::fread(&hdr_nl, sizeof(hdr_nl), 1, f) != 1) { stale(); return false; }
    if (std::fread(reserved, 1, 6, f) != 6) { stale(); return false; }
    if (hdr_nl != static_cast<uint8_t>(kNumLevels)) { corrupt(); return false; }

    int64_t expected_pc[kNumLevels];
    expected_pc[0] = (total_frames + kStrides[0] - 1) / kStrides[0];
    for (int L = 1; L < kNumLevels; L++) {
        expected_pc[L] = (expected_pc[L - 1] + kReductionFactor - 1) / kReductionFactor;
    }

    for (int L = 0; L < kNumLevels; L++) {
        int32_t hdr_stride = 0;
        int64_t hdr_pc     = 0;
        if (std::fread(&hdr_stride, sizeof(hdr_stride), 1, f) != 1 ||
            hdr_stride != kStrides[L]) {
            corrupt(); return false;
        }
        if (std::fread(&hdr_pc, sizeof(hdr_pc), 1, f) != 1 ||
            hdr_pc != expected_pc[L]) {
            corrupt(); return false;
        }

        levels[L].stride     = kStrides[L];
        levels[L].pair_count = hdr_pc;
        levels[L].pairs.assign(static_cast<size_t>(render_channels),
                               std::vector<int16_t>(static_cast<size_t>(2 * hdr_pc)));
        for (int ch = 0; ch < render_channels; ch++) {
            const size_t bytes = sizeof(int16_t) * 2 * static_cast<size_t>(hdr_pc);
            if (hdr_pc > 0 &&
                std::fread(levels[L].pairs[ch].data(), 1, bytes, f) != bytes) {
                corrupt(); return false;
            }
        }
    }

    std::fclose(f);
    std::fprintf(stderr, "[warptempo_gui] peaks cache hit for %s\n",
                 source_path.c_str());
    return true;
}

// Write `levels` to <basename>.peaks (the audio file's extension is
// swapped, see `cache_path_for`) using the .tmp + fsync + rename atomic
// pattern. Logs a single stderr line on any failure and returns false.
// Cache write failure is never fatal.
bool write_cache_to_disk(const std::string& source_path,
                         int64_t total_frames,
                         int     render_channels,
                         int     sample_rate,
                         const std::array<GuiAudio::PyramidLevel, 3>& levels) {

    const std::string cpath = cache_path_for(source_path);
    const std::string tpath = cpath + ".tmp";

    int64_t src_size = 0, src_mtime = 0;
    if (!stat_size_mtime(source_path, src_size, src_mtime)) {
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: %s\n",
            source_path.c_str(), std::strerror(errno));
        return false;
    }

    FILE* f = std::fopen(tpath.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: %s\n",
            source_path.c_str(), std::strerror(errno));
        return false;
    }

    auto fail = [&]() -> bool {
        const int err = errno;
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: %s\n",
            source_path.c_str(), std::strerror(err));
        std::fclose(f);
        ::unlink(tpath.c_str());
        return false;
    };

    if (std::fwrite(kCacheMagic, 1, 8, f) != 8) return fail();
    const uint16_t version = kCacheVersion;
    const uint16_t flags   = 0;
    if (std::fwrite(&version, sizeof(version), 1, f) != 1) return fail();
    if (std::fwrite(&flags,   sizeof(flags),   1, f) != 1) return fail();
    if (std::fwrite(&src_size,  sizeof(src_size),  1, f) != 1) return fail();
    if (std::fwrite(&src_mtime, sizeof(src_mtime), 1, f) != 1) return fail();
    const int32_t sr32 = static_cast<int32_t>(sample_rate);
    if (std::fwrite(&sr32, sizeof(sr32), 1, f) != 1) return fail();

    const int64_t tf = total_frames;
    if (std::fwrite(&tf, sizeof(tf), 1, f) != 1) return fail();
    const uint8_t rc = static_cast<uint8_t>(render_channels);
    if (std::fwrite(&rc, sizeof(rc), 1, f) != 1) return fail();
    const uint8_t nl = static_cast<uint8_t>(kNumLevels);
    if (std::fwrite(&nl, sizeof(nl), 1, f) != 1) return fail();
    const char zeros[6] = {0,0,0,0,0,0};
    if (std::fwrite(zeros, 1, 6, f) != 6) return fail();

    for (int L = 0; L < kNumLevels; L++) {
        const int32_t stride = levels[L].stride;
        const int64_t pc     = levels[L].pair_count;
        if (std::fwrite(&stride, sizeof(stride), 1, f) != 1) return fail();
        if (std::fwrite(&pc,     sizeof(pc),     1, f) != 1) return fail();
        for (int ch = 0; ch < render_channels; ch++) {
            const size_t bytes = sizeof(int16_t) * 2 * static_cast<size_t>(pc);
            if (pc > 0 &&
                std::fwrite(levels[L].pairs[ch].data(), 1, bytes, f) != bytes) {
                return fail();
            }
        }
    }

    if (std::fflush(f) != 0)         return fail();
    if (::fsync(::fileno(f)) != 0)   return fail();
    if (std::fclose(f) != 0) {
        const int err = errno;
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: %s\n",
            source_path.c_str(), std::strerror(err));
        ::unlink(tpath.c_str());
        return false;
    }
    if (::rename(tpath.c_str(), cpath.c_str()) != 0) {
        const int err = errno;
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: %s\n",
            source_path.c_str(), std::strerror(err));
        ::unlink(tpath.c_str());
        return false;
    }
    std::fprintf(stderr, "[warptempo_gui] peaks cache written for %s\n",
                 source_path.c_str());
    return true;
}

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

    reset_levels(levels_);

    // Try the on-disk peaks cache first. Cache hit skips the build entirely
    // and short-circuits progress reporting to 1.0; the dominant load cost
    // (PCM decode) has already been paid above.
    if (try_load_cache(path, total_frames_, render_channels_, sample_rate_, levels_)) {
        if (on_progress) on_progress(1.0f);
        return true;
    }

    if (on_progress) on_progress(0.0f);

    // Cache miss: stream samples_ through the shared pyramid builder.
    int64_t pos = 0;
    auto in_memory_iter = [&](float* out, int64_t want) -> int64_t {
        const int64_t avail = total_frames_ - pos;
        const int64_t n     = std::min<int64_t>(want, avail);
        if (n <= 0) return 0;
        std::memcpy(out,
                    samples_.data() + pos * channels_,
                    static_cast<size_t>(n) * channels_ * sizeof(float));
        pos += n;
        return n;
    };
    build_pyramid_streaming(in_memory_iter, total_frames_, channels_,
                            render_channels_, on_progress, levels_);

    // Persist the pyramid for next time. Failure is non-fatal — `levels_` is
    // populated either way so the current load() still succeeds.
    write_cache_to_disk(path, total_frames_, render_channels_, sample_rate_, levels_);
    return true;
}

int GuiAudio::num_levels() const {
    if (total_frames_ <= 0) return 0;
    return 1 + kNumLevels;  // level 0 (raw) + three cache levels
}

int64_t GuiAudio::level_size(int level) const {
    if (level <= 0) return total_frames_;
    if (level > kNumLevels) return 0;
    return levels_[level - 1].pair_count;
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

    if (level <= 0) {
        float lo =  std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();
        for (int64_t s = start_sample; s < end_sample; s++) {
            const float v = samples_[s * channels_ + channel];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (lo == std::numeric_limits<float>::infinity()) return empty;
        return {lo, hi};
    }

    // level >= 1 picks cache level (level - 1); anything beyond the top
    // cached level clamps to it.
    int cache_idx = level - 1;
    if (cache_idx >= kNumLevels) cache_idx = kNumLevels - 1;

    const auto&   data    = levels_[cache_idx];
    if (data.pair_count <= 0) return empty;
    const int64_t stride  = data.stride;
    const auto&   pairs   = data.pairs[channel];
    int64_t i0 = start_sample / stride;
    int64_t i1 = (end_sample + stride - 1) / stride;
    if (i1 > data.pair_count) i1 = data.pair_count;
    if (i1 <= i0) return empty;

    int16_t qlo = pairs[static_cast<size_t>(2 * i0)];
    int16_t qhi = pairs[static_cast<size_t>(2 * i0 + 1)];
    for (int64_t i = i0 + 1; i < i1; i++) {
        const int16_t a = pairs[static_cast<size_t>(2 * i)];
        const int16_t b = pairs[static_cast<size_t>(2 * i + 1)];
        if (a < qlo) qlo = a;
        if (b > qhi) qhi = b;
    }
    return { dequantize_i16(qlo), dequantize_i16(qhi) };
}

bool write_peaks_cache_for_wav(const std::string& wav_path) {
    SF_INFO info;
    std::memset(&info, 0, sizeof(info));
    SNDFILE* snd = sf_open(wav_path.c_str(), SFM_READ, &info);
    if (!snd) {
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: %s\n",
            wav_path.c_str(), sf_strerror(nullptr));
        return false;
    }
    const int     channels        = info.channels;
    const int     sample_rate     = info.samplerate;
    const int64_t claimed_frames  = info.frames;
    const int     render_channels = std::min(channels, 2);

    if (claimed_frames <= 0 || render_channels <= 0) {
        sf_close(snd);
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: empty audio\n",
            wav_path.c_str());
        return false;
    }

    int64_t actual_frames = 0;
    auto sndfile_iter = [&](float* out, int64_t want) -> int64_t {
        const sf_count_t n = sf_readf_float(snd, out, static_cast<sf_count_t>(want));
        if (n > 0) actual_frames += n;
        return n > 0 ? static_cast<int64_t>(n) : 0;
    };

    std::array<GuiAudio::PyramidLevel, 3> levels;
    build_pyramid_streaming(sndfile_iter, claimed_frames, channels,
                            render_channels, /*on_progress=*/{}, levels);
    sf_close(snd);

    if (actual_frames <= 0) {
        std::fprintf(stderr,
            "[warptempo_gui] peaks cache write failed for %s: no frames read\n",
            wav_path.c_str());
        return false;
    }

    return write_cache_to_disk(wav_path, actual_frames, render_channels,
                               sample_rate, levels);
}
