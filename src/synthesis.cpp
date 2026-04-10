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

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n)
                stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);

            std::vector<double> M(N / 2 + 1, 0.0), phi(N / 2 + 1, 0.0), theta(N / 2 + 1, 0.0);
            for (int k = 0; k <= N / 2; ++k) {
                M[k]   = std::sqrt(stft.fft_out[k][0] * stft.fft_out[k][0] +
                                   stft.fft_out[k][1] * stft.fft_out[k][1]);
                phi[k] = std::atan2(stft.fft_out[k][1], stft.fft_out[k][0]);
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

            // Per-bin chain: EQ -> HPF -> Routing
            for (int k = 0; k <= N / 2; ++k) {
                stft.phi_prev[ch][k]   = phi[k];
                stft.theta_prev[ch][k] = theta[k];

                // --- EQ: DC Block always; LR4 HPF only if hpf_hz > 0 ---
                if (k == 0) {
                    M[k] = 0.0;
                } else if (stft.hpf_hz > 0.0) {
                    double f  = k * stft.bin_hz_width;
                    double r8 = std::pow(f / stft.hpf_hz, 8.0);
                    M[k] *= r8 / (1.0 + r8);
                }

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
                // Step A: IFFT harmonic (ifft_in already loaded above)
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_harmonic[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];

                // Step B: IFFT percussive (M[k] now holds mp)
                for (int k = 0; k <= N / 2; ++k) {
                    stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                    stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_perc[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];
            } else {
                // Bypass: single IFFT -> combined stream
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
