#include "synthesis.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cstdio>

void Synthesis::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 4] Executing Final Synthesis with EQ, LR4 Dynamics & HPSS...\n";

    int N = stft.N;
    int R_s = stft.R_s;
    int channels = stft.channels;
    const auto& fm = stft.frame_map;
    int num_frames = static_cast<int>(fm.size());

    SF_INFO tgt_info = stft.src_info;
    tgt_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* tgt_snd = sf_open(stft.tgt_audio_file.c_str(), SFM_WRITE, &tgt_info);

    stft.reset_phase_state();

    int frames_to_skip = N / 2;

    std::vector<float> read_buf(N * channels, 0.0f);
    std::vector<float> write_buf(N * channels, 0.0f);

    stft.global_dyn_atten_sum = 0.0;
    stft.active_dyn_frames = 0;

    for (int frame_idx = 0; frame_idx < num_frames; ++frame_idx) {
        int64_t t_a_rounded = fm[frame_idx];
        int64_t R_a_actual = (frame_idx > 0) ? (fm[frame_idx] - fm[frame_idx - 1]) : 0;

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
            sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
            sf_readf_float(stft.src_snd, read_buf.data(), N);
        }

        // Dynamics compensation gate
        bool is_active_dyn = !stft.S_traj_L.empty() &&
                             (stft.S_traj_L[frame_idx] < 0.94 ||
                              stft.S_traj_M[frame_idx] < 0.94 ||
                              stft.S_traj_H[frame_idx] < 0.94);
        double frame_dyn_weighted_sum = 0.0;
        double frame_energy_sum = 0.0;
        double frame_hpss_weighted_sum = 0.0;
        double frame_energy_sum_hpss = 0.0;

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n)
                stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);

            std::vector<double> M(N / 2 + 1, 0.0), phi(N / 2 + 1, 0.0), theta(N / 2 + 1, 0.0);
            for (int k = 0; k <= N / 2; ++k) {
                M[k] = std::sqrt(stft.fft_out[k][0] * stft.fft_out[k][0] +
                                 stft.fft_out[k][1] * stft.fft_out[k][1]);
                phi[k] = std::atan2(stft.fft_out[k][1], stft.fft_out[k][0]);
            }

            // Dynamics compensation measurement
            if (is_active_dyn) {
                for (int k = 1; k <= N / 2; ++k) {
                    double E_k = M[k] * M[k];
                    double dyn_scalar = (stft.S_traj_L[frame_idx] * stft.W_L[k]) +
                                        (stft.S_traj_M[frame_idx] * stft.W_M[k]) +
                                        (stft.S_traj_H[frame_idx] * stft.W_H[k]);
                    double s_bin = std::max(1e-9, dyn_scalar);
                    frame_dyn_weighted_sum += E_k * 20.0 * std::log10(s_bin);
                    frame_energy_sum += E_k;
                }
            }

            // Phase vocoder
            if (frame_idx == 0) {
                for (int k = 0; k <= N / 2; ++k) theta[k] = phi[k];
            } else {
                std::vector<int> peaks;
                for (int k = 1; k < N / 2; ++k)
                    if (M[k] > M[k - 1] && M[k] > M[k + 1]) peaks.push_back(k);
                if (peaks.empty()) peaks.push_back(N / 4);

                for (int p : peaks) {
                    double omega_p = 2.0 * M_PI * p / N;
                    theta[p] = stft.theta_prev[ch][p] +
                               (omega_p + princarg(phi[p] - stft.phi_prev[ch][p] - omega_p * R_a_actual) / R_a_actual) * R_s;
                }
                size_t peak_idx = 0;
                for (int k = 0; k <= N / 2; ++k) {
                    if (peak_idx < peaks.size() - 1 &&
                        std::abs(k - peaks[peak_idx + 1]) < std::abs(k - peaks[peak_idx]))
                        peak_idx++;
                    int p = peaks[peak_idx];
                    if (k != p) theta[k] = theta[p] + phi[k] - phi[p];
                }
            }

            // Apply EQ + Dynamics + HPSS, then polar->complex via direct trig (NOT std::polar)
            for (int k = 0; k <= N / 2; ++k) {
                stft.phi_prev[ch][k] = phi[k];
                stft.theta_prev[ch][k] = theta[k];

                if (!stft.multiplier_array.empty())
                    M[k] *= stft.multiplier_array[k];

                if (!stft.S_traj_L.empty() && !stft.W_L.empty()) {
                    double dyn_scalar = (stft.S_traj_L[frame_idx] * stft.W_L[k]) +
                                        (stft.S_traj_M[frame_idx] * stft.W_M[k]) +
                                        (stft.S_traj_H[frame_idx] * stft.W_H[k]);
                    M[k] *= dyn_scalar;
                }

                // HPSS Foreground/Background mask application with energy tracking
                if (!stft.M_h_mask.empty() && !stft.M_p_mask.empty()) {
                    double hpss_scalar = (stft.G_bg * stft.M_h_mask[ch][frame_idx][k]) +
                                         (stft.G_fg * stft.M_p_mask[ch][frame_idx][k]);
                    double E_pre_hpss = M[k] * M[k];
                    double s_bin_hpss = std::max(1e-9, hpss_scalar);
                    frame_hpss_weighted_sum += E_pre_hpss * 20.0 * std::log10(s_bin_hpss);
                    frame_energy_sum_hpss += E_pre_hpss;
                    M[k] *= hpss_scalar;
                }

                stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
            }
            fftw_execute(stft.plan_inv);

            for (int n = 0; n < N; ++n)
                stft.overlap_add[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];
        }

        int write_offset = 0, write_len = R_s;
        if (frames_to_skip > 0) {
            if (frames_to_skip >= write_len) {
                frames_to_skip -= write_len;
                write_len = 0;
            } else {
                write_offset = frames_to_skip;
                write_len -= frames_to_skip;
                frames_to_skip = 0;
            }
        }
        if (write_len > 0) {
            for (int n = 0; n < write_len; ++n) {
                for (int ch = 0; ch < channels; ++ch)
                    write_buf[n * channels + ch] = static_cast<float>(stft.overlap_add[ch][write_offset + n]);
            }
            sf_writef_float(tgt_snd, write_buf.data(), write_len);
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) stft.overlap_add[ch][n] = stft.overlap_add[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n) stft.overlap_add[ch][n] = 0.0;
        }

        if (is_active_dyn && frame_energy_sum > 1e-6) {
            stft.global_dyn_atten_sum += (frame_dyn_weighted_sum / frame_energy_sum);
            stft.active_dyn_frames++;
        }

        if (frame_energy_sum_hpss > 1e-6) {
            stft.global_hpss_atten_sum += (frame_hpss_weighted_sum / frame_energy_sum_hpss);
            stft.active_hpss_frames++;
        }
    }

    // Flush remaining overlap
    int remaining = N - R_s;
    if (remaining > 0) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < remaining; ++n)
                write_buf[n * channels + ch] = static_cast<float>(stft.overlap_add[ch][n]);
        }
        sf_writef_float(tgt_snd, write_buf.data(), remaining);
    }

    std::cout << "[Success] Final Master Written.\n";

    // ========================================================================
    // Unification: Compute Makeup Gain
    // ========================================================================
    double eq_attenuation = (stft.global_src_energy_sum > 1e-20)
                                ? (stft.global_eq_weighted_sum / stft.global_src_energy_sum)
                                : 0.0;
    double eq_makeup_db = -eq_attenuation;

    double dyn_attenuation = (stft.active_dyn_frames > 0)
                                 ? (stft.global_dyn_atten_sum / stft.active_dyn_frames)
                                 : 0.0;
    double dyn_makeup_db = -dyn_attenuation;

    double hpss_attenuation = (stft.active_hpss_frames > 0)
                                  ? (stft.global_hpss_atten_sum / stft.active_hpss_frames)
                                  : 0.0;
    double hpss_makeup_db = -hpss_attenuation;

    double total_makeup_db = eq_makeup_db + dyn_makeup_db + hpss_makeup_db;
    double makeup_scalar = std::pow(10.0, total_makeup_db / 20.0);

    if (!stft.enable_makeup_gain) {
        std::cout << "  -> Makeup Gain: BYPASSED (scalar forced to 1.0)\n";
        total_makeup_db = 0.0;
        makeup_scalar   = 1.0;
    }

    std::cout << "\n[Loudness Match (Energy-Weighted)]\n";
    std::cout << "  -> EQ Compensation       : " << (eq_makeup_db > 0 ? "+" : "")
              << std::fixed << std::setprecision(2) << eq_makeup_db << " dB\n";
    std::cout << "  -> Dynamics Compensation : " << (dyn_makeup_db > 0 ? "+" : "")
              << std::fixed << std::setprecision(2) << dyn_makeup_db << " dB\n";
    std::cout << "  -> HPSS Compensation     : " << (hpss_makeup_db > 0 ? "+" : "")
              << std::fixed << std::setprecision(2) << hpss_makeup_db << " dB\n";
    std::cout << "  -> Total Makeup          : " << (total_makeup_db > 0 ? "+" : "")
              << std::fixed << std::setprecision(2) << total_makeup_db << " dB\n\n";

    // ========================================================================
    // Pass 5: Bake Makeup Gain
    // ========================================================================
    std::cout << "[Pass 5] Baking Makeup Gain (" << std::fixed << std::setprecision(2)
              << total_makeup_db << " dB) into final WAV...\n";

    sf_close(tgt_snd);

    std::string temp_audio_file = stft.tgt_audio_file + ".tmp";
    std::rename(stft.tgt_audio_file.c_str(), temp_audio_file.c_str());

    SF_INFO pass5_info = stft.src_info;
    pass5_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* in_snd = sf_open(temp_audio_file.c_str(), SFM_READ, &pass5_info);
    SNDFILE* out_snd = sf_open(stft.tgt_audio_file.c_str(), SFM_WRITE, &pass5_info);

    std::vector<float> gain_buf(4096 * channels);
    sf_count_t read_frames;
    while ((read_frames = sf_readf_float(in_snd, gain_buf.data(), 4096)) > 0) {
        for (int i = 0; i < read_frames * channels; ++i) gain_buf[i] *= makeup_scalar;
        sf_writef_float(out_snd, gain_buf.data(), read_frames);
    }

    sf_close(in_snd);
    sf_close(out_snd);
    std::remove(temp_audio_file.c_str());

    std::cout << "[Success] Final Master Loudness Matched & Written.\n";
}
