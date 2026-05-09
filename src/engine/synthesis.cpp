#include "synthesis.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <iostream>
#include <vector>

void Synthesis::synthesize_full(
    AudioSTFT& stft,
    std::complex<float>* spectra_cache,
    std::function<void(const float*, size_t)> write_cb,
    bool show_progress,
    const char* pass_label) {
    const int N          = stft.N;
    const int R_s        = stft.R_s;
    const int channels   = stft.channels;
    const int K          = N / 2 + 1;
    const auto& fm       = stft.frame_map;
    const int num_frames = static_cast<int>(fm.size());

    stft.reset_phase_state();

    std::vector<double> M(K), phi(K), theta(K);
    std::vector<int> peaks;
    peaks.reserve(N / 8);

    int phase_reset_cursor = 0;

    std::vector<std::vector<double>> ola_out(channels, std::vector<double>(N, 0.0));

    std::vector<float> read_buf(N * channels, 0.0f);
    std::vector<float> write_buf(N * channels, 0.0f);

    // Start-trim: the first N/2 samples are OLA ramp-up. Mirrors the phase vocoder pass.
    int frames_to_skip = N / 2;

    // Progress reporting every ~1% of frames (or every 100 frames, whichever is rarer).
    int progress_stride = std::max(100, num_frames / 100);
    int last_pct = -1;

    for (int frame_idx = 0; frame_idx < num_frames; ++frame_idx) {
        const int64_t t_a_rounded = fm[frame_idx];
        const int64_t R_a_actual  = (frame_idx > 0) ? (fm[frame_idx] - fm[frame_idx - 1]) : 0;

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
            sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
            sf_readf_float(stft.src_snd, read_buf.data(), N);
        }

        const double* atten_row = stft.attenuation_map[frame_idx].data();

        for (int ch = 0; ch < channels; ++ch) {
            stft.phase_vocoder_frame(ch, channels, R_a_actual, frame_idx,
                                     read_buf.data(), M, phi, theta, peaks, atten_row);

            if (spectra_cache) {
                std::complex<float>* dst = spectra_cache +
                    (static_cast<size_t>(frame_idx) * channels + ch) * K;
                for (int k = 0; k < K; ++k) {
                    dst[k] = std::complex<float>(
                        static_cast<float>(stft.ifft_in[k][0]),
                        static_cast<float>(stft.ifft_in[k][1]));
                }
            }

            fftw_execute(stft.plan_inv);
            const double inv_N = 1.0 / N;
            for (int n = 0; n < N; ++n)
                ola_out[ch][n] += (stft.ifft_out[n] * inv_N) * stft.synth_window[n];
        }

        while (phase_reset_cursor < static_cast<int>(stft.phase_reset_markers.size()) &&
               stft.phase_reset_markers[phase_reset_cursor].synth_frame == frame_idx) {
            for (int c = 0; c < channels; ++c)
                for (int k = 0; k < K; ++k)
                    stft.theta_prev[c][k] = stft.phi_prev[c][k];
            ++phase_reset_cursor;
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
        for (int n = write_offset; n < write_offset + write_len; ++n) {
            for (int ch = 0; ch < channels; ++ch) {
                double v = ola_out[ch][n];
                write_buf[(n - write_offset) * channels + ch] = static_cast<float>(v);
            }
        }
        if (write_len > 0)
            write_cb(write_buf.data(), static_cast<size_t>(write_len));

        for (int ch = 0; ch < channels; ++ch) {
            std::memmove(ola_out[ch].data(), ola_out[ch].data() + R_s,
                         static_cast<size_t>(N - R_s) * sizeof(double));
            std::fill(ola_out[ch].data() + (N - R_s), ola_out[ch].data() + N, 0.0);
        }

        // Live progress via carriage return, gated on show_progress.
        if (show_progress && num_frames > 0 && (frame_idx % progress_stride) == 0) {
            int pct = static_cast<int>((frame_idx * 100LL) / num_frames);
            if (pct != last_pct) {
                std::cout << "\r" << pass_label << pct << "%" << std::flush;
                last_pct = pct;
            }
        }
    }

    const int remaining = N - R_s;
    if (remaining > 0) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < remaining; ++n) {
                double v = ola_out[ch][n];
                write_buf[n * channels + ch] = static_cast<float>(v);
            }
        }
        write_cb(write_buf.data(), static_cast<size_t>(remaining));
    }

    if (show_progress) {
        std::cout << "\r" << pass_label << "100%\n";
    }
}

void Synthesis::process(AudioSTFT& stft) {
    SF_INFO tgt_info = stft.src_info;
    tgt_info.format = SF_FORMAT_WAV |
        (stft.output_24bit_pcm ? SF_FORMAT_PCM_24 : SF_FORMAT_FLOAT);

    SNDFILE* output_snd = sf_open(stft.output_audio_file.c_str(), SFM_WRITE, &tgt_info);
    if (!output_snd) {
        std::cerr << "  ! could not open output '" << stft.output_audio_file << "'\n";
        return;
    }

    auto write_to_file = [output_snd](const float* buf, size_t n_frames) {
        sf_writef_float(output_snd, buf, static_cast<sf_count_t>(n_frames));
    };

    synthesize_full(stft, nullptr, write_to_file,
                    /*show_progress=*/true,
                    /*pass_label=*/"[Pass 3/3] Synthesis........................ ");
    sf_close(output_snd);
}
