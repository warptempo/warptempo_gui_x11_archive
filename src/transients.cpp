#include "transients.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

void Transients::process(AudioSTFT& stft) {
    const auto& dp       = stft.detect_params;

    stft.transient_markers.clear();

    if (!dp.enabled) {
        std::cout << "\n[Detector] Disabled; no transient markers produced.\n";
        return;
    }

    std::cout << "\n[Detector] Analyzing transients (LR4 mid-band, two-path crossings)...\n";

    const int N          = stft.N;
    const int R_s        = stft.R_s;
    const int channels   = stft.channels;
    const int K          = N / 2 + 1;
    const double bin_hz  = stft.bin_hz_width;
    const int fs         = stft.src_info.samplerate;
    const auto& fm       = stft.frame_map;
    const int total_frames = static_cast<int>(fm.size());

    if (total_frames <= 1) {
        std::cout << "[Detector] Fewer than two frames; no detection possible.\n";
        return;
    }

    // --- LR4 mid-band weight (W_M = 1 - W_L - W_H), clamped to [0, 1] ---
    std::vector<double> W_M(K, 0.0);
    for (int k = 1; k <= N / 2; ++k) {
        double hz = k * bin_hz;
        double W_L = 1.0 / (1.0 + std::pow(hz / dp.xover_low, 8.0));
        double W_H = 1.0 / (1.0 + std::pow(dp.xover_high / hz, 8.0));
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
                                    ((dp.tau_back_ms / 1000.0) * fs));
    for (int m = total_frames - 2; m >= 0; --m)
        energy[m] = c_back * energy[m] + (1.0 - c_back) * energy[m + 1];

    // --- dB conversion ---
    const double norm = static_cast<double>(N) / 2.0;
    std::vector<double> db(total_frames, 0.0);
    for (int m = 0; m < total_frames; ++m)
        db[m] = 20.0 * std::log10(std::sqrt(energy[m]) / norm + 1e-9);

    // --- Two-path crossing detection ---
    std::vector<int> loud_raw, quiet_raw;
    for (int m = 1; m < total_frames; ++m) {
        if (db[m] >= dp.loud_thresh_db && db[m - 1] < dp.loud_thresh_db)
            loud_raw.push_back(m);
        if (db[m] <= dp.quiet_thresh_db && db[m - 1] > dp.quiet_thresh_db)
            quiet_raw.push_back(m);
    }

    auto ms_to_frames = [fs, R_s](double ms) {
        return static_cast<int>(std::llround(ms * fs / (1000.0 * R_s)));
    };

    // --- Merge both paths into one time-ordered list with path tags ---
    struct Crossing { int frame; bool is_loud; };
    std::vector<Crossing> merged_raw;
    merged_raw.reserve(loud_raw.size() + quiet_raw.size());
    for (int f : loud_raw)  merged_raw.push_back({f, true});
    for (int f : quiet_raw) merged_raw.push_back({f, false});
    std::stable_sort(merged_raw.begin(), merged_raw.end(),
        [](const Crossing& a, const Crossing& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            return a.is_loud && !b.is_loud;  // tie-break: loud before quiet
        });

    // --- Warp marker synthesis frames (loud-path refractory reset only) ---
    std::vector<int> warp_frames;
    warp_frames.reserve(stft.timemap.size());
    for (const auto& seg : stft.timemap) {
        int wf = static_cast<int>(seg.tgt_frame / R_s);
        if (wf >= 0 && wf < total_frames) warp_frames.push_back(wf);
    }
    std::sort(warp_frames.begin(), warp_frames.end());

    // --- Unified asymmetric refractory ---
    // Loud detections consult only the loud tracker; quiet detections consult
    // both, yielding to any recent loud activity.
    const int refr = ms_to_frames(dp.refractory_ms);
    int last_loud  = std::numeric_limits<int>::min() / 2;
    int last_quiet = std::numeric_limits<int>::min() / 2;
    std::vector<Crossing> accepted;
    accepted.reserve(merged_raw.size());
    size_t warp_cursor = 0;
    int n_warp_resets = 0;
    auto apply_warps_upto = [&](int frame) {
        while (warp_cursor < warp_frames.size() &&
               warp_frames[warp_cursor] <= frame) {
            last_loud = warp_frames[warp_cursor] - refr;
            ++warp_cursor;
            ++n_warp_resets;
        }
    };
    for (const auto& c : merged_raw) {
        apply_warps_upto(c.frame);
        if (c.is_loud) {
            if (c.frame - last_loud >= refr) {
                accepted.push_back(c);
                last_loud = c.frame;
            }
        } else {
            if (c.frame - last_quiet >= refr && c.frame - last_loud >= refr) {
                accepted.push_back(c);
                last_quiet = c.frame;
            }
        }
    }
    while (warp_cursor < warp_frames.size()) { ++warp_cursor; ++n_warp_resets; }

    // --- Per-path shifts (loud earlier, quiet later) ---
    const int loud_shift  = ms_to_frames(dp.loud_anticipation_ms);
    const int quiet_shift = ms_to_frames(dp.quiet_delay_ms);

    std::vector<Crossing> shifted;
    shifted.reserve(accepted.size());
    int n_loud = 0, n_quiet = 0;
    for (const auto& c : accepted) {
        int s = c.is_loud ? (c.frame - loud_shift) : (c.frame + quiet_shift);
        if (s < 0 || s >= total_frames) continue;
        shifted.push_back({s, c.is_loud});
        if (c.is_loud) ++n_loud; else ++n_quiet;
    }
    std::sort(shifted.begin(), shifted.end(),
        [](const Crossing& a, const Crossing& b) { return a.frame < b.frame; });

    stft.transient_markers.reserve(shifted.size());
    for (const auto& s : shifted)
        stft.transient_markers.push_back({s.frame, fm[s.frame], s.is_loud});

    const int n_total = static_cast<int>(stft.transient_markers.size());
    std::cout << "[Detector] Loud path: " << n_loud
              << " detections. Quiet path: " << n_quiet
              << " detections. Total: " << n_total << ".\n";
    std::cout << "[Detector] Processed " << n_warp_resets
              << " warp marker reset events.\n";
    if (n_total < 3)
        std::cout << "[Detector] Warning: fewer than 3 detections; "
                     "output may be unusable without retuning thresholds.\n";
}
