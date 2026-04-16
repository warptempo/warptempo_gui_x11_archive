#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "hpss.h"
#include "synthesis.h"

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <source_audio> <timemap_file> <source_audio_md5> [key=value ...]\n"
                  << "\n"
                  << "  <source_audio_md5>  MD5 hex digest of the original (untrimmed) source\n"
                  << "                      audio used when generating the timemap. Passed\n"
                  << "                      explicitly so trimmed or resampled variants of the\n"
                  << "                      source do not silently produce mismatched output paths.\n"
                  << "\n"
                  << "  N=4096                    FFT size (divisible by 4)\n"
                  << "  hpss_enabled=true         Perform HPSS (true|false)\n"
                  << "  beta=2.0                  HPSS mask sharpness exponent\n"
                  << "  L_h=31                    Horizontal context half-width (frames)\n"
                  << "  L_p=7                     Vertical filter max clamp (bins)\n"
                  << "  yin_enabled=true          Enable YIN extraction (requires hpss_enabled)\n"
                  << "  yin_f0_min=500.0          Minimum tracked f0 in Hz\n"
                  << "  yin_f0_max=1200.0         Maximum tracked f0 in Hz\n"
                  << "  yin_confidence=0.65       Confidence threshold (1 - d_prime at tau_0)\n"
                  << "  yin_alpha=1.0             Comb filter extraction depth [0, 1]\n"
                  << "  yin_sigma=1.5             Comb filter half-width in bins\n"
                  << "  yin_threshold=0.35        d_prime threshold for voiced frame detection\n"
                  << "  yin_diag=false            Write pitch log and correction WAV\n"
;
        return 1;
    }

    const std::string md5 = argv[3];
    if (md5.size() != 32) {
        std::cerr << "Error: <source_audio_md5> must be a 32-character hex digest, got '"
                  << md5 << "'.\n";
        return 1;
    }

    // --- Parse CLI (key=value) — argv[4+] ---
    AudioSTFT audio_stft;

    std::unordered_map<std::string, std::string> kv;
    for (int i = 4; i < argc; ++i) {
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

    audio_stft.yin_enabled    = kb("yin_enabled",     true);
    audio_stft.yin_f0_min     = kd("yin_f0_min",    500.0);
    audio_stft.yin_f0_max     = kd("yin_f0_max",   1200.0);
    audio_stft.yin_confidence = kd("yin_confidence",  0.65);
    audio_stft.yin_alpha      = kd("yin_alpha",       1.00);
    audio_stft.yin_sigma      = kd("yin_sigma",        1.5);
    audio_stft.yin_threshold  = kd("yin_threshold",   0.35);
    audio_stft.yin_diag       = kb("yin_diag",       false);

    // --- Derive output paths from MD5 ---
    std::string base = "." + md5 + "-tmp";
    if (audio_stft.hpss_enabled) {
        audio_stft.perc_audio_file     = base + "_percussive.wav";
        audio_stft.harmonic_audio_file = base + "_harmonic.wav";
    } else {
        audio_stft.perc_audio_file     = base + ".wav";
    }
    audio_stft.tgt_audio_file = base;  // visualizer appends _eq.png
    if (audio_stft.yin_diag) {
        audio_stft.yin_diag_pitch_file      = base + "_yin_pitch.txt";
        audio_stft.yin_diag_correction_file = base + "_yin_correction.wav";
    }

    if (audio_stft.N % 4 != 0) {
        std::cerr << "Error: N must be divisible by 4.\n";
        return 1;
    }
    if (audio_stft.yin_enabled && !audio_stft.hpss_enabled) {
        std::cerr << "Error: yin_enabled=true requires hpss_enabled=true.\n";
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

    // Validate timemap: both src_frame and tgt_frame must be strictly monotonically increasing
    for (size_t i = 1; i < audio_stft.timemap.size(); ++i) {
        if (audio_stft.timemap[i].src_frame <= audio_stft.timemap[i - 1].src_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic src_frame ("
                      << audio_stft.timemap[i - 1].src_frame << " -> "
                      << audio_stft.timemap[i].src_frame << ").\n";
            return 1;
        }
        if (audio_stft.timemap[i].tgt_frame <= audio_stft.timemap[i - 1].tgt_frame) {
            std::cerr << "Error: timemap entry " << i << " has non-monotonic tgt_frame ("
                      << audio_stft.timemap[i - 1].tgt_frame << " -> "
                      << audio_stft.timemap[i].tgt_frame << ").\n";
            return 1;
        }
    }

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
    if (audio_stft.hpss_enabled) hpss.process(audio_stft);
    synthesis.process(audio_stft);

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
