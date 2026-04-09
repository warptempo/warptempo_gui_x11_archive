#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "hpss.h"
#include "synthesis.h"

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <source_audio> <timemap_file> <target_audio_percussive> <target_audio_harmonic> [key=value ...]\n"
                  << "\n"
                  << "  N=4096              FFT size (divisible by 4)\n"
                  << "  beta=2.0            HPSS mask sharpness exponent\n"
                  << "  L_h=31              Horizontal context half-width (frames)\n"
                  << "  L_p=7               Vertical filter max clamp (bins)\n"
                  << "  hpf_hz=30           Zero-phase sub-rumble HPF cutoff (Hz; 0=bypass)\n";
        return 1;
    }

    // --- Parse CLI (key=value) ---
    AudioSTFT audio_stft;

    audio_stft.perc_audio_file     = argv[3];
    audio_stft.harmonic_audio_file = argv[4];

    std::unordered_map<std::string, std::string> kv;
    for (int i = 5; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq != std::string::npos)
            kv[arg.substr(0, eq)] = arg.substr(eq + 1);
    }
    auto kd = [&](const char* k, double d) -> double {
        auto it = kv.find(k); return (it != kv.end()) ? std::stod(it->second) : d;
    };
    auto ki = [&](const char* k, int d) -> int {
        auto it = kv.find(k); return (it != kv.end()) ? std::stoi(it->second) : d;
    };
    audio_stft.N                = ki("N",           4096);
    audio_stft.beta             = kd("beta",          2.0);
    audio_stft.L_h              = ki("L_h",            31);
    audio_stft.L_p_max          = ki("L_p",             7);
    audio_stft.hpf_hz           = kd("hpf_hz",       30.0);

    if (audio_stft.N % 4 != 0) {
        std::cerr << "Error: N must be divisible by 4.\n";
        return 1;
    }

    // --- Parse Timemap ---
    std::ifstream file(argv[2]);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open timemap.\n";
        return 1;
    }
    size_t src_f, tgt_f;
    while (file >> src_f >> tgt_f) audio_stft.timemap.push_back({src_f, tgt_f});
    file.close();

    // --- Open Source Audio ---
    audio_stft.src_info.format = 0;
    audio_stft.src_snd = sf_open(argv[1], SFM_READ, &audio_stft.src_info);
    if (!audio_stft.src_snd) {
        std::cerr << "Error: Could not open source file: '" << argv[1] << "'\n";
        return 1;
    }
    audio_stft.channels = audio_stft.src_info.channels;
    audio_stft.nyquist = audio_stft.src_info.samplerate / 2.0;
    audio_stft.bin_hz_width = static_cast<double>(audio_stft.src_info.samplerate) / audio_stft.N;
    audio_stft.target_total_frames = audio_stft.timemap.back().tgt_frame + audio_stft.N;

    // --- Init FFTW ---
    audio_stft.init_fftw();

    // --- Generate canonical frame map (once, shared by all passes) ---
    audio_stft.frame_map = audio_stft.generate_frame_map();
    std::cout << "[Frame Map] Generated " << audio_stft.frame_map.size() << " frames (int64_t).\n";

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // ========================================================================
    PhaseVocoder stft_engine;
    HPSS         hpss;
    Synthesis    synthesis;

    stft_engine.process(audio_stft);
    hpss.process(audio_stft);
    synthesis.process(audio_stft);

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
