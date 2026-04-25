#include "gui_render_pipeline.h"

#include "engine/engine.h"
#include "gui_render.h"
#include "timemap.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <sndfile.h>

namespace {

std::string settings_get(
    const std::vector<std::pair<std::string,std::string>>& passthrough,
    const std::string& key) {
    for (const auto& kv : passthrough) {
        if (kv.first == key) {
            const std::string& v = kv.second;
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
                return v.substr(1, v.size() - 2);
            }
            return v;
        }
    }
    return {};
}

bool parse_bool(const std::string& s, bool default_val) {
    if (s.empty()) return default_val;
    if (s == "true" || s == "1" || s == "yes" || s == "on")  return true;
    if (s == "false" || s == "0" || s == "no"  || s == "off") return false;
    return default_val;
}

int parse_int(const std::string& s, int default_val) {
    if (s.empty()) return default_val;
    try { return std::stoi(s); } catch (...) { return default_val; }
}

double parse_double(const std::string& s, double default_val) {
    if (s.empty()) return default_val;
    try { return std::stod(s); } catch (...) { return default_val; }
}

// Directory that contains the currently-running warptempo_gui binary.
// Used to locate the sibling adapters/ tree. Linux-only via /proc/self/exe.
std::string gui_binary_dir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::filesystem::path p(buf);
    return p.parent_path().string();
}

// Silent-on-missing unlink wrapper.
void unlink_silent(const std::string& path) {
    if (path.empty()) return;
    ::unlink(path.c_str());
}

// fork + execvp + waitpid. Returns true if the child exited 0.
bool run_subprocess(const std::string& prog, const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "warptempo_gui: render error: fork failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(prog.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(prog.c_str(), argv.data());
        std::fprintf(stderr, "warptempo_gui: render error: execvp('%s') failed: %s\n",
                     prog.c_str(), std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        std::fprintf(stderr, "warptempo_gui: render error: waitpid failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr,
            "warptempo_gui: render error: '%s' exited with non-zero status\n",
            prog.c_str());
        return false;
    }
    return true;
}

bool write_standard_timemap(const std::string& path,
                            const std::vector<TimemapSegment>& segs,
                            bool drop_zero_zero) {
    std::ofstream of(path);
    if (!of) {
        std::fprintf(stderr,
            "warptempo_gui: render error: could not write timemap '%s'\n",
            path.c_str());
        return false;
    }
    for (const auto& s : segs) {
        if (drop_zero_zero && s.src_frame == 0 && s.tgt_frame == 0) continue;
        of << s.src_frame << " " << s.tgt_frame << "\n";
    }
    return true;
}

bool write_midi_tempomap(const std::string& path,
                         const std::vector<TempomapEntry>& entries) {
    std::ofstream of(path);
    if (!of) {
        std::fprintf(stderr,
            "warptempo_gui: render error: could not write tempomap '%s'\n",
            path.c_str());
        return false;
    }
    of << std::fixed << std::setprecision(16);
    for (const auto& e : entries) {
        of << e.target_time_sec << " " << e.multiplier << "\n";
    }
    return true;
}

bool run_ffmpeg_alimiter(const std::string& in_path,
                         const std::string& out_path,
                         double ceiling_dbfs) {
    char limit_buf[32];
    std::snprintf(limit_buf, sizeof(limit_buf), "%gdB", ceiling_dbfs);
    std::string af =
        std::string("alimiter=latency=enabled:level=disabled:asc=disabled:")
        + "level_in=0dB:limit=" + limit_buf
        + ":level_out=0dB:attack=10:release=20";
    return run_subprocess("ffmpeg", {
        "-y", "-i", in_path,
        "-c:a", "pcm_s24le",
        "-af",  af,
        out_path
    });
}

// Resolve each GuiMarker to a MarkerForRender. Filters out markers that are:
//   - references to disabled-defined labels
//   - disabled label-definition markers (and thereby all refs to them)
// The inherit walk-back is applied here so MarkerForRender carries a
// concrete tempo_base / tempo_scale — same rule as resolve_inherited_tempo.
std::vector<MarkerForRender> resolve_markers_for_render(
    const std::vector<GuiMarker>& src) {

    // First pass: collect disabled label names.
    std::vector<std::string> disabled;
    for (const auto& m : src) {
        if (!m.label_def.empty() && m.disabled) disabled.push_back(m.label_def);
    }
    auto is_disabled_ref = [&](const std::string& label) {
        for (const auto& d : disabled) if (d == label) return true;
        return false;
    };

    std::vector<MarkerForRender> out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const auto& g = src[i];
        if (!g.label_def.empty() && g.disabled) continue;
        if (!g.label_ref.empty() && is_disabled_ref(g.label_ref)) continue;

        MarkerForRender m;
        m.time_seconds  = g.time_seconds;
        m.label_def     = g.label_def;
        m.label_ref     = g.label_ref;
        m.is_begin_time = g.is_begin_time;
        m.is_end_time   = g.is_end_time;

        if (!g.label_ref.empty()) {
            m.tempo_base = 0.0;
            m.tempo_scale.clear();
        } else if (g.tempo_inherits) {
            // Walk backward through SOURCE markers (not `out`) to match the
            // existing resolve_inherited_tempo contract. Stop at the nearest
            // earlier marker that owns its tempo and isn't a label_ref.
            double base  = 1.0;
            std::string scale_str;
            for (int j = static_cast<int>(i) - 1; j >= 0; --j) {
                const auto& p = src[j];
                if (!p.tempo_inherits && p.label_ref.empty()) {
                    base      = p.tempo_base;
                    scale_str = p.tempo_scale;
                    break;
                }
            }
            m.tempo_base  = base;
            m.tempo_scale = scale_str;
        } else {
            m.tempo_base  = g.tempo_base;
            m.tempo_scale = g.tempo_scale;
        }
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace

void do_render(const RenderRequest& req) {
    if (req.source_audio_path.empty()) return;

    // --- Read settings. ---
    std::string title = settings_get(req.settings_passthrough, "title");
    if (title.empty()) {
        std::fprintf(stderr, "warptempo_gui: render error: title not set in settings\n");
        return;
    }

    std::string engine = settings_get(req.settings_passthrough, "engine");
    if (engine.empty()) engine = "warptempo";
    const bool is_supported_engine =
        engine == "warptempo" || engine == "rubberband" ||
        engine == "bungee"    || engine == "stretch"    ||
        engine == "soundtouch"|| engine == "midi";
    if (engine == "none") {
        std::fprintf(stderr,
            "warptempo_gui: render error: engine 'none' is no longer supported\n");
        return;
    }
    if (!is_supported_engine) {
        std::fprintf(stderr,
            "warptempo_gui: render error: unknown engine '%s'\n", engine.c_str());
        return;
    }

    const double scale            = parse_double(
        settings_get(req.settings_passthrough, "scale"), 1.0);
    const bool   user_limiter_en  = parse_bool(
        settings_get(req.settings_passthrough, "limiter_enabled"), true);
    const int    N_fft            = parse_int(
        settings_get(req.settings_passthrough, "N"), 4096);
    const int    fftw_threads     = parse_int(
        settings_get(req.settings_passthrough, "fftw_threads"), 0);
    const double transients_tau_back_ms = parse_double(
        settings_get(req.settings_passthrough, "transients_tau_back_ms"), 30.0);

    // --- Probe source audio for sample rate / total frames. ---
    SF_INFO src_info{};
    src_info.format = 0;
    SNDFILE* sf = sf_open(req.source_audio_path.c_str(), SFM_READ, &src_info);
    if (!sf) {
        std::fprintf(stderr,
            "warptempo_gui: render error: could not open source '%s'\n",
            req.source_audio_path.c_str());
        return;
    }
    const long sample_rate  = src_info.samplerate;
    const long total_frames = static_cast<long>(src_info.frames);
    sf_close(sf);

    // --- Build timemap from in-memory markers. ---
    TimemapBuildInput tmin;
    tmin.markers      = resolve_markers_for_render(req.markers);
    tmin.scale        = scale;
    tmin.sample_rate  = sample_rate;
    tmin.total_frames = total_frames;

    TimemapBuildResult tmres;
    if (!build_timemaps(tmin, tmres)) {
        std::fprintf(stderr, "warptempo_gui: render error: timemap build failed\n");
        return;
    }

    // --- Compute output path. ---
    std::filesystem::path src(req.source_audio_path);
    std::filesystem::path dir = src.parent_path();
    if (dir.empty()) dir = std::filesystem::path(".");

    const bool midi_engine = (engine == "midi");
    // Prefix is applied iff the output WAV will be genuinely unlimited.
    // warptempo+trimmed always runs through ffmpeg alimiter downstream, so
    // limiter_enabled=false on the engine side is still limited on disk.
    const bool output_unlimited =
        !midi_engine && !user_limiter_en &&
        !(engine == "warptempo" && tmres.trimmed);
    std::string out_filename;
    if (output_unlimited) {
        out_filename = "engine=" + engine + ";limiter=false;" + title
                     + (midi_engine ? ".mid" : ".wav");
    } else {
        out_filename = title + (midi_engine ? ".mid" : ".wav");
    }
    std::string output_audio_path = (dir / out_filename).string();

    // --- Temp file paths (pid-scoped). ---
    const pid_t pid = ::getpid();
    std::string tmp_prefix = "/tmp/warptempo_" + std::to_string(pid) + "_";
    std::string tmp_tm          = tmp_prefix + "timemap";
    std::string tmp_tm_midi     = tmp_prefix + "timemap-midi";
    std::string tmp_trimmed_wav = tmp_prefix + "trimmed.wav";
    std::string tmp_engine_wav  = tmp_prefix + "engine.wav";

    auto cleanup_all = [&]() {
        unlink_silent(tmp_tm);
        unlink_silent(tmp_tm_midi);
        unlink_silent(tmp_trimmed_wav);
        unlink_silent(tmp_engine_wav);
    };

    // --- Trim, if requested. ---
    std::string engine_input_path = req.source_audio_path;
    if (tmres.trimmed) {
        if (!write_trimmed_wav(req.source_audio_path, tmp_trimmed_wav,
                               tmres.trim_begin_frame, tmres.trim_end_frame)) {
            cleanup_all();
            return;
        }
        engine_input_path = tmp_trimmed_wav;
    }

    std::fprintf(stderr, "warptempo_gui: rendering %s -> %s\n",
                 engine.c_str(), output_audio_path.c_str());

    const double ceiling_dbfs = -0.3;  // shared limit for engine + ffmpeg
    const std::string gui_dir = gui_binary_dir();
    const std::string adapter_base = gui_dir + "/../adapters";

    // --- Engine dispatch. ---
    if (engine == "warptempo") {
        EngineParams ep;
        ep.source_audio_path = engine_input_path;
        ep.output_audio_path = tmres.trimmed ? tmp_engine_wav : output_audio_path;
        ep.timemap.reserve(tmres.standard.size());
        for (const auto& s : tmres.standard) {
            ep.timemap.emplace_back(s.src_frame, s.tgt_frame);
        }
        ep.N                      = N_fft;
        ep.fftw_threads           = fftw_threads;
        ep.transients_tau_back_ms = transients_tau_back_ms;
        ep.limiter_ceiling_dbfs   = ceiling_dbfs;
        // When trimmed, ffmpeg alimiter runs downstream and owns limiting.
        ep.limiter_enabled        = tmres.trimmed ? false : user_limiter_en;
        ep.limiter_diag           = false;
        ep.transients_diag        = false;
        // 24-bit PCM only when the engine's write is the final file AND
        // its internal limiter ran. Trimmed path writes intermediate float.
        ep.output_24bit_pcm       = !tmres.trimmed && user_limiter_en;

        if (!run_warptempo_engine(ep)) {
            std::fprintf(stderr, "warptempo_gui: render error: engine failed\n");
            cleanup_all();
            return;
        }
        if (tmres.trimmed) {
            if (!run_ffmpeg_alimiter(tmp_engine_wav, output_audio_path, ceiling_dbfs)) {
                cleanup_all();
                return;
            }
        }
    } else if (engine == "rubberband") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/true)) {
            cleanup_all();
            return;
        }
        if (!run_subprocess("rubberband", {
                "-t", "1",
                "--timemap", tmp_tm,
                "--fine",
                "--ignore-clipping",
                engine_input_path,
                tmp_engine_wav
            })) {
            cleanup_all();
            return;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, output_audio_path, ceiling_dbfs)) {
            cleanup_all();
            return;
        }
    } else if (engine == "bungee") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/false)) {
            cleanup_all();
            return;
        }
        std::string adapter = adapter_base + "/bungee/build/bungee_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return;
        }
        if (!run_subprocess(adapter, {engine_input_path, tmp_tm, tmp_engine_wav})) {
            cleanup_all();
            return;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, output_audio_path, ceiling_dbfs)) {
            cleanup_all();
            return;
        }
    } else if (engine == "stretch") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/false)) {
            cleanup_all();
            return;
        }
        std::string adapter = adapter_base + "/stretch/build/stretch_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return;
        }
        if (!run_subprocess(adapter, {engine_input_path, tmp_tm, tmp_engine_wav})) {
            cleanup_all();
            return;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, output_audio_path, ceiling_dbfs)) {
            cleanup_all();
            return;
        }
    } else if (engine == "soundtouch") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/false)) {
            cleanup_all();
            return;
        }
        std::string adapter = adapter_base + "/soundtouch/build/soundtouch_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return;
        }
        if (!run_subprocess(adapter, {engine_input_path, tmp_engine_wav, tmp_tm})) {
            cleanup_all();
            return;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, output_audio_path, ceiling_dbfs)) {
            cleanup_all();
            return;
        }
    } else if (engine == "midi") {
        if (!write_midi_tempomap(tmp_tm_midi, tmres.midi)) {
            cleanup_all();
            return;
        }
        std::string adapter = adapter_base + "/midi/build/midi_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return;
        }
        std::ostringstream bbs;
        bbs << static_cast<double>(sample_rate) * 0.002;
        if (!run_subprocess(adapter, {
                tmp_tm_midi,
                output_audio_path,
                bbs.str(),
                "30000"
            })) {
            cleanup_all();
            return;
        }
    }

    cleanup_all();
    std::fprintf(stderr, "warptempo_gui: render complete: %s\n",
                 output_audio_path.c_str());
}
