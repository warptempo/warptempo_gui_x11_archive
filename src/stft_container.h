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

// --- DSP Helpers ---
inline double princarg(double phase) {
    return phase - 2.0 * M_PI * std::floor((phase + M_PI) / (2.0 * M_PI));
}

// get_alpha() returns tgt_dur / src_dur.
// Less than 1.0 means speedup (output shorter than source).
// Greater than 1.0 means slowdown (output longer than source).
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

// --- Central Pipeline Container ---
// Minimum system RAM: 4 GB (array footprint ~1.64 GB + OS + FFTW planning)
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

    // Phase vocoder accumulators
    std::vector<std::vector<double>> phi_prev;
    std::vector<std::vector<double>> theta_prev;
    std::vector<std::vector<double>> overlap_add;

    // Virtual target buffer (Pass 1 output)
    std::vector<float> virtual_tgt_buf;

    // HPSS Parameters
    double beta    = 2.0; // Mask exponent (Wiener-filter; higher = sharper separation)
    int    L_h     = 31;  // 63-frame horizontal context (slow-moving background sponge)
    int    L_p_max = 7;   // Max vertical filter clamp — do not increase beyond 7

    // HPSS Mask Arrays (flat): index as ch * M_total * K + m * K + k
    // where M_total = frame_map.size(), K = N/2+1
    std::vector<double> M_h_mask; // Background (Harmonic/Horizontal)
    std::vector<double> M_p_mask; // Foreground (Percussive/Vertical)

    // HPSS enable flag
    bool hpss_enabled = true;

    // YIN Extraction (Pass 5) parameters
    bool   yin_enabled   = true;
    double yin_f0_min    = 500.0;
    double yin_f0_max    = 1200.0;
    double yin_confidence = 0.65;
    double yin_alpha     = 1.0;
    double yin_sigma     = 1.5;
    double yin_threshold = 0.35;
    bool   yin_diag      = false;
    std::string yin_diag_pitch_file;
    std::string yin_diag_correction_file;

    // Output paths (derived from MD5 of source audio)
    std::string perc_audio_file;
    std::string harmonic_audio_file;
    std::string tgt_audio_file;     // base path for eq_analysis output naming

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
    // On return: M[k] holds magnitude, theta[k] holds synthesised phase.
    // phi_prev[ch] and theta_prev[ch] are updated in place.
    void phase_vocoder_frame(int ch, int ch_stride, int64_t R_a_actual, int frame_idx,
                              const float* frame_buf,
                              std::vector<double>& M, std::vector<double>& phi,
                              std::vector<double>& theta, std::vector<int>& peaks) {
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
    }

    void cleanup() {
        fftw_destroy_plan(plan_fwd);
        fftw_destroy_plan(plan_inv);
        fftw_free(fft_in);
        fftw_free(fft_out);
        fftw_free(ifft_in);
        fftw_free(ifft_out);
        if (src_snd) sf_close(src_snd);
    }
};
