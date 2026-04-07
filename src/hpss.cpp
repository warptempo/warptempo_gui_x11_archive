#include "hpss.h"
#include <iostream>
#include <algorithm>

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

            // Apply EQ and Dynamics so median filters work on post-processed geometry.
            for (int k = 0; k < K; ++k) {
                double mag = std::hypot(stft.fft_out[k][0], stft.fft_out[k][1]);
                double eq_scalar  = stft.multiplier_array.empty() ? 1.0 : stft.multiplier_array[k];
                double dyn_scalar = 1.0;
                if (!stft.S_traj_L.empty() && !stft.W_L.empty()) {
                    dyn_scalar = (stft.S_traj_L[m] * stft.W_L[k]) +
                                 (stft.S_traj_M[m] * stft.W_M[k]) +
                                 (stft.S_traj_H[m] * stft.W_H[k]);
                }
                X[m][k] = mag * eq_scalar * dyn_scalar;
            }
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
