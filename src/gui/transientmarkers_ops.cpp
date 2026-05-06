#include "transientmarkers_ops.h"

#include "audio.h"
#include "render_pipeline.h"

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
//   do_detection                → engine free function
//   stop_playback_if_playing,
//   clear_hover_popup,
//   find_flag,
//   open_prompt_detect_confirm  → std::function refs (called as f())

// Drop a transient marker at `time_seconds`. Equal-frame collisions
// are accepted (mid-edit nudges may transit through them); save()
// dedups. Selection collapses to the freshly-inserted index. If the
// transient list does not yet carry a frame-0 entry after insertion,
// a silent companion at frame 0 is inserted alongside (frame-0
// invariant: phase reset at render start is always correct).
void GuiTransientMarkersOps::drop_transient_at_position(double time_seconds) {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const int64_t frame = static_cast<int64_t>(std::llround(
        time_seconds * static_cast<double>(sr)));
    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;
    GuiTransientMarker nm;
    nm.src_frame   = frame;
    nm.is_inserted = true;
    int new_idx = app.transientmarkers.insert_marker(std::move(nm));
    // Frame-0 companion. If the post-insert list's head isn't at
    // frame 0, insert one. The companion always lands at index 0,
    // so the user's marker shifts up by one.
    if (app.transientmarkers.markers().front().effective_frame() != 0) {
        GuiTransientMarker zero;
        zero.src_frame   = 0;
        zero.is_inserted = true;
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
    viewport.move_playhead_to(frame);
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
        // Detected entries can't be deleted; only disabled or merged
        // away by re-running detection.
        if (!tv[idx].is_inserted) return;
        if (idx == 0 || tv[idx].effective_frame() == 0) {
            std::fprintf(stderr,
                "warptempo_gui: cannot delete first transient (frame 0)\n");
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

// Compute (delta_min, delta_max) sample bounds for shifting the
// currently-selected transients by a uniform delta. Same shape as the
// warp version: nearest non-selected neighbor on each side, intersected.
// Operates on effective_frame (the visible position) — for a
// D-with-displacement entry, that's displaced_frame.
// No trim clamp — transients aren't bounded by trim flags during edit.
std::pair<int64_t, int64_t> GuiTransientMarkersOps::compute_transient_delta_bounds(bool& ok) {
    ok = false;
    const auto& tv = app.transientmarkers.markers();
    if (app.selected_markers.empty()) return {0, 0};
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(tv.size())) return {0, 0};
        if (idx == 0 || tv[idx].effective_frame() == 0) return {0, 0};
    }
    int64_t d_min = std::numeric_limits<int64_t>::min();
    int64_t d_max = std::numeric_limits<int64_t>::max();
    for (int idx : app.selected_markers) {
        const int64_t orig = tv[idx].effective_frame();
        int prev = idx - 1;
        while (prev >= 0 && app.selected_markers.count(prev)) --prev;
        if (prev >= 0) {
            const int64_t lb = (tv[prev].effective_frame() + 1) - orig;
            if (lb > d_min) d_min = lb;
        }
        int next = idx + 1;
        while (next < static_cast<int>(tv.size()) &&
               app.selected_markers.count(next)) ++next;
        if (next < static_cast<int>(tv.size())) {
            const int64_t ub = (tv[next].effective_frame() - 1) - orig;
            if (ub < d_max) d_max = ub;
        }
    }
    ok = true;
    return {d_min, d_max};
}

// Nudge selected transients by +/- 1 source-pixel. Direction: -1 for
// earlier, +1 for later. Symmetric with nudge_selected_markers.
void GuiTransientMarkersOps::nudge_selected_transients(int direction) {
    if (app.loading || audio.total_frames() <= 0) return;
    stop_playback_if_playing();
    if (app.selected_markers.empty()) return;
    const double spp = current_samples_per_pixel(app, audio);
    const int64_t step = std::max<int64_t>(1,
        static_cast<int64_t>(std::llround(spp))) *
        static_cast<int64_t>(direction);
    if (step == 0) return;

    bool ok = false;
    auto [d_min, d_max] = compute_transient_delta_bounds(ok);
    if (!ok) return;
    int64_t delta = step;
    if (delta < d_min) delta = d_min;
    if (delta > d_max) delta = d_max;
    if (delta == 0) return;

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
    const auto& tv = app.transientmarkers.markers();
    if (app.last_selected_marker >= static_cast<int>(tv.size())) return;
    const int64_t anchor_f = tv[app.last_selected_marker].effective_frame();
    const int64_t delta    = app.playhead_sample - anchor_f;
    if (delta == 0) return;

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

    const int64_t m_frame = tv[idx].effective_frame();
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

    const int64_t m_frame = tv[idx].effective_frame();
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

// Two-pass merge: existing D entries indexed by their immutable
// src_frame anchor are matched against fresh detections so user edits
// (disabled, displaced position, b=/e= flags) survive re-detection.
// Existing I (manually inserted) entries always carry over; D entries
// whose src_frame the new detector no longer places are dropped. The
// merged list is sorted by effective_frame() and the frame-0 invariant
// is restored. Does NOT push undo — detection is destructive by spec.
void GuiTransientMarkersOps::merge_detection(const std::vector<int64_t>& fresh_src_frames) {
    std::map<int64_t, GuiTransientMarker> old_d_by_src;
    std::vector<GuiTransientMarker> old_i;
    for (const auto& m : app.transientmarkers.markers()) {
        if (m.is_inserted) old_i.push_back(m);
        else               old_d_by_src.emplace(m.src_frame, m);
    }

    std::vector<GuiTransientMarker> merged;
    merged.reserve(fresh_src_frames.size() + old_i.size() + 1);
    for (int64_t f : fresh_src_frames) {
        auto it = old_d_by_src.find(f);
        if (it != old_d_by_src.end()) {
            merged.push_back(it->second);
        } else {
            GuiTransientMarker m;
            m.src_frame   = f;
            m.is_inserted = false;
            merged.push_back(m);
        }
    }
    for (auto& m : old_i) merged.push_back(std::move(m));

    std::sort(merged.begin(), merged.end(),
        [](const GuiTransientMarker& a, const GuiTransientMarker& b) {
            return a.effective_frame() < b.effective_frame();
        });

    if (merged.empty() || merged.front().effective_frame() > 0) {
        GuiTransientMarker zero;
        zero.src_frame   = 0;
        zero.is_inserted = true;
        merged.insert(merged.begin(), zero);
    }

    app.transientmarkers.markers_mut() = std::move(merged);
}

// Run the engine's detection-only pass against the loaded source +
// current marker set, then merge the results into app.transientmarkers.
// After a successful merge, write the .transientmarkers sibling file
// immediately — detection is not on the undo stack so the on-disk
// file is the authoritative record. The transient_dirty bit is reset
// explicitly: the merge mutated app.transientmarkers but no UndoEntry was
// pushed, so the post-merge state is the one we want to call "saved".
void GuiTransientMarkersOps::run_detect_now() {
    if (app.source_audio_path.empty())   return;
    if (audio.total_frames() <= 0)       return;

    // Clear any in-flight transient selection — indices into the
    // pre-merge list are not meaningful afterwards.
    if (app.active_mode == 'T') app.last_selected_marker = -1;

    DetectionRequest dr;
    dr.source_audio_path    = app.source_audio_path;
    dr.markers              = app.warpmarkers.markers();
    dr.settings_passthrough = app.settings_passthrough;

    std::vector<int64_t> fresh;
    if (!do_detection(dr, fresh)) {
        std::fprintf(stderr,
            "warptempo_gui: detection failed; transients unchanged\n");
        return;
    }
    std::sort(fresh.begin(), fresh.end());
    fresh.erase(std::unique(fresh.begin(), fresh.end()), fresh.end());

    merge_detection(fresh);

    // Write the sibling file. Empty list (only the auto frame-0 head)
    // would be unusual after detection, but the save path already
    // handles the empty case.
    if (!app.transientmarkers_path.empty()) {
        if (app.transientmarkers.markers().empty()) {
            app.transientmarkers.delete_file(app.transientmarkers_path);
        } else if (!app.transientmarkers.save(app.transientmarkers_path)) {
            std::fprintf(stderr,
                "warptempo_gui: transient save failed: %s\n",
                app.transientmarkers_path.c_str());
        }
    }

    // Detection is not undoable. Reset the transient-side dirty bit
    // so the dialog doesn't gate a subsequent close/revert because of
    // the merge. Warp dirty is unaffected.
    app.transient_dirty = false;
    undo.recompute_dirty();
    viewport.invalidate_all();
    std::fprintf(stderr,
        "warptempo_gui: detection produced %zu transients\n",
        app.transientmarkers.markers().size());
}

// Ctrl+Alt+T entry point. Confirms before clobbering an existing
// detection (any D entry in the list). With no prior detection (only
// I entries or the auto frame-0 head), runs immediately.
void GuiTransientMarkersOps::detect_transients() {
    if (app.prompt.active)             return;
    if (app.source_audio_path.empty()) return;
    if (audio.total_frames() <= 0)     return;

    bool has_prior_detection = false;
    for (const auto& m : app.transientmarkers.markers()) {
        if (!m.is_inserted) { has_prior_detection = true; break; }
    }
    if (has_prior_detection) {
        open_prompt_detect_confirm();
        return;
    }
    run_detect_now();
}

// Ctrl+Shift+Alt+T: drop all transients (both I and D), undoable. The
// frame-0 invariant means a non-empty list always carries an entry at
// 0; clearing wholesale removes that too — load() will re-materialize
// it on the next read of an empty file (which is itself the empty
// list, since save() removes the file when empty). The undo restores
// the full pre-clear state.
void GuiTransientMarkersOps::clear_all_transients() {
    if (app.transientmarkers.markers().empty()) return;

    std::vector<GuiTransientMarker> pre_state = app.transientmarkers.markers();
    const int pre_last = app.last_selected_marker;

    app.transientmarkers.clear();
    if (app.active_mode == 'T') app.last_selected_marker = -1;

    undo.push_undo_transient(std::move(pre_state), OpKind::Destroy, pre_last);
    undo.recompute_dirty();
    viewport.invalidate_all();
}
