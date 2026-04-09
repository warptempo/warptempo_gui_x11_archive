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
                  << "  pem_frames=3        PEM zone energy window (±frames)\n"
                  << "  pem_apply=true      PEM routing switch: true | false (Default is true)\n"
                  << "  pem_floor=-40       RMS energy gate lower anchor (dBFS)\n"
                  << "  pem_peak=-18        RMS energy gate upper anchor (dBFS)\n"
                  << "  pem_knee=6          RMS energy gate knee width (dB)\n"
                  << "  pem_xover_0=50      LR4 safety floor crossover (Hz)\n"
                  << "  pem_xover_1=500     LR4 low/mid crossover (Hz)\n"
                  << "  pem_xover_2=2000    LR4 mid/high crossover (Hz)\n"
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
    audio_stft.pem_frames        = ki("pem_frames",      3);
    audio_stft.pem_floor_db      = kd("pem_floor",    -40.0);
    audio_stft.pem_peak_db       = kd("pem_peak",     -18.0);
    audio_stft.pem_knee_db       = kd("pem_knee",       6.0);
    audio_stft.pem_xover_0       = kd("pem_xover_0",      50.0);
    audio_stft.pem_xover_1       = kd("pem_xover_1",     500.0);
    audio_stft.pem_xover_2       = kd("pem_xover_2",    2000.0);
    audio_stft.hpf_hz           = kd("hpf_hz",       30.0);
    {
        auto it = kv.find("pem_apply");
        if (it != kv.end())
            audio_stft.pem_apply = (it->second == "true" || it->second == "1");
    }

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

    // --- Pre-calculate LR4 Transient Matcher frequency weights ---
    // Four bands sum to exactly 1.0 at every bin by construction:
    //   W_Z0 = LP(xover_0)
    //   W_Z3 = HP(xover_2)
    //   middle = 1 - W_Z0 - W_Z3
    //   W_Z1 = middle * LP(xover_1)
    //   W_Z2 = middle * HP(xover_1)
    //   Sum = W_Z0 + middle*(LP1 + HP1) + W_Z3 = 1
    {
        const int K = audio_stft.N / 2 + 1;
        audio_stft.pem_W_Z0.resize(K);
        audio_stft.pem_W_Z1.resize(K);
        audio_stft.pem_W_Z2.resize(K);
        audio_stft.pem_W_Z3.resize(K);

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
            double w_z0   = lr4_lp(f, audio_stft.pem_xover_0);
            double w_z3   = lr4_hp(f, audio_stft.pem_xover_2);
            double middle = 1.0 - w_z0 - w_z3;
            audio_stft.pem_W_Z0[k] = w_z0;
            audio_stft.pem_W_Z1[k] = middle * lr4_lp(f, audio_stft.pem_xover_1);
            audio_stft.pem_W_Z2[k] = middle * lr4_hp(f, audio_stft.pem_xover_1);
            audio_stft.pem_W_Z3[k] = w_z3;
        }
        std::cout << "[LR4 Weights] Computed " << K << " bins ("
                  << audio_stft.pem_xover_0 << "/" << audio_stft.pem_xover_1
                  << "/" << audio_stft.pem_xover_2 << " Hz crossovers, unity-sum guaranteed).\n";
    }

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
