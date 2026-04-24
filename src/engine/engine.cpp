#include "engine.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fftw3.h>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "transients.h"
#include "limiter.h"
#include "synthesis.h"

bool run_warptempo_engine(const EngineParams& p) {
    AudioSTFT audio_stft;

    audio_stft.N = p.N;

    int fftw_threads = p.fftw_threads;
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

    auto& tp = audio_stft.transients_params;
    tp.enabled              = p.transients_enabled;
    tp.xover_low            = p.transients_xover_low;
    tp.xover_high           = p.transients_xover_high;
    tp.tau_back_ms          = p.transients_tau_back_ms;
    tp.thresh_db            = p.transients_thresh_db;
    tp.refractory_ms        = p.transients_refractory_ms;
    tp.anticipation_ms      = p.transients_anticipation_ms;
    audio_stft.transients_diag = p.transients_diag;

    auto& lp = audio_stft.limiter_params;
    lp.enabled              = p.limiter_enabled;
    lp.ceiling_dbfs         = p.limiter_ceiling_dbfs;
    lp.tolerance_db         = p.limiter_tolerance_db;
    lp.num_bands_override   = p.limiter_num_bands;
    lp.diag                 = p.limiter_diag;

    audio_stft.output_audio_file = p.output_audio_path;

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
    for (size_t i = 1; i < audio_stft.timemap.size(); ++i) {
        if (audio_stft.timemap[i].src_frame <= audio_stft.timemap[i - 1].src_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic src_frame ("
                      << audio_stft.timemap[i - 1].src_frame << " -> "
                      << audio_stft.timemap[i].src_frame << ").\n";
            return false;
        }
        if (audio_stft.timemap[i].tgt_frame <= audio_stft.timemap[i - 1].tgt_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic tgt_frame ("
                      << audio_stft.timemap[i - 1].tgt_frame << " -> "
                      << audio_stft.timemap[i].tgt_frame << ").\n";
            return false;
        }
    }

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

    PhaseVocoder   stft_engine;
    Transients     detector;
    Limiter        limiter;
    Synthesis      synthesis;

    std::cout << "[Pass 1/4] Analysis + virtual buffer........ " << std::flush;
    stft_engine.process(audio_stft);
    std::cout << "done\n";

    detector.process(audio_stft);
    limiter.process(audio_stft);
    synthesis.process(audio_stft);

    if (audio_stft.transients_diag) {
        SF_INFO probe_info{};
        probe_info.format = 0;
        SNDFILE* probe = sf_open(audio_stft.output_audio_file.c_str(), SFM_READ, &probe_info);
        if (!probe) {
            std::cerr << "  ! could not reopen output to size transient diag file; skipping\n";
        } else {
            const sf_count_t total = probe_info.frames;
            sf_close(probe);

            std::string diag_path = audio_stft.output_audio_file;
            auto dot = diag_path.find_last_of('.');
            if (dot != std::string::npos) diag_path.insert(dot, "-diag");
            else                          diag_path += "-diag";

            SF_INFO diag_info = audio_stft.src_info;
            diag_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
            diag_info.channels = 1;
            SNDFILE* diag_snd = sf_open(diag_path.c_str(), SFM_WRITE, &diag_info);
            if (!diag_snd) {
                std::cerr << "  ! could not create transient diag file '" << diag_path << "'\n";
            } else {
                const int64_t R_s = audio_stft.R_s;
                std::vector<float> diag_buf(static_cast<size_t>(total), 0.0f);

                auto poke = [&](int64_t pos, float amp) {
                    if (pos < 0 || pos >= total) return;
                    diag_buf[static_cast<size_t>(pos)] = amp;
                };

                for (const auto& m : audio_stft.transient_markers) {
                    int64_t center = static_cast<int64_t>(m.synth_frame) * R_s;
                    poke(center - 1, 0.5f);
                    poke(center,     1.0f);
                    poke(center + 1, 0.5f);
                }

                sf_writef_float(diag_snd, diag_buf.data(), total);
                sf_close(diag_snd);
            }
        }
    }

    std::cout << "[Success] " << audio_stft.output_audio_file << "\n";

    audio_stft.cleanup();
    return true;
}
