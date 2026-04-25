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
};

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
