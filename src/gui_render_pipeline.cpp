#include "gui_render_pipeline.h"

#include "engine/engine.h"
#include "gui_audio.h"
#include "gui_render.h"
#include "gui_transients.h"
#include "timemap.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <set>
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
        "-f", "wav",
        out_path
    });
}

// Resolve each GuiMarker to a MarkerForRender. Filters out markers that are:
//   - references to disabled-defined labels
//   - disabled label-definition markers (and thereby all refs to them)
// EXCEPT: a disabled-cascade marker that carries an `is_begin_time` /
// `is_end_time` trim flag is kept (the trim flag is positionally real
// regardless of whether the marker's tempo participates), with its tempo
// pulled from the inheritance walk-back.
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

    // Walk backward through SOURCE markers from at_index, returning the
    // nearest earlier marker's tempo if it owns its tempo and is not
    // itself disabled. Skips: tempo_inherits markers, label_ref markers,
    // and disabled markers (any kind — chunk U patch 3 allows `disabled`
    // on non-label-defs, and a disabled marker is never a valid tempo
    // source). Default if none found: {1.0, ""}.
    auto walk_back_owning_tempo = [&](size_t at_index)
        -> std::pair<double, std::string> {
        for (int j = static_cast<int>(at_index) - 1; j >= 0; --j) {
            const auto& p = src[j];
            if (p.tempo_inherits) continue;
            if (!p.label_ref.empty()) continue;
            if (p.disabled) continue;
            return { p.tempo_base, p.tempo_scale };
        }
        return { 1.0, std::string{} };
    };

    std::vector<MarkerForRender> out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const auto& g = src[i];
        const bool is_disabled_label_ref_cascade =
            !g.disabled && !g.label_ref.empty()
            && is_disabled_ref(g.label_ref);
        // Chunk U patch 3: `disabled` is allowed on any marker; whatever
        // its kind, a disabled marker's tempo is silenced. The label_ref
        // cascade is a separate path (the ref itself is not disabled but
        // its target is).
        const bool is_effectively_disabled =
            g.disabled || is_disabled_label_ref_cascade;
        const bool has_trim_flag = g.is_begin_time || g.is_end_time;

        if (is_effectively_disabled && !has_trim_flag) continue;

        MarkerForRender m;
        m.time_seconds  = g.time_seconds;
        m.label_def     = g.label_def;
        m.label_ref     = g.label_ref;
        m.is_begin_time = g.is_begin_time;
        m.is_end_time   = g.is_end_time;

        if (is_effectively_disabled) {
            // Kept solely for the trim flag — the marker's own tempo is
            // silenced, so resolve via walk-back through earlier
            // non-disabled, non-inheriting, non-label_ref markers.
            auto [base, scale] = walk_back_owning_tempo(i);
            m.tempo_base  = base;
            m.tempo_scale = scale;
        } else if (!g.label_ref.empty()) {
            m.tempo_base = 0.0;
            m.tempo_scale.clear();
        } else if (g.tempo_inherits) {
            auto [base, scale] = walk_back_owning_tempo(i);
            m.tempo_base  = base;
            m.tempo_scale = scale;
        } else {
            m.tempo_base  = g.tempo_base;
            m.tempo_scale = g.tempo_scale;
        }
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace

bool do_render(const RenderRequest& req) {
    if (req.source_audio_path.empty()) return false;

    // --- Read settings. ---
    std::string title = settings_get(req.settings_passthrough, "title");
    if (title.empty()) {
        std::fprintf(stderr, "warptempo_gui: render error: title not set in settings\n");
        return false;
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
        return false;
    }
    if (!is_supported_engine) {
        std::fprintf(stderr,
            "warptempo_gui: render error: unknown engine '%s'\n", engine.c_str());
        return false;
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
        return false;
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
        return false;
    }

    // --- Compute output path. ---
    const bool midi_engine = (engine == "midi");
    const bool batch_render = !req.batch_folder.empty();
    std::string final_output_path;
    if (batch_render) {
        // Batch render: <batch_folder>/<batch_basename>.{wav,mid}. Sidecars
        // (.warpmarkers / .transientmarkers / .peaks) are written after the
        // wav rename succeeds — see the post-render block below. The
        // engine/limiter-prefix naming does not apply.
        const std::string ext = midi_engine ? ".mid" : ".wav";
        final_output_path =
            (std::filesystem::path(req.batch_folder) /
             (req.batch_basename + ext)).string();
    } else {
        std::filesystem::path src(req.source_audio_path);
        std::filesystem::path dir = src.parent_path();
        if (dir.empty()) dir = std::filesystem::path(".");
        // Prefix is applied iff the output WAV will be genuinely unlimited.
        // warptempo+trimmed always runs through ffmpeg alimiter downstream,
        // so limiter_enabled=false on the engine side is still limited on
        // disk.
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
        final_output_path = (dir / out_filename).string();
    }
    // Staging path: every engine path writes its final output here, and on
    // success we atomically rename to final_output_path. A killed render
    // leaves <final>.tmp behind; the next render's O_TRUNC semantics handle
    // that (libsndfile / ffmpeg / adapters all open with truncate).
    const std::string staging_output_path = final_output_path + ".tmp";

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
        unlink_silent(staging_output_path);
    };

    // --- Trim, if requested. ---
    std::string engine_input_path = req.source_audio_path;
    if (tmres.trimmed) {
        if (!write_trimmed_wav(req.source_audio_path, tmp_trimmed_wav,
                               tmres.trim_begin_frame, tmres.trim_end_frame)) {
            cleanup_all();
            return false;
        }
        engine_input_path = tmp_trimmed_wav;
    }

    std::fprintf(stderr, "warptempo_gui: rendering %s -> %s\n",
                 engine.c_str(), final_output_path.c_str());

    const double ceiling_dbfs = -0.3;  // shared limit for engine + ffmpeg
    const std::string gui_dir = gui_binary_dir();
    const std::string adapter_base = gui_dir + "/../adapters";

    // --- Engine dispatch. ---
    if (engine == "warptempo") {
        EngineParams ep;
        ep.source_audio_path = engine_input_path;
        ep.output_audio_path = tmres.trimmed ? tmp_engine_wav
                                             : staging_output_path;
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
        ep.transient_frames       = req.transient_frames;

        if (!run_warptempo_engine(ep)) {
            std::fprintf(stderr, "warptempo_gui: render error: engine failed\n");
            cleanup_all();
            return false;
        }
        if (tmres.trimmed) {
            if (!run_ffmpeg_alimiter(tmp_engine_wav, staging_output_path, ceiling_dbfs)) {
                cleanup_all();
                return false;
            }
        }
    } else if (engine == "rubberband") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/true)) {
            cleanup_all();
            return false;
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
            return false;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, staging_output_path, ceiling_dbfs)) {
            cleanup_all();
            return false;
        }
    } else if (engine == "bungee") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/false)) {
            cleanup_all();
            return false;
        }
        std::string adapter = adapter_base + "/bungee/build/bungee_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return false;
        }
        if (!run_subprocess(adapter, {engine_input_path, tmp_tm, tmp_engine_wav})) {
            cleanup_all();
            return false;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, staging_output_path, ceiling_dbfs)) {
            cleanup_all();
            return false;
        }
    } else if (engine == "stretch") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/false)) {
            cleanup_all();
            return false;
        }
        std::string adapter = adapter_base + "/stretch/build/stretch_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return false;
        }
        if (!run_subprocess(adapter, {engine_input_path, tmp_tm, tmp_engine_wav})) {
            cleanup_all();
            return false;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, staging_output_path, ceiling_dbfs)) {
            cleanup_all();
            return false;
        }
    } else if (engine == "soundtouch") {
        if (!write_standard_timemap(tmp_tm, tmres.standard, /*drop_zero_zero=*/false)) {
            cleanup_all();
            return false;
        }
        std::string adapter = adapter_base + "/soundtouch/build/soundtouch_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return false;
        }
        if (!run_subprocess(adapter, {engine_input_path, tmp_engine_wav, tmp_tm})) {
            cleanup_all();
            return false;
        }
        if (!run_ffmpeg_alimiter(tmp_engine_wav, staging_output_path, ceiling_dbfs)) {
            cleanup_all();
            return false;
        }
    } else if (engine == "midi") {
        if (!write_midi_tempomap(tmp_tm_midi, tmres.midi)) {
            cleanup_all();
            return false;
        }
        std::string adapter = adapter_base + "/midi/build/midi_adapter";
        if (!std::filesystem::exists(adapter)) {
            std::fprintf(stderr,
                "warptempo_gui: render error: adapter not found '%s'\n",
                adapter.c_str());
            cleanup_all();
            return false;
        }
        std::ostringstream bbs;
        bbs << static_cast<double>(sample_rate) * 0.002;
        if (!run_subprocess(adapter, {
                tmp_tm_midi,
                staging_output_path,
                bbs.str(),
                "30000"
            })) {
            cleanup_all();
            return false;
        }
    }

    // Atomic publish: every engine path above wrote to staging_output_path.
    // Promote it to final_output_path with a single rename so observers
    // (the queue walker, downstream tools) never see a half-written file.
    std::error_code ec;
    std::filesystem::rename(staging_output_path, final_output_path, ec);
    if (ec) {
        std::fprintf(stderr,
            "warptempo_gui: render error: rename '%s' -> '%s' failed: %s\n",
            staging_output_path.c_str(), final_output_path.c_str(),
            ec.message().c_str());
        cleanup_all();
        return false;
    }

    // Deposit a peak-pyramid sidecar next to the rendered WAV. Fire-and-forget;
    // the function logs its own errors and never affects render success.
    if (!midi_engine) {
        write_peaks_cache_for_wav(final_output_path);
    }

    // Batch render: capture the per-render marker + transient sidecars now
    // that the wav rename has succeeded. These are the markers and
    // transients THIS render was produced from, not snapshots of the
    // current source authoring state — render-view loads them later to
    // display alongside the rendered audio. Sidecar write failures are
    // logged but never abort: the wav itself is the primary artifact.
    if (batch_render) {
        const std::filesystem::path bf(req.batch_folder);
        const std::string wm_path =
            (bf / (req.batch_basename + ".warpmarkers")).string();
        if (!GuiMarkers::save(wm_path, req.markers)) {
            std::fprintf(stderr,
                "warptempo_gui: render warning: failed to write '%s'\n",
                wm_path.c_str());
        }
        if (!req.transients.empty()) {
            const std::string tm_path =
                (bf / (req.batch_basename + ".transientmarkers")).string();
            if (!GuiTransients::save(tm_path, req.transients)) {
                std::fprintf(stderr,
                    "warptempo_gui: render warning: failed to write '%s'\n",
                    tm_path.c_str());
            }
        }

        // Render-domain sidecars (.renderwarpmarkers / .rendertransientmarkers).
        // Render-view loads these instead of the source-domain pair so
        // visible marker positions match the rendered audio's time axis.
        // The source-domain pair above stays authoritative for
        // Ctrl+Alt+C commit and Ctrl+S authoring saves; the render-domain
        // pair is display-only and never read back into authoring memory.
        if (!tmres.standard.empty() && sample_rate > 0) {
            const auto& seg = tmres.standard;
            const int64_t trim_begin =
                static_cast<int64_t>(tmres.trim_begin_frame);
            const int64_t trim_end = tmres.trimmed
                ? static_cast<int64_t>(tmres.trim_end_frame)
                : static_cast<int64_t>(total_frames);
            const double sr_d = static_cast<double>(sample_rate);

            // Markers: lockstep walk between req.markers and tmres.standard.
            // Each surviving marker (post resolve filter + post trim filter)
            // pairs with the next-in-order surviving segment. seg.tgt_frame
            // is already post-shift (render-domain) so the render-time is
            // tgt_frame / sr directly.
            std::set<std::string> disabled_label_defs;
            for (const auto& m : req.markers) {
                if (!m.label_def.empty() && m.disabled) {
                    disabled_label_defs.insert(m.label_def);
                }
            }
            auto is_cascade_disabled_ref = [&](const GuiMarker& m) {
                return !m.disabled && !m.label_ref.empty() &&
                       disabled_label_defs.count(m.label_ref) > 0;
            };

            size_t seg_idx = 0;
            std::vector<GuiMarker> warped_markers;
            warped_markers.reserve(req.markers.size());
            for (const auto& g : req.markers) {
                const bool eff_disabled =
                    g.disabled || is_cascade_disabled_ref(g);
                const bool has_trim_flag = g.is_begin_time || g.is_end_time;

                // resolve_markers_for_render filter.
                if (eff_disabled && !has_trim_flag) continue;

                // Trim-range filter (inclusive both ends — matches the
                // post-pass at timemap.cpp line 209).
                const int64_t sf_abs = static_cast<int64_t>(
                    std::llround(g.time_seconds * sr_d));
                if (sf_abs < trim_begin || sf_abs > trim_end) continue;

                if (seg_idx >= seg.size()) break;
                const auto& s = seg[seg_idx];
                ++seg_idx;

                // Disabled-with-trim-flag survives the resolve filter but
                // is not display-eligible in render-view (locked design).
                if (eff_disabled) continue;

                GuiMarker w     = g;
                w.time_seconds  = static_cast<double>(s.tgt_frame) / sr_d;
                w.is_begin_time = false;
                w.is_end_time   = false;
                warped_markers.push_back(std::move(w));
            }
            const std::string wmd_path =
                (bf / (req.batch_basename + ".renderwarpmarkers")).string();
            if (!GuiMarkers::save(wmd_path, warped_markers)) {
                std::fprintf(stderr,
                    "warptempo_gui: render warning: failed to write '%s'\n",
                    wmd_path.c_str());
            }

            // Transients: not 1:1 with segments. Linear interpolate each
            // transient's effective_frame() in tmres.standard. Drop
            // out-of-trim and disabled. Stripping displacement is
            // intentional — the user-visible position lands in src_frame.
            if (!req.transients.empty()) {
                auto interp_render_frame =
                    [&](int64_t sf_abs, int64_t& out_frame) -> bool {
                    if (sf_abs < trim_begin || sf_abs > trim_end) return false;
                    const int64_t sf_rel = sf_abs - trim_begin;
                    if (seg.empty()) return false;
                    if (sf_rel <= static_cast<int64_t>(seg.front().src_frame)) {
                        out_frame = static_cast<int64_t>(seg.front().tgt_frame);
                        return true;
                    }
                    if (sf_rel >= static_cast<int64_t>(seg.back().src_frame)) {
                        out_frame = static_cast<int64_t>(seg.back().tgt_frame);
                        return true;
                    }
                    size_t lo = 0, hi = seg.size() - 1;
                    while (lo + 1 < hi) {
                        const size_t mid = lo + (hi - lo) / 2;
                        if (static_cast<int64_t>(seg[mid].src_frame) <= sf_rel)
                            lo = mid;
                        else
                            hi = mid;
                    }
                    const int64_t lo_src = static_cast<int64_t>(seg[lo].src_frame);
                    const int64_t hi_src = static_cast<int64_t>(seg[lo + 1].src_frame);
                    const int64_t lo_tgt = static_cast<int64_t>(seg[lo].tgt_frame);
                    const int64_t hi_tgt = static_cast<int64_t>(seg[lo + 1].tgt_frame);
                    if (hi_src == lo_src) {
                        out_frame = lo_tgt;
                    } else {
                        const double frac =
                            static_cast<double>(sf_rel - lo_src) /
                            static_cast<double>(hi_src - lo_src);
                        out_frame = lo_tgt + static_cast<int64_t>(
                            std::llround(frac * (hi_tgt - lo_tgt)));
                    }
                    return true;
                };

                std::vector<GuiTransient> warped_transients;
                warped_transients.reserve(req.transients.size());
                for (const auto& t : req.transients) {
                    if (t.disabled) continue;
                    int64_t render_frame = 0;
                    if (!interp_render_frame(t.effective_frame(),
                                             render_frame)) {
                        continue;
                    }
                    GuiTransient w;
                    w.src_frame        = render_frame;
                    w.is_inserted      = t.is_inserted;
                    w.disabled         = false;
                    w.is_begin_time    = false;
                    w.is_end_time      = false;
                    w.has_displacement = false;
                    w.displaced_frame  = 0;
                    warped_transients.push_back(std::move(w));
                }
                const std::string tmd_path =
                    (bf / (req.batch_basename + ".rendertransientmarkers"))
                    .string();
                if (!GuiTransients::save(tmd_path, warped_transients)) {
                    std::fprintf(stderr,
                        "warptempo_gui: render warning: failed to write '%s'\n",
                        tmd_path.c_str());
                }
            }
        }
    }

    cleanup_all();
    std::fprintf(stderr, "warptempo_gui: render complete: %s\n",
                 final_output_path.c_str());
    return true;
}

bool do_detection(const DetectionRequest& req,
                  std::vector<int64_t>& out_src_frames) {
    out_src_frames.clear();
    if (req.source_audio_path.empty()) {
        std::fprintf(stderr,
            "warptempo_gui: detection error: no source audio path\n");
        return false;
    }

    SF_INFO src_info{};
    src_info.format = 0;
    SNDFILE* sf = sf_open(req.source_audio_path.c_str(), SFM_READ, &src_info);
    if (!sf) {
        std::fprintf(stderr,
            "warptempo_gui: detection error: could not open source '%s'\n",
            req.source_audio_path.c_str());
        return false;
    }
    const long sample_rate  = src_info.samplerate;
    const long total_frames = static_cast<long>(src_info.frames);
    sf_close(sf);

    const double scale = parse_double(
        settings_get(req.settings_passthrough, "scale"), 1.0);

    TimemapBuildInput tmin;
    tmin.markers      = resolve_markers_for_render(req.markers);
    tmin.scale        = scale;
    tmin.sample_rate  = sample_rate;
    tmin.total_frames = total_frames;

    TimemapBuildResult tmres;
    if (!build_timemaps(tmin, tmres)) {
        std::fprintf(stderr,
            "warptempo_gui: detection error: timemap build failed\n");
        return false;
    }

    DetectionParams dp;
    dp.source_audio_path = req.source_audio_path;
    dp.timemap.reserve(tmres.standard.size());
    for (const auto& s : tmres.standard) {
        dp.timemap.emplace_back(s.src_frame, s.tgt_frame);
    }
    dp.N           = parse_int(
        settings_get(req.settings_passthrough, "N"), 4096);
    dp.fftw_threads = parse_int(
        settings_get(req.settings_passthrough, "fftw_threads"), 0);
    dp.transients_xover_low = parse_double(
        settings_get(req.settings_passthrough, "transients_xover_low"), 120.0);
    dp.transients_xover_high = parse_double(
        settings_get(req.settings_passthrough, "transients_xover_high"), 3500.0);
    dp.transients_tau_back_ms = parse_double(
        settings_get(req.settings_passthrough, "transients_tau_back_ms"), 30.0);
    dp.transients_thresh_db = parse_double(
        settings_get(req.settings_passthrough, "transients_thresh_db"), -20.0);
    dp.transients_refractory_ms = parse_double(
        settings_get(req.settings_passthrough, "transients_refractory_ms"), 1500.0);
    dp.transients_anticipation_ms = parse_double(
        settings_get(req.settings_passthrough, "transients_anticipation_ms"), 100.0);

    return run_warptempo_detection(dp, out_src_frames);
}

