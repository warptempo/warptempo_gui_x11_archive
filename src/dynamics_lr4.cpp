#include "dynamics_lr4.h"
#include <iostream>
#include <algorithm>

void DynamicsLR4::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 3] Analyzing LR4 Multiband Dynamics (Energy-Weighted)...\n";

    int N = stft.N;
    int R_s = stft.R_s;
    int channels = stft.channels;
    double bin_hz_width = stft.bin_hz_width;
    int total_analysis_frames = stft.total_analysis_frames;
    const auto& dp = stft.dyn_params;
    const auto& fm = stft.frame_map;

    // --- LR4 Weight Initialization ---
    stft.W_L.assign(N / 2 + 1, 0.0);
    stft.W_M.assign(N / 2 + 1, 0.0);
    stft.W_H.assign(N / 2 + 1, 0.0);
    for (int k = 1; k <= N / 2; ++k) {
        double hz = k * bin_hz_width;
        stft.W_L[k] = 1.0 / (1.0 + std::pow(hz / dp.xover_low, 8.0));
        stft.W_H[k] = 1.0 / (1.0 + std::pow(dp.xover_high / hz, 8.0));
        stft.W_M[k] = 1.0 - stft.W_L[k] - stft.W_H[k];
        if (stft.W_M[k] < 0.0) stft.W_M[k] = 0.0;
    }

    // --- Trajectory Arrays ---
    stft.S_traj_L.assign(total_analysis_frames, 1.0);
    stft.S_traj_M.assign(total_analysis_frames, 1.0);
    stft.S_traj_H.assign(total_analysis_frames, 1.0);

    // Task 1: EQ Compensation Measurement
    stft.global_eq_weighted_sum = 0.0;
    stft.global_src_energy_sum = 0.0;

    std::vector<float> read_buf(N * channels, 0.0f);

    auto evaluate_zone = [](double db, double thresh, double knee, double max_depth,
                            double raw_scalar, double max_atten) {
        double lower_bound = thresh - (knee / 2.0);
        double upper_bound = thresh + (knee / 2.0);
        double active_depth = 0.0;

        if (db <= lower_bound)
            active_depth = 0.0;
        else if (db >= upper_bound)
            active_depth = max_depth;
        else {
            double factor = (db - lower_bound) / knee;
            active_depth = max_depth * (factor * factor);
        }

        double c_max_clamp = std::pow(10.0, max_atten / 20.0);
        double s_depth = std::pow(raw_scalar, active_depth);
        return std::max(c_max_clamp, s_depth);
    };

    for (int f_idx = 0; f_idx < total_analysis_frames; ++f_idx) {
        int64_t t_a_rounded = fm[f_idx];
        size_t t_s = static_cast<size_t>(f_idx) * R_s;

        std::vector<double> M_src(N / 2 + 1, 0.0), M_tgt(N / 2 + 1, 0.0);

        // Source FFT
        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
            sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
            sf_readf_float(stft.src_snd, read_buf.data(), N);
        }
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n) stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);
            for (int k = 0; k <= N / 2; ++k)
                M_src[k] += (stft.fft_out[k][0] * stft.fft_out[k][0] +
                             stft.fft_out[k][1] * stft.fft_out[k][1]) / channels;
        }
        for (int k = 0; k <= N / 2; ++k) M_src[k] = std::sqrt(M_src[k]);

        // Task 1: EQ Compensation
        for (int k = 1; k <= N / 2; ++k) {
            double E_k = M_src[k] * M_src[k];
            double m_val = stft.multiplier_array.empty() ? 1.0 : std::max(1e-9, stft.multiplier_array[k]);
            stft.global_eq_weighted_sum += E_k * 20.0 * std::log10(m_val);
            stft.global_src_energy_sum += E_k;
        }

        // Target FFT (from virtual buffer)
        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_s + N <= stft.virtual_tgt_buf.size() / channels) {
            std::copy(stft.virtual_tgt_buf.begin() + t_s * channels,
                      stft.virtual_tgt_buf.begin() + (t_s + N) * channels,
                      read_buf.begin());
        }
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n) stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);
            for (int k = 0; k <= N / 2; ++k)
                M_tgt[k] += (stft.fft_out[k][0] * stft.fft_out[k][0] +
                             stft.fft_out[k][1] * stft.fft_out[k][1]) / channels;
        }
        for (int k = 0; k <= N / 2; ++k) M_tgt[k] = std::sqrt(M_tgt[k]);

        // --- Energy-Weighted Macro-Scalars ---
        double den_L = 0.0, den_M = 0.0, den_H = 0.0;
        double num_L = 0.0, num_M = 0.0, num_H = 0.0;

        for (int k = 1; k <= N / 2; ++k) {
            double eq_scalar = stft.multiplier_array.empty() ? 1.0 : stft.multiplier_array[k];
            double M_tgt_eq = M_tgt[k] * eq_scalar;
            double s_raw_k = (M_tgt_eq > M_src[k]) ? (M_src[k] / (M_tgt_eq + 1e-12)) : 1.0;

            double mag_sq = M_src[k] * M_src[k];

            den_L += mag_sq * stft.W_L[k];
            num_L += s_raw_k * mag_sq * stft.W_L[k];

            den_M += mag_sq * stft.W_M[k];
            num_M += s_raw_k * mag_sq * stft.W_M[k];

            den_H += mag_sq * stft.W_H[k];
            num_H += s_raw_k * mag_sq * stft.W_H[k];
        }

        double S_raw_L = (den_L > 1e-20) ? (num_L / den_L) : 1.0;
        double S_raw_M = (den_M > 1e-20) ? (num_M / den_M) : 1.0;
        double S_raw_H = (den_H > 1e-20) ? (num_H / den_H) : 1.0;

        // --- Soft Knee & Range Clamp ---
        double norm_factor = static_cast<double>(N) / 2.0;
        double db_L = 20.0 * std::log10(std::sqrt(den_L) / norm_factor + 1e-9);
        double db_M = 20.0 * std::log10(std::sqrt(den_M) / norm_factor + 1e-9);
        double db_H = 20.0 * std::log10(std::sqrt(den_H) / norm_factor + 1e-9);

        stft.S_traj_L[f_idx] = evaluate_zone(db_L, dp.thresh_low, dp.knee_low, dp.depth_low, S_raw_L, dp.atten_low);
        stft.S_traj_M[f_idx] = evaluate_zone(db_M, dp.thresh_mid, dp.knee_mid, dp.depth_mid, S_raw_M, dp.atten_mid);
        stft.S_traj_H[f_idx] = evaluate_zone(db_H, dp.thresh_high, dp.knee_high, dp.depth_high, S_raw_H, dp.atten_high);
    }

    // --- Temporal Smoothing ---
    std::cout << "          -> Applying Asymmetrical IIR (Fast Backward, Slow Forward)...\n";

    double c_back_L = 1.0 - std::exp(-(double)R_s / ((dp.tau_B_low / 1000.0) * stft.src_info.samplerate));
    double c_back_M = 1.0 - std::exp(-(double)R_s / ((dp.tau_B_mid / 1000.0) * stft.src_info.samplerate));
    double c_back_H = 1.0 - std::exp(-(double)R_s / ((dp.tau_B_high / 1000.0) * stft.src_info.samplerate));

    double c_fwd_L = 1.0 - std::exp(-(double)R_s / ((dp.tau_F_low / 1000.0) * stft.src_info.samplerate));
    double c_fwd_M = 1.0 - std::exp(-(double)R_s / ((dp.tau_F_mid / 1000.0) * stft.src_info.samplerate));
    double c_fwd_H = 1.0 - std::exp(-(double)R_s / ((dp.tau_F_high / 1000.0) * stft.src_info.samplerate));

    // Pass A: Pre-Echo Excavator (Backward)
    for (int m = total_analysis_frames - 2; m >= 0; --m) {
        stft.S_traj_L[m] = c_back_L * stft.S_traj_L[m] + (1.0 - c_back_L) * stft.S_traj_L[m + 1];
        stft.S_traj_M[m] = c_back_M * stft.S_traj_M[m] + (1.0 - c_back_M) * stft.S_traj_M[m + 1];
        stft.S_traj_H[m] = c_back_H * stft.S_traj_H[m] + (1.0 - c_back_H) * stft.S_traj_H[m + 1];
    }

    // Pass B: Post-Echo Release (Forward)
    for (int m = 1; m < total_analysis_frames; ++m) {
        stft.S_traj_L[m] = c_fwd_L * stft.S_traj_L[m] + (1.0 - c_fwd_L) * stft.S_traj_L[m - 1];
        stft.S_traj_M[m] = c_fwd_M * stft.S_traj_M[m] + (1.0 - c_fwd_M) * stft.S_traj_M[m - 1];
        stft.S_traj_H[m] = c_fwd_H * stft.S_traj_H[m] + (1.0 - c_fwd_H) * stft.S_traj_H[m - 1];
    }

    // Free virtual buffer now that dynamics analysis is complete
    stft.virtual_tgt_buf.clear();
    stft.virtual_tgt_buf.shrink_to_fit();
}
