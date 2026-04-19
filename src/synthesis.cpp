#include "synthesis.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdio>

void Synthesis::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 4] Executing Synthesis...\n";

    const int N        = stft.N;
    const int R_s      = stft.R_s;
    const int channels = stft.channels;
    const int K        = N / 2 + 1;
    const auto& fm     = stft.frame_map;
    const int num_frames = static_cast<int>(fm.size());
    const int fs       = stft.src_info.samplerate;

    SF_INFO tgt_info = stft.src_info;
    tgt_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* output_snd = sf_open(stft.output_audio_file.c_str(), SFM_WRITE, &tgt_info);
    std::cout << "  -> Output      : " << stft.output_audio_file << "\n";

    stft.reset_phase_state();

    // Allocated once, reused across all frames and channels (Issue 2)
    std::vector<double> M(K), phi(K), theta(K);
    std::vector<int> peaks;
    peaks.reserve(N / 8);

    // Transient phase reset infrastructure
    const bool has_transients = !stft.transient_markers.empty();
    const int flex_win = stft.flex_window;
    const int ring_size = 2 * flex_win + 2;
    int transient_cursor = 0;
    struct PendingEval { int marker_frame; int64_t src_frame; };
    std::deque<PendingEval> pending_evals;
    std::vector<std::vector<double>> ring_phi;
    std::vector<std::vector<int>> ring_peaks_buf;
    double recent_peak_amp = 1e-10;
    if (has_transients) {
        ring_phi.resize(channels * ring_size, std::vector<double>(K, 0.0));
        ring_peaks_buf.resize(channels * ring_size);
    }

    std::vector<std::vector<double>> ola_out(channels, std::vector<double>(N, 0.0));

    auto evaluate_and_apply_reset = [&](const PendingEval& pe, int eval_frame) {
        int best_frame = pe.marker_frame;
        double best_score = -1.0;

        for (int cand = pe.marker_frame - flex_win; cand <= pe.marker_frame + flex_win; ++cand) {
            if (cand < 0 || cand >= num_frames || cand > eval_frame) continue;

            // Criterion 1: Zero-crossing proximity
            int ola_pos = N / 2 + (cand - eval_frame) * R_s;
            double zc_score = 0.0;
            if (ola_pos >= 0 && ola_pos < N) {
                double avg_val = 0.0;
                for (int c = 0; c < channels; ++c)
                    avg_val += std::abs(ola_out[c][ola_pos]);
                avg_val /= channels;
                zc_score = 1.0 - std::min(avg_val / (recent_peak_amp + 1e-10), 1.0);
            }

            // Criterion 2: Phase lock group stability (Jaccard)
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

        // Apply reset: theta_prev = phi from chosen frame
        int best_slot = best_frame % ring_size;
        for (int c = 0; c < channels; ++c)
            for (int k = 0; k < K; ++k)
                stft.theta_prev[c][k] = ring_phi[c * ring_size + best_slot][k];

        // Diagnostic output
        int delta = best_frame - pe.marker_frame;
        int64_t flex_ta = fm[best_frame];
        double secs = std::max(0.0, static_cast<double>(flex_ta) / fs);
        int total_ms = static_cast<int>(secs * 1000);
        int mm = total_ms / 60000;
        int ss = (total_ms % 60000) / 1000;
        int ms = total_ms % 1000;
        std::printf("  transient src=%lld synth=%d flex=%d delta=%d at %02d:%02d.%03d\n",
                    (long long)pe.src_frame, pe.marker_frame, best_frame, delta, mm, ss, ms);
    };

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

            if (has_transients) {
                int slot = frame_idx % ring_size;
                std::copy(phi.begin(), phi.end(), ring_phi[ch * ring_size + slot].begin());
                ring_peaks_buf[ch * ring_size + slot] = peaks;
            }

            for (int k = 0; k < K; ++k) {
                stft.ifft_in[k][0] = M[k] * std::cos(theta[k]);
                stft.ifft_in[k][1] = M[k] * std::sin(theta[k]);
            }
            fftw_execute(stft.plan_inv);
            for (int n = 0; n < N; ++n)
                ola_out[ch][n] += (stft.ifft_out[n] * inv_N) * stft.synth_window[n];
        }

        // Transient phase reset processing
        if (has_transients) {
            double frame_max = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                for (int n = 0; n < R_s && n < N; ++n)
                    frame_max = std::max(frame_max, std::abs(ola_out[ch][n]));
            recent_peak_amp = std::max(recent_peak_amp * 0.995, frame_max);

            while (transient_cursor < static_cast<int>(stft.transient_markers.size()) &&
                   stft.transient_markers[transient_cursor].synth_frame == frame_idx) {
                pending_evals.push_back({frame_idx,
                    stft.transient_markers[transient_cursor].src_frame});
                ++transient_cursor;
            }

            while (!pending_evals.empty() &&
                   frame_idx >= pending_evals.front().marker_frame + flex_win) {
                evaluate_and_apply_reset(pending_evals.front(), frame_idx);
                pending_evals.pop_front();
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
            for (int n = 0; n < write_len; ++n)
                for (int ch = 0; ch < channels; ++ch)
                    write_buf[n * channels + ch] = static_cast<float>(ola_out[ch][write_offset + n]);
            sf_writef_float(output_snd, write_buf.data(), write_len);
        }

        // Shift OLA buffers
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n)
                ola_out[ch][n] = ola_out[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n)
                ola_out[ch][n] = 0.0;
        }
    }

    // Flush any remaining pending transient evaluations
    if (has_transients) {
        while (!pending_evals.empty()) {
            evaluate_and_apply_reset(pending_evals.front(), num_frames - 1);
            pending_evals.pop_front();
        }
    }

    // Flush remaining overlap
    const int remaining = N - R_s;
    if (remaining > 0) {
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < remaining; ++n)
                write_buf[n * channels + ch] = static_cast<float>(ola_out[ch][n]);
        sf_writef_float(output_snd, write_buf.data(), remaining);
    }

    sf_close(output_snd);
    std::cout << "[Success] Final Master Written.\n";
}
