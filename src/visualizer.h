#pragma once
#include "stft_container.h"

class Visualizer {
public:
    void render_eq(const AudioSTFT& stft);
    void render_dynamics(const AudioSTFT& stft);
};
