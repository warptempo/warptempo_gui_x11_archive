#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <fftw3.h>
#include <sndfile.h>

// --- Data Structures ---
struct TimeMapSegment {
    size_t src_frame;
    size_t tgt_frame;
};

struct TransientMarker {
    int synth_frame;
    int64_t src_frame;
};

struct TransientsParams {
    bool   enabled          = true;
    double xover_low        = 120.0;
    double xover_high       = 3500.0;
    double tau_back_ms      = 30.0;
    double thresh_db        = -20.0;
    double refractory_ms    = 1500.0;
    double anticipation_ms  = 100.0;
};

struct LimiterParams {
    bool   enabled               = true;
    double ceiling_dbfs          = -0.3;
    double tolerance_db          = 0.01;
    int    num_bands_override    = 0;    // 0 = auto-derive from 1/3-octave grid
    bool   diag                  = false;
};

// --- DSP Helpers ---
inline double princarg(double phase) {
    return phase - 2.0 * M_PI * std::floor((phase + M_PI) / (2.0 * M_PI));
}

// get_alpha() returns tgt_dur / src_dur, the engine-internal
// alpha used by the phase vocoder.
// alpha < 1.0: output shorter than source (user's tempo value > 1, speedup).
// alpha > 1.0: output longer than source (user's tempo value < 1, slowdown).
// Note: the engine's alpha is the reciprocal of the tempo value the
// user authors in the warp marker file, which is consumed by the
// parser as delta_tgt = delta_src / (tempo * scale).
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
    if (src_frame <= map.front().src_frame) return map.front().tgt_frame;
    for (size_t i = 0; i < map.size() - 1; ++i) {
        if (src_frame >= map[i].src_frame && src_frame < map[i+1].src_frame) {
            double src_dur = static_cast<double>(map[i+1].src_frame - map[i].src_frame);
            double tgt_dur = static_cast<double>(map[i+1].tgt_frame - map[i].tgt_frame);
            double offset = static_cast<double>(src_frame - map[i].src_frame);
            return map[i].tgt_frame + (offset * (tgt_dur / src_dur));
        }
    }
    const auto& last = map.back();
    if (src_frame >= last.src_frame) {
        return last.tgt_frame + (src_frame - last.src_frame);
    }
    return 0.0;
}

// --- Output sample timing convention ---
// Both phase_vocoder.cpp (Pass 1) and synthesis.cpp (Pass 4) emit samples with the
// same OLA ramp-up trim: the first N/2 samples are dropped via `frames_to_skip = N/2`.
// Consequences any downstream module must respect:
//   - Output sample 0 in the final WAV corresponds to pre-trim OLA position N/2.
//   - A transient with synth_frame m lands at output sample m * R_s; the +N/2
//     window-center offset is absorbed by the trim, so diag spikes must NOT add it.
//   - Total output length = num_frames * R_s + N/2 - R_s.
//     Any auxiliary buffer sized to match the output (limiter meas_ola, diag WAVs)
//     must use this formula; target_total_frames describes the *input* plan, not
//     the emitted sample count.
//
// --- Central Pipeline Container ---
// Peak memory dominated by overlap-add buffers, FFTW planning, and phase vocoder state arrays.
struct AudioSTFT {
    // Source metadata
    SF_INFO src_info{};
    SNDFILE* src_snd = nullptr;
    // Default matches CLI; eq_test_suite.cpp data collected at N=3328 but generalizes
    // due to 1/3-octave Gaussian smoothing.
    int N = 4096;
    int R_s = 0;
    int channels = 0;
    double nyquist = 0.0;
    double bin_hz_width = 0.0;
    size_t target_total_frames = 0;

    // Timemap
    std::vector<TimeMapSegment> timemap;

    // Windows
    std::vector<double> window;
    std::vector<double> synth_window;

    // FFTW resources (shared across modules)
    double* fft_in = nullptr;
    fftw_complex* fft_out = nullptr;
    fftw_plan plan_fwd{};
    fftw_complex* ifft_in = nullptr;
    double* ifft_out = nullptr;
    fftw_plan plan_inv{};
    bool fftw_threads_inited = false;

    // Phase vocoder accumulators
    std::vector<std::vector<double>> phi_prev;
    std::vector<std::vector<double>> theta_prev;
    std::vector<std::vector<double>> overlap_add;

    // Virtual target buffer (Pass 1 output)
    std::vector<float> virtual_tgt_buf;

    // Transient phase reset
    std::vector<TransientMarker> transient_markers;

    // Automatic transient detector
    TransientsParams transients_params;
    bool transients_diag = false;

    // Spectral limiter
    LimiterParams limiter_params;
    int num_bands = 0;
    std::vector<int> bin_to_band;                        // size K = N/2+1
    std::vector<std::vector<double>> attenuation_map;    // [num_frames][num_bands]

    // Output path (derived from MD5 of source audio)
    std::string output_audio_file;
    // When true, synthesis writes 24-bit PCM; otherwise 32-bit float.
    bool output_24bit_pcm = false;

    // Cached frame map (populated once in main, reused by all passes)
    std::vector<int64_t> frame_map;

    // --- Generate the canonical frame map ---
    // Centralizes the t_a accumulation logic to prevent floating-point drift
    // between modules. Returns int64_t sequence of t_a_rounded values.
    // t_s for frame m is implicitly m * R_s.
    // R_a_actual for frame m is frame_map[m] - frame_map[m-1] (caller derives).
    std::vector<int64_t> generate_frame_map() const {
        std::vector<int64_t> fmap;
        double t_a = -(double)N / 2.0;
        size_t t_s = 0;
        int idx = 0;
        // Forward-only cursor: O(segments + frames) instead of O(segments * frames)
        size_t seg = 0;

        while (t_s < target_total_frames) {
            // Advance cursor while the next segment starts at or before t_s
            while (timemap.size() >= 2 && seg + 2 < timemap.size() &&
                   t_s >= timemap[seg + 1].tgt_frame)
                ++seg;

            double alpha = 1.0;
            if (timemap.size() >= 2 && t_s > timemap.front().tgt_frame &&
                t_s >= timemap[seg].tgt_frame && t_s < timemap[seg + 1].tgt_frame) {
                double tgt_dur = static_cast<double>(timemap[seg + 1].tgt_frame - timemap[seg].tgt_frame);
                double src_dur = static_cast<double>(timemap[seg + 1].src_frame - timemap[seg].src_frame);
                alpha = tgt_dur / src_dur;
            }

            double R_a = R_s / alpha;
            if (idx > 0) t_a += R_a;
            int64_t t_a_rounded = static_cast<int64_t>(std::llround(t_a));
            fmap.push_back(t_a_rounded);

            t_s += R_s;
            idx++;
        }
        return fmap;
    }

    void init_fftw() {
        R_s = N / 4;
        window.resize(N);
        synth_window.resize(N);
        for (int n = 0; n < N; ++n) {
            window[n] = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)));
            synth_window[n] = window[n] / 1.5;
        }

        fft_in = fftw_alloc_real(N);
        fft_out = fftw_alloc_complex(N / 2 + 1);
        plan_fwd = fftw_plan_dft_r2c_1d(N, fft_in, fft_out, FFTW_ESTIMATE);
        ifft_in = fftw_alloc_complex(N / 2 + 1);
        ifft_out = fftw_alloc_real(N);
        plan_inv = fftw_plan_dft_c2r_1d(N, ifft_in, ifft_out, FFTW_ESTIMATE);

        phi_prev.assign(channels, std::vector<double>(N / 2 + 1, 0.0));
        theta_prev.assign(channels, std::vector<double>(N / 2 + 1, 0.0));
        overlap_add.assign(channels, std::vector<double>(N, 0.0));

        // 1/3-octave bin-to-band lookup (centers at 1000 * 2^(n/3), 20 Hz .. Nyquist)
        const int K = N / 2 + 1;
        bin_to_band.assign(K, 0);
        std::vector<double> centers;
        int n_min = static_cast<int>(std::ceil(3.0 * std::log2(20.0 / 1000.0)));
        int n_max = static_cast<int>(std::floor(3.0 * std::log2(nyquist / 1000.0)));
        for (int n = n_min; n <= n_max; ++n)
            centers.push_back(1000.0 * std::pow(2.0, n / 3.0));
        if (centers.empty()) centers.push_back(1000.0);
        num_bands = static_cast<int>(centers.size());
        for (int k = 0; k < K; ++k) {
            double hz = k * bin_hz_width;
            if (hz <= centers.front()) { bin_to_band[k] = 0; continue; }
            if (hz >= centers.back())  { bin_to_band[k] = num_bands - 1; continue; }
            double log_hz = std::log2(hz);
            int best = 0;
            double best_dist = std::abs(log_hz - std::log2(centers[0]));
            for (int b = 1; b < num_bands; ++b) {
                double d = std::abs(log_hz - std::log2(centers[b]));
                if (d < best_dist) { best_dist = d; best = b; }
            }
            bin_to_band[k] = best;
        }
    }

    void reset_phase_state() {
        for (int ch = 0; ch < channels; ++ch) {
            std::fill(phi_prev[ch].begin(), phi_prev[ch].end(), 0.0);
            std::fill(theta_prev[ch].begin(), theta_prev[ch].end(), 0.0);
            std::fill(overlap_add[ch].begin(), overlap_add[ch].end(), 0.0);
        }
    }

    // Shared phase vocoder core for one channel/frame.
    // M, phi, theta must be pre-allocated to K = N/2+1.
    // peaks must be pre-reserved (e.g. N/8); cleared on each call.
    // frame_buf is the interleaved read buffer; ch_stride is the channel count.
    // atten_row points to num_bands attenuation factors; nullptr means identity.
    // On return: M[k] holds magnitude, theta[k] holds synthesised phase, and
    //            ifft_in has been populated with the attenuated synthesis spectrum
    //            ready for fftw_execute(plan_inv).
    // phi_prev[ch] and theta_prev[ch] are updated in place.
    void phase_vocoder_frame(int ch, int ch_stride, int64_t R_a_actual, int frame_idx,
                              const float* frame_buf,
                              std::vector<double>& M, std::vector<double>& phi,
                              std::vector<double>& theta, std::vector<int>& peaks,
                              const double* atten_row) {
        const int K = N / 2 + 1;

        for (int n = 0; n < N; ++n)
            fft_in[n] = frame_buf[n * ch_stride + ch] * window[n];
        fftw_execute(plan_fwd);

        for (int k = 0; k < K; ++k) {
            M[k]   = std::hypot(fft_out[k][0], fft_out[k][1]);
            phi[k] = std::atan2(fft_out[k][1], fft_out[k][0]);
        }

        if (frame_idx == 0) {
            for (int k = 0; k < K; ++k) theta[k] = phi[k];
        } else {
            peaks.clear();
            for (int k = 1; k < N / 2; ++k)
                if (M[k] > M[k - 1] && M[k] > M[k + 1]) peaks.push_back(k);
            if (peaks.empty()) peaks.push_back(N / 4);

            for (int p : peaks) {
                double omega_p = 2.0 * M_PI * p / N;
                theta[p] = theta_prev[ch][p] +
                           (omega_p + princarg(phi[p] - phi_prev[ch][p] - omega_p * R_a_actual) / R_a_actual) * R_s;
            }
            size_t peak_idx = 0;
            for (int k = 0; k < K; ++k) {
                if (peak_idx < peaks.size() - 1 &&
                    std::abs(k - peaks[peak_idx + 1]) < std::abs(k - peaks[peak_idx]))
                    ++peak_idx;
                int p = peaks[peak_idx];
                if (k != p) theta[k] = theta[p] + phi[k] - phi[p];
            }
        }

        for (int k = 0; k < K; ++k) {
            phi_prev[ch][k]   = phi[k];
            theta_prev[ch][k] = theta[k];
        }

        // Synthesis: populate ifft_in with optional per-band attenuation
        if (atten_row) {
            for (int k = 0; k < K; ++k) {
                double scaled = M[k] * atten_row[bin_to_band[k]];
                ifft_in[k][0] = scaled * std::cos(theta[k]);
                ifft_in[k][1] = scaled * std::sin(theta[k]);
            }
        } else {
            for (int k = 0; k < K; ++k) {
                ifft_in[k][0] = M[k] * std::cos(theta[k]);
                ifft_in[k][1] = M[k] * std::sin(theta[k]);
            }
        }
    }

    void cleanup() {
        fftw_destroy_plan(plan_fwd);
        fftw_destroy_plan(plan_inv);
        fftw_free(fft_in);
        fftw_free(fft_out);
        fftw_free(ifft_in);
        fftw_free(ifft_out);
        if (fftw_threads_inited) fftw_cleanup_threads();
        if (src_snd) sf_close(src_snd);
    }
};
