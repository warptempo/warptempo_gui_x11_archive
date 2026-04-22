#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fftw3.h>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "transients.h"
#include "limiter.h"
#include "synthesis.h"

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <source_audio> <timemap_file> <source_audio_md5> [key=value ...]\n"
                  << "\n"
                  << "  <source_audio_md5>  MD5 hex digest of the original (untrimmed) source\n"
                  << "                      audio used when generating the timemap. Passed\n"
                  << "                      explicitly so trimmed or resampled variants of the\n"
                  << "                      source do not silently produce mismatched output paths.\n"
                  << "\n"
                  << "  N=4096                                FFT size (divisible by 4)\n"
                  << "  fftw_threads=0                        FFTW internal thread count (0 = hardware_concurrency / 2)\n"
                  << "\n"
                  << "  transients_enabled=true               Run detector (false produces zero markers)\n"
                  << "  transients_xover_low=120.0            Detection band lower edge (Hz)\n"
                  << "  transients_xover_high=3500.0          Detection band upper edge (Hz)\n"
                  << "  transients_tau_back_ms=30.0           Backward IIR time constant\n"
                  << "  transients_thresh_db=-20.0            dB threshold for upward crossings\n"
                  << "  transients_refractory_ms=1500.0       Target-time refractory between detections\n"
                  << "  transients_anticipation_ms=100.0      Shift detections earlier (ms)\n"
                  << "  transients_diag=false                 Write <output>-diag.wav with dirac spikes (mono)\n"
                  << "\n"
                  << "  limiter_enabled=true              Run the spectral limiter pass\n"
                  << "  limiter_ceiling_dbfs=-0.3         Peak ceiling in dBFS\n"
                  << "  limiter_tolerance_db=0.01         Half-width of success band centered on ceiling\n"
                  << "  limiter_num_bands=0               Band count override (0 = auto 1/3-octave grid)\n"
                  << "  limiter_diag=false                Write mono diagnostic spike WAV for limiter\n"
                  << "  clipper_enabled=false             Hard clip output at ceiling after limiter\n"
;
        return 1;
    }

    const std::string md5 = argv[3];
    if (md5.size() != 32) {
        std::cerr << "Error: <source_audio_md5> must be a 32-character hex digest, got '"
                  << md5 << "'.\n";
        return 1;
    }

    // --- Parse CLI (key=value) — argv[4+] ---
    AudioSTFT audio_stft;

    std::unordered_map<std::string, std::string> kv;
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq != std::string::npos)
            kv[arg.substr(0, eq)] = arg.substr(eq + 1);
    }
    auto ki = [&](const char* k, int d) -> int {
        auto it = kv.find(k); return (it != kv.end()) ? std::stoi(it->second) : d;
    };
    auto kd = [&](const char* k, double d) -> double {
        auto it = kv.find(k); return (it != kv.end()) ? std::stod(it->second) : d;
    };
    auto kb = [&](const char* k, bool d) -> bool {
        auto it = kv.find(k);
        if (it == kv.end()) return d;
        const std::string& v = it->second;
        return (v == "true" || v == "1" || v == "yes" || v == "on");
    };

    audio_stft.N            = ki("N",           4096);

    // --- FFTW threading ---
    // Default to hardware concurrency; CLI may override. 0 from either source
    // means "auto-detect"; a non-zero CLI value (including 1) is honored verbatim.
    int fftw_threads = ki("fftw_threads", 0);
    if (fftw_threads <= 0) {
        // hardware_concurrency() returns logical CPUs, which over-subscribes SMT
        // for compute-bound FFT work. /2 approximates physical cores; override via CLI.
        unsigned hc = std::thread::hardware_concurrency();
        fftw_threads = static_cast<int>(std::max(1u, hc / 2));
    }
    // FFTW threading is deterministic for a given thread count — the transform
    // result is mathematically identical to the serial version. Bit-identical
    // output across runs holds as long as thread count is held constant;
    // different counts can produce results that differ in the last few bits
    // due to different internal summation orders. Regression tests comparing
    // against prior renders must lock fftw_threads to match.
    if (fftw_init_threads()) {
        fftw_plan_with_nthreads(fftw_threads);
        audio_stft.fftw_threads_inited = true;
    } else {
        std::cerr << "  ! fftw_init_threads failed; FFTW will run single-threaded.\n";
    }

    auto& tp = audio_stft.transients_params;
    tp.enabled              = kb("transients_enabled",              tp.enabled);
    tp.xover_low            = kd("transients_xover_low",            tp.xover_low);
    tp.xover_high           = kd("transients_xover_high",           tp.xover_high);
    tp.tau_back_ms          = kd("transients_tau_back_ms",          tp.tau_back_ms);
    tp.thresh_db            = kd("transients_thresh_db",            tp.thresh_db);
    tp.refractory_ms        = kd("transients_refractory_ms",        tp.refractory_ms);
    tp.anticipation_ms      = kd("transients_anticipation_ms",      tp.anticipation_ms);
    audio_stft.transients_diag = kb("transients_diag", false);

    auto& lp = audio_stft.limiter_params;
    lp.enabled              = kb("limiter_enabled",         lp.enabled);
    lp.ceiling_dbfs         = kd("limiter_ceiling_dbfs",    lp.ceiling_dbfs);
    lp.tolerance_db         = kd("limiter_tolerance_db",    lp.tolerance_db);
    lp.num_bands_override   = ki("limiter_num_bands",       lp.num_bands_override);
    lp.diag                 = kb("limiter_diag",            lp.diag);
    lp.clipper_enabled      = kb("clipper_enabled",         lp.clipper_enabled);

    // --- Derive output path from MD5 ---
    audio_stft.output_audio_file = "." + md5 + "-tmp.wav";

    if (audio_stft.N % 4 != 0) {
        std::cerr << "Error: N must be divisible by 4.\n";
        return 1;
    }

    // --- Parse Timemap ---
    std::ifstream file(argv[2]);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open timemap.\n";
        return 1;
    }
    size_t src_f, tgt_f;
    while (file >> src_f >> tgt_f) audio_stft.timemap.push_back({src_f, tgt_f});
    file.close();

    for (size_t i = 1; i < audio_stft.timemap.size(); ++i) {
        if (audio_stft.timemap[i].src_frame <= audio_stft.timemap[i - 1].src_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic src_frame ("
                      << audio_stft.timemap[i - 1].src_frame << " -> "
                      << audio_stft.timemap[i].src_frame << ").\n";
            return 1;
        }
        if (audio_stft.timemap[i].tgt_frame <= audio_stft.timemap[i - 1].tgt_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic tgt_frame ("
                      << audio_stft.timemap[i - 1].tgt_frame << " -> "
                      << audio_stft.timemap[i].tgt_frame << ").\n";
            return 1;
        }
    }

    audio_stft.src_info.format = 0;
    audio_stft.src_snd = sf_open(argv[1], SFM_READ, &audio_stft.src_info);
    if (!audio_stft.src_snd) {
        std::cerr << "Error: Could not open source file: '" << argv[1] << "'\n";
        return 1;
    }
    audio_stft.channels = audio_stft.src_info.channels;
    audio_stft.nyquist = audio_stft.src_info.samplerate / 2.0;
    audio_stft.bin_hz_width = static_cast<double>(audio_stft.src_info.samplerate) / audio_stft.N;
    audio_stft.target_total_frames = audio_stft.timemap.back().tgt_frame + audio_stft.N;

    audio_stft.init_fftw();
    audio_stft.frame_map = audio_stft.generate_frame_map();

    // Attenuation map defaults to identity; limiter may modify it in place.
    audio_stft.attenuation_map.assign(audio_stft.frame_map.size(),
        std::vector<double>(audio_stft.num_bands, 1.0));

    // Consolidated header
    double duration_sec = static_cast<double>(audio_stft.target_total_frames) /
                          audio_stft.src_info.samplerate;
    std::cout << "[WarpTempo] Source: " << argv[1]
              << ", Target: " << std::fixed << duration_sec << "s at "
              << audio_stft.src_info.samplerate << "Hz\n";

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // ========================================================================
    // Note to the future Pass 3 (reset_check.cpp) implementer: the RMS
    // trajectory loop over hop indices is per-iteration independent and
    // should carry `#pragma omp parallel for`, matching the pattern used
    // elsewhere in this pipeline. Deferred from the initial OpenMP/FFTW
    // threading commit because the pass does not yet exist.
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

    // --- Transient diagnostic spike file (MONO) ---
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
    return 0;
}
