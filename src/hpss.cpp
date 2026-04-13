#include "hpss.h"
#include <iostream>
#include <algorithm>
#include <cmath>

#ifdef _OPENMP
#include <omp.h>
#endif

void HPSS::process(AudioSTFT& stft) {
    std::cout << "\n[HPSS] Computing Foreground/Background Separation Masks (Per-Channel Stereo)...\n";

    const int N = stft.N;
    const int K = N / 2 + 1;
    const int channels = stft.channels;
    const auto& fm = stft.frame_map;
    const int M_total = static_cast<int>(fm.size());

    // Pre-allocate flat mask arrays: index as ch * M_total * K + m * K + k
    stft.M_h_mask.assign(static_cast<size_t>(channels) * M_total * K, 0.0);
    stft.M_p_mask.assign(static_cast<size_t>(channels) * M_total * K, 0.0);

    std::vector<float> read_buf(N * channels, 0.0f);

    for (int ch = 0; ch < channels; ++ch) {

        // ====================================================================
        // PHASE A: Per-Channel Magnitude Spectrogram
        // ====================================================================
        std::cout << "          -> Ch" << ch << " Phase A: Building magnitude spectrogram ("
                  << M_total << " frames x " << K << " bins)...\n";

        // X stored row-major [frame][bin] for vertical filter.
        // X_T stored [bin][frame] for cache-friendly horizontal filter reads.
        std::vector<double> X  (static_cast<size_t>(M_total) * K, 0.0);
        std::vector<double> X_T(static_cast<size_t>(K) * M_total, 0.0);

        for (int m = 0; m < M_total; ++m) {
            int64_t t_a_rounded = fm[m];

            std::fill(read_buf.begin(), read_buf.end(), 0.0f);
            if (t_a_rounded >= 0 && t_a_rounded < stft.src_info.frames) {
                sf_seek(stft.src_snd, t_a_rounded, SEEK_SET);
                sf_readf_float(stft.src_snd, read_buf.data(), N);
            }

            for (int n = 0; n < N; ++n)
                stft.fft_in[n] = read_buf[n * channels + ch] * stft.window[n];
            fftw_execute(stft.plan_fwd);

            for (int k = 0; k < K; ++k) {
                double mag = std::hypot(stft.fft_out[k][0], stft.fft_out[k][1]);
                X  [m * K + k] = mag;
                X_T[k * M_total + m] = mag;
            }
        }

        // ====================================================================
        // PHASE B: Median Filtering
        // ====================================================================
        std::cout << "          -> Ch" << ch << " Phase B: Horizontal (L_h=" << stft.L_h
                  << ") & Vertical (L_p_max=" << stft.L_p_max << ")...\n";

        std::vector<double> H(static_cast<size_t>(M_total) * K, 0.0);
        std::vector<double> P(static_cast<size_t>(M_total) * K, 0.0);

        // Horizontal Filter: median along time axis.
        // Uses X_T[k][m] — contiguous reads per bin.
        #pragma omp parallel for schedule(dynamic, 64)
        for (int m = 0; m < M_total; ++m) {
            std::vector<double> scratch;
            for (int k = 0; k < K; ++k) {
                int lo = std::max(0, m - stft.L_h);
                int hi = std::min(M_total - 1, m + stft.L_h);
                int n  = hi - lo + 1;
                scratch.resize(n);
                const double* row = X_T.data() + static_cast<size_t>(k) * M_total;
                for (int i = 0; i < n; ++i) scratch[i] = row[lo + i];
                std::nth_element(scratch.begin(), scratch.begin() + n / 2, scratch.end());
                H[m * K + k] = scratch[n / 2];
            }
        }

        // Vertical Filter: median along frequency axis.
        // Uses X[m][k] — contiguous reads per frame.
        #pragma omp parallel for schedule(dynamic, 64)
        for (int m = 0; m < M_total; ++m) {
            std::vector<double> scratch;
            const double* row = X.data() + static_cast<size_t>(m) * K;
            for (int k = 0; k < K; ++k) {
                int L_p  = std::max(3, std::min(stft.L_p_max,
                           static_cast<int>(std::round(17.0 * std::pow(k / 28.0, 0.5)))));
                int k_lo = std::max(0, k - L_p);
                int k_hi = std::min(K - 1, k + L_p);
                int n    = k_hi - k_lo + 1;
                scratch.resize(n);
                for (int i = 0; i < n; ++i) scratch[i] = row[k_lo + i];
                std::nth_element(scratch.begin(), scratch.begin() + n / 2, scratch.end());
                P[m * K + k] = scratch[n / 2];
            }
        }

        // FORCE DEALLOCATION: Keep peak memory bounded.
        X.clear();   X.shrink_to_fit();
        X_T.clear(); X_T.shrink_to_fit();

        // ====================================================================
        // PHASE C: Mask Generation
        // ====================================================================
        std::cout << "          -> Ch" << ch << " Phase C: Generating soft masks (beta=" << stft.beta << ")...\n";

        #pragma omp parallel for schedule(static)
        for (int m = 0; m < M_total; ++m) {
            for (int k = 0; k < K; ++k) {
                double P_beta = std::pow(P[m * K + k], stft.beta);
                double H_beta = std::pow(H[m * K + k], stft.beta);
                double denom  = P_beta + H_beta + 1e-8;
                size_t idx = static_cast<size_t>(ch) * M_total * K + m * K + k;
                stft.M_p_mask[idx] = P_beta / denom;
                stft.M_h_mask[idx] = H_beta / denom;
            }
        }

        H.clear(); H.shrink_to_fit();
        P.clear(); P.shrink_to_fit();

        std::cout << "          -> Ch" << ch << ": Masks computed. Temporaries freed.\n";
    }

    std::cout << "          -> HPSS complete (" << channels << " channels, "
              << M_total << " frames). Independent stereo masks ready.\n";
}
