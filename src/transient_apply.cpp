#include "transient_apply.h"
#include "synthesis.h"

void TransientApply::process(AudioSTFT& stft) {
    auto noop_write = [](const float*, size_t) {};
    Synthesis::synthesize_full(
        stft,
        /*apply_clipper=*/false,
        /*spectra_cache=*/nullptr,
        noop_write,
        /*lock_transient_resets=*/true,
        /*show_progress=*/true,
        /*pass_label=*/"[Pass 3/5] Transient apply.................. ");
}
