#pragma once

#include "gui_markers.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Self-contained view of the AppState fields do_render reads. Constructed by
// the Ctrl+Alt+R handler in gui_main.cpp so do_render stays decoupled from
// AppState's (anonymous-namespace) shape.
struct RenderRequest {
    std::string            source_audio_path;
    std::vector<GuiMarker> markers;
    std::vector<std::pair<std::string, std::string>> settings_passthrough;

    // User-curated transient frame list (source-frame domain). When non-empty
    // and the active engine is "warptempo", this overrides the engine's
    // internal detection — typical population is the union of inserted +
    // active-detected entries from the GUI's transient list, with disabled
    // entries filtered out and effective_frame() applied per entry.
    std::vector<int64_t>   transient_frames;

    // When non-empty, do_render writes its final WAV (or .mid) into this
    // directory under the literal name "output.wav" (or "output.mid"),
    // bypassing the title/engine/limiter-prefix naming used for source-
    // directory renders. Used by the render-all queue walker; empty for
    // the immediate Ctrl+Alt+R path. The directory must already exist;
    // do_render does not create it.
    std::string output_dir_override;
};

// Build a RenderRequest from a queue-entry directory: loads warpmarkers
// (required) and transientmarkers (optional) from `entry_dir`. The source
// path and live in-memory settings come from the caller — entries do not
// snapshot settings. Returns false if warpmarkers is missing or fails to
// parse; logs to stderr.
bool load_render_request_from_dir(
    const std::string& source_audio_path,
    const std::string& entry_dir,
    const std::vector<std::pair<std::string, std::string>>&
        live_settings_passthrough,
    RenderRequest& out);

// Synchronous render. Blocks the caller until the pipeline finishes (or
// errors out). All progress / error reporting goes to stderr; this function
// has no return value because the UI doesn't react to render outcomes.
void do_render(const RenderRequest& req);

// Standalone transient detection entry point. Builds the same timemap +
// settings the render path uses, then runs the engine's detection-only pass
// against the source audio. On success, populates `out_src_frames` with
// detected positions in the source-frame domain; on failure, logs to stderr
// and returns false.
struct DetectionRequest {
    std::string            source_audio_path;
    std::vector<GuiMarker> markers;
    std::vector<std::pair<std::string, std::string>> settings_passthrough;
};

bool do_detection(const DetectionRequest& req,
                  std::vector<int64_t>& out_src_frames);
