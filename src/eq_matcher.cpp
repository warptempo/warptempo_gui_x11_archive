// This module derives direction-aware EQ correction curves from empirical
// measurements of Laroche-Dolson phase vocoder spectral damage.
//
// The design is grounded in data collected by the test suite binary
// (eq_test_suite.cpp, retained in the codebase). That binary measured raw PSD
// delta across a controlled matrix of stretch ratios (0.6 to 1.4) and input gain
// levels (-12 to +12 dB) using orchestral source material. Key findings that
// shaped this implementation:
//
// Vocoder damage is completely independent of input amplitude. The gain axis
// produced zero measurable variation (max spread 0.0001 dB across 24 dB of gain
// range). This eliminated all loudness-based depth modulation from the architecture.
//
// Speedup and slowdown produce different per-band damage profiles. Speedup
// (alpha greater than 1) produces high-shelf-dominant damage from princarg
// wrapping failure. Slowdown (alpha less than 1) produces mid-band-dominant
// damage from inter-partial interference in the wider analysis window. A single
// curve cannot correctly compensate both directions.
//
// Damage scales sublinearly with distance from unity stretch, consistent with
// Michaelis-Menten saturation (ratio of damage at alpha 1.4 to 1.2 is
// approximately 1.5, not 2.0).
//
// The saturation rate is uniform across frequency bands (ratio approximately
// 1.48 in all bands), so a single shape constant c is used rather than per-band
// values.
//
// At unity stretch (alpha equals 1.0), residual spectral damage is negligible
// (0.003 dB), confirming that no correction is needed for unstretched passages.
//
// These findings produce a simple architecture: two static curves (speedup and
// slowdown) derived from the most-stretched regions of the source, scaled by a
// single stretch-dependent depth scalar. See eq_test_suite.cpp for the raw
// measurements.
//
// NOTE ON ALPHA COORDINATES: get_alpha() returns tgt_dur/src_dur, which is the
// inverse of the conventional playback-speed stretch factor. In this coordinate
// system: speedup (play faster, shorter output) yields get_alpha() < 1.0;
// slowdown (play slower, longer output) yields get_alpha() > 1.0. All alpha_ref
// values stored in AudioSTFT use this same convention.

#include "eq_matcher.h"
#include <iostream>
#include <algorithm>
#include <numeric>

static constexpr double MIN_STRETCH_SEC            = 5.0;
static constexpr int    TOP_STRETCH_REGIONS        = 10;
static constexpr double SLOWDOWN_FALLBACK_THRESHOLD = 0.10;
static constexpr double UNITY_EPSILON              = 1e-6;

// ============================================================
// Contiguous stretch-direction region, defined in source-frame space.
// mean_alpha is in get_alpha() coordinates: < 1 = speedup, > 1 = slowdown.
// ============================================================
struct StretchRegion {
    size_t src_start;
    size_t src_end;
    double mean_alpha;
    double duration_sec;
};

// ============================================================
// Accumulate PSD for one direction, then compute raw delta + smoothed curve.
// ============================================================
static void accumulate_and_smooth(
        AudioSTFT& stft,
        const std::vector<StretchRegion>& regions,
        std::vector<double>& raw_delta_out,
        std::vector<Point>&  smoothed_out,
        double& alpha_ref_out)
{
    const int N       = stft.N;
    const int R_s     = stft.R_s;
    const int channels= stft.channels;
    const int K       = N / 2 + 1;
    const int num_frames = static_cast<int>(stft.frame_map.size());
    const size_t vbuf_frames = stft.virtual_tgt_buf.size() / channels;

    std::vector<double> src_psd(K, 0.0);
    std::vector<double> tgt_psd(K, 0.0);
    size_t total_windows = 0;
    double alpha_sum = 0.0;

    std::vector<float> src_chunk(N * channels, 0.0f);
    std::vector<float> tgt_chunk(N * channels, 0.0f);

    for (int m = 0; m < num_frames; ++m) {
        int64_t src_pos = stft.frame_map[m];
        if (src_pos < 0 || src_pos >= stft.src_info.frames) continue;

        // Check if this source position falls in any qualifying region
        bool in_region = false;
        for (const auto& r : regions) {
            if (static_cast<size_t>(src_pos) >= r.src_start &&
                static_cast<size_t>(src_pos) <  r.src_end) {
                in_region = true;
                break;
            }
        }
        if (!in_region) continue;

        // Target position in virtual_tgt_buf: frame m → position m * R_s
        size_t tgt_pos = static_cast<size_t>(m) * R_s;
        if (tgt_pos + N > vbuf_frames) continue;

        // Read source
        sf_seek(stft.src_snd, src_pos, SEEK_SET);
        sf_readf_float(stft.src_snd, src_chunk.data(), N);

        // Read target
        std::copy(stft.virtual_tgt_buf.begin() + tgt_pos * channels,
                  stft.virtual_tgt_buf.begin() + (tgt_pos + N) * channels,
                  tgt_chunk.begin());

        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < N; ++i)
                stft.fft_in[i] = src_chunk[i * channels + ch] * stft.window[i];
            fftw_execute(stft.plan_fwd);
            for (int k = 1; k < K; ++k)
                src_psd[k] += stft.fft_out[k][0] * stft.fft_out[k][0]
                            + stft.fft_out[k][1] * stft.fft_out[k][1];

            for (int i = 0; i < N; ++i)
                stft.fft_in[i] = tgt_chunk[i * channels + ch] * stft.window[i];
            fftw_execute(stft.plan_fwd);
            for (int k = 1; k < K; ++k)
                tgt_psd[k] += stft.fft_out[k][0] * stft.fft_out[k][0]
                            + stft.fft_out[k][1] * stft.fft_out[k][1];
        }

        alpha_sum += get_alpha(static_cast<size_t>(m) * R_s, stft.timemap);
        total_windows++;
    }

    alpha_ref_out = (total_windows > 0) ? (alpha_sum / total_windows) : 1.0;

    // Raw delta
    raw_delta_out.assign(K, 0.0);
    const double count = static_cast<double>(total_windows * channels);
    if (count > 0) {
        for (int k = 1; k < K; ++k) {
            if (src_psd[k] > 0 && tgt_psd[k] > 0)
                raw_delta_out[k] = 10.0 * std::log10(src_psd[k] / count)
                                 - 10.0 * std::log10(tgt_psd[k] / count);
        }
    }

    // Gaussian smoothing
    const double nyquist      = stft.nyquist;
    const double bin_hz_width = stft.bin_hz_width;
    const double log_min      = std::log10(10.0);
    const double log_max      = std::log10(nyquist);
    const double sigma_octaves= 1.0 / 3.0 / 2.355;

    smoothed_out.clear();
    for (int i = 0; i < CURVE_RESOLUTION; ++i) {
        double hz = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
        double wsum = 0.0, dsum = 0.0;
        for (int k = 1; k < K; ++k) {
            double bin_hz = k * bin_hz_width;
            if (bin_hz < 10.0) continue;
            double w = std::exp(-0.5 * std::pow(std::log2(bin_hz / hz) / sigma_octaves, 2.0));
            if (w > 0.001) { wsum += w; dsum += w * raw_delta_out[k]; }
        }
        smoothed_out.push_back({hz, (wsum > 0.0) ? (dsum / wsum) : 0.0});
    }

    // Nyquist taper
    for (auto& pt : smoothed_out) {
        if (pt.x > nyquist - 20.0)
            pt.y *= std::max(0.0, std::min(1.0, (nyquist - pt.x) / 20.0));
    }
}

// ============================================================
// Main entry point
// ============================================================
void EQMatcher::process(AudioSTFT& stft) {
    std::cout << "\n[EQ Match] Analysing stretch regions...\n";

    (void)static_cast<int>(stft.frame_map.size()); // frame_map size used inside accumulate_and_smooth
    const double sample_rate  = static_cast<double>(stft.src_info.samplerate);
    const double src_total_sec= stft.src_info.frames / sample_rate;

    // ============================================================
    // 1. Contiguous region detection from timemap segments
    // ============================================================
    struct SegInfo {
        size_t src_start, src_end;
        double alpha;          // get_alpha() coordinates
        double duration_sec;
    };

    std::vector<SegInfo> segs;
    for (size_t i = 0; i + 1 < stft.timemap.size(); ++i) {
        const auto& a = stft.timemap[i];
        const auto& b = stft.timemap[i + 1];
        if (b.src_frame <= a.src_frame) continue;
        double src_dur = static_cast<double>(b.src_frame - a.src_frame);
        double tgt_dur = static_cast<double>(b.tgt_frame - a.tgt_frame);
        double alpha   = tgt_dur / src_dur;
        segs.push_back({a.src_frame, b.src_frame, alpha, src_dur / sample_rate});
    }

    // Group consecutive same-direction segments into contiguous regions
    auto direction = [](double a) -> int {
        if (a < 1.0 - UNITY_EPSILON) return -1; // speedup
        if (a > 1.0 + UNITY_EPSILON) return  1; // slowdown
        return 0; // unity
    };

    std::vector<StretchRegion> speedup_regions, slowdown_regions;

    // Accumulate into current region, flush when direction changes
    struct Building {
        int   dir = 0;
        size_t src_start = 0, src_end = 0;
        double alpha_dur_sum = 0.0; // weighted sum of alpha * dur
        double dur_sum = 0.0;
    } cur;

    auto flush = [&]() {
        if (cur.dir == 0 || cur.dur_sum < 1e-9) return;
        StretchRegion r{cur.src_start, cur.src_end,
                        cur.alpha_dur_sum / cur.dur_sum, cur.dur_sum};
        if (cur.dir == -1) speedup_regions.push_back(r);
        else               slowdown_regions.push_back(r);
    };

    for (const auto& s : segs) {
        int d = direction(s.alpha);
        if (d != cur.dir) {
            flush();
            cur.dir           = d;
            cur.src_start     = s.src_start;
            cur.src_end       = s.src_end;
            cur.alpha_dur_sum = s.alpha * s.duration_sec;
            cur.dur_sum       = s.duration_sec;
        } else {
            cur.src_end        = s.src_end;
            cur.alpha_dur_sum += s.alpha * s.duration_sec;
            cur.dur_sum       += s.duration_sec;
        }
    }
    flush();

    // Filter by minimum duration, sort by duration descending, take top N
    auto filter_and_rank = [](std::vector<StretchRegion>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const StretchRegion& r) {
                                   return r.duration_sec < MIN_STRETCH_SEC;
                               }),
                v.end());
        std::sort(v.begin(), v.end(),
                  [](const StretchRegion& a, const StretchRegion& b) {
                      return a.duration_sec > b.duration_sec;
                  });
        if (static_cast<int>(v.size()) > TOP_STRETCH_REGIONS)
            v.resize(TOP_STRETCH_REGIONS);
    };
    filter_and_rank(speedup_regions);
    filter_and_rank(slowdown_regions);

    std::cout << "  Speedup regions: " << speedup_regions.size()
              << "  Slowdown regions: " << slowdown_regions.size() << "\n";

    // ============================================================
    // 2. Slowdown fallback check
    // ============================================================
    double slowdown_total_sec = 0.0;
    for (const auto& r : slowdown_regions) slowdown_total_sec += r.duration_sec;
    stft.has_slowdown_frames = (slowdown_total_sec / src_total_sec >= SLOWDOWN_FALLBACK_THRESHOLD);

    if (!stft.has_slowdown_frames)
        std::cout << "  [EQ Match] Slowdown content < 10% of source — using speedup curve as fallback.\n";

    // ============================================================
    // 3. Speedup PSD accumulation
    // ============================================================
    if (!speedup_regions.empty()) {
        accumulate_and_smooth(stft, speedup_regions,
                              stft.raw_delta_db_speedup,
                              stft.smoothed_curve_speedup,
                              stft.alpha_ref_speedup);
        std::cout << "  Speedup: alpha_ref=" << stft.alpha_ref_speedup
                  << "  windows accumulated from "
                  << speedup_regions.size() << " region(s).\n";
    } else {
        std::cerr << "  [WARN] No qualifying speedup regions found.\n";
    }

    // ============================================================
    // 4. Slowdown PSD accumulation (skipped if fallback)
    // ============================================================
    if (stft.has_slowdown_frames && !slowdown_regions.empty()) {
        accumulate_and_smooth(stft, slowdown_regions,
                              stft.raw_delta_db_slowdown,
                              stft.smoothed_curve_slowdown,
                              stft.alpha_ref_slowdown);
        std::cout << "  Slowdown: alpha_ref=" << stft.alpha_ref_slowdown
                  << "  windows accumulated from "
                  << slowdown_regions.size() << " region(s).\n";
    }

    // ============================================================
    // 5. Fallback copies
    // ============================================================
    // No qualifying slowdown → use speedup curve for slowdown
    if (!stft.has_slowdown_frames && !stft.smoothed_curve_speedup.empty()) {
        stft.raw_delta_db_slowdown  = stft.raw_delta_db_speedup;
        stft.smoothed_curve_slowdown= stft.smoothed_curve_speedup;
        stft.alpha_ref_slowdown     = stft.alpha_ref_speedup;
    }

    // No qualifying speedup → reverse fallback: use slowdown for speedup
    if (speedup_regions.empty() && !stft.smoothed_curve_slowdown.empty()) {
        std::cerr << "  [WARN] No qualifying speedup regions — using slowdown curve as reverse fallback.\n";
        stft.raw_delta_db_speedup   = stft.raw_delta_db_slowdown;
        stft.smoothed_curve_speedup = stft.smoothed_curve_slowdown;
        stft.alpha_ref_speedup      = stft.alpha_ref_slowdown;
    }

    // Both empty → curves remain empty; synthesis will use G_eq=1.0 everywhere
    if (stft.smoothed_curve_speedup.empty() && stft.smoothed_curve_slowdown.empty())
        std::cerr << "  [WARN] No qualifying stretch regions found — EQ correction disabled.\n";

    std::cout << "  -> Delta curves computed.\n";
}
