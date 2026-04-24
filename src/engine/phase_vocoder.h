#pragma once
#include "stft_container.h"

class PhaseVocoder {
public:
    void process(AudioSTFT& stft);
};
