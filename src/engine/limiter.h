#pragma once
#include "stft_container.h"

class Limiter {
public:
    void process(AudioSTFT& stft);
};
