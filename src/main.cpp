#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/engine.h"

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
;
        return 1;
    }

    const std::string md5 = argv[3];
    if (md5.size() != 32) {
        std::cerr << "Error: <source_audio_md5> must be a 32-character hex digest, got '"
                  << md5 << "'.\n";
        return 1;
    }

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

    EngineParams params;
    params.source_audio_path = argv[1];
    params.output_audio_path = "." + md5 + "-tmp.wav";

    params.N                          = ki("N",                           params.N);
    params.fftw_threads               = ki("fftw_threads",                params.fftw_threads);
    params.transients_enabled         = kb("transients_enabled",          params.transients_enabled);
    params.transients_xover_low       = kd("transients_xover_low",        params.transients_xover_low);
    params.transients_xover_high      = kd("transients_xover_high",       params.transients_xover_high);
    params.transients_tau_back_ms     = kd("transients_tau_back_ms",      params.transients_tau_back_ms);
    params.transients_thresh_db       = kd("transients_thresh_db",        params.transients_thresh_db);
    params.transients_refractory_ms   = kd("transients_refractory_ms",    params.transients_refractory_ms);
    params.transients_anticipation_ms = kd("transients_anticipation_ms",  params.transients_anticipation_ms);
    params.transients_diag            = kb("transients_diag",             params.transients_diag);
    params.limiter_enabled            = kb("limiter_enabled",             params.limiter_enabled);
    params.limiter_ceiling_dbfs       = kd("limiter_ceiling_dbfs",        params.limiter_ceiling_dbfs);
    params.limiter_tolerance_db       = kd("limiter_tolerance_db",        params.limiter_tolerance_db);
    params.limiter_num_bands          = ki("limiter_num_bands",           params.limiter_num_bands);
    params.limiter_diag               = kb("limiter_diag",                params.limiter_diag);

    std::ifstream file(argv[2]);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open timemap.\n";
        return 1;
    }
    size_t src_f, tgt_f;
    while (file >> src_f >> tgt_f) params.timemap.push_back({src_f, tgt_f});
    file.close();

    return run_warptempo_engine(params) ? 0 : 1;
}
