#include "synthesis.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

void Synthesis::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 4] Executing HPSS Synthesis...\n";

    int N = stft.N;
    int R_s = stft.R_s;
    int channels = stft.channels;
    const auto& fm = stft.frame_map;
    int num_frames = static_cast<int>(fm.size());

    SF_INFO tgt_info = stft.src_info;
    tgt_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* harmonic_snd = sf_open(stft.harmonic_audio_file.c_str(), SFM_WRITE, &tgt_info);
    SNDFILE* perc_snd     = sf_open(stft.perc_audio_file.c_str(),     SFM_WRITE, &tgt_info);
    std::cout << "  -> Harmonic    : " << stft.harmonic_audio_file << "\n";
    std::cout << "  -> Percussive  : " << stft.perc_audio_file     << "\n";

    stft.reset_phase_state();

    // Independent OLA buffers for harmonic and percussive streams
    std::vector<std::vector<double>> ola_harmonic(channels, std::vector<double>(N, 0.0));
    std::vector<std::vector<double>> ola_perc    (channels, std::vector<double>(N, 0.0));

    int frames_to_skip = N / 2;
    double total_input_energy  = 0.0;
    double total_output_energy = 0.0;

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

            // Per-bin chain: EQ -> Dynamics -> Routing Matrix
            std::vector<double> M_harmonic_arr(N / 2 + 1), M_perc_arr(N / 2 + 1);
            for (int k = 0; k <= N / 2; ++k) {
                stft.phi_prev[ch][k]   = phi[k];
                stft.theta_prev[ch][k] = theta[k];

                const double M_raw = M[k];

                // --- EQ: DC Block always; LR4 HPF only if hpf_hz > 0 ---
                if (k == 0) {
                    M[k] = 0.0;
                } else if (stft.hpf_hz > 0.0) {
                    double f  = k * stft.bin_hz_width;
                    double r8 = std::pow(f / stft.hpf_hz, 8.0);
                    M[k] *= r8 / (1.0 + r8);
                }

                // --- Routing Matrix ---
                double mh = M[k] * stft.M_h_mask[ch][frame_idx][k];
                double mp = M[k] * stft.M_p_mask[ch][frame_idx][k];
                M_harmonic_arr[k] = mh;
                M_perc_arr[k]     = mp;

                // Energy meter: combined output vs raw input
                const double fold = (k == 0 || k == N / 2) ? 1.0 : 2.0;
                total_input_energy  += fold * M_raw * M_raw;
                total_output_energy += fold * (mh + mp) * (mh + mp);
            }

            // --- Step A: IFFT harmonic -> ola_harmonic ---
            for (int k = 0; k <= N / 2; ++k) {
                stft.ifft_in[k][0] = M_harmonic_arr[k] * std::cos(theta[k]);
                stft.ifft_in[k][1] = M_harmonic_arr[k] * std::sin(theta[k]);
            }
            fftw_execute(stft.plan_inv);
            for (int n = 0; n < N; ++n)
                ola_harmonic[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];

            // --- Step B: IFFT percussive -> ola_perc ---
            for (int k = 0; k <= N / 2; ++k) {
                stft.ifft_in[k][0] = M_perc_arr[k] * std::cos(theta[k]);
                stft.ifft_in[k][1] = M_perc_arr[k] * std::sin(theta[k]);
            }
            fftw_execute(stft.plan_inv);
            for (int n = 0; n < N; ++n)
                ola_perc[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];
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
            for (int n = 0; n < write_len; ++n)
                for (int ch = 0; ch < channels; ++ch)
                    write_buf[n * channels + ch] = static_cast<float>(ola_harmonic[ch][write_offset + n]);
            sf_writef_float(harmonic_snd, write_buf.data(), write_len);
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
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < remaining; ++n)
                write_buf[n * channels + ch] = static_cast<float>(ola_harmonic[ch][n]);
        sf_writef_float(harmonic_snd, write_buf.data(), remaining);
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < remaining; ++n)
                write_buf[n * channels + ch] = static_cast<float>(ola_perc[ch][n]);
        sf_writef_float(perc_snd, write_buf.data(), remaining);
    }

    sf_close(harmonic_snd);
    sf_close(perc_snd);
    std::cout << "[Success] Final Master Written.\n";

    // ========================================================================
    // Suggested Makeup Gain (Meter-Only — Not Applied)
    // ========================================================================
    double total_makeup_db = 0.0;
    if (total_input_energy > 1e-12)
        total_makeup_db = -10.0 * std::log10(total_output_energy / total_input_energy);

    std::cout << "\n[Loudness Match (Suggested — Not Applied)]\n";
    std::cout << "  -> Suggested Makeup Gain : " << (total_makeup_db > 0 ? "+" : "")
              << std::fixed << std::setprecision(2) << total_makeup_db << " dB\n";
}
