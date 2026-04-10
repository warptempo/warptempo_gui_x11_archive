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

    // EQ match is active only when the flag is set AND the curve was computed
    const bool eq_active = stft.eq_match_enabled && !stft.smoothed_curve.empty();
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
    // 5A — Constants and precomputation
    // =========================================================================
    constexpr double S_floor = 0.1;
    constexpr double S_cap   = 1.5;
    constexpr double c_low   = 0.4;
    constexpr double c_mid   = 0.3;
    constexpr double c_high  = 0.5;

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

    // Band assignment per bin (0=bypass, 1=low, 2=mid, 3=high)
    std::vector<int> band_assign(K, 0);
    // C_eq interpolated onto every bin (dB)
    std::vector<double> C_eq_per_bin(K, 0.0);

    if (eq_active) {
        for (int k = 0; k < K; ++k) {
            double w0 = stft.eq_W_Z0[k], w1 = stft.eq_W_Z1[k],
                   w2 = stft.eq_W_Z2[k], w3 = stft.eq_W_Z3[k];
            double best = w0; int band = 0;
            if (w1 > best) { best = w1; band = 1; }
            if (w2 > best) { best = w2; band = 2; }
            if (w3 > best) { band = 3; }
            band_assign[k] = band;
        }

        const auto& curve = stft.smoothed_curve;
        for (int k = 0; k < K; ++k) {
            double f_k = k * stft.bin_hz_width;
            if (f_k <= curve.front().x) {
                C_eq_per_bin[k] = curve.front().y;
            } else if (f_k >= curve.back().x) {
                C_eq_per_bin[k] = curve.back().y;
            } else {
                for (size_t i = 0; i + 1 < curve.size(); ++i) {
                    if (f_k >= curve[i].x && f_k <= curve[i + 1].x) {
                        double t = (f_k - curve[i].x) / (curve[i + 1].x - curve[i].x);
                        C_eq_per_bin[k] = curve[i].y + t * (curve[i + 1].y - curve[i].y);
                        break;
                    }
                }
            }
        }
    }

    // Release envelope state (per band)
    double D_prev_low  = 0.0;
    double D_prev_mid  = 0.0;
    double D_prev_high = 0.0;

    // Exponential decay coefficients
    double decay_low = 1.0, decay_mid = 1.0, decay_high = 1.0;
    if (eq_active) {
        double f_s = stft.src_info.samplerate;
        decay_low  = std::exp(-1.0 / (stft.eq_match_release_low  * f_s / (1000.0 * R_s)));
        decay_mid  = std::exp(-1.0 / (stft.eq_match_release_mid  * f_s / (1000.0 * R_s)));
        decay_high = std::exp(-1.0 / (stft.eq_match_release_high * f_s / (1000.0 * R_s)));
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
        // 5B — Per-frame serial block: compute D_low, D_mid, D_high
        // =====================================================================
        double D_low = 0.0, D_mid = 0.0, D_high = 0.0;

        if (eq_active) {
            // Step 1 — local stretch ratio at this synthesis frame
            double alpha = get_alpha(static_cast<size_t>(frame_idx) * R_s, stft.timemap);

            // Step 2 — S_b(α)
            double S_low, S_mid, S_high;
            if (alpha < 1.0 || (stft.alpha_ref - 1.0) < 1e-10) {
                S_low = S_mid = S_high = S_floor;
            } else {
                auto compute_S = [&](double c_b) -> double {
                    double num = (alpha - 1.0) * (stft.alpha_ref - 1.0 + c_b);
                    double den = (stft.alpha_ref - 1.0) * (alpha - 1.0 + c_b);
                    return std::min(S_cap, S_floor + (1.0 - S_floor) * num / den);
                };
                S_low  = compute_S(c_low);
                S_mid  = compute_S(c_mid);
                S_high = compute_S(c_high);
            }

            // Step 3 — per-band source energy: mono average over channels
            double E_low = 0.0, E_mid = 0.0, E_high = 0.0;
            for (int ch = 0; ch < channels; ++ch) {
                for (int n = 0; n < N; ++n)
                    stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
                fftw_execute(stft.plan_fwd);
                for (int k = 0; k < K; ++k) {
                    double mag2 = stft.fft_out[k][0] * stft.fft_out[k][0]
                                + stft.fft_out[k][1] * stft.fft_out[k][1];
                    E_low  += stft.eq_W_Z1[k] * mag2;
                    E_mid  += stft.eq_W_Z2[k] * mag2;
                    E_high += stft.eq_W_Z3[k] * mag2;
                }
            }
            E_low  /= channels;
            E_mid  /= channels;
            E_high /= channels;

            // Step 4 — convert to dBFS
            const double E_ref = N * 3.0 / 8.0;
            double L_low  = 10.0 * std::log10(E_low  / E_ref + 1e-12);
            double L_mid  = 10.0 * std::log10(E_mid  / E_ref + 1e-12);
            double L_high = 10.0 * std::log10(E_high / E_ref + 1e-12);

            // Step 5 — loudness gate with instant attack, trailing release
            double floor_db = stft.eq_match_floor_db;
            double peak_db  = stft.eq_match_peak_db;
            double range    = peak_db - floor_db;

            auto clamp01 = [](double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); };

            double D_raw_low  = clamp01((L_low  - floor_db) / range);
            double D_raw_mid  = clamp01((L_mid  - floor_db) / range);
            double D_raw_high = clamp01((L_high - floor_db) / range);

            double D_loud_low  = std::max(D_raw_low,  D_prev_low  * decay_low);
            double D_loud_mid  = std::max(D_raw_mid,  D_prev_mid  * decay_mid);
            double D_loud_high = std::max(D_raw_high, D_prev_high * decay_high);

            D_prev_low  = D_loud_low;
            D_prev_mid  = D_loud_mid;
            D_prev_high = D_loud_high;

            // Step 6 — final depth scalar
            D_low  = D_loud_low  * S_low;
            D_mid  = D_loud_mid  * S_mid;
            D_high = D_loud_high * S_high;
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
            // 5C — Per-bin gain chain: EQ match → HPF → HPSS routing
            // =====================================================================
            for (int k = 0; k < K; ++k) {
                stft.phi_prev[ch][k]   = phi[k];
                stft.theta_prev[ch][k] = theta[k];

                // Step 1 — EQ match gain
                if (eq_active) {
                    int band = band_assign[k];
                    double G_eq;
                    if (band == 0)      G_eq = 1.0;
                    else if (band == 1) G_eq = std::pow(10.0, C_eq_per_bin[k] * D_low  / 20.0);
                    else if (band == 2) G_eq = std::pow(10.0, C_eq_per_bin[k] * D_mid  / 20.0);
                    else                G_eq = std::pow(10.0, C_eq_per_bin[k] * D_high / 20.0);
                    M[k] *= G_eq;
                }

                // Step 2 — HPF
                M[k] *= H_hpf[k];

                // Step 3 — HPSS routing
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
                // Step A: IFFT harmonic
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_harmonic[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];

                // Step B: IFFT percussive (M[k] now holds mp)
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
