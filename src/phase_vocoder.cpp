#include "phase_vocoder.h"
#include <iostream>
#include <algorithm>

void PhaseVocoder::process(AudioSTFT& stft) {
    std::cout << "[Pass 1] Executing Dry Pass (N=" << stft.N << ") -> Virtual Memory Buffer...\n";

    const int N        = stft.N;
    const int R_s      = stft.R_s;
    const int channels = stft.channels;
    const int K        = N / 2 + 1;
    const auto& fm     = stft.frame_map;
    const int num_frames = static_cast<int>(fm.size());

    stft.virtual_tgt_buf.clear();
    stft.virtual_tgt_buf.reserve(stft.target_total_frames * channels);
    stft.reset_phase_state();

    std::vector<float> read_buf(N * channels, 0.0f);
    int frames_to_skip = N / 2;

    // Allocated once, reused across all frames and channels (Issue 2)
    std::vector<double> M(K), phi(K), theta(K);
    std::vector<int> peaks;
    peaks.reserve(N / 8);

    for (int frame_idx = 0; frame_idx < num_frames; ++frame_idx) {
        const int64_t t_a_rounded = fm[frame_idx];
        const int64_t R_a_actual  = (frame_idx > 0) ? (fm[frame_idx] - fm[frame_idx - 1]) : 0;

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
            sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
            sf_readf_float(stft.src_snd, read_buf.data(), N);
        }

        for (int ch = 0; ch < channels; ++ch) {
            stft.phase_vocoder_frame(ch, channels, R_a_actual, frame_idx,
                                     read_buf.data(), M, phi, theta, peaks);

            for (int k = 0; k < K; ++k) {
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
                write_offset   = frames_to_skip;
                write_len     -= frames_to_skip;
                frames_to_skip = 0;
            }
        }
        for (int n = write_offset; n < write_offset + write_len; ++n)
            for (int ch = 0; ch < channels; ++ch)
                stft.virtual_tgt_buf.push_back(static_cast<float>(stft.overlap_add[ch][n]));

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) stft.overlap_add[ch][n] = stft.overlap_add[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n)  stft.overlap_add[ch][n] = 0.0;
        }
    }
}
