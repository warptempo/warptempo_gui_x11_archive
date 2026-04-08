#include "hpss.h"
#include <iostream>
#include <algorithm>
#include <cmath>

// 2D median helper for vertical (frequency-axis) filtering.
static double median_of_2d_col(std::vector<double>& scratch, const std::vector<std::vector<double>>& X,
                                int m, int k_lo, int k_hi) {
    int n = k_hi - k_lo + 1;
    scratch.resize(n);
    for (int i = 0; i < n; ++i) scratch[i] = X[m][k_lo + i];
    std::nth_element(scratch.begin(), scratch.begin() + n / 2, scratch.end());
    return scratch[n / 2];
}

void HPSS::process(AudioSTFT& stft) {
    std::cout << "\n[HPSS] Computing Foreground/Background Separation Masks (Per-Channel Stereo)...\n";

    const int N = stft.N;
    const int K = N / 2 + 1;
    const int channels = stft.channels;
    const auto& fm = stft.frame_map;
    const int M_total = static_cast<int>(fm.size());

    // Pre-allocate 3D mask arrays: [channel][frame][bin]
    stft.M_h_mask.assign(channels, std::vector<std::vector<double>>(M_total, std::vector<double>(K, 0.0)));
    stft.M_p_mask.assign(channels, std::vector<std::vector<double>>(M_total, std::vector<double>(K, 0.0)));

    std::vector<float> read_buf(N * channels, 0.0f);

    // ========================================================================
    // TRANSIENT MATCHER ANALYSIS
    // Builds per-frame C_rms and per-zone delta scalars used by synthesis.cpp
    // to apply restorative gain (G_p) on the Foreground (transient) channel.
    //
    // Both X_src and X_tgt are built from raw magnitudes (no EQ/Dynamics).
    // They are identical in the current pipeline; delta == 1 → G_p == 1 for
    // unprocessed content. C_rms still provides frame-level RMS modulation.
    // Peak memory: ~2 × M_total × K × 8 bytes, freed before per-channel loop.
    // ========================================================================
    if (stft.apply_tm == "none") {
        stft.C_rms.assign(M_total, 0.0);
        stft.delta_low.assign(M_total, 1.0);
        stft.delta_mid.assign(M_total, 1.0);
        stft.delta_high.assign(M_total, 1.0);
        std::cout << "          -> TM Analysis: bypassed (apply_tm=none).\n";
    } else {
    std::cout << "          -> TM Analysis: building source/target spectrograms for delta computation...\n";
    {
        stft.C_rms.assign(M_total, 0.0);
        stft.delta_low.assign(M_total, 1.0);
        stft.delta_mid.assign(M_total, 1.0);
        stft.delta_high.assign(M_total, 1.0);

        std::vector<std::vector<double>> X_src(M_total, std::vector<double>(K, 0.0));
        std::vector<std::vector<double>> X_tgt(M_total, std::vector<double>(K, 0.0));

        for (int m = 0; m < M_total; ++m) {
            // --- Source FFT (mono average of raw source frames) ---
            int64_t t_a = fm[m];
            std::fill(read_buf.begin(), read_buf.end(), 0.0f);
            if (t_a >= 0 && t_a < stft.src_info.frames) {
                sf_seek(stft.src_snd, t_a, SEEK_SET);
                sf_readf_float(stft.src_snd, read_buf.data(), N);
            }
            for (int n = 0; n < N; ++n) {
                double s = 0.0;
                for (int c = 0; c < channels; ++c) s += read_buf[n * channels + c];
                stft.fft_in[n] = (s / channels) * stft.window[n];
            }
            fftw_execute(stft.plan_fwd);
            for (int k = 0; k < K; ++k)
                X_src[m][k] = std::hypot(stft.fft_out[k][0], stft.fft_out[k][1]);

            // --- Target FFT (mono average of Phase Vocoder output from virtual_tgt_buf) ---
            int64_t tgt_offset = static_cast<int64_t>(m) * stft.R_s;
            for (int n = 0; n < N; ++n) {
                double s = 0.0;
                int64_t buf_idx = (tgt_offset + n) * channels;
                if (buf_idx >= 0 &&
                    buf_idx + channels <= static_cast<int64_t>(stft.virtual_tgt_buf.size())) {
                    for (int c = 0; c < channels; ++c)
                        s += stft.virtual_tgt_buf[buf_idx + c];
                }
                stft.fft_in[n] = (s / channels) * stft.window[n];
            }
            fftw_execute(stft.plan_fwd);
            for (int k = 0; k < K; ++k)
                X_tgt[m][k] = std::hypot(stft.fft_out[k][0], stft.fft_out[k][1]);
        }

        // C_rms: trailing 21-frame spectral RMS through a piecewise quadratic (C1) spline.
        // Five-stage gain gate; louder frames receive more transient correction.
        // S_rms aligns the raw FFTW RMS to the AES17 dBFS scale (0 dBFS = full-scale sine).
        const double S_rms    = std::sqrt(static_cast<double>(stft.N) * 3.0 / 8.0);
        const double spline_L = stft.tm_floor_db / 20.0;
        const double spline_U = stft.tm_peak_db  / 20.0;
        double       spline_W = stft.tm_knee_db  / 20.0;
        if (spline_W >= (spline_U - spline_L)) {
            spline_W = (spline_U - spline_L) * 0.99;
            std::cerr << "[HPSS] WARNING: tm_knee too wide; clamped to "
                      << (spline_W * 20.0) << " dB.\n";
        }

        for (int m = 0; m < M_total; ++m) {
            double sum_sq = 0.0;
            int count = 0;
            for (int i = std::max(0, m - 20); i <= m; ++i) {
                for (int k = 1; k < K; ++k)  // skip DC
                    sum_sq += X_src[i][k] * X_src[i][k];
                ++count;
            }
            double rms = (count > 0 && K > 1)
                             ? std::sqrt(sum_sq / (static_cast<double>(count) * (K - 1)))
                             : 0.0;

            const double x = std::log10(rms / S_rms + 1e-12);
            double C;
            if (x < spline_L - spline_W / 2) {
                C = 0.0;
            } else if (x < spline_L + spline_W / 2) {
                double t = x - spline_L + spline_W / 2;
                C = (t * t) / (2.0 * spline_W * (spline_U - spline_L));
            } else if (x < spline_U - spline_W / 2) {
                C = (x - spline_L) / (spline_U - spline_L);
            } else if (x < spline_U + spline_W / 2) {
                double t = spline_U + spline_W / 2 - x;
                C = 1.0 - (t * t) / (2.0 * spline_W * (spline_U - spline_L));
            } else {
                C = 1.0;
            }
            stft.C_rms[m] = C;
        }

        // Zone boundary bins (first bin at or above each crossover frequency)
        const int k_z0 = std::min(K - 1, static_cast<int>(std::ceil(stft.tm_xover_0 / stft.bin_hz_width)));
        const int k_z1 = std::min(K - 1, static_cast<int>(std::ceil(stft.tm_xover_1 / stft.bin_hz_width)));
        const int k_z2 = std::min(K - 1, static_cast<int>(std::ceil(stft.tm_xover_2 / stft.bin_hz_width)));
        const int W    = stft.tm_W_frames;

        for (int m = 0; m < M_total; ++m) {
            double src_low = 0.0, src_mid = 0.0, src_high = 0.0;
            double tgt_low = 0.0, tgt_mid = 0.0, tgt_high = 0.0;

            for (int dm = -W; dm <= W; ++dm) {
                int mi = std::max(0, std::min(M_total - 1, m + dm));
                for (int k = k_z0; k < k_z1; ++k) { src_low  += X_src[mi][k]; tgt_low  += X_tgt[mi][k]; }
                for (int k = k_z1; k < k_z2; ++k) { src_mid  += X_src[mi][k]; tgt_mid  += X_tgt[mi][k]; }
                for (int k = k_z2; k < K;    ++k) { src_high += X_src[mi][k]; tgt_high += X_tgt[mi][k]; }
            }

            stft.delta_low[m]  = (tgt_low  > 1e-9) ? (src_low  / tgt_low)  : 1.0;
            stft.delta_mid[m]  = (tgt_mid  > 1e-9) ? (src_mid  / tgt_mid)  : 1.0;
            stft.delta_high[m] = (tgt_high > 1e-9) ? (src_high / tgt_high) : 1.0;
        }

        X_src.clear(); X_src.shrink_to_fit();
        X_tgt.clear(); X_tgt.shrink_to_fit();
        std::cout << "          -> TM Analysis: C_rms and deltas computed. Temporaries freed.\n";
    }
    } // end apply_tm != "none"

    // Process each channel independently to bound peak memory to ~400 MB per pass.
    for (int ch = 0; ch < channels; ++ch) {

        // ====================================================================
        // PHASE A: Per-Channel Magnitude Spectrogram
        // ====================================================================
        std::cout << "          -> Ch" << ch << " Phase A: Building magnitude spectrogram ("
                  << M_total << " frames x " << K << " bins)...\n";

        std::vector<std::vector<double>> X(M_total, std::vector<double>(K, 0.0));

        for (int m = 0; m < M_total; ++m) {
            int64_t t_a_rounded = fm[m];

            // OOB Policy: zero-pad if outside source bounds
            std::fill(read_buf.begin(), read_buf.end(), 0.0f);
            if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
                sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
                sf_readf_float(stft.src_snd, read_buf.data(), N);
            }

            for (int n = 0; n < N; ++n)
                stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);

            for (int k = 0; k < K; ++k)
                X[m][k] = std::hypot(stft.fft_out[k][0], stft.fft_out[k][1]);
        }

        // ====================================================================
        // PHASE B: Median Filtering
        // ====================================================================
        std::cout << "          -> Ch" << ch << " Phase B: Horizontal (L_h=" << stft.L_h
                  << ") & Vertical (L_p_max=" << stft.L_p_max << ")...\n";

        // H and P are derived independently from X.
        std::vector<std::vector<double>> H(M_total, std::vector<double>(K, 0.0));
        std::vector<std::vector<double>> P(M_total, std::vector<double>(K, 0.0));

        // Horizontal Filter (H): median of X along the time axis
        {
            std::vector<double> scratch;
            for (int m = 0; m < M_total; ++m) {
                for (int k = 0; k < K; ++k) {
                    int lo = std::max(0, m - stft.L_h);
                    int hi = std::min(M_total - 1, m + stft.L_h);
                    int n  = hi - lo + 1;
                    scratch.resize(n);
                    for (int i = 0; i < n; ++i) scratch[i] = X[lo + i][k];
                    std::nth_element(scratch.begin(), scratch.begin() + n / 2, scratch.end());
                    H[m][k] = scratch[n / 2];
                }
            }
        }

        // Vertical Filter (P): median of X along the frequency axis
        {
            std::vector<double> scratch;
            for (int m = 0; m < M_total; ++m) {
                for (int k = 0; k < K; ++k) {
                    // TUNING NOTE: L_p_max is deliberately restricted to 7 (~160 Hz). This captures
                    // narrow melodic harmonic clusters as 'Foreground' rather than requiring massive
                    // broadband noise to trigger. Do not increase to 17.
                    int L_p  = std::max(3, std::min(stft.L_p_max,
                               static_cast<int>(std::round(17.0 * std::pow(k / 28.0, 0.5)))));
                    int k_lo = std::max(0, k - L_p);
                    int k_hi = std::min(K - 1, k + L_p);
                    P[m][k]  = median_of_2d_col(scratch, X, m, k_lo, k_hi);
                }
            }
        }

        // ====================================================================
        // PHASE C: Mask Generation
        // ====================================================================
        std::cout << "          -> Ch" << ch << " Phase C: Generating soft masks (beta=" << stft.beta << ")...\n";

        for (int m = 0; m < M_total; ++m) {
            for (int k = 0; k < K; ++k) {
                double P_beta = std::pow(P[m][k], stft.beta);
                double H_beta = std::pow(H[m][k], stft.beta);
                double denom  = P_beta + H_beta + 1e-8;
                stft.M_p_mask[ch][m][k] = P_beta / denom;
                stft.M_h_mask[ch][m][k] = H_beta / denom;
            }
        }

        // FORCE DEALLOCATION: Keep peak memory bounded to ~400 MB per channel pass.
        X.clear(); X.shrink_to_fit();
        H.clear(); H.shrink_to_fit();
        P.clear(); P.shrink_to_fit();

        std::cout << "          -> Ch" << ch << ": Masks computed. Temporaries freed.\n";
    }

    std::cout << "          -> HPSS complete (" << channels << " channels, "
              << M_total << " frames). Independent stereo masks ready.\n";
}
