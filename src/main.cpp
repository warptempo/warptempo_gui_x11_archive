#include <iostream>
#include <fstream>
#include <cstdio>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "eq_matcher.h"
#include "dynamics_lr4.h"
#include "synthesis.h"
#include "visualizer.h"

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <source_audio> <timemap_file> <target_audio> "
                  << "[N=3328] [low_thresh_hz=50.0]\n"
                  << "       [thresh_L/M/H=-25,-20,-25] [knee=6.0] [atten_max=-24.0]\n"
                  << "       [tauB_L/M/H=50,30,20] [tauF_L/M/H=250,150,80] [depth_L/M/H=1.0,1.0,1.0]\n"
                  << "       [xover_L,H=120,3500]\n";
        return 1;
    }

    // --- Parse CLI ---
    AudioSTFT audio_stft;

    std::string src_audio_file = argv[1];
    std::string map_file = argv[2];
    audio_stft.tgt_audio_file = argv[3];

    audio_stft.N = (argc >= 5) ? std::stoi(argv[4]) : 3328;
    audio_stft.low_threshold_hz = (argc >= 6) ? std::stod(argv[5]) : 50.0;

    auto& dp = audio_stft.dyn_params;
    if (argc >= 7)  std::sscanf(argv[6], "%lf,%lf,%lf", &dp.thresh_low, &dp.thresh_mid, &dp.thresh_high);
    if (argc >= 8)  dp.knee_low = dp.knee_mid = dp.knee_high = std::stod(argv[7]);
    if (argc >= 9)  dp.atten_low = dp.atten_mid = dp.atten_high = std::stod(argv[8]);
    if (argc >= 10) std::sscanf(argv[9], "%lf,%lf,%lf", &dp.tau_B_low, &dp.tau_B_mid, &dp.tau_B_high);
    if (argc >= 11) std::sscanf(argv[10], "%lf,%lf,%lf", &dp.tau_F_low, &dp.tau_F_mid, &dp.tau_F_high);
    if (argc >= 12) std::sscanf(argv[11], "%lf,%lf,%lf", &dp.depth_low, &dp.depth_mid, &dp.depth_high);
    if (argc >= 13) std::sscanf(argv[12], "%lf,%lf", &dp.xover_low, &dp.xover_high);

    if (audio_stft.N % 4 != 0) {
        std::cerr << "Error: N must be divisible by 4.\n";
        return 1;
    }

    // --- Parse Timemap ---
    std::ifstream file(map_file);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open timemap.\n";
        return 1;
    }
    size_t src_f, tgt_f;
    while (file >> src_f >> tgt_f) audio_stft.timemap.push_back({src_f, tgt_f});
    file.close();

    // --- Open Source Audio ---
    audio_stft.src_info.format = 0;
    audio_stft.src_snd = sf_open(src_audio_file.c_str(), SFM_READ, &audio_stft.src_info);
    if (!audio_stft.src_snd) {
        std::cerr << "Error: Could not open Source file: '" << src_audio_file << "'\n";
        return 1;
    }
    audio_stft.channels = audio_stft.src_info.channels;
    audio_stft.nyquist = audio_stft.src_info.samplerate / 2.0;
    audio_stft.bin_hz_width = static_cast<double>(audio_stft.src_info.samplerate) / audio_stft.N;
    audio_stft.target_total_frames = audio_stft.timemap.back().tgt_frame + audio_stft.N;

    // --- Init FFTW ---
    audio_stft.init_fftw();

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // Future insertion point: hpss.process(audio_stft) immediately after stft_engine
    // ========================================================================
    PhaseVocoder stft_engine;
    EQMatcher    eq_matcher;
    DynamicsLR4  dynamics_lr4;
    Visualizer   visualizer;
    Synthesis    synthesis;

    stft_engine.process(audio_stft);
    eq_matcher.process(audio_stft);
    visualizer.render_eq(audio_stft);
    dynamics_lr4.process(audio_stft);
    visualizer.render_dynamics(audio_stft);
    synthesis.process(audio_stft);

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
