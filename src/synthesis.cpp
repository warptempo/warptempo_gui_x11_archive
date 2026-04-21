#include "synthesis.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <deque>
#include <iostream>
#include <iterator>
#include <vector>

void Synthesis::synthesize_full(
    AudioSTFT& stft,
    bool apply_clipper,
    std::complex<float>* spectra_cache,
    std::function<void(const float*, size_t)> write_cb,
    bool lock_transient_resets,
    bool show_progress,
    const char* pass_label) {
    const int N          = stft.N;
    const int R_s        = stft.R_s;
    const int channels   = stft.channels;
    const int K          = N / 2 + 1;
    const auto& fm       = stft.frame_map;
    const int num_frames = static_cast<int>(fm.size());
    const double ceiling_amp =
        std::pow(10.0, stft.limiter_params.ceiling_dbfs / 20.0);

    stft.reset_phase_state();

    std::vector<double> M(K), phi(K), theta(K);
    std::vector<int> peaks;
    peaks.reserve(N / 8);

    const bool has_transients = !stft.transient_markers.empty();
    const int flex_win = stft.flex_window;
    const int ring_size = 2 * flex_win + 2;
    int transient_cursor = 0;
    struct PendingEval { int marker_frame; int64_t src_frame; int marker_index; };
    std::deque<PendingEval> pending_evals;
    bool fallback_warned = false;
    std::vector<std::vector<double>> ring_phi;
    std::vector<std::vector<int>> ring_peaks_buf;
    double recent_peak_amp = 1e-10;
    if (has_transients) {
        ring_phi.resize(channels * ring_size, std::vector<double>(K, 0.0));
        ring_peaks_buf.resize(channels * ring_size);
    }

    std::vector<std::vector<double>> ola_out(channels, std::vector<double>(N, 0.0));

    auto run_evaluator = [&](const PendingEval& pe, int eval_frame) -> int {
        int best_frame = pe.marker_frame;
        double best_score = -1.0;

        for (int cand = pe.marker_frame - flex_win; cand <= pe.marker_frame + flex_win; ++cand) {
            if (cand < 0 || cand >= num_frames || cand > eval_frame) continue;

            int ola_pos = N / 2 + (cand - eval_frame) * R_s;
            double zc_score = 0.0;
            if (ola_pos >= 0 && ola_pos < N) {
                double avg_val = 0.0;
                for (int c = 0; c < channels; ++c)
                    avg_val += std::abs(ola_out[c][ola_pos]);
                avg_val /= channels;
                zc_score = 1.0 - std::min(avg_val / (recent_peak_amp + 1e-10), 1.0);
            }

            double jaccard_avg = 0.0;
            int jaccard_count = 0;
            for (int c = 0; c < channels; ++c) {
                if (cand < eval_frame - ring_size + 1) break;
                int cand_slot = cand % ring_size;
                const auto& cp = ring_peaks_buf[c * ring_size + cand_slot];

                std::vector<int> neighbor_union;
                for (int nb : {cand - 1, cand + 1}) {
                    if (nb < 0 || nb >= num_frames || nb > eval_frame) continue;
                    if (nb < eval_frame - ring_size + 1) continue;
                    int nb_slot = nb % ring_size;
                    const auto& np = ring_peaks_buf[c * ring_size + nb_slot];
                    std::vector<int> merged;
                    std::set_union(neighbor_union.begin(), neighbor_union.end(),
                                   np.begin(), np.end(), std::back_inserter(merged));
                    neighbor_union = std::move(merged);
                }

                if (neighbor_union.empty() && cp.empty()) {
                    jaccard_avg += 1.0;
                } else if (!neighbor_union.empty() && !cp.empty()) {
                    int isect = 0, usize = 0;
                    size_t i = 0, j = 0;
                    while (i < cp.size() && j < neighbor_union.size()) {
                        if (cp[i] == neighbor_union[j]) { ++isect; ++usize; ++i; ++j; }
                        else if (cp[i] < static_cast<int>(neighbor_union[j])) { ++usize; ++i; }
                        else { ++usize; ++j; }
                    }
                    usize += static_cast<int>((cp.size() - i) + (neighbor_union.size() - j));
                    jaccard_avg += static_cast<double>(isect) / usize;
                }
                ++jaccard_count;
            }
            if (jaccard_count > 0) jaccard_avg /= jaccard_count;

            double combined = zc_score * jaccard_avg;
            if (combined > best_score) {
                best_score = combined;
                best_frame = cand;
            }
        }
        return best_frame;
    };

    auto apply_reset_from_frame = [&](int best_frame) {
        int best_slot = best_frame % ring_size;
        for (int c = 0; c < channels; ++c)
            for (int k = 0; k < K; ++k)
                stft.theta_prev[c][k] = ring_phi[c * ring_size + best_slot][k];
    };

    auto evaluate_and_apply_reset = [&](const PendingEval& pe, int eval_frame) {
        TransientMarker& marker = stft.transient_markers[pe.marker_index];
        int best_frame;
        if (lock_transient_resets) {
            best_frame = run_evaluator(pe, eval_frame);
            marker.chosen_reset_frame = best_frame;
        } else if (marker.chosen_reset_frame >= 0) {
            best_frame = marker.chosen_reset_frame;
        } else {
            if (!fallback_warned) {
                std::cerr << "  ! Transient marker at synth_frame " << pe.marker_frame
                          << " has no locked reset frame; falling back to evaluator\n";
                fallback_warned = true;
            }
            best_frame = run_evaluator(pe, eval_frame);
        }
        apply_reset_from_frame(best_frame);
    };

    std::vector<float> read_buf(N * channels, 0.0f);
    std::vector<float> write_buf(N * channels, 0.0f);

    // Start-trim: the first N/2 samples are OLA ramp-up. Mirrors phase_vocoder.cpp.
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

            if (has_transients) {
                int slot = frame_idx % ring_size;
                std::copy(phi.begin(), phi.end(), ring_phi[ch * ring_size + slot].begin());
                ring_peaks_buf[ch * ring_size + slot] = peaks;
            }

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

        if (has_transients) {
            double frame_max = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                for (int n = 0; n < R_s && n < N; ++n)
                    frame_max = std::max(frame_max, std::abs(ola_out[ch][n]));
            recent_peak_amp = std::max(recent_peak_amp * 0.995, frame_max);

            while (transient_cursor < static_cast<int>(stft.transient_markers.size()) &&
                   stft.transient_markers[transient_cursor].synth_frame == frame_idx) {
                pending_evals.push_back({frame_idx,
                    stft.transient_markers[transient_cursor].src_frame,
                    transient_cursor});
                ++transient_cursor;
            }

            while (!pending_evals.empty() &&
                   frame_idx >= pending_evals.front().marker_frame + flex_win) {
                evaluate_and_apply_reset(pending_evals.front(), frame_idx);
                pending_evals.pop_front();
            }
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
                if (apply_clipper) {
                    if (v >  ceiling_amp) v =  ceiling_amp;
                    if (v < -ceiling_amp) v = -ceiling_amp;
                }
                write_buf[(n - write_offset) * channels + ch] = static_cast<float>(v);
            }
        }
        if (write_len > 0)
            write_cb(write_buf.data(), static_cast<size_t>(write_len));

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n)
                ola_out[ch][n] = ola_out[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n)
                ola_out[ch][n] = 0.0;
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

    if (has_transients) {
        while (!pending_evals.empty()) {
            evaluate_and_apply_reset(pending_evals.front(), num_frames - 1);
            pending_evals.pop_front();
        }
    }

    const int remaining = N - R_s;
    if (remaining > 0) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < remaining; ++n) {
                double v = ola_out[ch][n];
                if (apply_clipper) {
                    if (v >  ceiling_amp) v =  ceiling_amp;
                    if (v < -ceiling_amp) v = -ceiling_amp;
                }
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
    tgt_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* output_snd = sf_open(stft.output_audio_file.c_str(), SFM_WRITE, &tgt_info);
    if (!output_snd) {
        std::cerr << "  ! could not open output '" << stft.output_audio_file << "'\n";
        return;
    }

    auto write_to_file = [output_snd](const float* buf, size_t n_frames) {
        sf_writef_float(output_snd, buf, static_cast<sf_count_t>(n_frames));
    };

    synthesize_full(stft, stft.limiter_params.clipper_enabled, nullptr, write_to_file,
                    /*lock_transient_resets=*/false,
                    /*show_progress=*/true,
                    /*pass_label=*/"[Pass 5/5] Synthesis........................ ");
    sf_close(output_snd);
}
