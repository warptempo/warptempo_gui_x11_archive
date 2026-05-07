#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fftw3.h>

#include "stft_container.h"
#include "limiter.h"
#include "synthesis.h"

namespace {

// Shared FFTW thread init for full-render and detection-only paths. Sets
// audio_stft.fftw_threads_inited if init succeeded.
void init_fftw_threads(AudioSTFT& audio_stft, int requested_threads) {
    int fftw_threads = requested_threads;
    if (fftw_threads <= 0) {
        unsigned hc = std::thread::hardware_concurrency();
        fftw_threads = static_cast<int>(std::max(1u, hc / 2));
    }
    if (fftw_init_threads()) {
        fftw_plan_with_nthreads(fftw_threads);
        audio_stft.fftw_threads_inited = true;
    } else {
        std::cerr << "  ! fftw_init_threads failed; FFTW will run single-threaded.\n";
    }
}

// Validate strict monotonicity of a (src,tgt) timemap. Returns true if OK.
bool validate_timemap_monotonic(const std::vector<TimeMapSegment>& tm) {
    for (size_t i = 1; i < tm.size(); ++i) {
        if (tm[i].src_frame <= tm[i - 1].src_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic src_frame ("
                      << tm[i - 1].src_frame << " -> "
                      << tm[i].src_frame << ").\n";
            return false;
        }
        if (tm[i].tgt_frame <= tm[i - 1].tgt_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic tgt_frame ("
                      << tm[i - 1].tgt_frame << " -> "
                      << tm[i].tgt_frame << ").\n";
            return false;
        }
    }
    return true;
}

} // namespace

bool run_warptempo_engine(const EngineParams& p,
                          std::vector<int64_t>* out_frame_map,
                          int* out_R_s) {
    AudioSTFT audio_stft;

    audio_stft.N = p.N;

    init_fftw_threads(audio_stft, p.fftw_threads);

    auto& lp = audio_stft.limiter_params;
    lp.enabled              = p.limiter_enabled;
    lp.ceiling_dbfs         = p.limiter_ceiling_dbfs;
    lp.tolerance_db         = p.limiter_tolerance_db;
    lp.num_bands_override   = p.limiter_num_bands;
    lp.diag                 = p.limiter_diag;

    audio_stft.output_audio_file = p.output_audio_path;
    audio_stft.output_24bit_pcm  = p.output_24bit_pcm;

    if (audio_stft.N % 4 != 0) {
        std::cerr << "Error: N must be divisible by 4.\n";
        return false;
    }

    // Populate timemap from caller and validate monotonicity.
    audio_stft.timemap.clear();
    audio_stft.timemap.reserve(p.timemap.size());
    for (const auto& e : p.timemap) {
        audio_stft.timemap.push_back({e.first, e.second});
    }
    if (!validate_timemap_monotonic(audio_stft.timemap)) return false;

    audio_stft.src_info.format = 0;
    audio_stft.src_snd = sf_open(p.source_audio_path.c_str(), SFM_READ, &audio_stft.src_info);
    if (!audio_stft.src_snd) {
        std::cerr << "Error: Could not open source file: '" << p.source_audio_path << "'\n";
        return false;
    }
    audio_stft.channels = audio_stft.src_info.channels;
    audio_stft.nyquist = audio_stft.src_info.samplerate / 2.0;
    audio_stft.bin_hz_width = static_cast<double>(audio_stft.src_info.samplerate) / audio_stft.N;
    audio_stft.target_total_frames = audio_stft.timemap.back().tgt_frame + audio_stft.N;

    audio_stft.init_fftw();
    audio_stft.frame_map = audio_stft.generate_frame_map();

    audio_stft.attenuation_map.assign(audio_stft.frame_map.size(),
        std::vector<double>(audio_stft.num_bands, 1.0));

    double duration_sec = static_cast<double>(audio_stft.target_total_frames) /
                          audio_stft.src_info.samplerate;
    std::cout << "[WarpTempo] Source: " << p.source_audio_path
              << ", Target: " << std::fixed << duration_sec << "s at "
              << audio_stft.src_info.samplerate << "Hz\n";

    Limiter        limiter;
    Synthesis      synthesis;

    auto pass_ms = [](std::chrono::steady_clock::time_point t0,
                      std::chrono::steady_clock::time_point t1) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    };

    auto t_p2_0 = std::chrono::steady_clock::now();
    audio_stft.transient_markers.clear();
    audio_stft.transient_markers.reserve(p.transient_frames.size());
    const auto& fm = audio_stft.frame_map;
    for (int64_t F : p.transient_frames) {
        auto it = std::upper_bound(fm.begin(), fm.end(), F);
        if (it == fm.begin()) continue;
        --it;
        size_t s = static_cast<size_t>(it - fm.begin());
        if (s >= fm.size()) continue;
        audio_stft.transient_markers.push_back(
            {static_cast<int>(s), F});
    }
    std::cout << "[Pass 1/3] Transient placement............... "
              << audio_stft.transient_markers.size()
              << " transients\n";
    auto t_p2_1 = std::chrono::steady_clock::now();
    std::cout << "  (" << pass_ms(t_p2_0, t_p2_1) << " ms)\n";

    auto t_p3_0 = std::chrono::steady_clock::now();
    limiter.process(audio_stft);
    auto t_p3_1 = std::chrono::steady_clock::now();
    std::cout << "  (" << pass_ms(t_p3_0, t_p3_1) << " ms)\n";

    auto t_p4_0 = std::chrono::steady_clock::now();
    synthesis.process(audio_stft);
    auto t_p4_1 = std::chrono::steady_clock::now();
    std::cout << "  (" << pass_ms(t_p4_0, t_p4_1) << " ms)\n";

    std::cout << "[Success] " << audio_stft.output_audio_file << "\n";

    if (out_frame_map) *out_frame_map = audio_stft.frame_map;
    if (out_R_s)       *out_R_s       = audio_stft.R_s;

    audio_stft.cleanup();
    return true;
}
