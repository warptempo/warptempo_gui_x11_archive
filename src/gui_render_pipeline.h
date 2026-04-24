#pragma once

#include "gui_markers.h"

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
};

// Synchronous render. Blocks the caller until the pipeline finishes (or
// errors out). All progress / error reporting goes to stderr; this function
// has no return value because the UI doesn't react to render outcomes.
void do_render(const RenderRequest& req);
