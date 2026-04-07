#include "phase_vocoder.h"
#include <iostream>
#include <algorithm>

void PhaseVocoder::process(AudioSTFT& stft) {
    std::cout << "[Pass 1] Executing Dry Pass (N=" << stft.N << ") -> Virtual Memory Buffer...\n";

    int N = stft.N;
    int R_s = stft.R_s;
    int channels = stft.channels;
    const auto& fm = stft.frame_map;
    int num_frames = static_cast<int>(fm.size());

    stft.virtual_tgt_buf.clear();
    stft.virtual_tgt_buf.reserve(stft.target_total_frames * channels);
    stft.reset_phase_state();

    std::vector<float> read_buf(N * channels, 0.0f);
    int frames_to_skip = N / 2;

    for (int frame_idx = 0; frame_idx < num_frames; ++frame_idx) {
        int64_t t_a_rounded = fm[frame_idx];
        int64_t R_a_actual = (frame_idx > 0) ? (fm[frame_idx] - fm[frame_idx - 1]) : 0;

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
                M[k] = std::sqrt(stft.fft_out[k][0] * stft.fft_out[k][0] +
                                 stft.fft_out[k][1] * stft.fft_out[k][1]);
                phi[k] = std::atan2(stft.fft_out[k][1], stft.fft_out[k][0]);
            }

            if (frame_idx == 0) {
                for (int k = 0; k <= N / 2; ++k) theta[k] = phi[k];
            } else {
                std::vector<int> peaks;
                for (int k = 1; k < N / 2; ++k) {
                    if (M[k] > M[k - 1] && M[k] > M[k + 1]) peaks.push_back(k);
                }
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

            for (int k = 0; k <= N / 2; ++k) {
                stft.phi_prev[ch][k] = phi[k];
                stft.theta_prev[ch][k] = theta[k];
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
        for (int n = write_offset; n < write_offset + write_len; ++n) {
            for (int ch = 0; ch < channels; ++ch)
                stft.virtual_tgt_buf.push_back(static_cast<float>(stft.overlap_add[ch][n]));
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) stft.overlap_add[ch][n] = stft.overlap_add[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n) stft.overlap_add[ch][n] = 0.0;
        }
    }

    stft.total_analysis_frames = num_frames;
}
