#include "selection.h"

#include "audio.h"
#include "playback.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

void Selection::repair_last_selected() {
    if (app.last_selected_marker < 0) return;
    if (app.selected_markers.count(app.last_selected_marker)) return;
    if (app.selected_markers.empty()) {
        app.last_selected_marker = -1;
    } else {
        // Pick the largest remaining index (spec: "the largest remaining
        // index in selected_markers, or -1 if empty").
        app.last_selected_marker = *app.selected_markers.rbegin();
    }
}

void Selection::set_single_selection(int idx) {
    app.selected_markers.clear();
    if (idx >= 0) app.selected_markers.insert(idx);
    app.last_selected_marker = (idx >= 0) ? idx : -1;
    viewport.invalidate_top_strip();
}

void Selection::clear_selection() {
    if (app.selected_markers.empty() && app.last_selected_marker == -1) return;
    app.selected_markers.clear();
    app.last_selected_marker = -1;
    viewport.invalidate_top_strip();
}

bool Selection::toggle_selection_membership(int idx) {
    if (idx < 0) return false;
    bool added;
    auto it = app.selected_markers.find(idx);
    if (it == app.selected_markers.end()) {
        app.selected_markers.insert(idx);
        app.last_selected_marker = idx;
        added = true;
    } else {
        app.selected_markers.erase(it);
        if (app.last_selected_marker == idx) repair_last_selected();
        added = false;
    }
    viewport.invalidate_top_strip();
    return added;
}

void Selection::sanitize_selection_after_restore(int n) {
    std::set<int> cleaned;
    for (int idx : app.selected_markers) {
        if (idx >= 0 && idx < n) cleaned.insert(idx);
    }
    app.selected_markers = std::move(cleaned);
    if (!app.selected_markers.count(app.last_selected_marker)) {
        app.last_selected_marker = -1;
    }
}

void Selection::cycle_selection(bool forward) {
    const int sr = audio.sample_rate();
    const bool transient = (app.active_mode == 'T');
    const int n = transient
        ? static_cast<int>(app.transientmarkers.markers().size())
        : static_cast<int>(app.warpmarkers.markers().size());
    if (n == 0) return;

    // Helper to read frame-of-index in source samples regardless of mode.
    auto frame_of = [&](int i) -> int64_t {
        if (transient) {
            return static_cast<int64_t>(std::nearbyint(
                app.transientmarkers.markers()[i].time_seconds *
                static_cast<double>(sr)));
        }
        return static_cast<int64_t>(std::nearbyint(
            app.warpmarkers.markers()[i].time_seconds *
            static_cast<double>(sr)));
    };

    const int sel_size = static_cast<int>(app.selected_markers.size());
    int new_sel = -1;

    if (sel_size >= 2) {
        const int anchor = app.last_selected_marker;
        if (forward) {
            auto it = app.selected_markers.upper_bound(anchor);
            if (it == app.selected_markers.end()) {
                it = app.selected_markers.begin();
            }
            new_sel = *it;
        } else {
            auto it = app.selected_markers.lower_bound(anchor);
            if (it == app.selected_markers.begin()) {
                it = app.selected_markers.end();
            }
            --it;
            new_sel = *it;
        }
    } else if (sel_size == 1) {
        const int cur = *app.selected_markers.begin();
        const int64_t cur_f = frame_of(cur);
        if (forward) {
            for (int i = cur + 1; i < n; ++i) {
                if (frame_of(i) > cur_f) { new_sel = i; break; }
            }
        } else {
            for (int i = cur - 1; i >= 0; --i) {
                if (frame_of(i) < cur_f) { new_sel = i; break; }
            }
        }
    } else {
        const int64_t ph_f = app.playhead_sample;
        if (forward) {
            for (int i = 0; i < n; ++i) {
                if (frame_of(i) >= ph_f) { new_sel = i; break; }
            }
        } else {
            for (int i = n - 1; i >= 0; --i) {
                if (frame_of(i) <= ph_f) { new_sel = i; break; }
            }
        }
    }

    if (new_sel < 0) return;

    if (sel_size >= 2) {
        // Within-set cycling: only the focus changes.
        if (new_sel == app.last_selected_marker) return;
        app.last_selected_marker = new_sel;
        viewport.invalidate_top_strip();
    } else {
        if (app.selected_markers.count(new_sel) &&
            app.last_selected_marker == new_sel) return;
        set_single_selection(new_sel);
    }

    const int64_t sample = frame_of(new_sel);
    const int64_t visible = samples_visible(app, audio);
    if (visible > 0) {
        const int64_t old_vp = app.viewport_start_sample;
        const int64_t vp_end = old_vp + visible;
        if (sample < old_vp) {
            app.viewport_start_sample = sample;
        } else if (sample >= vp_end) {
            const double spp = current_samples_per_pixel(app, audio);
            const int64_t one_px =
                static_cast<int64_t>(std::nearbyint(spp));
            app.viewport_start_sample =
                sample - (visible - std::max<int64_t>(one_px, 1));
        }
        clamp_viewport_start(app, audio);
        if (app.viewport_start_sample != old_vp) {
            viewport.invalidate_waveform_area();
        }
    }
}

void Selection::select_next_marker() { cycle_selection(true);  }
void Selection::select_prev_marker() { cycle_selection(false); }

void Selection::prune_live_selection() {
    int n = 0;
    if (app.render_view_enabled) {
        n = (app.active_mode == 'T')
            ? static_cast<int>(app.render_view_transients.size())
            : static_cast<int>(app.render_view_markers.size());
    } else {
        n = (app.active_mode == 'T')
            ? static_cast<int>(app.transientmarkers.markers().size())
            : static_cast<int>(app.warpmarkers.markers().size());
    }
    for (auto it = app.selected_markers.begin();
         it != app.selected_markers.end();) {
        if (*it < 0 || *it >= n) {
            it = app.selected_markers.erase(it);
        } else {
            ++it;
        }
    }
    if (app.last_selected_marker < 0 ||
        app.last_selected_marker >= n ||
        !app.selected_markers.count(app.last_selected_marker)) {
        app.last_selected_marker =
            app.selected_markers.empty()
                ? -1
                : *app.selected_markers.rbegin();
    }
}

void Selection::sync_playhead_to_last_selected() {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const int last = app.last_selected_marker;
    if (last < 0) return;

    int64_t target_sample = 0;
    if (app.active_mode == 'T') {
        const auto& tv = app.transientmarkers.markers();
        if (last >= static_cast<int>(tv.size())) return;
        target_sample = static_cast<int64_t>(std::nearbyint(
            tv[last].time_seconds * static_cast<double>(sr)));
    } else {
        const auto& mv = app.warpmarkers.markers();
        if (last >= static_cast<int>(mv.size())) return;
        target_sample = static_cast<int64_t>(std::nearbyint(
            mv[last].time_seconds * static_cast<double>(sr)));
    }
    jump_playhead_to(target_sample);
}

void Selection::jump_playhead_to(int64_t target_sample) {
    app.playhead_sample = target_sample;

    const int64_t visible = samples_visible(app, audio);
    const bool offscreen =
        target_sample <  app.viewport_start_sample ||
        target_sample >= app.viewport_start_sample + visible;
    if (offscreen) {
        app.viewport_start_sample = target_sample - visible / 2;
        clamp_viewport_start(app, audio);
    }
    if (playback.is_playing()) playback.resync_predictor();
}
