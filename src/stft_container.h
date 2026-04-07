#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <fftw3.h>
#include <sndfile.h>

// --- Configuration Constants ---
const double ATTACK_TOLERANCE_LU = 3.0;
const double RELEASE_TOLERANCE_LU = 8.0;
const double MIN_GAP_SEC = 0.5;
const double MIN_PHRASE_SEC = 5.0;
const size_t TOP_X_CHUNKS = 10;
const int CURVE_RESOLUTION = 500;

// --- Data Structures ---
struct TimeMapSegment {
    size_t src_frame;
    size_t tgt_frame;
};

struct AcousticBlock {
    size_t start_frame;
    size_t end_frame;
    double duration_sec;
};

struct Point {
    double x, y;
};

// --- DSP Helpers ---
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

// --- Dynamics Parameters ---
struct DynamicsParams {
    double thresh_low = -25.0, thresh_mid = -20.0, thresh_high = -25.0;
    double knee_low = 6.0, knee_mid = 6.0, knee_high = 6.0;
    double atten_low = -24.0, atten_mid = -24.0, atten_high = -24.0;
    double tau_B_low = 50.0, tau_B_mid = 30.0, tau_B_high = 20.0;
    double tau_F_low = 250.0, tau_F_mid = 150.0, tau_F_high = 80.0;
    double depth_low = 1.0, depth_mid = 1.0, depth_high = 1.0;
    double xover_low = 120.0, xover_high = 3500.0;
};

// --- Central Pipeline Container ---
// Minimum system RAM: 4 GB (array footprint ~1.64 GB + OS + FFTW planning)
struct AudioSTFT {
    // Source metadata
    SF_INFO src_info{};
    SNDFILE* src_snd = nullptr;
    int N = 3328;
    int R_s = 0;
    int channels = 0;
    double nyquist = 0.0;
    double bin_hz_width = 0.0;
    double low_threshold_hz = 50.0;
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

    // EQ matcher output
    std::vector<double> multiplier_array;
    std::vector<double> raw_delta_db;
    std::vector<Point> smoothed_curve;
    double global_eq_weighted_sum = 0.0;
    double global_src_energy_sum = 0.0;

    // LR4 Dynamics output
    std::vector<double> W_L, W_M, W_H;
    std::vector<double> S_traj_L, S_traj_M, S_traj_H;
    int total_analysis_frames = 0;
    DynamicsParams dyn_params;

    // HPSS Parameters
    double G_fg = 1.0;    // Foreground Gain (Unity default for transparent operation)
    double G_bg = 1.0;    // Background Gain (Unity default for transparent operation)
    double beta = 0.5;    // Mask exponent (0.5 = Wiener-filter exponent, sharpens transient/tonal split)
    int L_h = 31;         // 63-frame horizontal context (Background room sponge)
    int L_p_max = 7;      // Max vertical filter clamp

    // HPSS Mask Arrays: [channel][frame][bin]
    std::vector<std::vector<std::vector<double>>> M_h_mask;
    std::vector<std::vector<std::vector<double>>> M_p_mask;

    // Synthesis compensation
    double global_dyn_atten_sum = 0.0;
    size_t active_dyn_frames = 0;
    double global_hpss_atten_sum = 0.0;
    size_t active_hpss_frames = 0;

    // Output
    std::string tgt_audio_file;
    bool enable_makeup_gain = true;

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

        while (t_s < target_total_frames) {
            double alpha = get_alpha(t_s, timemap);
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
