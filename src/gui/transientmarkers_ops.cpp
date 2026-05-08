#include "transientmarkers_ops.h"

#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <utility>
#include <vector>

// X.7.4: transient-authoring cluster. Method bodies are byte-identical to
// the lambdas they replaced in main.cpp, with these mechanical rewrites:
//
//   push_undo*                  → undo.push_undo*
//   recompute_dirty             → undo.recompute_dirty
//   sync_playhead_to_last_selected → selection.sync_playhead_to_last_selected
//   invalidate_waveform_area    → viewport.invalidate_waveform_area
//   invalidate_timestamp_area   → viewport.invalidate_timestamp_area
//   invalidate_all              → viewport.invalidate_all
//   move_playhead_to            → viewport.move_playhead_to
//   apply_transient_position_delta → free function (defined in main.cpp)
//   current_samples_per_pixel   → free function
//   stop_playback_if_playing,
//   clear_hover_popup,
//   find_flag                   → std::function refs (called as f())

// Drop a transient marker at `time_seconds`. Equal-frame collisions
// are accepted (mid-edit nudges may transit through them); save()
// dedups. Selection collapses to the freshly-inserted index. If the
// transient list does not yet carry a frame-0 entry after insertion,
// a silent companion at frame 0 is inserted alongside (frame-0
// invariant: phase reset at render start is always correct).
void GuiTransientMarkersOps::drop_transient_at_position(double time_seconds) {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    // No duplicate-position guard here is intentional — see drop_marker for the warp side's contrasting behavior.
    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;
    GuiTransientMarker nm;
    nm.time_seconds = time_seconds;
    int new_idx = app.transientmarkers.insert_marker(std::move(nm));
    // Time-0 companion. If the post-insert list's head isn't at
    // time 0.0, insert one. The companion always lands at index 0,
    // so the user's marker shifts up by one.
    if (app.transientmarkers.markers().front().time_seconds != 0.0) {
        GuiTransientMarker zero;
        zero.time_seconds = 0.0;
        app.transientmarkers.insert_marker(std::move(zero));
        new_idx += 1;
    }
    app.selected_markers.clear();
    app.selected_markers.insert(new_idx);
    app.last_selected_marker = new_idx;
    undo.push_undo_transient(std::move(pre_state), OpKind::Create, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
    // Match drop_marker: move playhead to the new transient. When
    // dropping at the current playhead, this is a no-op.
    viewport.move_playhead_to(static_cast<int64_t>(std::llround(
        time_seconds * static_cast<double>(sr))));
}

void GuiTransientMarkersOps::drop_transient_at_playhead() {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double t = static_cast<double>(app.playhead_sample) /
                     static_cast<double>(sr);
    drop_transient_at_position(t);
}

// Delete every selected transient. No label/cascade rules — transients
// don't have labels. Mirrors warp's time-0 protection: the frame-0
// entry is the transient list's anchor (phase-reset invariant) and
// cannot be removed.
void GuiTransientMarkersOps::delete_selected_transient() {
    if (app.selected_markers.empty()) return;
    const auto& tv = app.transientmarkers.markers();
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(tv.size())) {
            std::fprintf(stderr,
                "warptempo_gui: transient delete rejected: stale index\n");
            return;
        }
        if (idx == 0 || tv[idx].time_seconds == 0.0) {
            std::fprintf(stderr,
                "warptempo_gui: cannot delete first transient (time 0)\n");
            return;
        }
    }
    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;
    for (auto it = app.selected_markers.rbegin();
         it != app.selected_markers.rend(); ++it) {
        app.transientmarkers.remove_marker(*it);
    }
    app.selected_markers.clear();
    app.last_selected_marker = -1;
    undo.push_undo_transient(std::move(pre_state), OpKind::Destroy, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Toggle the disabled flag on each selected transient. Unconditional —
// transients have no label-def gating like warp markers do.
void GuiTransientMarkersOps::toggle_transient_disabled() {
    if (app.selected_markers.empty()) return;
    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;
    bool changed = false;
    for (int idx : app.selected_markers) {
        GuiTransientMarker* m = app.transientmarkers.marker_mut(idx);
        if (!m) continue;
        m->disabled = !m->disabled;
        changed = true;
    }
    if (!changed) return;
    undo.push_undo_transient(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Compute (delta_min, delta_max) seconds bounds for shifting the
// currently-selected transients by a uniform delta. Same shape as the
// warp version: nearest non-selected neighbor on each side, intersected,
// with a 3-pixel-at-current-zoom visual gap enforced via eps. No trim
// clamp — transients aren't bounded by trim flags during edit.
std::pair<double, double> GuiTransientMarkersOps::compute_transient_delta_bounds(bool& ok) {
    ok = false;
    const auto& tv = app.transientmarkers.markers();
    if (app.selected_markers.empty()) return {0.0, 0.0};
    const int sr = audio.sample_rate();
    if (sr <= 0) return {0.0, 0.0};
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(tv.size())) return {0.0, 0.0};
        if (idx == 0 || tv[idx].time_seconds == 0.0) return {0.0, 0.0};
    }
    const double sr_d = static_cast<double>(sr);
    const double spp  = current_samples_per_pixel(app, audio);
    const double eps  = 3.0 * spp / sr_d;  // 3 pixels at current zoom

    double d_min = -std::numeric_limits<double>::infinity();
    double d_max =  std::numeric_limits<double>::infinity();
    for (int idx : app.selected_markers) {
        const double orig_t = tv[idx].time_seconds;
        int prev = idx - 1;
        while (prev >= 0 && app.selected_markers.count(prev)) --prev;
        if (prev >= 0) {
            const double lb = (tv[prev].time_seconds + eps) - orig_t;
            if (lb > d_min) d_min = lb;
        }
        int next = idx + 1;
        while (next < static_cast<int>(tv.size()) &&
               app.selected_markers.count(next)) ++next;
        if (next < static_cast<int>(tv.size())) {
            const double ub = (tv[next].time_seconds - eps) - orig_t;
            if (ub < d_max) d_max = ub;
        }
    }
    ok = true;
    return {d_min, d_max};
}

// Nudge selected transients by +/- 1 source-pixel of seconds. Direction:
// -1 for earlier, +1 for later. Symmetric with nudge_selected_markers.
void GuiTransientMarkersOps::nudge_selected_transients(int direction) {
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
    auto [d_min, d_max] = compute_transient_delta_bounds(ok);
    if (!ok) return;
    double delta = delta_s;
    if (delta < d_min) delta = d_min;
    if (delta > d_max) delta = d_max;
    if (delta == 0.0) return;

    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;
    for (int idx : app.selected_markers) {
        GuiTransientMarker* m = app.transientmarkers.marker_mut(idx);
        if (!m) continue;
        apply_transient_position_delta(*m, delta);
    }
    undo.push_undo_transient(std::move(pre_state), OpKind::Move, hint_last);
    selection.sync_playhead_to_last_selected();
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// `j` for transient mode: shift the selection so last_selected lands
// on the playhead. All-or-nothing clamp check.
void GuiTransientMarkersOps::jump_transient_selection_to_playhead() {
    if (app.selected_markers.empty()) return;
    if (app.last_selected_marker < 0) return;
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const auto& tv = app.transientmarkers.markers();
    if (app.last_selected_marker >= static_cast<int>(tv.size())) return;
    const double anchor_t = tv[app.last_selected_marker].time_seconds;
    const double ph_t     = static_cast<double>(app.playhead_sample) /
                            static_cast<double>(sr);
    const double delta    = ph_t - anchor_t;
    if (delta == 0.0) return;

    bool ok = false;
    auto [d_min, d_max] = compute_transient_delta_bounds(ok);
    if (!ok || delta < d_min || delta > d_max) {
        std::fprintf(stderr,
            "warptempo_gui: transient jump rejected: would violate "
            "marker ordering\n");
        return;
    }
    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;
    for (int idx : app.selected_markers) {
        GuiTransientMarker* m = app.transientmarkers.marker_mut(idx);
        if (!m) continue;
        apply_transient_position_delta(*m, delta);
    }
    undo.push_undo_transient(std::move(pre_state), OpKind::Move, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Toggle b= flag on the single selected transient. Mirrors the warp
// version's toggle / auto-replace / equal-frame refusal and likewise
// resolves cross-file: an existing b= on the warp side is cleared, and
// a swap target on the warp side is honored.
void GuiTransientMarkersOps::toggle_transient_begin_time() {
    if (app.selected_markers.size() != 1) return;
    const int idx = app.last_selected_marker;
    if (idx < 0) return;
    const auto& tv = app.transientmarkers.markers();
    if (idx >= static_cast<int>(tv.size())) return;

    std::vector<GuiWarpMarker>    warp_pre  = app.warpmarkers.markers();
    std::vector<GuiTransientMarker> trans_pre = tv;
    const int                 hint_last = app.last_selected_marker;

    if (tv[idx].is_begin_time) {
        app.transientmarkers.marker_mut(idx)->is_begin_time = false;
        undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'T', OpKind::Other, hint_last);
        undo.recompute_dirty();
        viewport.invalidate_waveform_area();
        viewport.invalidate_timestamp_area();
        return;
    }

    const int sr_b = audio.sample_rate();
    const int64_t m_frame = static_cast<int64_t>(std::llround(
        tv[idx].time_seconds * static_cast<double>(sr_b)));
    const FlagLoc e_loc   = find_flag(/*want_begin=*/false,
                                      /*excl_trans=*/false, -1);
    const FlagLoc b_other = find_flag(/*want_begin=*/true,
                                      /*excl_trans=*/true, idx);

    const bool needs_swap   = e_loc.valid && (m_frame >= e_loc.frame);
    const bool equal_frames = e_loc.valid && (m_frame == e_loc.frame);
    if (equal_frames) {
        std::fprintf(stderr,
            "warptempo_gui: b refused: would collapse trim region\n");
        return;
    }

    if (b_other.valid) {
        if (b_other.transient) {
            app.transientmarkers.marker_mut(b_other.idx)->is_begin_time = false;
        } else {
            app.warpmarkers.marker_mut(b_other.idx)->is_begin_time = false;
        }
    }
    if (needs_swap) {
        if (e_loc.transient) {
            app.transientmarkers.marker_mut(e_loc.idx)->is_end_time   = false;
            app.transientmarkers.marker_mut(e_loc.idx)->is_begin_time = true;
        } else {
            app.warpmarkers.marker_mut(e_loc.idx)->is_end_time   = false;
            app.warpmarkers.marker_mut(e_loc.idx)->is_begin_time = true;
        }
        app.transientmarkers.marker_mut(idx)->is_end_time = true;
    } else {
        app.transientmarkers.marker_mut(idx)->is_begin_time = true;
    }

    undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                   'T', OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

void GuiTransientMarkersOps::toggle_transient_end_time() {
    if (app.selected_markers.size() != 1) return;
    const int idx = app.last_selected_marker;
    if (idx < 0) return;
    const auto& tv = app.transientmarkers.markers();
    if (idx >= static_cast<int>(tv.size())) return;

    std::vector<GuiWarpMarker>    warp_pre  = app.warpmarkers.markers();
    std::vector<GuiTransientMarker> trans_pre = tv;
    const int                 hint_last = app.last_selected_marker;

    if (tv[idx].is_end_time) {
        app.transientmarkers.marker_mut(idx)->is_end_time = false;
        undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'T', OpKind::Other, hint_last);
        undo.recompute_dirty();
        viewport.invalidate_waveform_area();
        viewport.invalidate_timestamp_area();
        return;
    }

    const int sr_e = audio.sample_rate();
    const int64_t m_frame = static_cast<int64_t>(std::llround(
        tv[idx].time_seconds * static_cast<double>(sr_e)));
    const FlagLoc b_loc   = find_flag(/*want_begin=*/true,
                                      /*excl_trans=*/false, -1);
    const FlagLoc e_other = find_flag(/*want_begin=*/false,
                                      /*excl_trans=*/true, idx);

    const bool needs_swap   = b_loc.valid && (m_frame <= b_loc.frame);
    const bool equal_frames = b_loc.valid && (m_frame == b_loc.frame);
    if (equal_frames) {
        std::fprintf(stderr,
            "warptempo_gui: e refused: would collapse trim region\n");
        return;
    }

    if (e_other.valid) {
        if (e_other.transient) {
            app.transientmarkers.marker_mut(e_other.idx)->is_end_time = false;
        } else {
            app.warpmarkers.marker_mut(e_other.idx)->is_end_time = false;
        }
    }
    if (needs_swap) {
        if (b_loc.transient) {
            app.transientmarkers.marker_mut(b_loc.idx)->is_begin_time = false;
            app.transientmarkers.marker_mut(b_loc.idx)->is_end_time   = true;
        } else {
            app.warpmarkers.marker_mut(b_loc.idx)->is_begin_time = false;
            app.warpmarkers.marker_mut(b_loc.idx)->is_end_time   = true;
        }
        app.transientmarkers.marker_mut(idx)->is_begin_time = true;
    } else {
        app.transientmarkers.marker_mut(idx)->is_end_time = true;
    }

    undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                   'T', OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

