#pragma once

#include "warpmarkers.h"
#include "transientmarkers.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Self-contained view of the AppState fields do_render reads. Constructed by
// the Ctrl+Alt+R handler in main.cpp so do_render stays decoupled from
// AppState's (anonymous-namespace) shape.
struct RenderRequest {
    std::string            source_audio_path;
    std::vector<GuiWarpMarker> markers;
    std::vector<std::pair<std::string, std::string>> settings_passthrough;

    // User-curated transient frame list (source-frame domain). When non-empty
    // and the active engine is "warptempo", this overrides the engine's
    // internal detection — typical population is the union of inserted +
    // active-detected entries from the GUI's transient list, with disabled
    // entries filtered out and effective_frame() applied per entry.
    std::vector<int64_t>   transient_frames;

    // Full transient store snapshot. Only consumed when batch_folder is set
    // — written verbatim as `<batch_folder>/<batch_basename>.transientmarkers`
    // alongside the rendered .wav so render-view can later display the
    // transients this render was produced from. When empty, no
    // .transientmarkers sidecar is written. The single-transient sidecar
    // path used by the source-directory render branch (Ctrl+Alt+R) does not
    // read this field.
    std::vector<GuiTransientMarker> transients;

    // Batch render output. When `batch_folder` is non-empty, do_render
    // writes its final output to `<batch_folder>/<batch_basename>.wav` (or
    // `.mid` for midi) and, on success, deposits the per-render
    // `<batch_basename>.warpmarkers`, `<batch_basename>.transientmarkers`
    // (when transients is non-empty), and `<batch_basename>.peaks` sidecars
    // in the same folder. The folder must already exist; do_render does
    // not create it. When `batch_folder` is empty, the source-directory
    // title/engine/limiter-prefix naming is used (unchanged from the
    // immediate Ctrl+Alt+R path) and no sidecars are written.
    std::string batch_folder;
    std::string batch_basename;
};

// Synchronous render. Blocks the caller until the pipeline finishes (or
// errors out). All progress / error reporting goes to stderr. Returns true
// iff the full pipeline ran to completion (including the rename-into-place
// of the staged output); returns false on every early-return failure path.
// The Ctrl+Alt+R queue walker uses the return to count actual successes;
// other callers may ignore it.
bool do_render(const RenderRequest& req);

// Standalone transient detection entry point. Builds the same timemap +
// settings the render path uses, then runs the engine's detection-only pass
// against the source audio. On success, populates `out_src_frames` with
// detected positions in the source-frame domain; on failure, logs to stderr
// and returns false.
struct DetectionRequest {
    std::string            source_audio_path;
    std::vector<GuiWarpMarker> markers;
    std::vector<std::pair<std::string, std::string>> settings_passthrough;
};

bool do_detection(const DetectionRequest& req,
                  std::vector<int64_t>& out_src_frames);
