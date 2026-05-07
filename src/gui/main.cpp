#include "app_state.h"
#include "audio.h"
#include "warpmarkers.h"
#include "flag_editor.h"
#include "input_handler.h"
#include "paint_handler.h"
#include "playback.h"
#include "render.h"
#include "render_pipeline.h"
#include "render_view.h"
#include "selection.h"
#include "tab_mode.h"
#include "text_display.h"
#include "text_editor.h"
#include "transientmarkers.h"
#include "transientmarkers_ops.h"
#include "undo.h"
#include "viewport.h"
#include "warpmarkers_ops.h"
#include "x11.h"

#include <cairo/cairo.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <sndfile.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// X.7.8a: kProgressBarHeight, kChannelGapPx, kFlagFontSize, kTimestampPadX,
// kTimestampBaselineFromBottom, kTabLetterGapPx, kIterPopupVerticalGapPx,
// and kIterPopupVPadExtraPx now live in paint_handler.h so paint_handler.cpp
// can reach them; the constants below are paint-handler-independent and
// stay file-local.

constexpr double   kTopStripRatio     = 0.10;
constexpr double   kBottomStripRatio  = 0.10;

// X.7.8b-2: kMarkerHitHalfPx, kDoubleClickMs, kDoubleClickPixels moved to
// app_state.h so the hit_test_* free functions and the GuiInputHandler
// mouse handler can reach them.

// Time the cursor must dwell on a popup-eligible flag rect before the
// hover popup appears. Distinct from kDoubleClickMs (point-event window
// vs continuous-state duration) even though they currently share a value.
constexpr int kHoverDelayMs       = 500;

// ms-per-pixel for each numeric zoom level. Level 0 is most zoomed in.
// kNumZoomLevels (in app_state.h) is the count of entries here; the
// static_assert below pins them together so the table can't drift.
constexpr double kZoomMsPerPixel[] = {
    1.25, 2.6, 5.2, 10.4, 20.8, 41.7, 83.3, 166.7
};
static_assert(sizeof(kZoomMsPerPixel) / sizeof(kZoomMsPerPixel[0])
              == static_cast<size_t>(kNumZoomLevels),
              "kZoomMsPerPixel size must match kNumZoomLevels");

// Region width includes room for the A/B tab letter and the dirty indicator
// past the timestamp text edge.
constexpr int kTimestampRegionW           = 200;
constexpr int kTimestampRegionH           = 30;
constexpr double kDirtyGapPx              = 8.0;

// Half-width of the column invalidated around a playhead position. Wide
// enough to cover the playhead line, the 12px-wide triangle indicator
// (±6 px of playhead_x), and subpixel rounding margin.
constexpr int kPlayheadHalfPx = 8;

// X.7.8b-1: Brief X.3 BPM-sweep math primitive (BaseTempoScale +
// compute_base_tempo_scale) moved out of this anonymous namespace into
// input_handler.h so input_handler.cpp can reach it. on_key (Ctrl+Alt+M)
// is the sole caller after this brief.

// X.7.8a: IterPopupHit, BpmPopupHit, and the compute_*_popup_hits
// helpers were promoted to paint_handler.{h,cpp} so paint_handler.cpp
// can reach them. They remain reachable from this TU via
// `#include "paint_handler.h"` below.

// X.7.8b-3: compute_hover_popup_text moved to render.{h,cpp} so
// input_handler.cpp can reach it from on_motion. It sits next to
// resolve_inherited_tempo / flag_text_for_marker — same rendering-
// time text formatting role over GuiWarpMarker, same TU.

// OpKind, UndoEntry, DragState, UndoHistory, PlayheadDragState,
// HoverPopupState, DialogTrigger, PromptState, ViewState, AppState live in
// app_state.h (extracted in brief X.7.1 alongside the Viewport struct).


// Parsed contents of .settings, separated into tab-handled keys (typed with
// presence flags so defaults can be applied per key) and the pass-through
// vector that preserves any other lines verbatim in their original order.
struct ParsedSettings {
    bool    has_tab_a_vp   = false;
    int64_t tab_a_vp       = 0;
    bool    has_tab_a_zoom = false;
    int     tab_a_zoom     = 0;
    bool    has_tab_a_ph   = false;
    int64_t tab_a_ph       = 0;
    bool    has_tab_b_vp   = false;
    int64_t tab_b_vp       = 0;
    bool    has_tab_b_zoom = false;
    int     tab_b_zoom     = 0;
    bool    has_tab_b_ph   = false;
    int64_t tab_b_ph       = 0;
    bool    has_follow     = false;
    bool    follow         = true;
    std::vector<std::pair<std::string, std::string>> passthrough;
};

// X.7.8a: WaveformCache was promoted to paint_handler.{h,cpp} so
// paint_handler.cpp can reach it. The instance is still a local in
// main() and is passed by reference into GuiPaintHandler.

} // namespace

// Geometry helpers — public to viewport.cpp via app_state.h. The strip-height
// helpers and samples_per_pixel_at remain main-private (`static`).

static int top_strip_height(int window_height) {
    return static_cast<int>(std::lround(window_height * kTopStripRatio));
}

static int bottom_strip_height(int window_height) {
    return static_cast<int>(std::lround(window_height * kBottomStripRatio));
}

GuiRect waveform_area(const AppState& a) {
    const int top_h = top_strip_height(a.height);
    const int bot_h = bottom_strip_height(a.height);
    return GuiRect{0, top_h, a.width, a.height - top_h - bot_h};
}

GuiRect top_strip_area(const AppState& a) {
    return GuiRect{0, 0, a.width, top_strip_height(a.height)};
}

// Scan warp markers and transients for begin_time / end_time flags; fall
// back to full file if either is absent. The S.1 invariant is that at most
// one b= and one e= exist across both files; if both lists somehow carry
// the same flag (only reachable via hand-edit), the warp-side value wins
// for determinism and a one-line stderr warning is emitted.
std::pair<long long, long long> compute_trim_samples(
    const std::vector<GuiWarpMarker>& warp_markers,
    const std::vector<GuiTransientMarker>& transients,
    int sample_rate, long long total_frames) {
    long long begin = 0;
    long long end   = total_frames;
    bool have_begin_warp  = false;
    bool have_end_warp    = false;
    bool have_begin_trans = false;
    bool have_end_trans   = false;

    for (const auto& m : warp_markers) {
        if (m.is_begin_time) {
            begin = static_cast<long long>(std::llround(
                m.time_seconds * static_cast<double>(sample_rate)));
            have_begin_warp = true;
        }
        if (m.is_end_time) {
            end = static_cast<long long>(std::llround(
                m.time_seconds * static_cast<double>(sample_rate)));
            have_end_warp = true;
        }
    }
    for (const auto& t : transients) {
        if (t.is_begin_time) {
            if (have_begin_warp) {
                have_begin_trans = true;
            } else {
                begin = static_cast<long long>(t.effective_frame());
                have_begin_trans = true;
            }
        }
        if (t.is_end_time) {
            if (have_end_warp) {
                have_end_trans = true;
            } else {
                end = static_cast<long long>(t.effective_frame());
                have_end_trans = true;
            }
        }
    }
    if (have_begin_warp && have_begin_trans) {
        std::fprintf(stderr,
            "warptempo_gui: duplicate b= flag (warp + transient); "
            "using warp value\n");
    }
    if (have_end_warp && have_end_trans) {
        std::fprintf(stderr,
            "warptempo_gui: duplicate e= flag (warp + transient); "
            "using warp value\n");
    }
    if (begin < 0) begin = 0;
    if (end > total_frames) end = total_frames;
    if (end < begin) end = begin;
    return {begin, end};
}

static double samples_per_pixel_at(int zoom_level,
                                   int waveform_width_px,
                                   int64_t total_frames,
                                   int sample_rate) {
    if (zoom_level == kFitFileLevel) {
        if (waveform_width_px <= 0) return 1.0;
        double spp = static_cast<double>(total_frames) /
                     static_cast<double>(waveform_width_px);
        if (spp < 1e-9) spp = 1e-9;
        return spp;
    }
    return kZoomMsPerPixel[zoom_level] *
           static_cast<double>(sample_rate) / 1000.0;
}

// Largest numeric level L (in [0, kNumZoomLevels)) whose samples_visible does
// not exceed total_frames. Returns -1 if even level 0 shows more than the
// file — in which case fit-file is the only valid level.
int max_valid_numeric_level(int waveform_width_px,
                            int64_t total_frames,
                            int sample_rate) {
    int best = -1;
    for (int L = 0; L < kNumZoomLevels; L++) {
        const double spp =
            samples_per_pixel_at(L, waveform_width_px, total_frames, sample_rate);
        const double visible = spp * waveform_width_px;
        if (visible <= static_cast<double>(total_frames)) best = L;
        else break; // table is monotonic
    }
    return best;
}

int64_t samples_visible(const AppState& a, const GuiAudio& audio) {
    const GuiRect area = waveform_area(a);
    const double spp = samples_per_pixel_at(
        a.zoom_level, area.w, audio.total_frames(), audio.sample_rate());
    return static_cast<int64_t>(std::llround(spp * area.w));
}

double current_samples_per_pixel(const AppState& a, const GuiAudio& audio) {
    const GuiRect area = waveform_area(a);
    return samples_per_pixel_at(
        a.zoom_level, area.w, audio.total_frames(), audio.sample_rate());
}

// Applies a position delta to the transient's src_frame.
void apply_transient_position_delta(GuiTransientMarker& m, int64_t delta) {
    if (delta == 0) return;
    m.src_frame += delta;
}

void clamp_viewport_start(AppState& a, const GuiAudio& audio) {
    const int64_t visible = samples_visible(a, audio);
    const int64_t total   = audio.total_frames();
    if (visible >= total) {
        a.viewport_start_sample = 0;
        return;
    }
    if (a.viewport_start_sample < 0) a.viewport_start_sample = 0;
    const int64_t max_start = total - visible;
    if (a.viewport_start_sample > max_start) a.viewport_start_sample = max_start;
}

double playhead_pixel_x(const AppState& a, const GuiAudio& audio) {
    const double spp = current_samples_per_pixel(a, audio);
    if (spp <= 0.0) return -1.0;
    return static_cast<double>(a.playhead_sample - a.viewport_start_sample) / spp;
}

// Computes a viewport_start such that `sample` renders at pixel column
// `target_pixel_x`, then clamps. Used for playhead-centered zoom.
int64_t viewport_start_for_pixel(int64_t sample,
                                 double target_pixel_x,
                                 double samples_per_pixel) {
    const double start = static_cast<double>(sample) -
                         target_pixel_x * samples_per_pixel;
    if (start <= 0.0) return 0;
    return static_cast<int64_t>(std::llround(start));
}

// Shrink-and-pad: produce a union rectangle covering both inputs. Used to
// bundle the two playhead-column invalidations into a single expose when
// they overlap (e.g., arrow key at zoom level 0 moves by 1 pixel).
GuiRect union_rect(GuiRect a, GuiRect b) {
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w);
    const int y1 = std::max(a.y + a.h, b.y + b.h);
    return GuiRect{x0, y0, x1 - x0, y1 - y0};
}

bool rects_intersect(GuiRect a, GuiRect b) {
    if (a.x + a.w <= b.x || b.x + b.w <= a.x) return false;
    if (a.y + a.h <= b.y || b.y + b.h <= a.y) return false;
    return true;
}

GuiRect playhead_invalidate_rect(const GuiRect& area, double px_x) {
    const int col = static_cast<int>(std::floor(px_x + 0.5));
    const int x0 = std::max(area.x, col - kPlayheadHalfPx);
    const int x1 = std::min(area.x + area.w, col + kPlayheadHalfPx + 1);
    if (x1 <= x0) return GuiRect{area.x, 0, 0, 0};
    // Envelope extends up from the top of the window to the bottom of the
    // waveform area so it covers the playhead line inside the waveform AND
    // the chunk-P triangle indicator in the flag strip above it.
    const int y0 = 0;
    const int y1 = area.y + area.h;
    return GuiRect{x0, y0, x1 - x0, y1 - y0};
}

GuiRect timestamp_invalidate_rect(int window_height, int window_width,
                                  bool wide_strip) {
    if (wide_strip) {
        return GuiRect{0, window_height - kTimestampRegionH,
                       window_width, kTimestampRegionH};
    }
    return GuiRect{0, window_height - kTimestampRegionH,
                   kTimestampRegionW, kTimestampRegionH};
}

namespace {

// Ensure `p` exists with `contents`. If the file already exists, leave it
// alone. Returns true on success or if file already exists. Failures are
// non-fatal — the audio load still proceeds.
bool create_if_missing(const std::filesystem::path& p,
                       const std::string& contents) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return true;
    std::ofstream f(p);
    if (!f) {
        std::fprintf(stderr,
                     "warptempo_gui: could not create '%s'\n",
                     p.string().c_str());
        return false;
    }
    f << contents;
    return static_cast<bool>(f);
}

std::string trim_ws(const std::string& s) {
    size_t a = 0;
    while (a < s.size() &&
           std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a &&
           std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parse_int64_full(const std::string& s, int64_t& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = static_cast<int64_t>(v);
    return true;
}

bool parse_int_full(const std::string& s, int& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    if (v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

// Parse `.settings`. Missing file → empty result (all has_* false, empty
// passthrough). Returns false only on a file-open failure of an existing
// file; per-line errors are silent-skip. Tab values are stored raw, without
// range validation — the caller clamps against the current audio file.
bool parse_settings_file(const std::string& path, ParsedSettings& out) {
    out = ParsedSettings{};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return true;  // nothing to load
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const std::string trimmed = trim_ws(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        const std::string key   = trim_ws(trimmed.substr(0, eq));
        const std::string value = trim_ws(trimmed.substr(eq + 1));
        if (key.empty()) continue;

        if (key == "tab_a_viewport_start") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_a_vp = true; out.tab_a_vp = v; }
        } else if (key == "tab_a_zoom") {
            int v;
            if (parse_int_full(value, v)) { out.has_tab_a_zoom = true; out.tab_a_zoom = v; }
        } else if (key == "tab_a_playhead") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_a_ph = true; out.tab_a_ph = v; }
        } else if (key == "tab_b_viewport_start") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_b_vp = true; out.tab_b_vp = v; }
        } else if (key == "tab_b_zoom") {
            int v;
            if (parse_int_full(value, v)) { out.has_tab_b_zoom = true; out.tab_b_zoom = v; }
        } else if (key == "tab_b_playhead") {
            int64_t v;
            if (parse_int64_full(value, v)) { out.has_tab_b_ph = true; out.tab_b_ph = v; }
        } else if (key == "follow") {
            std::string lower = value;
            for (char& c : lower) c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            if (lower == "true")       { out.has_follow = true; out.follow = true;  }
            else if (lower == "false") { out.has_follow = true; out.follow = false; }
            // Any other value: silent-skip; default (true) applies at the call site.
        } else {
            out.passthrough.emplace_back(key, value);
        }
    }
    return true;
}

// First-open default `.settings` template. Line ordering must match
// write_settings_file's output (passthrough fields first, then follow=,
// then the tab_a_* / tab_b_* triplets) — keep these in sync if either
// side changes.
std::string format_default_settings_template(const std::string& stem,
                                             const std::string& ext_no_dot) {
    std::string s;
    s += "title=";       s += stem; s += "-rendered\n";
    s += "audio_input="; s += stem; s += '.'; s += ext_no_dot; s += '\n';
    s += "scale=1.000000\n";
    s += "engine=warptempo\n";
    s += "N=4096\n";
    s += "fftw_threads=16\n";
    s += "limiter_enabled=false\n";
    s += "follow=true\n";
    s += "tab_a_viewport_start=0\n";
    s += "tab_a_zoom=0\n";
    s += "tab_a_playhead=0\n";
    s += "tab_b_viewport_start=0\n";
    s += "tab_b_zoom=0\n";
    s += "tab_b_playhead=0\n";
    return s;
}

// Atomic write: pass-through lines first in their original order, then the
// six canonical tab lines. Matches the `.warpmarkers` write pattern
// (tmp → fsync → rename). Best-effort: failure is logged by the caller.
bool write_settings_file(
    const std::string& path,
    const ViewState& tab_a,
    const ViewState& tab_b,
    bool follow,
    const std::vector<std::pair<std::string, std::string>>& passthrough) {
    std::string data;
    for (const auto& kv : passthrough) {
        data += kv.first;
        data += '=';
        data += kv.second;
        data += '\n';
    }
    data += "follow=";
    data += follow ? "true" : "false";
    data += '\n';
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_a.viewport_start_sample));
    data += "tab_a_viewport_start="; data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%d", tab_a.zoom_level);
    data += "tab_a_zoom=";            data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_a.playhead_sample));
    data += "tab_a_playhead=";        data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_b.viewport_start_sample));
    data += "tab_b_viewport_start="; data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%d", tab_b.zoom_level);
    data += "tab_b_zoom=";            data += buf; data += '\n';
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(tab_b.playhead_sample));
    data += "tab_b_playhead=";        data += buf; data += '\n';

    mode_t mode = 0644;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) mode = st.st_mode & 07777;

    const std::string tmp_path = path + ".tmp";
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;

    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written,
                                  data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp_path.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp_path.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    ::chmod(tmp_path.c_str(), mode);
    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* cli_path = nullptr;
    if (argc == 1) {
        // Empty window; wait for a drag-and-drop.
    } else if (argc == 2) {
        cli_path = argv[1];
    } else {
        std::fprintf(stderr, "usage: warptempo_gui [<audio_file>]\n");
        return 1;
    }

    AppState     app;
    GuiAudio     audio;
    GuiPlayback  playback;
    GuiX11       gui;
    WaveformCache wf_cache;
    if (!gui.init(app.width, app.height, "Warptempo")) {
        return 1;
    }

    // -- Viewport + invalidation helpers ------------------------------------
    //
    // X.7.1: the viewport-mutator and invalidation lambdas have been hoisted
    // onto the Viewport struct in viewport.{cpp,h}. The lambdas below are
    // one-line forwarders so callsites elsewhere in main() don't need to
    // change. `invalidate_timestamp_area` is gone — its body was
    // byte-identical to invalidate_timestamp_area, so all of its former
    // callsites now call invalidate_timestamp_area directly. `bottom_strip
    // _wide` was promoted to a free function in app_state.{h,cpp} in
    // X.7.8a so paint_handler.cpp can reach it without a capture.

    // V.A3b Addendum 3: forward-declared so the viewport methods below can
    // invoke it. The body is assigned later (after clear_hover_popup is in
    // scope; X.7.8b-2 made hit_test_flag a free function in app_state.{h,
    // cpp} so it's no longer a scope concern). Guarded inside Viewport
    // with a truthiness check because callbacks are wired after this
    // assignment.
    std::function<void()> recompute_hover_at_cursor;

    // X.7.3: forward-declared so the Undo struct can capture references.
    // Bodies are assigned later at their original definition sites — same
    // forward-declare-then-assign pattern as recompute_hover_at_cursor.
    std::function<void()> clear_hover_popup;
    std::function<void()> stop_playback_if_playing;

    // X.7.4: forward-declared so the Transients struct can capture
    // references. Bodies are assigned later at their original definition
    // sites — same pattern as clear_hover_popup / stop_playback_if_playing.
    std::function<FlagLoc(bool, bool, int)> find_flag;

    // X.7.6: forward-declared so GuiRenderView can capture a reference.
    // Body is assigned later at its original definition site — same
    // pattern as the four prior promotions.
    std::function<void()> refresh_active_tab_from_app;

    // X.7.8b-1: forward-declared so GuiInputHandler can capture references.
    // Bodies are assigned later at their original definition sites — same
    // pattern as the prior promotions. The on_key handler reads these
    // through the std::function refs; the residual lambdas stay in
    // main.cpp because their bodies still reach lambdas that have not yet
    // been hoisted (proceed_with_trigger, open_prompt_unsaved, etc.).
    std::function<bool()>                save_markers;
    std::function<void(DialogTrigger)>   request_close_or_revert;
    std::function<void(char)>            prompt_activate_response;
    std::function<void()>                toggle_playback;
    std::function<void(float)>           set_playback_speed;

    Viewport viewport(app, audio, gui, playback, recompute_hover_at_cursor);
    Selection selection(app, audio, viewport, playback);
    Undo undo(app, viewport, selection,
              clear_hover_popup, stop_playback_if_playing);
    GuiTransientMarkersOps transients(app, audio, viewport, selection, undo,
                                      clear_hover_popup, stop_playback_if_playing,
                                      find_flag);
    GuiWarpMarkersOps warpops(app, audio, gui, viewport, selection, undo,
                              clear_hover_popup, stop_playback_if_playing,
                              find_flag);
    GuiFlagEditor flag_editor(app, viewport, undo, clear_hover_popup);
    GuiRenderView render_view(app, audio, playback, gui, selection,
                              clear_hover_popup, refresh_active_tab_from_app);
    GuiTabMode tab_mode(app, audio, viewport, selection,
                        clear_hover_popup, stop_playback_if_playing);
    GuiPaintHandler paint_handler(app, audio, playback, wf_cache, gui);
    GuiInputHandler input_handler(app, audio, gui, playback,
                                  viewport, selection, undo,
                                  warpops, transients, flag_editor,
                                  render_view, tab_mode,
                                  clear_hover_popup, stop_playback_if_playing,
                                  save_markers, request_close_or_revert,
                                  prompt_activate_response, toggle_playback,
                                  set_playback_speed);

    auto trim_begin_sample           = [&]() { return viewport.trim_begin_sample(); };
    auto trim_end_sample             = [&]() { return viewport.trim_end_sample(); };
    auto invalidate_timestamp_area   = [&]() { viewport.invalidate_timestamp_area(); };
    auto invalidate_playhead_columns = [&](double a, double b) { viewport.invalidate_playhead_columns(a, b); };
    auto follow_scroll_if_needed     = [&]() { viewport.follow_scroll_if_needed(); };

    // -- Redraw -------------------------------------------------------------

    gui.set_on_redraw([&](cairo_t* cr, int x, int y, int w, int h) {
        paint_handler.on_redraw(cr, x, y, w, h);
    });

    gui.set_on_resize([&](int w, int h) {
        paint_handler.on_resize(w, h);
    });

    auto invalidate_top_strip     = [&]() { viewport.invalidate_top_strip(); };

    // X.7.8b-3: popup_eligible_marker moved to a free function in
    // app_state.{h,cpp}. The remaining callers in this TU
    // (recompute_hover_at_cursor below, on_tick) reach it directly with
    // the new (app, idx) signature; on_motion calls it from
    // input_handler.cpp. V.A3b / V.B comments live above the
    // declaration in app_state.h.

    // Reset the hover popup state. If the popup was visible, invalidate the
    // top strip so the next paint erases it. Safe to call from any path.
    clear_hover_popup = [&]() {
        const bool was_visible = app.hover_popup.visible;
        app.hover_popup = HoverPopupState{};
        if (was_visible) invalidate_top_strip();
    };

    // -- Undo/redo helpers --------------------------------------------------
    //
    // X.7.3: the undo-cluster lambdas have been hoisted onto the Undo struct
    // in undo.{cpp,h}. The lambdas below are one-line forwarders so callsites
    // elsewhere in main() don't need to change. apply_post_restore_rules_warp
    // and apply_post_restore_rules_transient have no callers outside the
    // undo cluster, so their forwarders are dropped — they remain public on
    // the Undo struct for consistency.

    auto recompute_dirty = [&]() { undo.recompute_dirty(); };

    // Cross-file flag scan. `want_begin` selects the b= scan vs the e=
    // scan. The (excl_trans, excl_idx) pair excludes one marker from the
    // search — used by toggle to skip the marker the user just toggled.
    // Pass excl_idx == -1 for no exclusion. Warp list is scanned first;
    // on a duplicate (parser-protected, only via hand-edit), the warp-
    // side hit wins to match compute_trim_samples.
    find_flag = [&](bool want_begin, bool excl_trans, int excl_idx)
        -> FlagLoc {
        FlagLoc f;
        const int sr = audio.sample_rate();
        const auto& mv = app.warpmarkers.markers();
        for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
            const bool has = want_begin ? mv[i].is_begin_time
                                        : mv[i].is_end_time;
            if (!has) continue;
            if (!excl_trans && i == excl_idx) continue;
            f.valid     = true;
            f.transient = false;
            f.idx       = i;
            f.frame     = static_cast<int64_t>(std::llround(
                mv[i].time_seconds * static_cast<double>(sr)));
            return f;
        }
        const auto& tv = app.transientmarkers.markers();
        for (int i = 0; i < static_cast<int>(tv.size()); ++i) {
            const bool has = want_begin ? tv[i].is_begin_time
                                        : tv[i].is_end_time;
            if (!has) continue;
            if (excl_trans && i == excl_idx) continue;
            f.valid     = true;
            f.transient = true;
            f.idx       = i;
            f.frame     = tv[i].effective_frame();
            return f;
        }
        return f;
    };

    // Gesture-stop: called at the top of any handler that will move the
    // visible playhead (keys, button press, Ctrl+wheel, undo/redo, tab
    // switch). Stops the audio thread and keeps the LSP in sync with the
    // visible playhead so the next Space-to-play captures the right
    // launch position. Does NOT return-to-launch — the gesture is about
    // to commit a new playhead position.
    stop_playback_if_playing = [&]() {
        if (!playback.is_playing() && !app.is_playing) return;
        playback.stop();
        app.is_playing        = false;
        app.last_space_sample = app.playhead_sample;
    };

    refresh_active_tab_from_app = [&]() { tab_mode.refresh_active_tab_from_app(); };

    save_markers = [&]() -> bool {
        if (app.warpmarkers_path.empty()) return false;
        if (app.first_save_pending && app.warpmarkers.had_nonstandard_content()) {
            std::fprintf(stderr,
                "warptempo_gui: first save in this session will discard "
                "comments and freeform text from %s. Canonical format will "
                "be written.\n",
                app.warpmarkers_path.c_str());
        }
        // Capture the active tab's current values before any writes — both
        // the .warpmarkers and .settings paths see a consistent snapshot.
        refresh_active_tab_from_app();

        const bool ok = app.warpmarkers.save(app.warpmarkers_path);
        if (!ok) {
            std::fprintf(stderr,
                "warptempo_gui: save failed: %s\n",
                app.warpmarkers_path.c_str());
            return false;
        }

        // Transients sibling write. Empty list deletes the on-disk file so
        // a project never carries a stale empty .transientmarkers.
        if (!app.transientmarkers_path.empty()) {
            if (app.transientmarkers.markers().empty()) {
                if (!app.transientmarkers.delete_file(app.transientmarkers_path)) {
                    std::fprintf(stderr,
                        "warptempo_gui: failed to delete: %s\n",
                        app.transientmarkers_path.c_str());
                }
            } else {
                if (!app.transientmarkers.save(app.transientmarkers_path)) {
                    std::fprintf(stderr,
                        "warptempo_gui: transient save failed: %s\n",
                        app.transientmarkers_path.c_str());
                    return false;
                }
            }
        }

        app.first_save_pending = false;
        // Save rebinds the saved reference to the current timeline position
        // without touching either stack — undo still reverts the last op.
        const bool was_dirty = app.dirty;
        app.history.mark_saved();
        recompute_dirty();
        if (was_dirty != app.dirty) {
            invalidate_timestamp_area();
        }

        // Best-effort .settings write. Failure is logged but does not fail
        // the overall save — the .warpmarkers write is the primary target.
        if (!app.settings_path.empty()) {
            if (!write_settings_file(app.settings_path,
                                     app.tab_a, app.tab_b,
                                     app.follow_mode,
                                     app.settings_passthrough)) {
                std::fprintf(stderr,
                    "warptempo_gui: settings save failed: %s: %s\n",
                    app.settings_path.c_str(),
                    std::strerror(errno));
            }
        }
        return true;
    };

    // Snap the visible playhead back to where Space was last pressed and
    // refresh the affected regions. Used by both Space-to-stop and natural
    // end-of-playback.
    auto restore_playhead_to_lsp = [&]() {
        const double old_px = playhead_pixel_x(app, audio);
        app.playhead_sample = app.last_space_sample;
        const double new_px = playhead_pixel_x(app, audio);
        invalidate_playhead_columns(old_px, new_px);
        invalidate_timestamp_area();
        // The triangle shares the top strip with any selected-flag
        // highlight; restore jumps can uncover/cover both, so invalidate
        // the flag strip too.
        const GuiRect ts = top_strip_area(app);
        gui.invalidate_region(ts.x, ts.y, ts.w, ts.h);
        app.is_playing      = false;
        app.playback_cursor = app.playhead_sample;
    };

    // -- Unsaved-work dialog + blank-state revert (chunk Q) -----------------

    auto invalidate_all = [&]() { viewport.invalidate_all(); };

    // Drop the currently loaded audio and reset all per-file UI state to
    // what the GUI looks like when launched with no argument. Leaves the
    // window open so the user can drop another file or quit. Bound to
    // Ctrl+W (proceed path from either the clean branch or the dialog).
    auto revert_to_blank = [&]() {
        // Stop the audio thread before the sample buffer it borrows goes
        // away. Same invariant as load_file.
        playback.stop();
        playback.shutdown();
        app.is_playing      = false;
        app.playback_cursor = 0;

        audio = GuiAudio{};
        app.audio_generation++;
        wf_cache.destroy_surface();

        app.playhead_sample       = 0;
        app.viewport_start_sample = 0;
        app.zoom_level            = 0;
        app.follow_mode           = true;
        app.last_space_sample     = 0;
        app.playback_speed        = 1.0f;

        app.warpmarkers.clear();
        app.transientmarkers.clear();
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        app.active_mode    = 'W';
        app.drag          = DragState{};
        app.playhead_drag = PlayheadDragState{};
        app.hover_popup   = HoverPopupState{};
        app.history.reset();
        app.dirty              = false;
        app.warp_dirty         = false;
        app.transient_dirty    = false;
        app.first_save_pending = true;

        app.warpmarkers_path.clear();
        app.transientmarkers_path.clear();
        app.settings_path.clear();
        app.source_audio_path.clear();
        app.pending_drop_path.clear();
        app.settings_passthrough.clear();

        app.tab_a = ViewState{};
        app.tab_b = ViewState{};
        app.active_tab = 'A';

        invalidate_all();
    };

    auto proceed_with_trigger = [&](DialogTrigger t) {
        switch (t) {
        case DialogTrigger::CLOSE_WINDOW:
            gui.request_exit();
            break;
        case DialogTrigger::REVERT_TO_BLANK:
            revert_to_blank();
            break;
        }
    };

    auto open_prompt_unsaved = [&](DialogTrigger t) {
        app.prompt.active          = true;
        app.prompt.text            = "Save unsaved changes?";
        // Sentinel chars for non-letter keys: 0x7F = Delete, 0x1B = Escape.
        // The keysym → char mapping in input_handler.cpp's prompt dispatch
        // produces these for XK_Delete / XK_Escape; the prompt machinery
        // remains a vector<char> match.
        app.prompt.response_keys   = {'s', '\x7f', '\x1b'};
        app.prompt.response_labels = {"[S]ave", "[Delete]", "[Esc]"};
        app.prompt.trigger         = t;
        clear_hover_popup();
        invalidate_all();
    };

    // Single-key response dispatch. The trigger captured at prompt-open
    // time selects which response set is in play; the key picks the
    // response. On a Save failure, the prompt mutates in place to a
    // retry/discard/cancel state — same trigger, new text and response
    // set — rather than dismissing.
    prompt_activate_response = [&](char k) {
        if (!app.prompt.active) return;
        const DialogTrigger trigger = app.prompt.trigger;
        // Sentinels: '\x7f' = Delete (discard), '\x1b' = Escape (cancel).
        // See open_prompt_unsaved above.

        if (trigger == DialogTrigger::CLOSE_WINDOW ||
            trigger == DialogTrigger::REVERT_TO_BLANK) {
            if (k == 's' || k == 'r') {
                const bool ok = save_markers();
                if (!ok) {
                    app.prompt.text            = "Save failed.";
                    app.prompt.response_keys   = {'r', '\x7f', '\x1b'};
                    app.prompt.response_labels =
                        {"[R]etry", "[Delete]", "[Esc]"};
                    invalidate_all();
                    return;
                }
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == '\x7f') {
                app.prompt.active = false;
                invalidate_all();
                proceed_with_trigger(trigger);
                return;
            }
            if (k == '\x1b') {
                app.prompt.active = false;
                invalidate_all();
                return;
            }
            return;
        }
    };

    // Route a close / revert gesture through the prompt when history is
    // dirty; otherwise proceed immediately. Centralizes the decision so
    // Ctrl+Q, Ctrl+W, and the WM-close callback share identical behavior.
    request_close_or_revert = [&](DialogTrigger t) {
        if (app.prompt.active) return; // already gated; ignore re-entry
        if (app.dirty) open_prompt_unsaved(t);
        else           proceed_with_trigger(t);
    };

    // Space-bar: start/stop playback. Playback runs from the playhead to
    // trim_end (or total_frames if no e= marker). Pressing space with the
    // playhead at or past trim-end is a silent no-op. Space-to-stop
    // returns the visible playhead to the position where Space-to-play
    // was last pressed (return-to-launch).
    toggle_playback = [&]() {
        if (playback.is_playing()) {
            playback.stop();
            restore_playhead_to_lsp();
            return;
        }
        const int64_t end = trim_end_sample();
        if (app.playhead_sample >= end) return;
        // Clamp the start position into the trim range in case the playhead
        // is sitting at trim_end - 1 (valid) or somehow slipped.
        const int64_t start = std::max(app.playhead_sample, trim_begin_sample());
        app.last_space_sample = app.playhead_sample;
        app.playback_cursor = start;
        app.is_playing = true;
        playback.set_speed(app.playback_speed);
        playback.play(start, end);
    };

    // Helpers for Shift+<digit> speed selection.
    set_playback_speed = [&](float s) {
        app.playback_speed = s;
        playback.set_speed(s);
        // Speed change without resync would cause a backward cursor jump:
        // the predictor would retroactively apply the new speed to the
        // entire elapsed-since-anchor period.
        if (playback.is_playing()) playback.resync_predictor();
    };

    // V.A3b Addendum 3: re-evaluate hover at the cursor's last on_motion
    // coordinates. Called after viewport mutations (zoom, scroll, center,
    // playhead-driven viewport shift) so a stationary cursor's hover state
    // tracks the rects that just slid under it. Mirrors the on_motion
    // hover-detection branch: same gating, same hit-test, same state
    // transitions; the tick handler still drives the dwell-to-visible flip.
    recompute_hover_at_cursor = [&]() {
        if (app.last_mouse_x < 0 || app.last_mouse_y < 0) return;
        // Dialog / drag / editor / queue still suppress hover in either
        // view. Source-view also requires warp mode + iter mode off;
        // render-view bypasses the mode checks because hover always
        // applies against the loaded render's warpmarkers.
        if (app.prompt.active ||
            app.drag.active ||
            app.playhead_drag.active ||
            text_editor::is_active(app.top_flag_editor) ||
            app.queue_running) {
            clear_hover_popup();
            return;
        }
        if (!app.render_view_enabled &&
            (app.active_mode != 'W' || app.iteration_mode_enabled)) {
            clear_hover_popup();
            return;
        }
        const int hit = hit_test_flag(app, audio,
                                      app.last_mouse_x, app.last_mouse_y);
        if (hit != app.hover_popup.marker_index) {
            if (app.hover_popup.visible) invalidate_top_strip();
            app.hover_popup.marker_index = hit;
            app.hover_popup.visible      = false;
            app.hover_popup.entry_time   =
                std::chrono::steady_clock::now();
            // Precompute the popup's display text at rect-entry so the
            // delay-completion paint doesn't have to recompute it. Empty
            // when `hit` is not popup-eligible (the redraw branch then
            // skips paint and keeps the strip clean).
            if (app.render_view_enabled) {
                app.hover_popup.cached_text =
                    popup_eligible_marker(app, hit)
                        ? compute_hover_popup_text(
                              app.render_view_markers, hit,
                              app.render_view_src_sr)
                        : std::string();
            } else {
                app.hover_popup.cached_text =
                    popup_eligible_marker(app, hit)
                        ? compute_hover_popup_text(
                              app.warpmarkers.markers(), hit,
                              audio.sample_rate())
                        : std::string();
            }
        }
    };

    // X.7.5a: the drag and selection-shift lambdas have been hoisted onto
    // the GuiWarpMarkersOps struct in warpmarkers_ops.{cpp,h}.

    // X.7.8b-2: the shared wheel handler (handle_wheel) moved to
    // GuiInputHandler as a private helper method. on_button_press is
    // its only caller after this brief.

    // X.7.8b-1: the multi-render queue runner (run_render_batch +
    // RenderBatchResult) had no callers outside the on_key body. It moved
    // to GuiInputHandler as a private helper method (see input_handler.h).

    gui.set_on_key([&](KeySym keysym, unsigned int mods) {
        input_handler.on_key(keysym, mods);
    });

    gui.set_on_close([&]() {
        // Window-manager close (title-bar X) routes through the unsaved-
        // work dialog when dirty, same as Ctrl+Q.
        request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
    });

    gui.set_on_button_press([&](unsigned int button, int x, int y,
                                unsigned int mods) {
        input_handler.on_button_press(button, x, y, mods);
    });

    gui.set_on_button_release([&](unsigned int button, int x, int y,
                                  unsigned int mods) {
        input_handler.on_button_release(button, x, y, mods);
    });

    gui.set_on_motion([&](int mouse_x, int mouse_y, unsigned int mods) {
        input_handler.on_motion(mouse_x, mouse_y, mods);
    });

    // -- File loading --------------------------------------------------------

    // Loads `path` into a fresh GuiAudio, preflights via libsndfile, and
    // swaps the new audio in on success. On failure, the prior audio (if
    // any) remains loaded unchanged. Resets per-file navigation state;
    // follow_mode is reapplied from the loaded file's .settings (default
    // true when absent).
    auto load_file = [&](const std::string& path) -> bool {
        // Preflight.
        {
            SF_INFO probe_info;
            std::memset(&probe_info, 0, sizeof(probe_info));
            SNDFILE* probe = sf_open(path.c_str(), SFM_READ, &probe_info);
            if (!probe) {
                std::fprintf(stderr,
                             "warptempo_gui: '%s': %s\n",
                             path.c_str(), sf_strerror(nullptr));
                return false;
            }
            sf_close(probe);
        }

        // Stop and tear down the audio device before the sample buffer it
        // borrows is replaced. Playing into a freed buffer would crash the
        // audio thread. Order (stop → shutdown → load → init) is fixed.
        playback.stop();
        playback.shutdown();
        app.is_playing     = false;
        app.playback_cursor = 0;
        app.hover_popup    = HoverPopupState{};

        app.loading       = true;
        app.load_progress = 0.0f;
        gui.invalidate_region(0, 0, app.width, app.height);
        gui.drain_events();

        GuiAudio next;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = next.load(path, [&](float p) {
            app.load_progress = p;
            const int bar_y = app.height - kProgressBarHeight;
            gui.invalidate_region(0, bar_y, app.width, kProgressBarHeight);
            gui.drain_events();
        });
        const auto t1 = std::chrono::steady_clock::now();

        if (!ok) {
            app.loading       = false;
            app.load_progress = 0.0f;
            gui.invalidate_region(0, 0, app.width, app.height);
            return false;
        }

        audio = std::move(next);
        app.audio_generation++;
        app.loading       = false;
        app.load_progress = 0.0f;

        app.playhead_sample       = 0;
        app.viewport_start_sample = 0;
        const int max_num = max_valid_numeric_level(
            waveform_area(app).w, audio.total_frames(), audio.sample_rate());
        app.zoom_level = (max_num >= 0) ? 0 : kFitFileLevel;
        clamp_viewport_start(app, audio);

        // Reset playback bookkeeping; the device is brought up after markers
        // are parsed so the initial playhead has the final trim-begin.
        app.playback_speed = 1.0f;

        // Companion files: discover paths, create <basename>.warpmarkers
        // and <basename>.settings if missing. Companion file convention is
        // <source_dir>/<source_basename>.<ext> (sibling, basename-prefixed),
        // not the legacy hidden `./.warpmarkers` form.
        std::filesystem::path apath(path);
        std::filesystem::path parent = apath.parent_path();
        if (parent.empty()) parent = std::filesystem::path(".");
        const std::string stem = apath.stem().string();
        const std::string ext = apath.extension().string();
        const std::string ext_no_dot = ext.empty() ? "" : ext.substr(1);
        const std::filesystem::path wm_path  = parent / (stem + ".warpmarkers");
        const std::filesystem::path tm_path  = parent / (stem + ".transientmarkers");
        const std::filesystem::path set_path = parent / (stem + ".settings");
        app.warpmarkers_path      = wm_path.string();
        app.transientmarkers_path = tm_path.string();
        app.settings_path         = set_path.string();
        app.source_audio_path     = path;

        create_if_missing(wm_path, "00:00.000|1.00\n");
        create_if_missing(set_path,
                          format_default_settings_template(stem, ext_no_dot));

        // Load the markers file. Parse failures are non-fatal: we log each
        // error to stderr and leave app.warpmarkers empty. The GUI still works
        // as a waveform viewer.
        app.warpmarkers.clear();
        app.transientmarkers.clear();
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        app.active_mode    = 'W';
        app.drag = DragState{};
        app.playhead_drag = PlayheadDragState{};
        // Fresh file = fresh history. Both stacks cleared; the loaded state
        // is the saved baseline (signed_distance = 0, valid).
        app.history.reset();
        app.dirty              = false;
        app.warp_dirty         = false;
        app.transient_dirty    = false;
        app.first_save_pending = true;
        const bool markers_ok = app.warpmarkers.load(wm_path.string());
        if (!markers_ok) {
            for (const auto& err : app.warpmarkers.errors()) {
                if (err.line_number > 0) {
                    std::fprintf(stderr,
                                 "warptempo_gui: %s:%d: %s\n",
                                 wm_path.string().c_str(),
                                 err.line_number, err.message.c_str());
                } else {
                    std::fprintf(stderr,
                                 "warptempo_gui: %s: %s\n",
                                 wm_path.string().c_str(),
                                 err.message.c_str());
                }
            }
        } else {
            std::fprintf(stderr,
                         "[warptempo_gui] parsed %zu markers from %s\n",
                         app.warpmarkers.markers().size(),
                         wm_path.string().c_str());
        }

        // Load .transientmarkers if present. Missing file is fine — the
        // transient list is just empty. Parse errors are logged to stderr;
        // the warp side stays usable regardless.
        if (std::filesystem::exists(tm_path)) {
            const bool tr_ok = app.transientmarkers.load(tm_path.string());
            if (!tr_ok) {
                for (const auto& err : app.transientmarkers.errors()) {
                    if (err.line_number > 0) {
                        std::fprintf(stderr,
                                     "warptempo_gui: %s:%d: %s\n",
                                     tm_path.string().c_str(),
                                     err.line_number, err.message.c_str());
                    } else {
                        std::fprintf(stderr,
                                     "warptempo_gui: %s: %s\n",
                                     tm_path.string().c_str(),
                                     err.message.c_str());
                    }
                }
            } else {
                std::fprintf(stderr,
                             "[warptempo_gui] parsed %zu transients from %s\n",
                             app.transientmarkers.markers().size(),
                             tm_path.string().c_str());
            }
        }

        // Initial playhead: land at trim-begin if a b= marker was parsed,
        // otherwise sample 0. Must happen after marker parse so the trim
        // range reflects the on-disk state. Scroll the viewport so the
        // playhead is visible rather than lurking off the left edge.
        app.playhead_sample = trim_begin_sample();
        if (app.zoom_level != kFitFileLevel) {
            app.viewport_start_sample = app.playhead_sample;
            clamp_viewport_start(app, audio);
        }

        // Seed both tabs with the freshly-computed default post-load state.
        // Parsed .settings values overwrite per-key below.
        ViewState default_tab;
        default_tab.viewport_start_sample = app.viewport_start_sample;
        default_tab.zoom_level            = app.zoom_level;
        default_tab.playhead_sample       = app.playhead_sample;
        app.tab_a          = default_tab;
        app.tab_b          = default_tab;
        app.active_tab     = 'A';
        app.settings_passthrough.clear();

        // Parse .settings (if present) and apply tab values with silent
        // coerce on out-of-range. Missing file → all keys default.
        {
            ParsedSettings ps;
            if (!parse_settings_file(app.settings_path, ps)) {
                std::fprintf(stderr,
                    "warptempo_gui: could not read '%s'\n",
                    app.settings_path.c_str());
            }
            const int64_t total = audio.total_frames();
            auto valid_zoom = [](int z) -> bool {
                if (z == kFitFileLevel) return true;
                return z >= 0 && z < kNumZoomLevels;
            };
            auto apply = [&](bool has_vp, int64_t vp,
                             bool has_zoom, int zoom,
                             bool has_ph, int64_t ph,
                             ViewState& dst) {
                if (has_vp   && vp   >= 0 && vp   <  total)  dst.viewport_start_sample = vp;
                if (has_zoom && valid_zoom(zoom))            dst.zoom_level            = zoom;
                if (has_ph   && ph   >= 0 && ph   <= total)  dst.playhead_sample       = ph;
            };
            apply(ps.has_tab_a_vp, ps.tab_a_vp,
                  ps.has_tab_a_zoom, ps.tab_a_zoom,
                  ps.has_tab_a_ph, ps.tab_a_ph, app.tab_a);
            apply(ps.has_tab_b_vp, ps.tab_b_vp,
                  ps.has_tab_b_zoom, ps.tab_b_zoom,
                  ps.has_tab_b_ph, ps.tab_b_ph, app.tab_b);
            app.follow_mode = ps.has_follow ? ps.follow : true;
            app.settings_passthrough = std::move(ps.passthrough);
        }

        // Activate tab A: copy its snapshot into the live AppState fields.
        app.viewport_start_sample = app.tab_a.viewport_start_sample;
        app.zoom_level            = app.tab_a.zoom_level;
        app.playhead_sample       = app.tab_a.playhead_sample;
        clamp_viewport_start(app, audio);

        // Bring up the audio device bound to the new sample buffer. Init
        // failure disables playback but leaves the rest of the GUI usable.
        if (!playback.init(audio.sample_rate(), audio.channels(),
                           audio.samples_ptr(), audio.total_frames())) {
            std::fprintf(stderr,
                "warptempo_gui: playback disabled; space bar will no-op.\n");
        }

        const double load_ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::fprintf(stderr,
                     "[warptempo_gui] loaded %s: sr=%d, channels=%d, frames=%lld, "
                     "pyramid_levels=%d, load_time=%.1f ms\n",
                     path.c_str(), audio.sample_rate(), audio.channels(),
                     static_cast<long long>(audio.total_frames()),
                     audio.num_levels(), load_ms);

        gui.invalidate_region(0, 0, app.width, app.height);
        return true;
    };

    // Process `path` and any drops that arrived while the load was running.
    // Pending slot is last-wins, not a queue; rapid drags collapse.
    auto load_then_drain = [&](std::string path) {
        while (true) {
            load_file(path);
            if (app.pending_drop_path.empty()) break;
            path = std::move(app.pending_drop_path);
            app.pending_drop_path.clear();
        }
    };

    gui.set_drop_accept_predicate([&](int x, int y) -> bool {
        const GuiRect area = waveform_area(app);
        return x >= area.x && x < area.x + area.w &&
               y >= area.y && y < area.y + area.h;
    });

    gui.set_on_file_drop([&](const std::string& path) {
        if (app.loading) {
            app.pending_drop_path = path;
            return;
        }
        load_then_drain(path);
    });

    // Tick: runs once per event-loop iteration. During playback, snapshots
    // the audio thread's cursor and mirrors it into the main-thread playhead,
    // invalidating just the columns and timestamp that changed. Also
    // detects natural end-of-playback via the atomic playing flag.
    gui.set_on_tick([&]() {
        // Blink the editor cursor independently of playback. Compare the
        // current visibility against the last painted state and invalidate
        // the top strip when it flips. Cheap: top_strip is small.
        if (text_editor::is_active(app.top_flag_editor)) {
            const bool now_visible =
                text_editor::cursor_visible_now(app.top_flag_editor);
            if (now_visible != app.top_flag_editor_blink_last) {
                app.top_flag_editor_blink_last = now_visible;
                invalidate_top_strip();
            }
        }

        // V.A3b: dwell-driven popup show. The motion handler already gates
        // on warp mode + no editor + no drag + no dialog and clears
        // hover_popup when those conditions break, so here it's enough to
        // check the elapsed time and re-validate eligibility.
        if (!app.hover_popup.visible &&
            app.hover_popup.marker_index >= 0 &&
            popup_eligible_marker(app, app.hover_popup.marker_index)) {
            const auto now = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - app.hover_popup.entry_time).count();
            if (ms >= kHoverDelayMs) {
                app.hover_popup.visible = true;
                invalidate_top_strip();
            }
        }

        if (app.loading || audio.total_frames() <= 0) return;

        const bool ma_playing = playback.is_playing();
        if (!app.is_playing && !ma_playing) return;

        if (ma_playing) {
            const int64_t cur = playback.cursor();
            if (cur != app.playhead_sample) {
                const double old_px = playhead_pixel_x(app, audio);
                app.playback_cursor = cur;
                app.playhead_sample = cur;
                const double new_px = playhead_pixel_x(app, audio);
                invalidate_playhead_columns(old_px, new_px);
                invalidate_timestamp_area();
                if (app.follow_mode) follow_scroll_if_needed();
            }
            app.is_playing = true;
            return;
        }

        // Playing was true last tick, now false — natural end. Return the
        // visible playhead to the launch position (same as Space-to-stop).
        if (app.is_playing) {
            restore_playhead_to_lsp();
            if (app.follow_mode) follow_scroll_if_needed();
        }
    });

    // Idle timeout: wake the poll loop every ~16 ms during playback so the
    // tick can advance the playhead even in the absence of input events.
    gui.set_idle_timeout_provider([&]() -> int {
        if (app.is_playing || playback.is_playing()) return gui.playback_tick_ms();
        // While the top-flag editor is active, wake periodically so the
        // cursor blink can flip. ~125ms gives ample resolution on a
        // 500ms half-period without burning CPU.
        if (text_editor::is_active(app.top_flag_editor)) return 125;
        // V.A3b: while a hover is pending (dwell timer running but popup
        // not yet visible), wake periodically so the tick-driven check
        // can flip visibility on time. Once visible, no extra wake is
        // needed — input events drive any state change.
        if (!app.hover_popup.visible && app.hover_popup.marker_index >= 0) {
            return 125;
        }
        return -1;
    });

    // Paint the initial background before any synchronous load begins so the
    // window isn't briefly blank on fast disks.
    gui.invalidate_region(0, 0, app.width, app.height);
    gui.drain_events();

    if (cli_path) {
        if (!load_file(cli_path)) {
            gui.shutdown();
            return 1;
        }
        // Any drops queued during the startup load run now.
        while (!app.pending_drop_path.empty()) {
            std::string next = std::move(app.pending_drop_path);
            app.pending_drop_path.clear();
            load_file(next);
        }
    }

    gui.run();
    // Tear the audio device down before the sample buffer goes out of scope.
    playback.shutdown();
    gui.shutdown();
    return 0;
}
