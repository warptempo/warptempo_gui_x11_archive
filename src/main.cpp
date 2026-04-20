#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "stft_container.h"
#include "phase_vocoder.h"
#include "transients.h"
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
                  << "  N=4096                            FFT size (divisible by 4)\n"
                  << "  flex_window=1                     Flex factor search half-width (synth frames)\n"
                  << "\n"
                  << "  detect_enabled=true               Run detector (false produces zero markers)\n"
                  << "  detect_xover_low=200.0            Detection band lower edge (Hz)\n"
                  << "  detect_xover_high=3000.0          Detection band upper edge (Hz)\n"
                  << "  detect_tau_back_ms=30.0           Backward IIR time constant\n"
                  << "  detect_loud_thresh_db=-20.0       Loud-path dB threshold (upward crossings)\n"
                  << "  detect_quiet_thresh_db=-40.0      Quiet-path dB threshold (downward crossings)\n"
                  << "  detect_refractory_ms=1500.0       Unified target-time refractory (loud always fires;\n"
                  << "                                    quiet yields to recent loud activity)\n"
                  << "  detect_loud_anticipation_ms=18.0  Shift loud detections earlier (ms)\n"
                  << "  detect_quiet_delay_ms=18.0        Shift quiet detections later (ms)\n"
                  << "  transient_diag=false              Write <output>-diag.wav with dirac spikes\n"
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
    auto ki = [&](const char* k, int d) -> int {
        auto it = kv.find(k); return (it != kv.end()) ? std::stoi(it->second) : d;
    };
    auto kd = [&](const char* k, double d) -> double {
        auto it = kv.find(k); return (it != kv.end()) ? std::stod(it->second) : d;
    };
    auto kb = [&](const char* k, bool d) -> bool {
        auto it = kv.find(k);
        if (it == kv.end()) return d;
        const std::string& v = it->second;
        return (v == "true" || v == "1" || v == "yes" || v == "on");
    };

    audio_stft.N            = ki("N",           4096);
    audio_stft.flex_window  = ki("flex_window",     1);

    auto& dp = audio_stft.detect_params;
    dp.enabled              = kb("detect_enabled",              dp.enabled);
    dp.xover_low            = kd("detect_xover_low",            dp.xover_low);
    dp.xover_high           = kd("detect_xover_high",           dp.xover_high);
    dp.tau_back_ms          = kd("detect_tau_back_ms",          dp.tau_back_ms);
    dp.loud_thresh_db       = kd("detect_loud_thresh_db",       dp.loud_thresh_db);
    dp.quiet_thresh_db      = kd("detect_quiet_thresh_db",      dp.quiet_thresh_db);
    dp.refractory_ms        = kd("detect_refractory_ms",        dp.refractory_ms);
    dp.loud_anticipation_ms = kd("detect_loud_anticipation_ms", dp.loud_anticipation_ms);
    dp.quiet_delay_ms       = kd("detect_quiet_delay_ms",       dp.quiet_delay_ms);
    audio_stft.transient_diag = kb("transient_diag", false);

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

    // ========================================================================
    // Pipeline Execution (order is acoustically critical — do not reorder)
    // ========================================================================
    PhaseVocoder stft_engine;
    Transients   detector;
    Synthesis    synthesis;

    stft_engine.process(audio_stft);
    detector.process(audio_stft);
    synthesis.process(audio_stft);

    // --- Diagnostic spike file (separate WAV, spikes only) ---
    if (audio_stft.transient_diag) {
        SF_INFO probe_info{};
        probe_info.format = 0;
        SNDFILE* probe = sf_open(audio_stft.output_audio_file.c_str(), SFM_READ, &probe_info);
        if (!probe) {
            std::cerr << "[Diag] Warning: could not reopen output to size diag file; skipping.\n";
        } else {
            const sf_count_t total = probe_info.frames;
            sf_close(probe);

            std::string diag_path = audio_stft.output_audio_file;
            auto dot = diag_path.find_last_of('.');
            if (dot != std::string::npos) diag_path.insert(dot, "-diag");
            else                          diag_path += "-diag";

            SF_INFO diag_info = audio_stft.src_info;
            diag_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
            SNDFILE* diag_snd = sf_open(diag_path.c_str(), SFM_WRITE, &diag_info);
            if (!diag_snd) {
                std::cerr << "[Diag] Error: could not create diag file '" << diag_path << "'\n";
            } else {
                const int ch      = audio_stft.channels;
                const int64_t R_s = audio_stft.R_s;
                std::vector<float> diag_buf(static_cast<size_t>(total) * ch, 0.0f);

                auto poke = [&](int64_t pos, float amp) {
                    if (pos < 0 || pos >= total) return;
                    for (int c = 0; c < ch; ++c)
                        diag_buf[static_cast<size_t>(pos) * ch + c] = amp;
                };

                const int64_t half_N = audio_stft.N / 2;
                for (const auto& m : audio_stft.transient_markers) {
                    int64_t center = static_cast<int64_t>(m.synth_frame) * R_s + half_N;
                    float sign = m.is_loud ? 1.0f : -1.0f;
                    poke(center - 1, 0.5f * sign);
                    poke(center,     1.0f * sign);
                    poke(center + 1, 0.5f * sign);
                }

                sf_writef_float(diag_snd, diag_buf.data(), total);
                sf_close(diag_snd);
                std::cout << "[Diag] Wrote diagnostic spike file: " << diag_path << "\n";
            }
        }
    }

    // --- Cleanup ---
    audio_stft.cleanup();

    return 0;
}
