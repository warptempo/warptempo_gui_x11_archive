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
                  << "  eq_match_enabled=true     EQ match correction (true|false)\n";
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
    audio_stft.eq_match_enabled = kb("eq_match_enabled", true);

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

    std::cout << "[EQ Match] enabled=" << (audio_stft.eq_match_enabled ? "true" : "false")
              << "  hpf_hz=" << audio_stft.hpf_hz << " Hz\n";

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
