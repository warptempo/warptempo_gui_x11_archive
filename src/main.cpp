#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "synthesis.h"

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <source_audio> <timemap_file> <transientmap_file> <source_audio_md5> [key=value ...]\n"
                  << "\n"
                  << "  <transientmap_file> File of source-frame transient markers (one integer\n"
                  << "                      per line). May be empty. Always required.\n"
                  << "\n"
                  << "  <source_audio_md5>  MD5 hex digest of the original (untrimmed) source\n"
                  << "                      audio used when generating the timemap. Passed\n"
                  << "                      explicitly so trimmed or resampled variants of the\n"
                  << "                      source do not silently produce mismatched output paths.\n"
                  << "\n"
                  << "  N=4096                    FFT size (divisible by 4)\n"
                  << "  flex_window=1             Flex factor search half-width (synth frames)\n"
;
        return 1;
    }

    const std::string md5 = argv[4];
    if (md5.size() != 32) {
        std::cerr << "Error: <source_audio_md5> must be a 32-character hex digest, got '"
                  << md5 << "'.\n";
        return 1;
    }

    // --- Parse CLI (key=value) — argv[4+] ---
    AudioSTFT audio_stft;

    std::unordered_map<std::string, std::string> kv;
    for (int i = 5; i < argc; ++i) {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq != std::string::npos)
            kv[arg.substr(0, eq)] = arg.substr(eq + 1);
    }
    auto ki = [&](const char* k, int d) -> int {
        auto it = kv.find(k); return (it != kv.end()) ? std::stoi(it->second) : d;
    };

    audio_stft.N            = ki("N",           4096);
    audio_stft.flex_window  = ki("flex_window",     1);

    // --- Derive output path from MD5 ---
    audio_stft.output_audio_file = "." + md5 + "-tmp.wav";

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

    // --- Parse Transientmap ---
    {
        std::ifstream tmap_file(argv[3]);
        if (!tmap_file.is_open()) {
            std::cerr << "Error: Could not open transientmap: '" << argv[3] << "'\n";
            return 1;
        }
        std::vector<int64_t> transient_src_frames;
        std::string line;
        int line_num = 0;
        while (std::getline(tmap_file, line)) {
            ++line_num;
            if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos)
                continue;
            int64_t val;
            try {
                size_t pos;
                val = std::stoll(line, &pos);
                if (line.find_first_not_of(" \t\r\n", pos) != std::string::npos) {
                    std::cerr << "Error: transientmap line " << line_num
                              << " contains non-integer content: '" << line << "'\n";
                    return 1;
                }
            } catch (...) {
                std::cerr << "Error: transientmap line " << line_num
                          << " contains non-integer content: '" << line << "'\n";
                return 1;
            }
            if (val < 0) {
                std::cerr << "Error: transientmap value " << val
                          << " is negative (line " << line_num << ")\n";
                return 1;
            }
            if (val > audio_stft.src_info.frames) {
                std::cerr << "Error: transientmap value " << val
                          << " exceeds source frames (" << audio_stft.src_info.frames
                          << ") at line " << line_num << "\n";
                return 1;
            }
            if (!transient_src_frames.empty() && val <= transient_src_frames.back()) {
                std::cerr << "Error: transientmap not strictly increasing: "
                          << transient_src_frames.back() << " -> " << val
                          << " at line " << line_num << "\n";
                return 1;
            }
            transient_src_frames.push_back(val);
        }
        tmap_file.close();
        std::cout << "[Transientmap] Loaded " << transient_src_frames.size()
                  << " transient marker" << (transient_src_frames.size() != 1 ? "s" : "")
                  << ".\n";

        // Convert source frames to synthesis frame indices (forward cursor)
        // Compare against window centers (fm[i] + N/2) by shifting src down by N/2.
        const auto& fm = audio_stft.frame_map;
        const int num_fm = static_cast<int>(fm.size());
        const int64_t half_N = audio_stft.N / 2;
        int cursor = 0;
        for (int64_t src : transient_src_frames) {
            int64_t shifted = src - half_N;
            if (shifted < 0) {
                std::cout << "[Transientmap] Skipping src=" << src
                          << " (before first analyzed frame)\n";
                continue;
            }
            while (cursor < num_fm && fm[cursor] < shifted)
                ++cursor;
            if (cursor >= num_fm) {
                std::cout << "[Transientmap] Skipping src=" << src
                          << " (after last analyzed frame)\n";
                continue;
            }
            int best = cursor;
            if (cursor > 0 && fm[cursor] > shifted) {
                if ((shifted - fm[cursor - 1]) <= (fm[cursor] - shifted))
                    best = cursor - 1;
            }
            if (best == 0 && fm[0] > shifted) {
                std::cout << "[Transientmap] Skipping src=" << src
                          << " (before first analyzed frame)\n";
                continue;
            }
            audio_stft.transient_markers.push_back({best, src});
        }
    }

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // ========================================================================
    PhaseVocoder stft_engine;
    Synthesis    synthesis;

    stft_engine.process(audio_stft);
    synthesis.process(audio_stft);

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
