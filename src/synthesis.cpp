#include "synthesis.h"
#include <iostream>
#include <algorithm>
#include <cmath>

void Synthesis::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 4] Executing " << (stft.hpss_enabled ? "HPSS" : "Bypass") << " Synthesis...\n";

    const int N        = stft.N;
    const int R_s      = stft.R_s;
    const int channels = stft.channels;
    const int K        = N / 2 + 1;
    const auto& fm     = stft.frame_map;
    const int num_frames = static_cast<int>(fm.size());

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

    // Allocated once, reused across all frames and channels (Issue 2)
    std::vector<double> M(K), phi(K), theta(K);
    std::vector<int> peaks;
    peaks.reserve(N / 8);

    std::vector<std::vector<double>> ola_harmonic(channels, std::vector<double>(N, 0.0));
    std::vector<std::vector<double>> ola_perc    (channels, std::vector<double>(N, 0.0));

    int frames_to_skip = N / 2;

    std::vector<float> read_buf(N * channels, 0.0f);
    std::vector<float> write_buf(N * channels, 0.0f);

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

            // HPSS routing: M[k] → harmonic and percussive streams
            if (stft.hpss_enabled) {
                const size_t base = static_cast<size_t>(ch) * num_frames * K +
                                    static_cast<size_t>(frame_idx) * K;
                for (int k = 0; k < K; ++k) {
                    double mh = M[k] * stft.M_h_mask[base + k];
                    double mp = M[k] * stft.M_p_mask[base + k];
                    stft.ifft_in[k][0] = mh * std::cos(theta[k]);
                    stft.ifft_in[k][1] = mh * std::sin(theta[k]);
                    M[k] = mp;  // repurpose for percussive IFFT pass
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_harmonic[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];

                for (int k = 0; k < K; ++k) {
                    stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                    stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_perc[ch][n] += (stft.ifft_out[n] / N) * stft.synth_window[n];
            } else {
                for (int k = 0; k < K; ++k) {
                    stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                    stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
                }
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
                write_offset   = frames_to_skip;
                write_len     -= frames_to_skip;
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
    const int remaining = N - R_s;
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
