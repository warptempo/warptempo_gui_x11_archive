#include "synthesis.h"
#include <iostream>
#include <algorithm>
#include <cmath>

void Synthesis::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 4] Executing " << (stft.hpss_enabled ? "HPSS" : "Bypass") << " Synthesis...\n";

    int N = stft.N;
    int R_s = stft.R_s;
    int channels = stft.channels;
    const auto& fm = stft.frame_map;
    int num_frames = static_cast<int>(fm.size());

    const bool eq_active = stft.eq_match_enabled
                        && !stft.smoothed_curve_speedup.empty()
                        && !stft.smoothed_curve_slowdown.empty();
    std::cout << "  -> EQ Match: " << (eq_active ? "active" : "bypassed") << "\n";

    SF_INFO tgt_info = stft.src_info;
    tgt_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* harmonic_snd = nullptr;
    SNDFILE* perc_snd     = sf_open(stft.perc_audio_file.c_str(), SFM_WRITE, &tgt_info);
    if (stft.hpss_enabled) {
        harmonic_snd = sf_open(stft.harmonic_audio_file.c_str(), SFM_WRITE, &tgt_info);
        std::cout << "  -> Harmonic    : " << stft.harmonic_audio_file << "\n";
        std::cout << "  -> Percussive  : " << stft.perc_audio_file << "\n";
    } else {
        std::cout << "  -> Combined    : " << stft.perc_audio_file << "\n";
    }

    stft.reset_phase_state();

    // =========================================================================
    // Precomputation
    // =========================================================================
    constexpr double c     = 0.35;
    constexpr double S_cap = 1.0;

    const int K = N / 2 + 1;

    // HPF weight per bin
    std::vector<double> H_hpf(K, 1.0);
    if (stft.hpf_hz > 0.0) {
        H_hpf[0] = 0.0;
        for (int k = 1; k < K; ++k) {
            double f  = k * stft.bin_hz_width;
            double r8 = std::pow(f / stft.hpf_hz, 8.0);
            H_hpf[k]  = r8 / (1.0 + r8);
        }
    }

    // Interpolate both smoothed curves onto every bin
    std::vector<double> C_eq_speedup(K, 0.0);
    std::vector<double> C_eq_slowdown(K, 0.0);

    if (eq_active) {
        auto interp_curve = [&](const std::vector<Point>& curve, std::vector<double>& out) {
            for (int k = 0; k < K; ++k) {
                double f_k = k * stft.bin_hz_width;
                if (f_k <= curve.front().x) {
                    out[k] = curve.front().y;
                } else if (f_k >= curve.back().x) {
                    out[k] = curve.back().y;
                } else {
                    for (size_t i = 0; i + 1 < curve.size(); ++i) {
                        if (f_k >= curve[i].x && f_k <= curve[i + 1].x) {
                            double t = (f_k - curve[i].x) / (curve[i + 1].x - curve[i].x);
                            out[k] = curve[i].y + t * (curve[i + 1].y - curve[i].y);
                            break;
                        }
                    }
                }
            }
        };
        interp_curve(stft.smoothed_curve_speedup,  C_eq_speedup);
        interp_curve(stft.smoothed_curve_slowdown, C_eq_slowdown);
    }

    // =========================================================================
    // Main frame loop
    // =========================================================================
    std::vector<std::vector<double>> ola_harmonic(channels, std::vector<double>(N, 0.0));
    std::vector<std::vector<double>> ola_perc    (channels, std::vector<double>(N, 0.0));

    int frames_to_skip = N / 2;

    std::vector<float> read_buf(N * channels, 0.0f);
    std::vector<float> write_buf(N * channels, 0.0f);

    for (int frame_idx = 0; frame_idx < num_frames; ++frame_idx) {
        int64_t t_a_rounded = fm[frame_idx];
        int64_t R_a_actual  = (frame_idx > 0) ? (fm[frame_idx] - fm[frame_idx - 1]) : 0;

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
            sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
            sf_readf_float(stft.src_snd, read_buf.data(), N);
        }

        // =====================================================================
        // Per-frame: compute single depth scalar S and select active curve
        // =====================================================================
        double S = 0.0;
        const double* C_eq_active = nullptr;

        if (eq_active) {
            double g_alpha = get_alpha(static_cast<size_t>(frame_idx) * R_s, stft.timemap);
            constexpr double eps = 1e-6;

            if (g_alpha < 1.0 - eps) {
                // Speedup: get_alpha() < 1 → distance from unity = 1 - g_alpha
                double d     = 1.0 - g_alpha;
                double d_ref = 1.0 - stft.alpha_ref_speedup;
                if (d_ref >= 0.001)
                    S = std::min(S_cap, d * (d_ref + c) / (d_ref * (d + c)));
                C_eq_active = C_eq_speedup.data();
            } else if (g_alpha > 1.0 + eps) {
                // Slowdown: get_alpha() > 1 → distance from unity = g_alpha - 1
                double d     = g_alpha - 1.0;
                double d_ref = stft.alpha_ref_slowdown - 1.0;
                if (d_ref >= 0.001)
                    S = std::min(S_cap, d * (d_ref + c) / (d_ref * (d + c)));
                C_eq_active = C_eq_slowdown.data();
            }
            // Unity: S stays 0, C_eq_active stays nullptr → G_eq = 1.0
        }

        // =====================================================================
        // Per-channel loop
        // =====================================================================
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n)
                stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);

            std::vector<double> M(K, 0.0), phi(K, 0.0), theta(K, 0.0);
            for (int k = 0; k < K; ++k) {
                M[k]   = std::sqrt(stft.fft_out[k][0] * stft.fft_out[k][0] +
                                   stft.fft_out[k][1] * stft.fft_out[k][1]);
                phi[k] = std::atan2(stft.fft_out[k][1], stft.fft_out[k][0]);
            }

            // Phase vocoder
            if (frame_idx == 0) {
                for (int k = 0; k < K; ++k) theta[k] = phi[k];
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
                for (int k = 0; k < K; ++k) {
                    if (peak_idx < peaks.size() - 1 &&
                        std::abs(k - peaks[peak_idx + 1]) < std::abs(k - peaks[peak_idx]))
                        peak_idx++;
                    int p = peaks[peak_idx];
                    if (k != p) theta[k] = theta[p] + phi[k] - phi[p];
                }
            }

            // =====================================================================
            // Per-bin gain chain: EQ match → HPF → HPSS routing
            // =====================================================================
            for (int k = 0; k < K; ++k) {
                stft.phi_prev[ch][k]   = phi[k];
                stft.theta_prev[ch][k] = theta[k];

                // EQ match: single scalar S, no bands
                if (S > 0.0 && C_eq_active != nullptr)
                    M[k] *= std::pow(10.0, C_eq_active[k] * S / 20.0);

                // HPF
                M[k] *= H_hpf[k];

                // HPSS routing
                if (stft.hpss_enabled) {
                    double mh = M[k] * stft.M_h_mask[ch][frame_idx][k];
                    double mp = M[k] * stft.M_p_mask[ch][frame_idx][k];
                    stft.ifft_in[k][0] = mh * std::cos(theta[k]);
                    stft.ifft_in[k][1] = mh * std::sin(theta[k]);
                    M[k] = mp;  // repurpose for percussive IFFT pass
                } else {
                    stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                    stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
                }
            }

            if (stft.hpss_enabled) {
                // IFFT harmonic
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_harmonic[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];

                // IFFT percussive (M[k] now holds mp)
                for (int k = 0; k < K; ++k) {
                    stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                    stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_perc[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];
            } else {
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_perc[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];
            }
        }

        // Write accumulated samples (respecting initial N/2-frame skip)
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
            if (stft.hpss_enabled) {
                for (int n = 0; n < write_len; ++n)
                    for (int ch = 0; ch < channels; ++ch)
                        write_buf[n * channels + ch] = static_cast<float>(ola_harmonic[ch][write_offset + n]);
                sf_writef_float(harmonic_snd, write_buf.data(), write_len);
            }
            for (int n = 0; n < write_len; ++n)
                for (int ch = 0; ch < channels; ++ch)
                    write_buf[n * channels + ch] = static_cast<float>(ola_perc[ch][write_offset + n]);
            sf_writef_float(perc_snd, write_buf.data(), write_len);
        }

        // Shift OLA buffers
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) {
                ola_harmonic[ch][n] = ola_harmonic[ch][n + R_s];
                ola_perc    [ch][n] = ola_perc    [ch][n + R_s];
            }
            for (int n = N - R_s; n < N; ++n) {
                ola_harmonic[ch][n] = 0.0;
                ola_perc    [ch][n] = 0.0;
            }
        }
    }

    // Flush remaining overlap
    int remaining = N - R_s;
    if (remaining > 0) {
        if (stft.hpss_enabled) {
            for (int ch = 0; ch < channels; ++ch)
                for (int n = 0; n < remaining; ++n)
                    write_buf[n * channels + ch] = static_cast<float>(ola_harmonic[ch][n]);
            sf_writef_float(harmonic_snd, write_buf.data(), remaining);
        }
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < remaining; ++n)
                write_buf[n * channels + ch] = static_cast<float>(ola_perc[ch][n]);
        sf_writef_float(perc_snd, write_buf.data(), remaining);
    }

    if (harmonic_snd) sf_close(harmonic_snd);
    sf_close(perc_snd);
    std::cout << "[Success] Final Master Written.\n";
}
