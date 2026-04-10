#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "eq_matcher.h"
#include "visualizer.h"
#include "hpss.h"
#include "synthesis.h"

// Extract md5 from timemap filename. Format: .<md5>-timemap
// Takes the basename, strips leading '.', strips trailing '-timemap'.
static std::string md5_from_timemap(const char* path) {
    std::string s(path);
    // Grab basename
    auto slash = s.rfind('/');
    if (slash != std::string::npos) s = s.substr(slash + 1);
    // Strip leading '.'
    if (!s.empty() && s[0] == '.') s = s.substr(1);
    // Strip trailing '-timemap'
    const std::string suffix = "-timemap";
    if (s.size() > suffix.size() &&
        s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
        s = s.substr(0, s.size() - suffix.size());
    return s;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <source_audio> <timemap_file> [key=value ...]\n"
                  << "\n"
                  << "  N=4096                    FFT size (divisible by 4)\n"
                  << "  hpss_enabled=true         Perform HPSS (true|false)\n"
                  << "  beta=2.0                  HPSS mask sharpness exponent\n"
                  << "  L_h=31                    Horizontal context half-width (frames)\n"
                  << "  L_p=7                     Vertical filter max clamp (bins)\n"
                  << "  hpf_hz=30                 Zero-phase sub-rumble HPF cutoff (Hz; 0=bypass)\n"
                  << "  eq_match_enabled=true     EQ match correction (true|false)\n"
                  << "  eq_match_xover_0=50       LR4 sub-bass/low crossover (Hz)\n"
                  << "  eq_match_xover_1=500      LR4 low/mid crossover (Hz)\n"
                  << "  eq_match_xover_2=2000     LR4 mid/high crossover (Hz)\n"
                  << "  eq_match_floor_db=-50     Loudness gate lower anchor (dBFS)\n"
                  << "  eq_match_peak_db=-14      Loudness gate upper anchor (dBFS)\n"
                  << "  eq_match_release_low=350  Low-band release time (ms)\n"
                  << "  eq_match_release_mid=300  Mid-band release time (ms)\n"
                  << "  eq_match_release_high=200 High-band release time (ms)\n";
        return 1;
    }

    // --- Extract MD5 from timemap filename (format: .<md5>-timemap) ---
    std::string md5 = md5_from_timemap(argv[2]);
    if (md5.empty()) {
        std::cerr << "Error: Could not extract MD5 from timemap filename '" << argv[2] << "'.\n";
        return 1;
    }

    // --- Parse CLI (key=value) ---
    AudioSTFT audio_stft;

    std::unordered_map<std::string, std::string> kv;
    for (int i = 3; i < argc; ++i) {
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
    auto kb = [&](const char* k, bool d) -> bool {
        auto it = kv.find(k); return (it != kv.end()) ? (it->second == "true") : d;
    };

    audio_stft.N            = ki("N",           4096);
    audio_stft.hpss_enabled = kb("hpss_enabled", true);
    audio_stft.beta         = kd("beta",          2.0);
    audio_stft.L_h          = ki("L_h",            31);
    audio_stft.L_p_max      = ki("L_p",             7);
    audio_stft.hpf_hz       = kd("hpf_hz",       30.0);

    audio_stft.eq_match_enabled      = kb("eq_match_enabled",      true);
    audio_stft.eq_match_xover_0      = kd("eq_match_xover_0",      50.0);
    audio_stft.eq_match_xover_1      = kd("eq_match_xover_1",     500.0);
    audio_stft.eq_match_xover_2      = kd("eq_match_xover_2",    2000.0);
    audio_stft.eq_match_floor_db     = kd("eq_match_floor_db",    -50.0);
    audio_stft.eq_match_peak_db      = kd("eq_match_peak_db",     -14.0);
    audio_stft.eq_match_release_low  = kd("eq_match_release_low",  350.0);
    audio_stft.eq_match_release_mid  = kd("eq_match_release_mid",  300.0);
    audio_stft.eq_match_release_high = kd("eq_match_release_high", 200.0);

    // --- Derive output paths from MD5 ---
    std::string base = "." + md5 + "-tmp";
    if (audio_stft.hpss_enabled) {
        audio_stft.perc_audio_file     = base + "_percussive.wav";
        audio_stft.harmonic_audio_file = base + "_harmonic.wav";
    } else {
        audio_stft.perc_audio_file     = base + ".wav";
    }
    audio_stft.tgt_audio_file = base;  // visualizer appends _eq.png

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

    // --- Precompute LR4 per-bin band weights ---
    // Four bands sum to exactly 1.0 at every bin by construction:
    //   W_Z0 = LP(xover_0)           — sub-bass bypass
    //   W_Z3 = HP(xover_2)           — high
    //   middle = 1 - W_Z0 - W_Z3
    //   W_Z1 = middle * LP(xover_1)  — low
    //   W_Z2 = middle * HP(xover_1)  — mid
    {
        const int K = audio_stft.N / 2 + 1;
        audio_stft.eq_W_Z0.resize(K);
        audio_stft.eq_W_Z1.resize(K);
        audio_stft.eq_W_Z2.resize(K);
        audio_stft.eq_W_Z3.resize(K);

        auto lr4_lp = [](double f, double fc) -> double {
            double r8 = std::pow(f / fc, 8.0);
            return 1.0 / (1.0 + r8);
        };
        auto lr4_hp = [](double f, double fc) -> double {
            double r8 = std::pow(f / fc, 8.0);
            return r8 / (1.0 + r8);
        };

        for (int k = 0; k < K; ++k) {
            double f      = k * audio_stft.bin_hz_width;
            double w_z0   = lr4_lp(f, audio_stft.eq_match_xover_0);
            double w_z3   = lr4_hp(f, audio_stft.eq_match_xover_2);
            double middle = 1.0 - w_z0 - w_z3;
            audio_stft.eq_W_Z0[k] = w_z0;
            audio_stft.eq_W_Z1[k] = middle * lr4_lp(f, audio_stft.eq_match_xover_1);
            audio_stft.eq_W_Z2[k] = middle * lr4_hp(f, audio_stft.eq_match_xover_1);
            audio_stft.eq_W_Z3[k] = w_z3;
        }

        // Count dominant bins per band for diagnostics
        int cnt0 = 0, cnt1 = 0, cnt2 = 0, cnt3 = 0;
        for (int k = 0; k < K; ++k) {
            double best = audio_stft.eq_W_Z0[k]; int band = 0;
            if (audio_stft.eq_W_Z1[k] > best) { best = audio_stft.eq_W_Z1[k]; band = 1; }
            if (audio_stft.eq_W_Z2[k] > best) { best = audio_stft.eq_W_Z2[k]; band = 2; }
            if (audio_stft.eq_W_Z3[k] > best) { band = 3; }
            if (band == 0) cnt0++; else if (band == 1) cnt1++;
            else if (band == 2) cnt2++; else cnt3++;
        }
        std::cout << "[EQ Match] enabled=" << (audio_stft.eq_match_enabled ? "true" : "false")
                  << "  xovers=" << audio_stft.eq_match_xover_0 << "/"
                  << audio_stft.eq_match_xover_1 << "/" << audio_stft.eq_match_xover_2 << " Hz"
                  << "  floor=" << audio_stft.eq_match_floor_db << " dB"
                  << "  peak=" << audio_stft.eq_match_peak_db << " dB\n";
        std::cout << "[EQ Match] release low=" << audio_stft.eq_match_release_low
                  << " mid=" << audio_stft.eq_match_release_mid
                  << " high=" << audio_stft.eq_match_release_high << " ms\n";
        std::cout << "[LR4 Weights] Computed " << K << " bins ("
                  << audio_stft.eq_match_xover_0 << "/" << audio_stft.eq_match_xover_1
                  << "/" << audio_stft.eq_match_xover_2
                  << " Hz crossovers, unity-sum guaranteed).\n";
        std::cout << "[LR4 Weights] Band bin counts: bypass=" << cnt0
                  << " low=" << cnt1 << " mid=" << cnt2 << " high=" << cnt3 << "\n";
    }

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // ========================================================================
    PhaseVocoder stft_engine;
    EQMatcher    eq_matcher;
    Visualizer   visualizer;
    HPSS         hpss;
    Synthesis    synthesis;

    stft_engine.process(audio_stft);
    if (audio_stft.eq_match_enabled) {
        eq_matcher.process(audio_stft);
        visualizer.render_eq(audio_stft);
    }
    if (audio_stft.hpss_enabled) hpss.process(audio_stft);
    synthesis.process(audio_stft);

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
