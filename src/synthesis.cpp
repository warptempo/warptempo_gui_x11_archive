#include "synthesis.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <fstream>

void Synthesis::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 4] Executing " << (stft.hpss_enabled ? "HPSS" : "Bypass") << " Synthesis...\n";
    if (stft.yin_enabled)
        std::cout << "[Pass 5] YIN Extraction (f0=" << stft.yin_f0_min << "-" << stft.yin_f0_max
                  << " Hz, alpha=" << stft.yin_alpha << ", threshold=" << stft.yin_threshold << ")...\n";

    const int N        = stft.N;
    const int R_s      = stft.R_s;
    const int channels = stft.channels;
    const int K        = N / 2 + 1;
    const auto& fm     = stft.frame_map;
    const int num_frames = static_cast<int>(fm.size());
    const int fs       = stft.src_info.samplerate;

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

    // YIN extraction buffers (allocated only when yin_enabled is true)
    int tau_max = 0, tau_min = 0;
    std::vector<double> P_mag, H_re, H_im, P_re, P_im, G, yin_buf, d_prime;
    if (stft.yin_enabled) {
        tau_max = static_cast<int>(std::floor(static_cast<double>(fs) / stft.yin_f0_min));
        tau_min = static_cast<int>(std::floor(static_cast<double>(fs) / stft.yin_f0_max));
        P_mag.resize(K);
        H_re.resize(K);
        H_im.resize(K);
        P_re.resize(K);
        P_im.resize(K);
        G.resize(K);
        yin_buf.resize(N);
        d_prime.resize(tau_max + 1);
    }

    // Diagnostic buffers (allocated only when yin_diag is true)
    std::vector<std::vector<double>> ola_correction;
    std::ofstream pitch_log;
    SNDFILE* correction_snd = nullptr;
    if (stft.yin_enabled && stft.yin_diag) {
        ola_correction.assign(channels, std::vector<double>(N, 0.0));
        if (!stft.yin_diag_pitch_file.empty())
            pitch_log.open(stft.yin_diag_pitch_file);
        if (!stft.yin_diag_correction_file.empty())
            correction_snd = sf_open(stft.yin_diag_correction_file.c_str(), SFM_WRITE, &tgt_info);
        if (correction_snd)
            std::cout << "  -> Correction  : " << stft.yin_diag_correction_file << "\n";
        if (pitch_log.is_open())
            std::cout << "  -> Pitch log   : " << stft.yin_diag_pitch_file << "\n";
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

    const double inv_N = 1.0 / N;

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

            if (stft.yin_enabled && stft.hpss_enabled) {
                // Pass 5: YIN extraction — detect f0, redistribute harmonic energy
                const size_t base = static_cast<size_t>(ch) * num_frames * K +
                                    static_cast<size_t>(frame_idx) * K;

                // Step 2: Foreground magnitude
                for (int k = 0; k < K; ++k)
                    P_mag[k] = M[k] * stft.M_p_mask[base + k];

                // Step 3: Harmonic complex spectrum
                for (int k = 0; k < K; ++k) {
                    double mh = M[k] * stft.M_h_mask[base + k];
                    H_re[k] = mh * std::cos(theta[k]);
                    H_im[k] = mh * std::sin(theta[k]);
                }

                // Step 4: Percussive complex spectrum
                for (int k = 0; k < K; ++k) {
                    P_re[k] = P_mag[k] * std::cos(theta[k]);
                    P_im[k] = P_mag[k] * std::sin(theta[k]);
                }

                // Step 5a: Temporary IFFT of percussive frame for YIN analysis
                for (int k = 0; k < K; ++k) {
                    stft.ifft_in[k][0] = P_re[k];
                    stft.ifft_in[k][1] = P_im[k];
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    yin_buf[n] = stft.ifft_out[n] * inv_N;

                // Step 5b+5c: YIN difference function and CMNDF (fused single pass)
                const int W = N - tau_max;
                d_prime[0] = 1.0;
                double running_sum = 0.0;
                for (int tau = 1; tau <= tau_max; ++tau) {
                    double d_val = 0.0;
                    for (int j = 0; j < W; ++j) {
                        double diff = yin_buf[j] - yin_buf[j + tau];
                        d_val += diff * diff;
                    }
                    running_sum += d_val;
                    d_prime[tau] = d_val / (running_sum / tau + 1e-10);
                }

                // Step 5d: Pitch period selection with parabolic interpolation
                double f0 = 0.0;
                double confidence = 0.0;
                int tau_raw = -1;
                for (int tau = tau_min; tau <= tau_max; ++tau) {
                    if (d_prime[tau] < stft.yin_threshold) {
                        tau_raw = tau;
                        break;
                    }
                }

                if (tau_raw >= 0) {
                    double b = d_prime[tau_raw];
                    double tau_refined;
                    if (tau_raw > tau_min && tau_raw < tau_max) {
                        double a = d_prime[tau_raw - 1];
                        double c = d_prime[tau_raw + 1];
                        double shift = 0.5 * (a - c) / (a - 2.0 * b + c + 1e-10);
                        tau_refined = static_cast<double>(tau_raw) + shift;
                    } else {
                        tau_refined = static_cast<double>(tau_raw);
                    }
                    f0 = static_cast<double>(fs) / tau_refined;
                    confidence = 1.0 - b;
                }

                // Step 6: Harmonic comb filter and energy redistribution
                if (confidence >= stft.yin_confidence) {
                    // 6a: Compute Gaussian comb filter weights G[k]
                    const double fs_d = static_cast<double>(fs);
                    for (int k = 0; k < K; ++k) {
                        double f_k = static_cast<double>(k) * fs_d / N;
                        double j_nearest = std::round(f_k / f0);
                        if (j_nearest >= 1.0) {
                            double distance_bins = (f_k - j_nearest * f0) * N / fs_d;
                            G[k] = std::exp(-(distance_bins * distance_bins) /
                                             (2.0 * stft.yin_sigma * stft.yin_sigma));
                        } else {
                            G[k] = 0.0;
                        }
                    }

                    // 6b: Apply correction: subtract melody from P, add to H
                    const double scale_base = stft.yin_alpha * confidence;
                    for (int k = 0; k < K; ++k) {
                        double scale = G[k] * scale_base;
                        double C_re = P_re[k] * scale;
                        double C_im = P_im[k] * scale;
                        P_re[k] -= C_re;
                        P_im[k] -= C_im;
                        H_re[k] += C_re;
                        H_im[k] += C_im;
                        if (stft.yin_diag) {
                            stft.ifft_in[k][0] = C_re;
                            stft.ifft_in[k][1] = C_im;
                        }
                    }

                    // Diag: IFFT correction → ola_correction (4th IFFT per frame)
                    if (stft.yin_diag) {
                        fftw_execute(stft.plan_inv);
                        for (int n = 0; n < N; ++n)
                            ola_correction[ch][n] += (stft.ifft_out[n] * inv_N) * stft.synth_window[n];
                    }
                }

                // Diag: log pitch data for this frame/channel
                if (stft.yin_diag && pitch_log.is_open()) {
                    bool voiced = (confidence >= stft.yin_confidence);
                    double mean_G = 0.0;
                    if (voiced) {
                        for (int k = 0; k < K; ++k) mean_G += G[k];
                        mean_G /= K;
                    }
                    pitch_log << frame_idx << " " << ch << " "
                              << (voiced ? f0 : 0.0) << " "
                              << confidence << " " << mean_G << "\n";
                }

                // Step 7: IFFT H → ola_harmonic
                for (int k = 0; k < K; ++k) {
                    stft.ifft_in[k][0] = H_re[k];
                    stft.ifft_in[k][1] = H_im[k];
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_harmonic[ch][n] += (stft.ifft_out[n] * inv_N) * stft.synth_window[n];

                // Step 8: IFFT P → ola_perc
                for (int k = 0; k < K; ++k) {
                    stft.ifft_in[k][0] = P_re[k];
                    stft.ifft_in[k][1] = P_im[k];
                }
                fftw_execute(stft.plan_inv);
                for (int n = 0; n < N; ++n)
                    ola_perc[ch][n] += (stft.ifft_out[n] * inv_N) * stft.synth_window[n];

            } else if (stft.hpss_enabled) {
                // Existing HPSS routing (unchanged)
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

            if (stft.yin_enabled && stft.yin_diag && correction_snd) {
                for (int n = 0; n < write_len; ++n)
                    for (int ch = 0; ch < channels; ++ch)
                        write_buf[n * channels + ch] = static_cast<float>(ola_correction[ch][write_offset + n]);
                sf_writef_float(correction_snd, write_buf.data(), write_len);
            }
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
            if (stft.yin_enabled && stft.yin_diag) {
                for (int n = 0; n < N - R_s; ++n)
                    ola_correction[ch][n] = ola_correction[ch][n + R_s];
                for (int n = N - R_s; n < N; ++n)
                    ola_correction[ch][n] = 0.0;
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

        if (stft.yin_enabled && stft.yin_diag && correction_snd) {
            for (int ch = 0; ch < channels; ++ch)
                for (int n = 0; n < remaining; ++n)
                    write_buf[n * channels + ch] = static_cast<float>(ola_correction[ch][n]);
            sf_writef_float(correction_snd, write_buf.data(), remaining);
        }
    }

    if (harmonic_snd) sf_close(harmonic_snd);
    sf_close(perc_snd);
    if (correction_snd) sf_close(correction_snd);
    std::cout << "[Success] Final Master Written.\n";
}
