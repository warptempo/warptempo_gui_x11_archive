#pragma once

#include <cstddef>
#include <string>
#include <vector>

// In-memory timemap-generation used by the engine. Math is organized as
// Pass 1, Pass 2, and a trim post-pass.

struct TimemapSegment {
    size_t src_frame;
    size_t tgt_frame;
};

struct TempomapEntry {
    double target_time_sec;
    double multiplier;
};

// Minimal POD the timemap math needs. The GUI's `GuiWarpMarker` resolves into
// this: tempo_inherits markers are walked back to their nearest owning
// ancestor and their effective tempo_base / tempo_scale are copied forward.
// Disabled markers (and any references to disabled-defined labels) are
// filtered out BEFORE conversion.
struct MarkerForRender {
    double      time_seconds = 0.0;
    double      tempo_base   = 1.0;   // resolved owning tempo; irrelevant for label_ref
    std::string tempo_scale;           // "" or the numeric string after '*'
    std::string label_def;
    std::string label_ref;
    bool        is_begin_time = false;
    bool        is_end_time   = false;
};

struct TrimFlagSource {
    double time_seconds  = 0.0;
    bool   is_begin_time = false;
    bool   is_end_time   = false;
};

struct TimemapBuildInput {
    std::vector<MarkerForRender> markers;

    // Trim flags (is_begin_time / is_end_time) sourced from the GUI's
    // transient marker list. Warp markers in `markers` take precedence on
    // conflict; this list contributes only when the warp list has no flag of
    // the same kind. Empty for callers that don't author trim flags on
    // transients.
    std::vector<TrimFlagSource> transient_trim_flags;

    double scale        = 1.0;   // from settings; 1.0 default
    long   sample_rate  = 0;     // from the source audio file
    long   total_frames = 0;     // from the source audio file
};

struct TimemapBuildResult {
    std::vector<TimemapSegment> standard;
    std::vector<TempomapEntry>  midi;

    // Populated when trim markers (is_begin_time / is_end_time) are present.
    bool   trimmed          = false;
    size_t trim_begin_frame = 0;
    size_t trim_end_frame   = 0;   // exclusive; == total_frames if no end
};

// Returns true on success; false on any validation failure (message logged
// to stderr). Failure conditions: tempo > 9.99, tempo <= 0,
// src_frame > total_frames, src_frame - prev_src_frame < 1, undefined label
// reference, duplicate label definition, final_multiplier > 9.9999 on label
// refs, begin_time at 00:00.000.
bool build_timemaps(const TimemapBuildInput& in, TimemapBuildResult& out);

// libsndfile-based slice: reads src_path samples [begin_frame, end_frame)
// and writes them to out_path as 32-bit float WAV preserving channel count
// and sample rate. Returns true on success; false (with stderr log) on any
// sndfile error. No sox dependency.
bool write_trimmed_wav(const std::string& src_path,
                       const std::string& out_path,
                       size_t begin_frame,
                       size_t end_frame);
