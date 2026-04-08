#include <iostream>
#include <fstream>
#include <cstdio>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "hpss.h"
#include "eq_matcher.h"
#include "dynamics_lr4.h"
#include "synthesis.h"
#include "visualizer.h"

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <source_audio> <timemap_file> <target_audio> "
                  << "[N=4096] [low_thresh_hz=50.0]\n"
                  << "       [thresh_L/M/H=-25,-20,-25] [knee=6.0] [atten_max=-24.0]\n"
                  << "       [tauB_L/M/H=50,30,20] [tauF_L/M/H=250,150,80] [depth_L/M/H=1.0,1.0,1.0]\n"
                  << "       [xover_L,H=120,3500]\n"
                  << "       [hpss_Gfg,Gbg,beta=1.5,1.0,0.5] [hpss_Lh,Lpmax=31,7]\n"
                  << "       [active_modules=hpss,makeup]  -- comma-separated whitelist of DSP modules to run.\n"
                  << "          Pass 'eq,dyn,hpss,makeup' to run all, or e.g. 'hpss,makeup' for HPSS only (default).\n"
                  << "          Omit 'makeup' to skip loudness compensation (e.g. 'hpss').\n"
                  << "          PhaseVocoder and Synthesis always run regardless of this flag.\n"
                  << "       [tm_params=3,0.01,0.501]  -- Transient Matcher: W_frames,RMS_floor,RMS_peak\n"
                  << "       [tm_xovers=50,500,2000]   -- Transient Matcher LR4 crossovers in Hz: xover_0,xover_1,xover_2\n";
        return 1;
    }

    // --- Parse CLI ---
    AudioSTFT audio_stft;

    std::string src_audio_file = argv[1];
    std::string map_file = argv[2];
    audio_stft.tgt_audio_file = argv[3];

    audio_stft.N = (argc >= 5) ? std::stoi(argv[4]) : 4096;
    audio_stft.low_threshold_hz = (argc >= 6) ? std::stod(argv[5]) : 50.0;

    auto& dp = audio_stft.dyn_params;
    if (argc >= 7)  std::sscanf(argv[6], "%lf,%lf,%lf", &dp.thresh_low, &dp.thresh_mid, &dp.thresh_high);
    if (argc >= 8)  dp.knee_low = dp.knee_mid = dp.knee_high = std::stod(argv[7]);
    if (argc >= 9)  dp.atten_low = dp.atten_mid = dp.atten_high = std::stod(argv[8]);
    if (argc >= 10) std::sscanf(argv[9], "%lf,%lf,%lf", &dp.tau_B_low, &dp.tau_B_mid, &dp.tau_B_high);
    if (argc >= 11) std::sscanf(argv[10], "%lf,%lf,%lf", &dp.tau_F_low, &dp.tau_F_mid, &dp.tau_F_high);
    if (argc >= 12) std::sscanf(argv[11], "%lf,%lf,%lf", &dp.depth_low, &dp.depth_mid, &dp.depth_high);
    if (argc >= 13) std::sscanf(argv[12], "%lf,%lf", &dp.xover_low, &dp.xover_high);

    // HPSS CLI parsing
    if (argc >= 14) std::sscanf(argv[13], "%lf,%lf,%lf", &audio_stft.G_fg, &audio_stft.G_bg, &audio_stft.beta);
    if (argc >= 15) std::sscanf(argv[14], "%d,%d", &audio_stft.L_h, &audio_stft.L_p_max);

    std::string active_modules = "hpss,makeup";
    if (argc >= 16) active_modules = argv[15];

    // Transient Matcher CLI parsing
    if (argc >= 17) std::sscanf(argv[16], "%d,%lf,%lf",
                                &audio_stft.tm_W_frames,
                                &audio_stft.tm_RMS_floor,
                                &audio_stft.tm_RMS_peak);
    if (argc >= 18) std::sscanf(argv[17], "%lf,%lf,%lf",
                                &audio_stft.tm_xover_0,
                                &audio_stft.tm_xover_1,
                                &audio_stft.tm_xover_2);

    audio_stft.enable_makeup_gain = active_modules.find("makeup") != std::string::npos;

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

    // --- Generate canonical frame map (once, shared by all passes) ---
    audio_stft.frame_map = audio_stft.generate_frame_map();
    std::cout << "[Frame Map] Generated " << audio_stft.frame_map.size() << " frames (int64_t).\n";

    // --- Pre-calculate LR4 Transient Matcher frequency weights ---
    // Four bands sum to exactly 1.0 at every bin by construction:
    //   W_Z0 = LP(xover_0)
    //   W_Z3 = HP(xover_2)
    //   middle = 1 - W_Z0 - W_Z3                  (guaranteed >= 0 inside passband)
    //   W_Z1 = middle * LP(xover_1)
    //   W_Z2 = middle * HP(xover_1)
    //   Sum  = W_Z0 + middle*(LP1 + HP1) + W_Z3 = W_Z0 + middle + W_Z3 = 1
    {
        const int K = audio_stft.N / 2 + 1;
        audio_stft.tm_W_Z0.resize(K);
        audio_stft.tm_W_Z1.resize(K);
        audio_stft.tm_W_Z2.resize(K);
        audio_stft.tm_W_Z3.resize(K);

        // LR4 power responses: LP(f,fc) = 1/(1+(f/fc)^8), HP = 1 - LP.
        // The power-of-8 exponent yields a 24 dB/oct roll-off and guarantees
        // LP(fc) = HP(fc) = 0.5, so LP + HP = 1 at every frequency.
        auto lr4_lp = [](double f, double fc) -> double {
            double r8 = std::pow(f / fc, 8.0);
            return 1.0 / (1.0 + r8);
        };
        auto lr4_hp = [](double f, double fc) -> double {
            double r8 = std::pow(f / fc, 8.0);
            return r8 / (1.0 + r8);
        };

        for (int k = 0; k < K; ++k) {
            double f = k * audio_stft.bin_hz_width;
            // f == 0 (DC): pow(0,8) = 0 → lp=1, hp=0 — handled correctly by std::pow
            double w_z0   = lr4_lp(f, audio_stft.tm_xover_0);
            double w_z3   = lr4_hp(f, audio_stft.tm_xover_2);
            double middle = 1.0 - w_z0 - w_z3;
            audio_stft.tm_W_Z0[k] = w_z0;
            audio_stft.tm_W_Z1[k] = middle * lr4_lp(f, audio_stft.tm_xover_1);
            audio_stft.tm_W_Z2[k] = middle * lr4_hp(f, audio_stft.tm_xover_1);
            audio_stft.tm_W_Z3[k] = w_z3;
        }
        std::cout << "[LR4 Weights] Computed " << K << " bins ("
                  << audio_stft.tm_xover_0 << "/" << audio_stft.tm_xover_1
                  << "/" << audio_stft.tm_xover_2 << " Hz crossovers, unity-sum guaranteed).\n";
    }

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // ========================================================================
    PhaseVocoder stft_engine;
    HPSS         hpss;
    EQMatcher    eq_matcher;
    DynamicsLR4  dynamics_lr4;
    Visualizer   visualizer;
    Synthesis    synthesis;

    const bool run_eq     = active_modules.find("eq")     != std::string::npos;
    const bool run_dyn    = active_modules.find("dyn")    != std::string::npos;
    const bool run_hpss   = active_modules.find("hpss")   != std::string::npos;

    std::cout << "[Pipeline] Active modules: ["
              << (run_eq     ? "eq"     : "eq=BYPASS")     << " | "
              << (run_dyn    ? "dyn"    : "dyn=BYPASS")    << " | "
              << (run_hpss   ? "hpss"   : "hpss=BYPASS")   << " | "
              << (audio_stft.enable_makeup_gain ? "makeup" : "makeup=BYPASS") << "]\n"
              << "           (PhaseVocoder and Synthesis always run)\n";

    stft_engine.process(audio_stft);

    if (run_eq) {
        eq_matcher.process(audio_stft);
        visualizer.render_eq(audio_stft);
    }

    if (run_dyn) {
        dynamics_lr4.process(audio_stft);
        visualizer.render_dynamics(audio_stft);
    }

    if (run_hpss) {
        hpss.process(audio_stft);
    }

    synthesis.process(audio_stft);

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
