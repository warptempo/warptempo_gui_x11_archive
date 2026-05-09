#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Parameter struct for the warptempo DSP pipeline. Constructed by the GUI's
// render pipeline and passed to run_warptempo_engine().
struct EngineParams {
    std::string source_audio_path;
    std::string output_audio_path;
    std::vector<std::pair<size_t, size_t>> timemap;  // src_frame, tgt_frame

    int    N                          = 4096;
    int    fftw_threads               = 0;   // 0 = auto
    bool   limiter_enabled            = true;
    double limiter_ceiling_dbfs       = -0.3;
    double limiter_tolerance_db       = 0.01;
    int    limiter_num_bands          = 0;
    bool   limiter_diag               = false;
    bool   output_24bit_pcm           = false;

    // User-curated phase reset frame list (source-frame domain). When non-empty,
    // the engine skips its internal phase reset detection and uses this list
    // verbatim for phase reset positioning. Must be sorted ascending.
    // Typical source: GUI's phase reset mode, providing the union of inserted
    // + active-detected (with displacement applied) entries.
    std::vector<int64_t> phase_reset_frames;
};

// Returns true on success, false on failure. Failure reasons are logged to
// stderr by the engine itself.
bool run_warptempo_engine(const EngineParams& p,
                          std::vector<int64_t>* out_frame_map = nullptr,
                          int* out_R_s = nullptr);
