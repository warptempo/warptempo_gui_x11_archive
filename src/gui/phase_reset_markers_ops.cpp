#include "phase_reset_markers_ops.h"

#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <utility>
#include <vector>

// X.7.4: phase reset authoring cluster. Method bodies are byte-identical to
// the lambdas they replaced in main.cpp, with these mechanical rewrites:
//
//   push_undo*                  → undo.push_undo*
//   recompute_dirty             → undo.recompute_dirty
//   sync_playhead_to_last_selected → selection.sync_playhead_to_last_selected
//   invalidate_waveform_area    → viewport.invalidate_waveform_area
//   invalidate_timestamp_area   → viewport.invalidate_timestamp_area
//   invalidate_all              → viewport.invalidate_all
//   move_playhead_to            → viewport.move_playhead_to
//   apply_phase_reset_position_delta → free function (defined in main.cpp)
//   current_samples_per_pixel   → free function
//   stop_playback_if_playing,
//   clear_hover_popup,
//   find_flag                   → std::function refs (called as f())

// Drop a phase reset marker at `time_seconds`. Rejects creation within
// 3 pixels at current zoom of an existing phase reset marker. Selection
// collapses to the freshly-inserted index. Frame-0 phase alignment is
// implicit by definition and needs no marker to assert it.
void GuiPhaseResetMarkersOps::drop_phase_reset_at_position(double time_seconds) {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double sr_d = static_cast<double>(sr);
    const double spp  = current_samples_per_pixel(app, audio);
    const double eps  = 3.0 * spp / sr_d;  // 3 pixels at current zoom
    const auto& tv = app.phase_reset_markers.markers();
    for (const auto& m : tv) {
        if (std::abs(m.time_seconds - time_seconds) < eps) {
            std::fprintf(stderr,
                "warptempo_gui: phase_reset marker already exists near %.3fs\n",
                time_seconds);
            return;
        }
    }
    std::vector<GuiPhaseResetMarker> pre_state = app.phase_reset_markers.markers();
    const int                 hint_last = app.last_selected_marker;
    GuiPhaseResetMarker nm;
    nm.time_seconds = time_seconds;
    const int new_idx = app.phase_reset_markers.insert_marker(std::move(nm));
    app.selected_markers.clear();
    app.selected_markers.insert(new_idx);
    app.last_selected_marker = new_idx;
    undo.push_undo_phase_reset(std::move(pre_state), OpKind::Create, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
    // Match drop_marker: move playhead to the new phase reset. When
    // dropping at the current playhead, this is a no-op.
    viewport.move_playhead_to(static_cast<int64_t>(std::nearbyint(
        time_seconds * static_cast<double>(sr))));
}

void GuiPhaseResetMarkersOps::drop_phase_reset_at_playhead() {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double t = static_cast<double>(app.playhead_sample) /
                     static_cast<double>(sr);
    drop_phase_reset_at_position(t);
}

// Delete every selected phase reset. No label/cascade rules — phase resets
// don't have labels. Any selected phase reset is deletable, including one
// that happens to sit at time 0 — frame-0 phase alignment is implicit
// and needs no marker to assert it.
void GuiPhaseResetMarkersOps::delete_selected_phase_reset() {
    if (app.selected_markers.empty()) return;
    const auto& tv = app.phase_reset_markers.markers();
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(tv.size())) {
            std::fprintf(stderr,
                "warptempo_gui: phase_reset delete rejected: stale selection index\n");
            return;
        }
    }
    std::vector<GuiPhaseResetMarker> pre_state = app.phase_reset_markers.markers();
    const int                 hint_last = app.last_selected_marker;
    for (auto it = app.selected_markers.rbegin();
         it != app.selected_markers.rend(); ++it) {
        app.phase_reset_markers.remove_marker(*it);
    }
    app.selected_markers.clear();
    app.last_selected_marker = -1;
    undo.push_undo_phase_reset(std::move(pre_state), OpKind::Destroy, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Toggle the disabled flag on each selected phase reset. Unconditional —
// phase resets have no label-def gating like warp markers do.
void GuiPhaseResetMarkersOps::toggle_phase_reset_disabled() {
    if (app.selected_markers.empty()) return;
    std::vector<GuiPhaseResetMarker> pre_state = app.phase_reset_markers.markers();
    const int                 hint_last = app.last_selected_marker;
    bool changed = false;
    for (int idx : app.selected_markers) {
        GuiPhaseResetMarker* m = app.phase_reset_markers.marker_mut(idx);
        if (!m) continue;
        m->disabled = !m->disabled;
        changed = true;
    }
    if (!changed) return;
    undo.push_undo_phase_reset(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Compute (delta_min, delta_max) seconds bounds for shifting the
// currently-selected phase resets by a uniform delta. Same shape as the
// warp version: nearest non-selected neighbor on each side, intersected,
// with a 3-pixel-at-current-zoom visual gap enforced via eps. No trim
// clamp — phase resets aren't bounded by trim flags during edit.
std::pair<double, double> GuiPhaseResetMarkersOps::compute_phase_reset_delta_bounds(bool& ok) {
    ok = false;
    const auto& tv = app.phase_reset_markers.markers();
    if (app.selected_markers.empty()) return {0.0, 0.0};
    const int sr = audio.sample_rate();
    if (sr <= 0) return {0.0, 0.0};
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(tv.size())) return {0.0, 0.0};
    }
    const double sr_d = static_cast<double>(sr);
    const double spp  = current_samples_per_pixel(app, audio);
    const double eps  = 3.0 * spp / sr_d;  // 3 pixels at current zoom
    const double total_duration =
        static_cast<double>(audio.total_frames()) / sr_d;

    double d_min = -std::numeric_limits<double>::infinity();
    double d_max =  std::numeric_limits<double>::infinity();
    for (int idx : app.selected_markers) {
        const double orig_t = tv[idx].time_seconds;
        int prev = idx - 1;
        while (prev >= 0 && app.selected_markers.count(prev)) --prev;
        if (prev >= 0) {
            const double lb = (tv[prev].time_seconds + eps) - orig_t;
            if (lb > d_min) d_min = lb;
        } else {
            const double lb = eps - orig_t;
            if (lb > d_min) d_min = lb;
        }
        int next = idx + 1;
        while (next < static_cast<int>(tv.size()) &&
               app.selected_markers.count(next)) ++next;
        if (next < static_cast<int>(tv.size())) {
            const double ub = (tv[next].time_seconds - eps) - orig_t;
            if (ub < d_max) d_max = ub;
        } else {
            const double ub = (total_duration - eps) - orig_t;
            if (ub < d_max) d_max = ub;
        }
    }
    ok = true;
    return {d_min, d_max};
}

// Nudge selected phase resets by +/- 1 source-pixel of seconds. Direction:
// -1 for earlier, +1 for later. Symmetric with nudge_selected_markers.
void GuiPhaseResetMarkersOps::nudge_selected_phase_resets(int direction) {
    if (app.loading || audio.total_frames() <= 0) return;
    stop_playback_if_playing();
    if (app.selected_markers.empty()) return;
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double spp = current_samples_per_pixel(app, audio);
    const double delta_s =
        static_cast<double>(direction) * spp / static_cast<double>(sr);
    if (delta_s == 0.0) return;

    bool ok = false;
    auto [d_min, d_max] = compute_phase_reset_delta_bounds(ok);
    if (!ok) return;
    double delta = delta_s;
    if (delta < d_min) delta = d_min;
    if (delta > d_max) delta = d_max;
    if (delta == 0.0) return;

    std::vector<GuiPhaseResetMarker> pre_state = app.phase_reset_markers.markers();
    const int                 hint_last = app.last_selected_marker;
    for (int idx : app.selected_markers) {
        GuiPhaseResetMarker* m = app.phase_reset_markers.marker_mut(idx);
        if (!m) continue;
        apply_phase_reset_position_delta(*m, delta);
    }
    undo.push_undo_phase_reset(std::move(pre_state), OpKind::Move, hint_last);
    selection.sync_playhead_to_last_selected();
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// `j` for phase reset mode: shift the selection so last_selected lands
// on the playhead. All-or-nothing clamp check.
void GuiPhaseResetMarkersOps::jump_phase_reset_selection_to_playhead() {
    if (app.selected_markers.empty()) return;
    if (app.last_selected_marker < 0) return;
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const auto& tv = app.phase_reset_markers.markers();
    if (app.last_selected_marker >= static_cast<int>(tv.size())) return;
    const double anchor_t = tv[app.last_selected_marker].time_seconds;
    const double ph_t     = static_cast<double>(app.playhead_sample) /
                            static_cast<double>(sr);
    const double delta    = ph_t - anchor_t;
    if (delta == 0.0) return;

    bool ok = false;
    auto [d_min, d_max] = compute_phase_reset_delta_bounds(ok);
    if (!ok || delta < d_min || delta > d_max) {
        std::fprintf(stderr,
            "warptempo_gui: phase_reset jump rejected: would violate "
            "marker ordering\n");
        return;
    }
    std::vector<GuiPhaseResetMarker> pre_state = app.phase_reset_markers.markers();
    const int                 hint_last = app.last_selected_marker;
    for (int idx : app.selected_markers) {
        GuiPhaseResetMarker* m = app.phase_reset_markers.marker_mut(idx);
        if (!m) continue;
        apply_phase_reset_position_delta(*m, delta);
    }
    undo.push_undo_phase_reset(std::move(pre_state), OpKind::Move, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

