#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// In-process entry point for the warptempo DSP pipeline. The standalone
// binary's main() parses CLI into one of these and calls
// run_warptempo_engine(); the GUI does the same thing without shelling out.
struct EngineParams {
    std::string source_audio_path;
    std::string output_audio_path;
    std::vector<std::pair<size_t, size_t>> timemap;  // src_frame, tgt_frame

    int    N                          = 4096;
    int    fftw_threads               = 0;   // 0 = auto
    bool   transients_enabled         = true;
    double transients_xover_low       = 120.0;
    double transients_xover_high      = 3500.0;
    double transients_tau_back_ms     = 30.0;
    double transients_thresh_db       = -20.0;
    double transients_refractory_ms   = 1500.0;
    double transients_anticipation_ms = 100.0;
    bool   transients_diag            = false;
    bool   limiter_enabled            = true;
    double limiter_ceiling_dbfs       = -0.3;
    double limiter_tolerance_db       = 0.01;
    int    limiter_num_bands          = 0;
    bool   limiter_diag               = false;
    bool   output_24bit_pcm           = false;
};

// Returns true on success, false on failure. Failure reasons are logged to
// stderr by the engine itself (unchanged text from the standalone binary).
bool run_warptempo_engine(const EngineParams& p);
