#pragma once
#include "stft_container.h"

class DynamicsLR4 {
public:
    void process(AudioSTFT& stft);
};
