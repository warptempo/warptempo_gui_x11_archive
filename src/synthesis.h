#pragma once
#include "stft_container.h"
#include <complex>
#include <cstddef>
#include <functional>

class Synthesis {
public:
    void process(AudioSTFT& stft);

    // Shared synthesis helper. Runs full OLA synthesis from frame 0 using
    // stft.attenuation_map. write_cb is invoked with per-iteration chunks (each
    // at most R_s frames; initial iterations contribute less during the N/2
    // start-trim, final flush is up to N - R_s frames). If spectra_cache is
    // non-null, it is filled with M[k]*exp(j*theta[k]) per (frame, channel, bin)
    // as a flat array of size num_frames * channels * (N/2+1). apply_clipper
    // clamps each output sample to +/- ceiling_amplitude.
    //
    // lock_transient_resets=true: run the reset evaluator and record its
    //   chosen frame into each TransientMarker.chosen_reset_frame. The
    //   TransientApply pass uses this mode to freeze reset decisions against a
    //   single reference OLA so downstream passes (limiter measurement, final
    //   synthesis) apply identical resets and produce comparable output.
    // lock_transient_resets=false: for each marker, apply the reset from its
    //   pre-recorded chosen_reset_frame directly. If a marker has no recorded
    //   frame (<0), fall back to running the evaluator and emit a one-shot
    //   warning.
    // show_progress=true: print live progress % via \r, terminating with 100%.
    // pass_label: full prefix printed before each progress tick (caller provides
    //   trailing dots/space to reach the pipeline's 45-char status column).
    static void synthesize_full(
        AudioSTFT& stft,
        bool apply_clipper,
        std::complex<float>* spectra_cache,
        std::function<void(const float*, size_t)> write_cb,
        bool lock_transient_resets,
        bool show_progress,
        const char* pass_label);
};
