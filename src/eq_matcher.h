#pragma once
#include "stft_container.h"

// --- EBUR128 Block Detection Tuning ---
static constexpr double ATTACK_TOLERANCE_LU  =  8.0;  // LU below global LUFS → attack gate
static constexpr double RELEASE_TOLERANCE_LU = 12.0;  // LU below global LUFS → release gate
static constexpr double MIN_GAP_SEC          =  0.5;  // Minimum silence gap before new block
static constexpr double MIN_PHRASE_SEC       =  2.0;  // Discard blocks shorter than this
static constexpr int    TOP_X_CHUNKS         =  5;    // Keep only the N longest blocks for PSD

struct AcousticBlock {
    size_t start_frame;
    size_t end_frame;
    double duration_sec;
};

class EQMatcher {
public:
    void process(AudioSTFT& stft);
};
