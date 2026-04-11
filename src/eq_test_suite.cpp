// eq_test_suite.cpp — Standalone vocoder PSD-delta test suite.
// Does not link against the production engine; logic is adapted inline.
// Dependencies: libsndfile, FFTW3, libebur128, C++17.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <ebur128.h>
#include <fftw3.h>
#include <sndfile.h>

namespace fs = std::filesystem;

// ============================================================
// Constants — copied / adapted from stft_container.h
// ============================================================
static constexpr int    N                   = 3328;
static constexpr int    R_s                 = N / 4;   // 832
static constexpr double ATTACK_TOLERANCE_LU = 3.0;
static constexpr double RELEASE_TOLERANCE_LU= 8.0;
static constexpr double MIN_GAP_SEC         = 0.5;
static constexpr double MIN_PHRASE_SEC      = 5.0;
static constexpr int    CURVE_RESOLUTION    = 500;
static constexpr double HEAD_TAIL_MARGIN_SEC= 15.0;

static constexpr double XOVER_0 = 50.0;
static constexpr double XOVER_1 = 200.0;
static constexpr double XOVER_2 = 2000.0;

static const std::vector<double> STRETCH_RATIOS = {0.6, 0.8, 1.0, 1.2, 1.4};
static const std::vector<double> GAIN_DB_VALUES  = {-12.0, -6.0, 0.0, 6.0, 12.0};

// ============================================================
// DSP helpers — verbatim from stft_container.h
// ============================================================
struct TimeMapSegment {
    size_t src_frame;
    size_t tgt_frame;
};

inline double princarg(double phase) {
    return phase - 2.0 * M_PI * std::floor((phase + M_PI) / (2.0 * M_PI));
}

inline double get_alpha(size_t t_s, const std::vector<TimeMapSegment>& map) {
    if (map.empty()) return 1.0;
    if (t_s <= map.front().tgt_frame) return 1.0;
    for (size_t i = 0; i < map.size() - 1; ++i) {
        if (t_s >= map[i].tgt_frame && t_s < map[i+1].tgt_frame) {
            double tgt_dur = static_cast<double>(map[i+1].tgt_frame - map[i].tgt_frame);
            double src_dur = static_cast<double>(map[i+1].src_frame - map[i].src_frame);
            return tgt_dur / src_dur;
        }
    }
    return 1.0;
}

inline double map_source_to_target(size_t src_frame, const std::vector<TimeMapSegment>& map) {
    if (map.empty()) return static_cast<double>(src_frame);
    if (src_frame <= map.front().src_frame) return static_cast<double>(map.front().tgt_frame);
    for (size_t i = 0; i < map.size() - 1; ++i) {
        if (src_frame >= map[i].src_frame && src_frame < map[i+1].src_frame) {
            double src_dur = static_cast<double>(map[i+1].src_frame - map[i].src_frame);
            double tgt_dur = static_cast<double>(map[i+1].tgt_frame - map[i].tgt_frame);
            double offset  = static_cast<double>(src_frame - map[i].src_frame);
            return map[i].tgt_frame + (offset * (tgt_dur / src_dur));
        }
    }
    const auto& last = map.back();
    if (src_frame >= last.src_frame)
        return last.tgt_frame + (src_frame - last.src_frame);
    return 0.0;
}

// ============================================================
// Shared FFTW state (created once at startup)
// ============================================================
struct FFTState {
    double*        fft_in   = nullptr;
    fftw_complex*  fft_out  = nullptr;
    fftw_plan      plan_fwd{};
    fftw_complex*  ifft_in  = nullptr;
    double*        ifft_out = nullptr;
    fftw_plan      plan_inv{};

    std::vector<double> window;       // Hann
    std::vector<double> synth_window; // Hann / 1.5

    void init() {
        window.resize(N);
        synth_window.resize(N);
        for (int n = 0; n < N; ++n) {
            window[n]       = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)));
            synth_window[n] = window[n] / 1.5;
        }
        fft_in  = fftw_alloc_real(N);
        fft_out = fftw_alloc_complex(N / 2 + 1);
        plan_fwd = fftw_plan_dft_r2c_1d(N, fft_in, fft_out, FFTW_ESTIMATE);
        ifft_in  = fftw_alloc_complex(N / 2 + 1);
        ifft_out = fftw_alloc_real(N);
        plan_inv = fftw_plan_dft_c2r_1d(N, ifft_in, ifft_out, FFTW_ESTIMATE);
    }

    void cleanup() {
        fftw_destroy_plan(plan_fwd);
        fftw_destroy_plan(plan_inv);
        fftw_free(fft_in);
        fftw_free(fft_out);
        fftw_free(ifft_in);
        fftw_free(ifft_out);
    }
};

static FFTState G; // global singleton

// ============================================================
// Block detection (Part 3A–3C)
// ============================================================
struct AcousticBlock {
    size_t start_frame;
    size_t end_frame;
};

// Returns loud blocks and optionally quiet blocks from the trimmed region.
// loud_out and quiet_out are filled. Returns integrated_lufs.
static double detect_blocks(const std::vector<float>& audio, int channels, int sample_rate,
                             std::vector<AcousticBlock>& loud_out,
                             std::vector<AcousticBlock>& quiet_out) {
    loud_out.clear();
    quiet_out.clear();

    size_t total_frames = audio.size() / channels;
    size_t margin       = static_cast<size_t>(HEAD_TAIL_MARGIN_SEC * sample_rate);
    if (2 * margin >= total_frames) {
        std::cerr << "[WARN] File too short for 15s head/tail margin; using full file.\n";
        margin = 0;
    }
    size_t trim_start = margin;
    size_t trim_end   = total_frames - margin;
    size_t trim_len   = trim_end - trim_start;

    // --- Integrated LUFS on trimmed region ---
    ebur128_state* st_i = ebur128_init(channels, sample_rate, EBUR128_MODE_M | EBUR128_MODE_I);
    {
        constexpr size_t CHUNK = 4096;
        for (size_t pos = trim_start; pos < trim_end; pos += CHUNK) {
            size_t n = std::min(CHUNK, trim_end - pos);
            ebur128_add_frames_float(st_i, audio.data() + pos * channels, n);
        }
    }
    double integrated_lufs;
    ebur128_loudness_global(st_i, &integrated_lufs);
    double attack_thresh  = integrated_lufs - ATTACK_TOLERANCE_LU;
    double release_thresh = integrated_lufs - RELEASE_TOLERANCE_LU;

    // --- Re-run momentary scan (reset and feed again) ---
    ebur128_destroy(&st_i);
    ebur128_state* st = ebur128_init(channels, sample_rate, EBUR128_MODE_M | EBUR128_MODE_I);

    // --- Loud block scan ---
    std::vector<AcousticBlock> raw_loud, raw_quiet;
    bool loud_active = false, quiet_active = false;
    size_t loud_start = 0, quiet_start = 0;

    constexpr size_t CHUNK = 4096;
    size_t current_frame = trim_start; // absolute frame within audio
    for (size_t pos = trim_start; pos < trim_end; pos += CHUNK) {
        size_t n = std::min(CHUNK, trim_end - pos);
        ebur128_add_frames_float(st, audio.data() + pos * channels, n);
        current_frame = pos + n;
        // Current position relative to trimmed start
        size_t rel_frame = current_frame - trim_start;

        double momentary;
        ebur128_loudness_momentary(st, &momentary);

        // Loud
        if (!loud_active && momentary >= attack_thresh) {
            loud_active = true;
            loud_start  = rel_frame;
        } else if (loud_active && momentary < release_thresh) {
            loud_active = false;
            raw_loud.push_back({loud_start, rel_frame});
        }
        // Quiet (inverted: starts below attack_thresh, ends above attack_thresh)
        if (!quiet_active && momentary < attack_thresh) {
            quiet_active = true;
            quiet_start  = rel_frame;
        } else if (quiet_active && momentary >= attack_thresh) {
            quiet_active = false;
            raw_quiet.push_back({quiet_start, rel_frame});
        }
    }
    if (loud_active)  raw_loud.push_back({loud_start,  trim_len});
    if (quiet_active) raw_quiet.push_back({quiet_start, trim_len});
    ebur128_destroy(&st);

    // --- Merge & filter helper ---
    auto merge_and_filter = [&](const std::vector<AcousticBlock>& raw) -> std::vector<AcousticBlock> {
        size_t min_gap = static_cast<size_t>(MIN_GAP_SEC * sample_rate);
        std::vector<AcousticBlock> merged;
        for (const auto& b : raw) {
            if (merged.empty()) { merged.push_back(b); continue; }
            if ((b.start_frame - merged.back().end_frame) < min_gap)
                merged.back().end_frame = b.end_frame;
            else
                merged.push_back(b);
        }
        std::vector<AcousticBlock> final_v;
        for (const auto& b : merged) {
            double dur = static_cast<double>(b.end_frame - b.start_frame) / sample_rate;
            if (dur >= MIN_PHRASE_SEC) final_v.push_back(b);
        }
        return final_v;
    };

    // Blocks use relative frame indices (0 = trim_start in original audio).
    // We add trim_start back so callers can index into the original audio buffer.
    auto adjust = [&](std::vector<AcousticBlock>& blocks) {
        for (auto& b : blocks) {
            b.start_frame += trim_start;
            b.end_frame   += trim_start;
        }
    };

    auto raw_loud_filtered  = merge_and_filter(raw_loud);
    auto raw_quiet_filtered = merge_and_filter(raw_quiet);
    adjust(raw_loud_filtered);
    adjust(raw_quiet_filtered);

    loud_out  = std::move(raw_loud_filtered);
    quiet_out = std::move(raw_quiet_filtered);

    return integrated_lufs;
}

// ============================================================
// Stitching with equal-power crossfade (Part 3D)
// ============================================================
static std::vector<float> stitch_blocks(const std::vector<float>& audio,
                                        const std::vector<AcousticBlock>& blocks,
                                        int channels,
                                        double& natural_lufs_out,
                                        int sample_rate) {
    std::vector<float> result;
    if (blocks.empty()) {
        natural_lufs_out = -std::numeric_limits<double>::infinity();
        return result;
    }

    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const auto& b = blocks[bi];
        size_t block_len = b.end_frame - b.start_frame;
        if (block_len == 0) continue;

        const float* src = audio.data() + b.start_frame * channels;

        if (result.empty()) {
            result.insert(result.end(), src, src + block_len * channels);
        } else {
            // Equal-power crossfade of N samples at the boundary
            size_t xfade = std::min(static_cast<size_t>(N), block_len);
            xfade = std::min(xfade, result.size() / channels);

            size_t tail_start = result.size() - xfade * channels;
            // Fade out end of result, fade in beginning of new block
            for (size_t n = 0; n < xfade; ++n) {
                double t = (xfade > 1) ? (static_cast<double>(n) / (xfade - 1)) : 1.0;
                double fade_out = std::sqrt(1.0 - t);
                double fade_in  = std::sqrt(t);
                for (int ch = 0; ch < channels; ++ch) {
                    result[tail_start + n * channels + ch] =
                        static_cast<float>(result[tail_start + n * channels + ch] * fade_out
                                         + src[n * channels + ch] * fade_in);
                }
            }
            // Append the rest of the new block after the crossfade
            if (block_len > xfade)
                result.insert(result.end(),
                               src + xfade * channels,
                               src + block_len * channels);
        }
    }

    // Compute natural LUFS
    if (!result.empty()) {
        ebur128_state* st = ebur128_init(channels, sample_rate, EBUR128_MODE_I);
        constexpr size_t CHUNK = 4096;
        size_t total_f = result.size() / channels;
        for (size_t pos = 0; pos < total_f; pos += CHUNK) {
            size_t n = std::min(CHUNK, total_f - pos);
            ebur128_add_frames_float(st, result.data() + pos * channels, n);
        }
        ebur128_loudness_global(st, &natural_lufs_out);
        ebur128_destroy(&st);
    } else {
        natural_lufs_out = -std::numeric_limits<double>::infinity();
    }

    return result;
}

// ============================================================
// Synthetic timemap generation (Part 4B)
// ============================================================
static std::vector<TimeMapSegment> make_timemap(size_t chunk_len_frames, double alpha) {
    size_t tgt_len = static_cast<size_t>(std::llround(chunk_len_frames / alpha));
    return {{0, 0}, {chunk_len_frames, tgt_len}};
}

// ============================================================
// Frame map generation — adapted from AudioSTFT::generate_frame_map()
// ============================================================
static std::vector<int64_t> generate_frame_map(size_t target_total_frames,
                                                const std::vector<TimeMapSegment>& timemap) {
    std::vector<int64_t> fmap;
    double t_a = -(double)N / 2.0;
    size_t t_s = 0;
    int    idx = 0;
    while (t_s < target_total_frames) {
        double alpha = get_alpha(t_s, timemap);
        double R_a   = R_s / alpha;
        if (idx > 0) t_a += R_a;
        fmap.push_back(static_cast<int64_t>(std::llround(t_a)));
        t_s += R_s;
        idx++;
    }
    return fmap;
}

// ============================================================
// Phase vocoder — adapted from phase_vocoder.cpp
// Reads from an in-memory float buffer instead of SNDFILE.
// ============================================================
static std::vector<float> run_vocoder(const std::vector<float>& gained_buf,
                                      int channels,
                                      const std::vector<TimeMapSegment>& timemap,
                                      size_t target_total_frames) {
    const std::vector<int64_t> frame_map = generate_frame_map(target_total_frames, timemap);
    int num_frames = static_cast<int>(frame_map.size());

    std::vector<float> out;
    out.reserve(target_total_frames * channels);

    // Phase state
    std::vector<std::vector<double>> phi_prev(channels,   std::vector<double>(N/2+1, 0.0));
    std::vector<std::vector<double>> theta_prev(channels, std::vector<double>(N/2+1, 0.0));
    std::vector<std::vector<double>> overlap_add(channels, std::vector<double>(N, 0.0));

    size_t src_frames = gained_buf.size() / channels;
    std::vector<float> read_buf(N * channels, 0.0f);
    int frames_to_skip = N / 2;

    for (int frame_idx = 0; frame_idx < num_frames; ++frame_idx) {
        int64_t t_a_rounded = frame_map[frame_idx];
        int64_t R_a_actual  = (frame_idx > 0) ? (frame_map[frame_idx] - frame_map[frame_idx-1]) : 0;

        // Read N frames from memory buffer
        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && static_cast<size_t>(t_a_rounded) < src_frames) {
            size_t to_copy = std::min(static_cast<size_t>(N), src_frames - t_a_rounded);
            std::copy(gained_buf.begin() + t_a_rounded * channels,
                      gained_buf.begin() + (t_a_rounded + to_copy) * channels,
                      read_buf.begin());
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n)
                G.fft_in[n] = read_buf[n * channels + ch] * G.window[n];
            fftw_execute(G.plan_fwd);

            std::vector<double> M(N/2+1), phi(N/2+1), theta(N/2+1);
            for (int k = 0; k <= N/2; ++k) {
                M[k]   = std::sqrt(G.fft_out[k][0]*G.fft_out[k][0] + G.fft_out[k][1]*G.fft_out[k][1]);
                phi[k] = std::atan2(G.fft_out[k][1], G.fft_out[k][0]);
            }

            if (frame_idx == 0) {
                for (int k = 0; k <= N/2; ++k) theta[k] = phi[k];
            } else {
                std::vector<int> peaks;
                for (int k = 1; k < N/2; ++k)
                    if (M[k] > M[k-1] && M[k] > M[k+1]) peaks.push_back(k);
                if (peaks.empty()) peaks.push_back(N/4);

                for (int p : peaks) {
                    double omega_p = 2.0 * M_PI * p / N;
                    theta[p] = theta_prev[ch][p] +
                               (omega_p + princarg(phi[p] - phi_prev[ch][p] - omega_p * R_a_actual) / R_a_actual) * R_s;
                }
                size_t peak_idx = 0;
                for (int k = 0; k <= N/2; ++k) {
                    if (peak_idx < peaks.size()-1 &&
                        std::abs(k - peaks[peak_idx+1]) < std::abs(k - peaks[peak_idx]))
                        peak_idx++;
                    int p = peaks[peak_idx];
                    if (k != p) theta[k] = theta[p] + phi[k] - phi[p];
                }
            }

            for (int k = 0; k <= N/2; ++k) {
                phi_prev[ch][k]   = phi[k];
                theta_prev[ch][k] = theta[k];
                G.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                G.ifft_in[k][1] = M[k] * std::sin(theta[k]);
            }
            fftw_execute(G.plan_inv);

            for (int n = 0; n < N; ++n)
                overlap_add[ch][n] += (G.ifft_out[n] / N) * G.synth_window[n];
        }

        int write_offset = 0, write_len = R_s;
        if (frames_to_skip > 0) {
            if (frames_to_skip >= write_len) {
                frames_to_skip -= write_len;
                write_len = 0;
            } else {
                write_offset    = frames_to_skip;
                write_len      -= frames_to_skip;
                frames_to_skip  = 0;
            }
        }
        for (int n = write_offset; n < write_offset + write_len; ++n)
            for (int ch = 0; ch < channels; ++ch)
                out.push_back(static_cast<float>(overlap_add[ch][n]));

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) overlap_add[ch][n] = overlap_add[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n) overlap_add[ch][n] = 0.0;
        }
    }
    return out;
}

// ============================================================
// PSD delta computation (Parts 4F, 4G)
// ============================================================
struct SmoothedResult {
    std::vector<double> raw_delta_db;       // size N/2+1
    std::vector<double> smoothed_hz;        // size CURVE_RESOLUTION
    std::vector<double> smoothed_db;        // size CURVE_RESOLUTION
};

static SmoothedResult compute_psd_delta(
        const std::vector<float>& src_buf,       // original unscaled chunk
        const std::vector<float>& tgt_buf,       // inverse-gained vocoder output
        int channels, double nyquist,
        const std::vector<TimeMapSegment>& timemap)
{
    const double bin_hz_width = nyquist * 2.0 / N; // sample_rate / N

    std::vector<double> src_psd(N/2+1, 0.0);
    std::vector<double> tgt_psd(N/2+1, 0.0);
    size_t total_psd_windows = 0;

    size_t src_frames = src_buf.size() / channels;
    size_t tgt_frames = tgt_buf.size() / channels;
    std::vector<float> src_chunk(N * channels, 0.0f);
    std::vector<float> tgt_chunk(N * channels, 0.0f);

    for (size_t s_frame = 0; s_frame + N <= src_frames; s_frame += N/2) {
        size_t t_start = static_cast<size_t>(map_source_to_target(s_frame, timemap));
        if (t_start + N > tgt_frames) break;

        // Read src window
        std::copy(src_buf.begin() + s_frame * channels,
                  src_buf.begin() + (s_frame + N) * channels,
                  src_chunk.begin());
        // Read tgt window
        std::copy(tgt_buf.begin() + t_start * channels,
                  tgt_buf.begin() + (t_start + N) * channels,
                  tgt_chunk.begin());

        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < N; ++i)
                G.fft_in[i] = src_chunk[i * channels + ch] * G.window[i];
            fftw_execute(G.plan_fwd);
            for (int k = 1; k <= N/2; ++k)
                src_psd[k] += G.fft_out[k][0]*G.fft_out[k][0] + G.fft_out[k][1]*G.fft_out[k][1];

            for (int i = 0; i < N; ++i)
                G.fft_in[i] = tgt_chunk[i * channels + ch] * G.window[i];
            fftw_execute(G.plan_fwd);
            for (int k = 1; k <= N/2; ++k)
                tgt_psd[k] += G.fft_out[k][0]*G.fft_out[k][0] + G.fft_out[k][1]*G.fft_out[k][1];
        }
        total_psd_windows++;
    }

    SmoothedResult res;
    res.raw_delta_db.assign(N/2+1, 0.0);
    double count = static_cast<double>(total_psd_windows * channels);
    if (count > 0) {
        for (int k = 1; k <= N/2; ++k) {
            if (src_psd[k] > 0 && tgt_psd[k] > 0)
                res.raw_delta_db[k] = 10.0 * std::log10(src_psd[k] / count)
                                    - 10.0 * std::log10(tgt_psd[k] / count);
        }
    }

    // Gaussian smoothing
    res.smoothed_hz.resize(CURVE_RESOLUTION);
    res.smoothed_db.resize(CURVE_RESOLUTION);
    double log_min = std::log10(10.0), log_max = std::log10(nyquist);
    double sigma_octaves = 1.0 / 3.0 / 2.355;

    for (int i = 0; i < CURVE_RESOLUTION; ++i) {
        double hz = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
        double wsum = 0.0, dsum = 0.0;
        for (int k = 1; k <= N/2; ++k) {
            double bin_hz = k * bin_hz_width;
            if (bin_hz < 10.0) continue;
            double w = std::exp(-0.5 * std::pow(std::log2(bin_hz / hz) / sigma_octaves, 2.0));
            if (w > 0.001) { wsum += w; dsum += w * res.raw_delta_db[k]; }
        }
        res.smoothed_hz[i] = hz;
        res.smoothed_db[i] = (wsum > 0.0) ? (dsum / wsum) : 0.0;
    }

    // Nyquist taper
    for (int i = 0; i < CURVE_RESOLUTION; ++i) {
        if (res.smoothed_hz[i] > nyquist - 20.0)
            res.smoothed_db[i] *= std::max(0.0, std::min(1.0, (nyquist - res.smoothed_hz[i]) / 20.0));
    }

    return res;
}

// ============================================================
// Band statistics (Part 6)
// ============================================================
struct BandStats {
    double mean_db;
    double p95_dev;
    double iqr_dev;
};

// Pre-compute dominant band assignment per smoothed curve point.
// Returns band index 0–3 for each of the CURVE_RESOLUTION points.
static std::vector<int> assign_bands(const std::vector<double>& hz_pts, double /*nyquist*/) {
    // Use LR4 weights to find dominant band per smoothed curve point.
    auto lr4_lp = [](double f, double fc) -> double {
        if (f <= 0.0) return 1.0;
        double r8 = std::pow(f / fc, 8.0);
        return 1.0 / (1.0 + r8);
    };
    auto lr4_hp = [](double f, double fc) -> double {
        if (f <= 0.0) return 0.0;
        double r8 = std::pow(f / fc, 8.0);
        return r8 / (1.0 + r8);
    };
    std::vector<int> bands(hz_pts.size());
    for (size_t i = 0; i < hz_pts.size(); ++i) {
        double f = hz_pts[i];
        double w0 = lr4_lp(f, XOVER_0);
        double w3 = lr4_hp(f, XOVER_2);
        double mid = 1.0 - w0 - w3;
        double w1 = mid * lr4_lp(f, XOVER_1);
        double w2 = mid * lr4_hp(f, XOVER_1);

        double best = w0; int b = 0;
        if (w1 > best) { best = w1; b = 1; }
        if (w2 > best) { best = w2; b = 2; }
        if (w3 > best) { b = 3; }
        bands[i] = b;
    }
    return bands;
}

static BandStats compute_band_stats(const std::vector<double>& curve_db,
                                    const std::vector<int>& band_assign,
                                    int target_band) {
    std::vector<double> vals;
    for (size_t i = 0; i < curve_db.size(); ++i)
        if (band_assign[i] == target_band) vals.push_back(curve_db[i]);

    if (vals.empty()) return {0.0, 0.0, 0.0};

    double mean = std::accumulate(vals.begin(), vals.end(), 0.0) / vals.size();

    std::vector<double> abs_dev;
    abs_dev.reserve(vals.size());
    for (double v : vals) abs_dev.push_back(std::abs(v - mean));
    std::sort(abs_dev.begin(), abs_dev.end());

    size_t cnt = abs_dev.size();
    double p95 = abs_dev[static_cast<size_t>(std::floor(0.95 * cnt))];
    double p75 = abs_dev[static_cast<size_t>(std::floor(0.75 * cnt))];
    double p25 = abs_dev[static_cast<size_t>(std::floor(0.25 * cnt))];
    return {mean, p95, p75 - p25};
}

// ============================================================
// Result record (per combination, per band)
// ============================================================
struct ComboResult {
    // Identity
    std::string symphony;
    int         movement;
    std::string chunk_type; // "loud" or "quiet"
    double      natural_lufs;
    double      effective_lufs;
    double      stretch_ratio;
    double      gain_db;
    int         band;
    double      band_mean_db;
    double      band_p95_dev;
    double      band_iqr_dev;
    double      alpha_ref;
    int         is_baseline;
    int         normalized;
};

// ============================================================
// Filename parsing
// ============================================================
static void parse_file_identity(const std::string& stem,
                                 std::string& symphony_out, int& movement_out,
                                 int file_index) {
    // Try to match "<sym><number>_mvt<number>" pattern
    auto uscore = stem.find('_');
    if (uscore != std::string::npos) {
        std::string sym = stem.substr(0, uscore);
        std::string rest = stem.substr(uscore + 1);
        // Look for "mvt" prefix in rest
        auto mvt_pos = rest.find("mvt");
        if (mvt_pos != std::string::npos) {
            std::string num_s = rest.substr(mvt_pos + 3);
            try {
                movement_out  = std::stoi(num_s);
                symphony_out  = sym;
                return;
            } catch (...) {}
        }
    }
    // Fallback: use stem as symphony, sequential movement
    symphony_out = stem;
    movement_out = file_index + 1;
}

// ============================================================
// TSV writer
// ============================================================
static void write_tsv(const std::vector<ComboResult>& rows, const fs::path& out_dir) {
    fs::path path = out_dir / "eq_test_suite.tsv";
    std::ofstream f(path);
    if (!f) { std::cerr << "[ERROR] Cannot write TSV to " << path << "\n"; return; }
    f << "symphony\tmovement\tchunk_type\tnatural_lufs\teffective_lufs"
      << "\tstretch_ratio\tgain_db\tband\tband_mean_db\tband_p95_dev\tband_iqr_dev"
      << "\talpha_ref\tis_baseline\tnormalized\n";
    f << std::fixed;
    f.precision(4);
    for (const auto& r : rows) {
        f << r.symphony    << '\t'
          << r.movement    << '\t'
          << r.chunk_type  << '\t'
          << r.natural_lufs << '\t'
          << r.effective_lufs << '\t'
          << r.stretch_ratio << '\t'
          << r.gain_db      << '\t'
          << r.band         << '\t'
          << r.band_mean_db << '\t'
          << r.band_p95_dev << '\t'
          << r.band_iqr_dev << '\t'
          << r.alpha_ref    << '\t'
          << r.is_baseline  << '\t'
          << r.normalized   << '\n';
    }
    std::cout << "[TSV] Written: " << path << "  (" << rows.size() << " rows)\n";
}

// ============================================================
// Per-file intermediate storage (before baseline normalization)
// ============================================================
struct ComboRaw {
    std::string chunk_type;
    double      stretch_ratio;
    double      gain_db;
    double      natural_lufs;
    std::vector<double> smoothed_hz;
    std::vector<double> smoothed_db;   // raw (before baseline subtraction)
    std::vector<double> norm_db;       // filled in during normalization pass
    bool        has_baseline = false;  // set on α=1.0 rows
    int         is_baseline  = 0;
    int         normalized   = 0;
};

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " [--out <dir>] [--diagnostics] <file1.wav> [file2.wav ...]\n";
        return 1;
    }

    fs::path out_dir = ".";
    bool diagnostics = false; (void)diagnostics; // reserved for --diagnostics PNG output
    std::vector<std::string> audio_files;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i+1 < argc) {
            out_dir = argv[++i];
        } else if (a == "--diagnostics") {
            diagnostics = true;
        } else {
            audio_files.push_back(a);
        }
    }

    if (audio_files.empty()) {
        std::cerr << "[ERROR] No audio files provided.\n";
        return 1;
    }
    if (!fs::exists(out_dir)) fs::create_directories(out_dir);

    // Initialize shared FFTW state
    G.init();

    std::vector<ComboResult> all_rows;

    for (int file_idx = 0; file_idx < static_cast<int>(audio_files.size()); ++file_idx) {
        const std::string& fpath = audio_files[file_idx];
        std::string stem = fs::path(fpath).stem().string();
        std::string symphony;
        int movement;
        parse_file_identity(stem, symphony, movement, file_idx);

        std::cout << "\n=== File " << file_idx+1 << "/" << audio_files.size()
                  << ": " << fpath << " (" << symphony << " mvt" << movement << ") ===\n";

        // --- Open with libsndfile ---
        SF_INFO info{};
        info.format = 0;
        SNDFILE* sf = sf_open(fpath.c_str(), SFM_READ, &info);
        if (!sf) {
            std::cerr << "[WARN] Cannot open " << fpath << ": " << sf_strerror(nullptr) << "\n";
            continue;
        }
        int channels   = info.channels;
        int samplerate = info.samplerate;
        double nyquist = samplerate / 2.0;
        sf_count_t total_frames = info.frames;

        // Read entire file into memory
        std::vector<float> audio(total_frames * channels);
        sf_readf_float(sf, audio.data(), total_frames);
        sf_close(sf);
        std::cout << "  Loaded " << total_frames << " frames, " << channels
                  << " ch, " << samplerate << " Hz\n";

        // --- Block detection ---
        std::vector<AcousticBlock> loud_blocks, quiet_blocks;
        double integrated_lufs = detect_blocks(audio, channels, samplerate,
                                               loud_blocks, quiet_blocks);
        std::cout << "  Integrated LUFS: " << integrated_lufs
                  << "  loud blocks: " << loud_blocks.size()
                  << "  quiet blocks: " << quiet_blocks.size() << "\n";

        // --- Stitch chunks ---
        struct ChunkInfo {
            std::string label;
            std::vector<float> buf;
            double natural_lufs;
        };
        std::vector<ChunkInfo> chunks;

        {
            double nl;
            auto loud_buf  = stitch_blocks(audio, loud_blocks,  channels, nl, samplerate);
            if (loud_buf.size() / channels >= static_cast<size_t>(2 * N)) {
                chunks.push_back({"loud", std::move(loud_buf), nl});
                std::cout << "  Loud chunk: " << chunks.back().buf.size()/channels
                          << " frames, LUFS=" << nl << "\n";
            } else {
                std::cerr << "[WARN] Loud chunk too short or empty for " << fpath << "\n";
            }
        }
        {
            double nl;
            auto quiet_buf = stitch_blocks(audio, quiet_blocks, channels, nl, samplerate);
            if (quiet_buf.size() / channels >= static_cast<size_t>(2 * N)) {
                chunks.push_back({"quiet", std::move(quiet_buf), nl});
                std::cout << "  Quiet chunk: " << chunks.back().buf.size()/channels
                          << " frames, LUFS=" << nl << "\n";
            } else {
                std::cerr << "[WARN] Quiet chunk too short or empty for " << fpath << "\n";
            }
        }
        audio.clear(); audio.shrink_to_fit(); // free raw audio; chunks are stitched

        // Pre-compute band assignments (same hz grid for all combos)
        // We'll build the hz grid from the first smoothed result, or compute it now.
        std::vector<double> hz_grid(CURVE_RESOLUTION);
        {
            double log_min = std::log10(10.0), log_max = std::log10(nyquist);
            for (int i = 0; i < CURVE_RESOLUTION; ++i)
                hz_grid[i] = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
        }
        std::vector<int> band_assign = assign_bands(hz_grid, nyquist);

        // --- Per-chunk processing ---
        for (auto& chunk : chunks) {
            size_t chunk_frames = chunk.buf.size() / channels;

            // We store all raw combos for this (chunk_type) group here,
            // then do baseline normalization before writing to TSV.
            // Key: (gain_db, stretch_ratio) → ComboRaw
            // Group: same (chunk_type, gain_db), baseline = α=1.0 combo
            // Organized as: raws[gain_db_idx][stretch_idx]
            const int n_stretch = static_cast<int>(STRETCH_RATIOS.size());
            const int n_gain    = static_cast<int>(GAIN_DB_VALUES.size());
            // [gain_idx][stretch_idx]
            std::vector<std::vector<ComboRaw>> raws(n_gain, std::vector<ComboRaw>(n_stretch));

            for (int gi = 0; gi < n_gain; ++gi) {
                double gain_db     = GAIN_DB_VALUES[gi];
                double gain_linear = std::pow(10.0, gain_db / 20.0);
                double inv_gain    = 1.0 / gain_linear;

                for (int si = 0; si < n_stretch; ++si) {
                    double alpha = STRETCH_RATIOS[si];

                    std::cout << "  [" << chunk.label << "] gain=" << gain_db
                              << " dB  alpha=" << alpha << "  ... " << std::flush;

                    // 4A — apply gain
                    std::vector<float> gained(chunk.buf.size());
                    for (size_t s = 0; s < chunk.buf.size(); ++s)
                        gained[s] = chunk.buf[s] * static_cast<float>(gain_linear);

                    // 4B — synthetic timemap
                    auto timemap = make_timemap(chunk_frames, alpha);
                    size_t tgt_total = timemap.back().tgt_frame + N;

                    // 4C — phase vocoder
                    auto tgt_buf = run_vocoder(gained, channels, timemap, tgt_total);
                    gained.clear(); gained.shrink_to_fit();

                    // 4D — inverse gain
                    for (float& s : tgt_buf) s *= static_cast<float>(inv_gain);

                    // 4F+4G — PSD delta + smoothing
                    auto result = compute_psd_delta(chunk.buf, tgt_buf, channels, nyquist, timemap);
                    tgt_buf.clear(); tgt_buf.shrink_to_fit();

                    ComboRaw& cr = raws[gi][si];
                    cr.chunk_type    = chunk.label;
                    cr.stretch_ratio = alpha;
                    cr.gain_db       = gain_db;
                    cr.natural_lufs  = chunk.natural_lufs;
                    cr.smoothed_hz   = result.smoothed_hz;
                    cr.smoothed_db   = result.smoothed_db;
                    cr.is_baseline   = (alpha == 1.0) ? 1 : 0;
                    cr.normalized    = 0; // to be updated

                    std::cout << "done\n";
                }
            }

            // --- Baseline normalization (Part 5) ---
            // Find α=1.0 index
            int unity_idx = -1;
            for (int si = 0; si < n_stretch; ++si)
                if (STRETCH_RATIOS[si] == 1.0) { unity_idx = si; break; }

            for (int gi = 0; gi < n_gain; ++gi) {
                const std::vector<double>* baseline_db = nullptr;
                if (unity_idx >= 0 && !raws[gi][unity_idx].smoothed_db.empty())
                    baseline_db = &raws[gi][unity_idx].smoothed_db;

                for (int si = 0; si < n_stretch; ++si) {
                    ComboRaw& cr = raws[gi][si];
                    if (cr.is_baseline) {
                        // α=1.0: use raw, no subtraction
                        cr.norm_db  = cr.smoothed_db;
                        cr.normalized = 0;
                    } else if (baseline_db) {
                        cr.norm_db.resize(CURVE_RESOLUTION);
                        for (int i = 0; i < CURVE_RESOLUTION; ++i)
                            cr.norm_db[i] = cr.smoothed_db[i] - (*baseline_db)[i];
                        cr.normalized = 1;
                    } else {
                        // No baseline — store raw
                        cr.norm_db  = cr.smoothed_db;
                        cr.normalized = 0;
                    }
                }
            }

            // --- Band statistics + TSV rows (Part 6+7) ---
            for (int gi = 0; gi < n_gain; ++gi) {
                for (int si = 0; si < n_stretch; ++si) {
                    const ComboRaw& cr = raws[gi][si];
                    for (int b = 0; b < 4; ++b) {
                        BandStats bs = compute_band_stats(cr.norm_db, band_assign, b);
                        ComboResult row;
                        row.symphony      = symphony;
                        row.movement      = movement;
                        row.chunk_type    = cr.chunk_type;
                        row.natural_lufs  = cr.natural_lufs;
                        row.effective_lufs = cr.natural_lufs + cr.gain_db;
                        row.stretch_ratio  = cr.stretch_ratio;
                        row.gain_db        = cr.gain_db;
                        row.band           = b;
                        row.band_mean_db   = bs.mean_db;
                        row.band_p95_dev   = bs.p95_dev;
                        row.band_iqr_dev   = bs.iqr_dev;
                        row.alpha_ref      = cr.stretch_ratio;
                        row.is_baseline    = cr.is_baseline;
                        row.normalized     = cr.normalized;
                        all_rows.push_back(row);
                    }
                }
            }
        } // chunks
    } // files

    write_tsv(all_rows, out_dir);
    G.cleanup();
    std::cout << "\nDone.\n";
    return 0;
}
