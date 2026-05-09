#pragma once

#include "warpmarkers.h"
#include "phase_reset_markers.h"

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

    // User-curated phase reset frame list (source-frame domain). When non-empty
    // and the active engine is "warptempo", this overrides the engine's
    // internal detection — typical population is the union of inserted +
    // active-detected entries from the GUI's phase reset list, with disabled
    // entries filtered out and time_seconds converted to source frames at
    // the GUI-to-engine boundary via banker's rounding.
    std::vector<int64_t>   phase_reset_frames;

    // Full phase reset store snapshot. Batch sidecar payload only: when
    // batch_folder is set and this list is non-empty, written verbatim as
    // `<batch_folder>/<batch_basename>.phaseresetmarkers` so render-view
    // can later display the phase reset set this render was produced from.
    // A second sidecar `<batch_basename>.renderphaseresetmarkers` carries
    // the same set warped into render-domain frame coordinates. The
    // single-phase reset sidecar path used by the immediate Ctrl+Alt+R
    // render branch does not read this field for sidecar emission.
    // Empty list disables sidecar emission cleanly.
    std::vector<GuiPhaseResetMarker> phase_resets;

    // Batch render output. When `batch_folder` is non-empty, do_render
    // writes its final output to `<batch_folder>/<batch_basename>.wav` (or
    // `.mid` for midi) and, on success, deposits the per-render
    // `<batch_basename>.warpmarkers`, `<batch_basename>.phaseresetmarkers`
    // (when phase resets is non-empty), and `<batch_basename>.peaks` sidecars
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
