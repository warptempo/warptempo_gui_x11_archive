#include "eq_matcher.h"
#include <ebur128.h>
#include <iostream>
#include <algorithm>
#include <cmath>

void EQMatcher::process(AudioSTFT& stft) {
    std::cout << "\n[Pass 2] EQ Diagnostic: PSD Delta Analysis...\n";

    int N = stft.N;
    int channels = stft.channels;

    // --- EBUR128 Loudness Profiling ---
    ebur128_state* st = ebur128_init(channels, stft.src_info.samplerate,
                                     EBUR128_MODE_M | EBUR128_MODE_I);

    std::vector<float> ebur_buf(4096 * channels);
    sf_seek(stft.src_snd, 0, SEEK_SET);
    while (sf_readf_float(stft.src_snd, ebur_buf.data(), 4096) > 0)
        ebur128_add_frames_float(st, ebur_buf.data(), 4096);

    double src_global_lufs;
    ebur128_loudness_global(st, &src_global_lufs);
    double attack_thresh  = src_global_lufs - ATTACK_TOLERANCE_LU;
    double release_thresh = src_global_lufs - RELEASE_TOLERANCE_LU;

    // --- Acoustic Block Detection ---
    std::vector<AcousticBlock> blocks;
    bool is_loud = false;
    size_t current_start = 0, current_frame = 0;
    sf_seek(stft.src_snd, 0, SEEK_SET);
    while (true) {
        sf_count_t read_count = sf_readf_float(stft.src_snd, ebur_buf.data(), 4096);
        if (read_count <= 0) break;
        ebur128_add_frames_float(st, ebur_buf.data(), read_count);
        current_frame += read_count;
        double momentary;
        ebur128_loudness_momentary(st, &momentary);
        if (!is_loud && momentary >= attack_thresh) {
            is_loud = true;
            current_start = current_frame;
        } else if (is_loud && momentary < release_thresh) {
            is_loud = false;
            blocks.push_back({current_start, current_frame, 0.0});
        }
    }
    if (is_loud) blocks.push_back({current_start, current_frame, 0.0});
    ebur128_destroy(&st);

    // --- Merge & Filter Blocks ---
    std::vector<AcousticBlock> merged_blocks;
    size_t min_gap = static_cast<size_t>(MIN_GAP_SEC * stft.src_info.samplerate);
    for (const auto& b : blocks) {
        if (merged_blocks.empty()) merged_blocks.push_back(b);
        else {
            if ((b.start_frame - merged_blocks.back().end_frame) < min_gap)
                merged_blocks.back().end_frame = b.end_frame;
            else
                merged_blocks.push_back(b);
        }
    }

    std::vector<AcousticBlock> final_blocks;
    for (auto& b : merged_blocks) {
        b.duration_sec = static_cast<double>(b.end_frame - b.start_frame) / stft.src_info.samplerate;
        if (b.duration_sec >= MIN_PHRASE_SEC) final_blocks.push_back(b);
    }
    std::sort(final_blocks.begin(), final_blocks.end(),
              [](const AcousticBlock& a, const AcousticBlock& b) { return a.duration_sec > b.duration_sec; });
    if (final_blocks.size() > static_cast<size_t>(TOP_X_CHUNKS))
        final_blocks.resize(TOP_X_CHUNKS);

    std::cout << "          -> " << final_blocks.size() << " loud blocks selected for PSD analysis.\n";

    // --- PSD Computation ---
    std::vector<double> src_psd_linked(N / 2 + 1, 0.0);
    std::vector<double> tgt_psd_linked(N / 2 + 1, 0.0);
    size_t total_psd_windows = 0;
    std::vector<float> src_chunk(N * channels), tgt_chunk(N * channels, 0.0f);

    for (const auto& b : final_blocks) {
        size_t s_frame = b.start_frame;
        while (s_frame + N <= b.end_frame) {
            size_t t_start = static_cast<size_t>(map_source_to_target(s_frame, stft.timemap));

            sf_seek(stft.src_snd, s_frame, SEEK_SET);
            sf_readf_float(stft.src_snd, src_chunk.data(), N);

            std::fill(tgt_chunk.begin(), tgt_chunk.end(), 0.0f);
            if (t_start + N <= stft.virtual_tgt_buf.size() / channels) {
                std::copy(stft.virtual_tgt_buf.begin() + t_start * channels,
                          stft.virtual_tgt_buf.begin() + (t_start + N) * channels,
                          tgt_chunk.begin());
            }

            for (int ch = 0; ch < channels; ++ch) {
                for (int i = 0; i < N; ++i)
                    stft.fft_in[i] = src_chunk[i * channels + ch] * stft.window[i];
                fftw_execute(stft.plan_fwd);
                for (int k = 1; k <= N / 2; ++k)
                    src_psd_linked[k] += stft.fft_out[k][0] * stft.fft_out[k][0]
                                       + stft.fft_out[k][1] * stft.fft_out[k][1];

                for (int i = 0; i < N; ++i)
                    stft.fft_in[i] = tgt_chunk[i * channels + ch] * stft.window[i];
                fftw_execute(stft.plan_fwd);
                for (int k = 1; k <= N / 2; ++k)
                    tgt_psd_linked[k] += stft.fft_out[k][0] * stft.fft_out[k][0]
                                       + stft.fft_out[k][1] * stft.fft_out[k][1];
            }
            total_psd_windows++;
            s_frame += N / 2;
        }
    }

    // --- Raw PSD Delta ---
    // Delta_dB[k] = 10*log10(PSD_src[k]/N) - 10*log10(PSD_tgt[k]/N)
    stft.raw_delta_db.assign(N / 2 + 1, 0.0);
    double total_measurements = static_cast<double>(total_psd_windows * channels);

    for (int k = 1; k <= N / 2; ++k) {
        if (src_psd_linked[k] > 0.0 && tgt_psd_linked[k] > 0.0) {
            stft.raw_delta_db[k] = 10.0 * std::log10(src_psd_linked[k] / total_measurements)
                                 - 10.0 * std::log10(tgt_psd_linked[k] / total_measurements);
        }
    }

    std::cout << "          -> raw_delta_db computed (" << (N / 2) << " bins).\n";
}
