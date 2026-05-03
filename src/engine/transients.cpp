#include "transients.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

void Transients::process(AudioSTFT& stft) {
    const auto& tp       = stft.transients_params;

    stft.transient_markers.clear();

    if (!tp.enabled) {
        std::cout << "[Pass 1/3] Transient detection.............. disabled\n";
        return;
    }

    const int N          = stft.N;
    const int R_s        = stft.R_s;
    const int channels   = stft.channels;
    const int K          = N / 2 + 1;
    const double bin_hz  = stft.bin_hz_width;
    const int fs         = stft.src_info.samplerate;
    const auto& fm       = stft.frame_map;
    const int total_frames = static_cast<int>(fm.size());

    if (total_frames <= 1) {
        std::cout << "[Pass 1/3] Transient detection.............. 0 transients\n";
        std::cout << "  ! fewer than two frames; no detection possible\n";
        return;
    }

    // --- LR4 mid-band weight (W_M = 1 - W_L - W_H), clamped to [0, 1] ---
    std::vector<double> W_M(K, 0.0);
    for (int k = 1; k <= N / 2; ++k) {
        double hz = k * bin_hz;
        double W_L = 1.0 / (1.0 + std::pow(hz / tp.xover_low, 8.0));
        double W_H = 1.0 / (1.0 + std::pow(tp.xover_high / hz, 8.0));
        double w = 1.0 - W_L - W_H;
        W_M[k] = (w < 0.0) ? 0.0 : w;
    }

    // --- Per-synthesis-frame mid-band energy trajectory ---
    std::vector<double> energy(total_frames, 0.0);
    std::vector<float> read_buf(N * channels, 0.0f);
    std::vector<double> M_sq(K, 0.0);

    for (int m = 0; m < total_frames; ++m) {
        int64_t t_a_rounded = fm[m];

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
            sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
            sf_readf_float(stft.src_snd, read_buf.data(), N);
        }

        std::fill(M_sq.begin(), M_sq.end(), 0.0);
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n)
                stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);
            for (int k = 0; k <= N / 2; ++k)
                M_sq[k] += (stft.fft_out[k][0] * stft.fft_out[k][0] +
                            stft.fft_out[k][1] * stft.fft_out[k][1]) / channels;
        }

        double den_M = 0.0;
        for (int k = 1; k <= N / 2; ++k)
            den_M += M_sq[k] * W_M[k];
        energy[m] = den_M;
    }

    // --- Single-pass backward IIR smoothing (fast, per addendum) ---
    double c_back = 1.0 - std::exp(-static_cast<double>(R_s) /
                                    ((tp.tau_back_ms / 1000.0) * fs));
    for (int m = total_frames - 2; m >= 0; --m)
        energy[m] = c_back * energy[m] + (1.0 - c_back) * energy[m + 1];

    // --- dB conversion ---
    const double norm = static_cast<double>(N) / 2.0;
    std::vector<double> db(total_frames, 0.0);
    for (int m = 0; m < total_frames; ++m)
        db[m] = 20.0 * std::log10(std::sqrt(energy[m]) / norm + 1e-9);

    // --- Upward-crossing detection ---
    std::vector<int> raw;
    for (int m = 1; m < total_frames; ++m) {
        if (db[m] >= tp.thresh_db && db[m - 1] < tp.thresh_db)
            raw.push_back(m);
    }

    auto ms_to_frames = [fs, R_s](double ms) {
        return static_cast<int>(std::llround(ms * fs / (1000.0 * R_s)));
    };

    // --- Warp marker synthesis frames ---
    std::vector<int> warp_frames;
    warp_frames.reserve(stft.timemap.size());
    for (const auto& seg : stft.timemap) {
        int wf = static_cast<int>(seg.tgt_frame / R_s);
        if (wf >= 0 && wf < total_frames) warp_frames.push_back(wf);
    }
    std::sort(warp_frames.begin(), warp_frames.end());

    // --- Refractory ---
    // Warp markers reset the refractory so musically significant attacks at
    // warp points can fire immediately.
    const int refr = ms_to_frames(tp.refractory_ms);
    int last_accepted = std::numeric_limits<int>::min() / 2;
    std::vector<int> accepted;
    accepted.reserve(raw.size());
    size_t warp_cursor = 0;
    auto apply_warps_upto = [&](int frame) {
        while (warp_cursor < warp_frames.size() &&
               warp_frames[warp_cursor] <= frame) {
            last_accepted = warp_frames[warp_cursor] - refr;
            ++warp_cursor;
        }
    };
    for (int f : raw) {
        apply_warps_upto(f);
        if (f - last_accepted >= refr) {
            accepted.push_back(f);
            last_accepted = f;
        }
    }

    // --- Anticipation shift ---
    const int anticipation_shift = ms_to_frames(tp.anticipation_ms);

    stft.transient_markers.reserve(accepted.size());
    for (int f : accepted) {
        int s = f - anticipation_shift;
        if (s < 0 || s >= total_frames) continue;
        stft.transient_markers.push_back({s, fm[s]});
    }

    const int n_total = static_cast<int>(stft.transient_markers.size());
    std::cout << "[Pass 1/3] Transient detection.............. "
              << n_total << " transients\n";
    if (n_total < 3)
        std::cout << "  ! fewer than 3 detections; output may be unusable\n";
}
