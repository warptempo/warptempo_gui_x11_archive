#include "app_state.h"

#include <string>

// X.7.7: promoted from a lambda in main(). Body is verbatim from the
// original at main.cpp:1969-1975, with `app.settings_passthrough`
// reached through the explicit AppState argument rather than a capture.
std::string settings_get(const AppState& app, const std::string& key,
                         const std::string& dflt) {
    for (const auto& kv : app.settings_passthrough) {
        if (kv.first == key) return kv.second;
    }
    return dflt;
}

// X.7.8a: promoted from a lambda in main(). Body is verbatim from the
// original at main.cpp:942-944, with the AppState reach reached through
// the explicit argument rather than a capture.
bool bottom_strip_wide(const AppState& app) {
    return app.prompt.active || !app.queue_progress_text.empty();
}
