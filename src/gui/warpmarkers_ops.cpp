#include "warpmarkers_ops.h"

#include "audio.h"
#include "render.h"
#include "transientmarkers_ops.h"
#include "x11.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

// X.7.5a: warp-authoring cluster. Method bodies are byte-identical to
// the lambdas they replaced in main.cpp, with these mechanical rewrites:
//
//   push_undo, push_undo_transient,
//   push_undo_both                 → undo.push_undo*
//   recompute_dirty                → undo.recompute_dirty
//   sync_playhead_to_last_selected → selection.sync_playhead_to_last_selected
//   invalidate_waveform_area       → viewport.invalidate_waveform_area
//   invalidate_timestamp_area      → viewport.invalidate_timestamp_area
//   invalidate_marker_column       → viewport.invalidate_marker_column
//   invalidate_markers_columns     → viewport.invalidate_markers_columns
//   invalidate_top_strip           → viewport.invalidate_top_strip
//   move_playhead_to               → viewport.move_playhead_to
//   stop_playback_if_playing,
//   clear_hover_popup,
//   find_flag                      → std::function refs (called as f())
//   apply_transient_position_delta → free function (declared in
//                                    transientmarkers_ops.h)
//   resolve_inherited_tempo,
//   resolve_inherited_tempo_scale,
//   current_samples_per_pixel,
//   waveform_area, union_rect,
//   playhead_invalidate_rect       → free functions, no qualifier change

void GuiWarpMarkersOps::drop_marker(double time_seconds, bool inherit) {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double one_sample = 1.0 / static_cast<double>(sr);
    const auto& mv = app.warpmarkers.markers();
    for (const auto& m : mv) {
        if (std::abs(m.time_seconds - time_seconds) < one_sample) {
            std::fprintf(stderr,
                "warptempo_gui: marker already exists at %.3fs\n",
                time_seconds);
            return;
        }
    }
    // Snapshot pre-mutation state for undo. Captured after the dup
    // check so rejected drops don't leave a no-op entry on the stack.
    std::vector<GuiWarpMarker> pre_state = mv;
    const int              hint_last = app.last_selected_marker;
    GuiWarpMarker nm;
    nm.time_seconds    = time_seconds;
    nm.tempo_inherits  = inherit;
    // pass markers carry inert defaults; their effective tempo is
    // resolved live from the marker list at every read site.
    if (inherit) {
        nm.tempo_base  = 1.0;
        nm.tempo_scale = "1.0000";
    } else {
        nm.tempo_base = 1.0;
        nm.tempo_scale.clear();
    }
    const int new_idx = app.warpmarkers.insert_marker(std::move(nm));
    // Insertion may have shifted indices of existing selections.
    // Rebuild selected_markers to reflect the shift.
    std::set<int> shifted;
    for (int i : app.selected_markers) {
        shifted.insert(i >= new_idx ? i + 1 : i);
    }
    app.selected_markers = std::move(shifted);
    if (app.last_selected_marker >= new_idx) app.last_selected_marker += 1;
    // Newly-dropped marker becomes the sole selection per chunk I.
    app.selected_markers.clear();
    app.selected_markers.insert(new_idx);
    app.last_selected_marker = new_idx;
    undo.push_undo(std::move(pre_state), OpKind::Create, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();

    // Move the playhead to the new marker for consistency with click-
    // to-select behavior. Done last so invalidations in the helper
    // don't double-paint with the ones above.
    const int64_t sample = static_cast<int64_t>(std::llround(
        time_seconds * static_cast<double>(sr)));
    viewport.move_playhead_to(sample);
}

void GuiWarpMarkersOps::drop_marker_at_playhead() {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double t = static_cast<double>(app.playhead_sample) /
                     static_cast<double>(sr);
    drop_marker(t, /*inherit=*/false);
}

void GuiWarpMarkersOps::drop_inherit_marker_at_playhead() {
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double t = static_cast<double>(app.playhead_sample) /
                     static_cast<double>(sr);
    drop_marker(t, /*inherit=*/true);
}

void GuiWarpMarkersOps::delete_selected_marker() {
    if (app.selected_markers.empty()) return;
    const auto& mv = app.warpmarkers.markers();

    // Validate the batch. Reject the whole operation if any member is
    // the time-0 first marker or has a label_def referenced from outside
    // the selection set.
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(mv.size())) {
            std::fprintf(stderr,
                "warptempo_gui: delete rejected: stale selection index\n");
            return;
        }
        if (mv[idx].time_seconds == 0.0) {
            std::fprintf(stderr,
                "warptempo_gui: cannot delete first marker (time 0)\n");
            return;
        }
        if (mv[idx].label_def.empty()) continue;
        std::string refs;
        int ref_count = 0;
        for (size_t i = 0; i < mv.size(); ++i) {
            if (app.selected_markers.count(static_cast<int>(i))) continue;
            if (!mv[i].label_ref.empty() &&
                mv[i].label_ref == mv[idx].label_def) {
                char tbuf[32];
                std::snprintf(tbuf, sizeof(tbuf), "%.3fs",
                              mv[i].time_seconds);
                if (!refs.empty()) refs += ", ";
                refs += tbuf;
                ++ref_count;
            }
        }
        if (ref_count > 0) {
            std::fprintf(stderr,
                "warptempo_gui: cannot delete marker: label '%s' is "
                "referenced at %s\n",
                mv[idx].label_def.c_str(), refs.c_str());
            return;
        }
    }

    // All validations passed — capture snapshot and selection hint
    // before mutating so the undo can restore the pre-delete selection.
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    // Delete in descending order so earlier indices stay valid.
    for (auto it = app.selected_markers.rbegin();
         it != app.selected_markers.rend(); ++it) {
        app.warpmarkers.remove_marker(*it);
    }
    app.selected_markers.clear();
    app.last_selected_marker = -1;
    undo.push_undo(std::move(pre_state), OpKind::Destroy, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Shift+Delete variant. Auto-cascades label_refs of any selected def
// into the deletion batch, so the user doesn't have to hand-pick each
// ref before deleting the def. With the cascade, the "label is
// referenced from outside the selection" check is unnecessary — every
// ref is now inside the batch by construction.
void GuiWarpMarkersOps::force_delete_selected_marker() {
    if (app.selected_markers.empty()) return;
    const auto& mv = app.warpmarkers.markers();

    std::set<int> expanded = app.selected_markers;
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(mv.size())) {
            std::fprintf(stderr,
                "warptempo_gui: delete rejected: stale selection index\n");
            return;
        }
        if (mv[idx].label_def.empty()) continue;
        for (size_t i = 0; i < mv.size(); ++i) {
            if (!mv[i].label_ref.empty() &&
                mv[i].label_ref == mv[idx].label_def) {
                expanded.insert(static_cast<int>(i));
            }
        }
    }

    for (int idx : expanded) {
        if (idx < 0 || idx >= static_cast<int>(mv.size())) {
            std::fprintf(stderr,
                "warptempo_gui: delete rejected: stale selection index\n");
            return;
        }
        if (mv[idx].time_seconds == 0.0) {
            std::fprintf(stderr,
                "warptempo_gui: cannot delete first marker (time 0)\n");
            return;
        }
    }

    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    for (auto it = expanded.rbegin(); it != expanded.rend(); ++it) {
        app.warpmarkers.remove_marker(*it);
    }
    app.selected_markers.clear();
    app.last_selected_marker = -1;
    undo.push_undo(std::move(pre_state), OpKind::Destroy, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Shift+P: convert each selected marker's tempo source. Cache-free —
// the only stored state on a pass marker is `tempo_inherits = true`
// plus inert defaults. Three input cases per marker:
//   - owning   → pass: inert defaults; label_def preserved.
//   - pass     → owning: freeze the resolved tempo/scale at this moment;
//                label_def preserved.
//   - label_ref → pass: clear the ref; inert defaults.
// The first marker is silently skipped (it must own its tempo).
void GuiWarpMarkersOps::toggle_inherits() {
    if (app.selected_markers.empty()) return;
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    const auto& mv_const = app.warpmarkers.markers();
    bool changed = false;
    for (int idx : app.selected_markers) {
        GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
        if (!m) continue;
        if (idx == 0) continue;
        if (!m->label_ref.empty()) {
            m->label_ref.clear();
            m->tempo_inherits = true;
            m->tempo_base     = 1.0;
            m->tempo_scale    = "1.0000";
        } else if (m->tempo_inherits) {
            const double resolved_tempo =
                resolve_inherited_tempo(mv_const, idx);
            const std::string resolved_scale =
                resolve_inherited_tempo_scale(mv_const, idx);
            m->tempo_inherits = false;
            m->tempo_base     = resolved_tempo;
            m->tempo_scale    = resolved_scale;
        } else {
            m->tempo_inherits = true;
            m->tempo_base     = 1.0;
            m->tempo_scale    = "1.0000";
        }
        changed = true;
    }
    if (!changed) return;
    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Toggle the disabled flag on each selected marker. Per chunk U patch 3
// the flag is allowed on any marker (cascade still applies only when the
// toggled marker is a label_def).
void GuiWarpMarkersOps::toggle_disabled() {
    if (app.selected_markers.empty()) return;
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    bool changed = false;
    for (int idx : app.selected_markers) {
        GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
        if (!m) continue;
        m->disabled = !m->disabled;
        changed = true;
    }
    if (!changed) return;
    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// `b` / `e` are single-marker operations. With 2+ selected they silent
// no-op; with exactly 1, they use last_selected_marker as the target.
// Re-press toggles off. Otherwise auto-replaces any existing flag
// (across BOTH warp and transient lists) and auto-swaps with the
// opposite flag if the resulting frame ordering would invert the trim
// region. Equal-frame swap is refused (would collapse trim to zero
// width).
void GuiWarpMarkersOps::toggle_begin_time() {
    if (app.selected_markers.size() != 1) return;
    const int idx = app.last_selected_marker;
    if (idx < 0) return;
    const auto& mv = app.warpmarkers.markers();
    if (idx >= static_cast<int>(mv.size())) return;

    const int sr = audio.sample_rate();
    const int64_t m_frame = static_cast<int64_t>(std::llround(
        mv[idx].time_seconds * static_cast<double>(sr)));

    std::vector<GuiWarpMarker>    warp_pre  = mv;
    std::vector<GuiTransientMarker> trans_pre = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;

    if (mv[idx].is_begin_time) {
        app.warpmarkers.marker_mut(idx)->is_begin_time = false;
        undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'W', OpKind::Other, hint_last);
        undo.recompute_dirty();
        viewport.invalidate_waveform_area();
        viewport.invalidate_timestamp_area();
        return;
    }

    // Find the existing e= and OTHER b= across both lists.
    const FlagLoc e_loc   = find_flag(/*want_begin=*/false,
                                      /*excl_trans=*/false, -1);
    const FlagLoc b_other = find_flag(/*want_begin=*/true,
                                      /*excl_trans=*/false, idx);

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
        // The marker that had e= becomes b=; the just-toggled warp
        // marker takes e=.
        if (e_loc.transient) {
            app.transientmarkers.marker_mut(e_loc.idx)->is_end_time   = false;
            app.transientmarkers.marker_mut(e_loc.idx)->is_begin_time = true;
        } else {
            app.warpmarkers.marker_mut(e_loc.idx)->is_end_time   = false;
            app.warpmarkers.marker_mut(e_loc.idx)->is_begin_time = true;
        }
        app.warpmarkers.marker_mut(idx)->is_end_time = true;
    } else {
        app.warpmarkers.marker_mut(idx)->is_begin_time = true;
    }

    undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                   'W', OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

void GuiWarpMarkersOps::toggle_end_time() {
    if (app.selected_markers.size() != 1) return;
    const int idx = app.last_selected_marker;
    if (idx < 0) return;
    const auto& mv = app.warpmarkers.markers();
    if (idx >= static_cast<int>(mv.size())) return;

    const int sr = audio.sample_rate();
    const int64_t m_frame = static_cast<int64_t>(std::llround(
        mv[idx].time_seconds * static_cast<double>(sr)));

    std::vector<GuiWarpMarker>    warp_pre  = mv;
    std::vector<GuiTransientMarker> trans_pre = app.transientmarkers.markers();
    const int                 hint_last = app.last_selected_marker;

    if (mv[idx].is_end_time) {
        app.warpmarkers.marker_mut(idx)->is_end_time = false;
        undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'W', OpKind::Other, hint_last);
        undo.recompute_dirty();
        viewport.invalidate_waveform_area();
        viewport.invalidate_timestamp_area();
        return;
    }

    const FlagLoc b_loc   = find_flag(/*want_begin=*/true,
                                      /*excl_trans=*/false, -1);
    const FlagLoc e_other = find_flag(/*want_begin=*/false,
                                      /*excl_trans=*/false, idx);

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
        app.warpmarkers.marker_mut(idx)->is_begin_time = true;
    } else {
        app.warpmarkers.marker_mut(idx)->is_end_time = true;
    }

    undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                   'W', OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Nudge every selected marker by `delta`. Label refs are silently
// skipped (no tempo to nudge — convert via Shift+P first). Pass markers
// resolve walk-backward to get their starting tempo/scale, then freeze
// to owning at the nudged value. Owning markers nudge in place.
// Clamps to [0.01, 9.99]. Only dirties / invalidates on real change.
void GuiWarpMarkersOps::adjust_tempo(double delta) {
    if (app.selected_markers.empty()) return;
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    const auto& mv_const = app.warpmarkers.markers();
    bool changed = false;
    for (int idx : app.selected_markers) {
        GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
        if (!m) continue;
        if (!m->label_ref.empty()) continue;
        double      start_tempo;
        std::string start_scale;
        if (m->tempo_inherits) {
            start_tempo = resolve_inherited_tempo(mv_const, idx);
            start_scale = resolve_inherited_tempo_scale(mv_const, idx);
        } else {
            start_tempo = m->tempo_base;
            start_scale = m->tempo_scale;
        }
        double new_tempo = start_tempo + delta;
        if (new_tempo < 0.01) new_tempo = 0.01;
        if (new_tempo > 9.99) new_tempo = 9.99;
        if (!m->tempo_inherits && new_tempo == m->tempo_base) continue;
        m->tempo_inherits = false;
        m->tempo_base     = new_tempo;
        m->tempo_scale    = start_scale;
        viewport.invalidate_marker_column(idx);
        changed = true;
    }
    if (!changed) return;
    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_top_strip();
    viewport.invalidate_timestamp_area();
}

// Clear any b= / e= flags so the whole file becomes editable again.
// No-op if no marker carries either flag.
void GuiWarpMarkersOps::clear_trim() {
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    bool changed = false;
    for (auto& m : app.warpmarkers.markers_mut()) {
        if (m.is_begin_time || m.is_end_time) {
            m.is_begin_time = false;
            m.is_end_time   = false;
            changed = true;
        }
    }
    if (!changed) return;
    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

bool GuiWarpMarkersOps::begin_drag(int hit, int mouse_x) {
    if (hit < 0) return false;
    const int sr = audio.sample_rate();
    if (sr <= 0) return false;
    const bool transient = (app.active_mode == 'T');
    const int n = transient
        ? static_cast<int>(app.transientmarkers.markers().size())
        : static_cast<int>(app.warpmarkers.markers().size());
    if (hit >= n) return false;

    const double sr_d = static_cast<double>(sr);
    auto t_of = [&](int idx) -> double {
        if (transient) {
            return static_cast<double>(
                app.transientmarkers.markers()[idx].effective_frame()) / sr_d;
        }
        return app.warpmarkers.markers()[idx].time_seconds;
    };

    // Drag target: entire selection if hit is in it, else just the hit.
    // In the single-drag case the selection collapse to {hit} is
    // deferred until motion is observed (see pending_collapse_to_hit).
    std::set<int> drag_set;
    bool pending_collapse = false;
    if (app.selected_markers.count(hit)) {
        drag_set = app.selected_markers;
    } else {
        drag_set.insert(hit);
        pending_collapse = true;
    }

    // First-marker protection: refuse index 0 and any effective-time-0
    // marker. Runs before any selection mutation so a refused drag
    // leaves selection genuinely unchanged.
    for (int idx : drag_set) {
        if (idx == 0 || t_of(idx) == 0.0) {
            std::fprintf(stderr,
                "warptempo_gui: first marker cannot be dragged\n");
            return false;
        }
    }

    DragState d;
    d.active = true;
    d.drag_mode = transient ? 'T' : 'W';
    d.dragging_markers.assign(drag_set.begin(), drag_set.end());
    d.original_times.reserve(d.dragging_markers.size());
    for (int idx : d.dragging_markers) {
        d.original_times.push_back(t_of(idx));
    }

    // Anchor mouse time — computed at mouse_x in the waveform's X axis.
    const GuiRect area = waveform_area(app);
    const double spp = current_samples_per_pixel(app, audio);
    const double vp_time = static_cast<double>(app.viewport_start_sample) / sr_d;
    d.anchor_mouse_time_seconds =
        vp_time + static_cast<double>(mouse_x - area.x) * spp / sr_d;

    // Compute scalar delta_min / delta_max from per-marker neighbor
    // bounds. Correct for both contiguous and non-contiguous drag sets.
    // eps enforces a 3-pixel visual gap at the current zoom — markers
    // never stack even at the tightest clamp.
    const double eps = 3.0 * spp / sr_d;

    d.delta_min = -std::numeric_limits<double>::infinity();
    d.delta_max =  std::numeric_limits<double>::infinity();

    for (size_t k = 0; k < d.dragging_markers.size(); ++k) {
        const int idx = d.dragging_markers[k];
        const double orig_t = d.original_times[k];

        // Nearest non-dragged neighbor to the left.
        int prev = idx - 1;
        while (prev >= 0 && drag_set.count(prev)) --prev;
        if (prev >= 0) {
            const double lb = (t_of(prev) + eps) - orig_t;
            if (lb > d.delta_min) d.delta_min = lb;
        }

        // Nearest non-dragged neighbor to the right.
        int next = idx + 1;
        while (next < n && drag_set.count(next)) ++next;
        if (next < n) {
            const double ub = (t_of(next) - eps) - orig_t;
            if (ub < d.delta_max) d.delta_max = ub;
        }
    }

    d.moved = false;
    // Capture the pre-drag list state for undo. Commit pushes the
    // active-mode snapshot if motion landed; otherwise it's discarded.
    if (transient) {
        d.pre_drag_transient_snapshot = app.transientmarkers.markers();
    } else {
        d.pre_drag_snapshot = app.warpmarkers.markers();
    }
    d.pre_drag_last_selected = app.last_selected_marker;
    d.hit_marker             = hit;
    d.pending_collapse_to_hit = pending_collapse;
    app.drag = std::move(d);
    clear_hover_popup();
    return true;
}

// Apply a raw delta (mouse-derived) to the dragging markers, clamped.
// Updates marker times in place and invalidates only the old-and-new
// pixel columns of each moved marker plus the flag strip. The waveform
// cache stays valid throughout the drag — viewport / trim / dimensions
// don't change — so these narrow invalidations blit the cache over
// tiny rects and repaint just markers + flags + playhead on top.
void GuiWarpMarkersOps::apply_drag_motion(double raw_delta) {
    if (!app.drag.active) return;
    double delta = raw_delta;
    if (delta < app.drag.delta_min) delta = app.drag.delta_min;
    if (delta > app.drag.delta_max) delta = app.drag.delta_max;

    const GuiRect area = waveform_area(app);
    const int sr       = audio.sample_rate();
    const double spp   = current_samples_per_pixel(app, audio);
    const double sr_d  = static_cast<double>(sr);
    const bool geom_ok = (sr > 0 && spp > 0.0 && area.w > 0);
    const double vp    = static_cast<double>(app.viewport_start_sample);

    auto col_rect_for_time = [&](double t_seconds) -> GuiRect {
        const double ms = t_seconds * sr_d;
        const double px = area.x + (ms - vp) / spp;
        return playhead_invalidate_rect(area, px);
    };

    const bool transient = (app.drag.drag_mode == 'T');
    bool any_changed = false;
    for (size_t k = 0; k < app.drag.dragging_markers.size(); ++k) {
        const int idx = app.drag.dragging_markers[k];
        const double new_t = app.drag.original_times[k] + delta;
        double old_t;
        if (transient) {
            GuiTransientMarker* m = app.transientmarkers.marker_mut(idx);
            if (!m) continue;
            old_t = static_cast<double>(m->effective_frame()) / sr_d;
            const int64_t new_frame = static_cast<int64_t>(
                std::llround(new_t * sr_d));
            const int64_t cur_frame = m->effective_frame();
            if (cur_frame == new_frame) continue;
            apply_transient_position_delta(*m, new_frame - cur_frame);
        } else {
            GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
            if (!m) continue;
            old_t = m->time_seconds;
            if (old_t == new_t) continue;
            m->time_seconds = new_t;
        }
        any_changed = true;
        if (!geom_ok) continue;
        const GuiRect r_old = col_rect_for_time(old_t);
        const GuiRect r_new = col_rect_for_time(new_t);
        const GuiRect u = union_rect(r_old, r_new);
        if (u.w > 0 && u.h > 0) {
            gui.invalidate_region(u.x, u.y, u.w, u.h);
        }
    }
    if (any_changed) {
        const bool first_motion = !app.drag.moved;
        app.drag.moved = true;
        // Apply the deferred selection collapse on the press-to-motion
        // edge, only if begin_drag noted that the hit marker was not
        // in the prior selection. After this point the drag is
        // committed to being a real drag, so the visible selection
        // should match what's being dragged.
        if (first_motion && app.drag.pending_collapse_to_hit) {
            const int hit = app.drag.hit_marker;
            const std::set<int> old = app.selected_markers;
            app.selected_markers.clear();
            app.selected_markers.insert(hit);
            app.last_selected_marker = hit;
            viewport.invalidate_markers_columns(old);
            viewport.invalidate_marker_column(hit);
            app.drag.pending_collapse_to_hit = false;
        }
        // Flag strip is at most ~top_strip_height px tall; repainting
        // the whole strip takes ~0.05 ms, so don't bother per-flag.
        viewport.invalidate_top_strip();
    }
}

// Commit the current drag. Caller ensures drag was active. Sets dirty
// only if the markers actually moved. Playhead is left wherever it
// ended up (tracked live in the motion handler) — no snap.
void GuiWarpMarkersOps::commit_drag() {
    if (!app.drag.active) return;
    const bool moved = app.drag.moved;
    const bool transient = (app.drag.drag_mode == 'T');
    std::vector<GuiWarpMarker>    snap_w =
        std::move(app.drag.pre_drag_snapshot);
    std::vector<GuiTransientMarker> snap_t =
        std::move(app.drag.pre_drag_transient_snapshot);
    const int                 hint_last = app.drag.pre_drag_last_selected;
    app.drag = DragState{};
    if (moved) {
        if (transient) {
            undo.push_undo_transient(std::move(snap_t), OpKind::Move, hint_last);
        } else {
            undo.push_undo(std::move(snap_w), OpKind::Move, hint_last);
        }
        undo.recompute_dirty();
        viewport.invalidate_timestamp_area();
    }
    viewport.invalidate_waveform_area();
}

// Compute (delta_min, delta_max) scalar bounds for shifting the current
// selection set by a uniform delta. Neighbors: for each selected marker,
// the nearest non-selected marker on each side. Trim is purely cosmetic
// and does not constrain edits. Returns (0, 0) if empty or time-0 marker
// present (move forbidden).
std::pair<double, double> GuiWarpMarkersOps::compute_selection_delta_bounds(bool& ok) {
    ok = false;
    const auto& mv = app.warpmarkers.markers();
    if (app.selected_markers.empty()) return {0.0, 0.0};
    const int sr = audio.sample_rate();
    if (sr <= 0) return {0.0, 0.0};
    for (int idx : app.selected_markers) {
        if (idx < 0 || idx >= static_cast<int>(mv.size())) return {0.0, 0.0};
        if (idx == 0 || mv[idx].time_seconds == 0.0) return {0.0, 0.0};
    }
    const double sr_d = static_cast<double>(sr);
    const double spp  = current_samples_per_pixel(app, audio);
    const double eps  = 3.0 * spp / sr_d;  // 3 pixels at current zoom

    double d_min = -std::numeric_limits<double>::infinity();
    double d_max =  std::numeric_limits<double>::infinity();
    for (int idx : app.selected_markers) {
        const double orig_t = mv[idx].time_seconds;
        int prev = idx - 1;
        while (prev >= 0 && app.selected_markers.count(prev)) --prev;
        if (prev >= 0) {
            const double lb = (mv[prev].time_seconds + eps) - orig_t;
            if (lb > d_min) d_min = lb;
        }
        int next = idx + 1;
        while (next < static_cast<int>(mv.size()) &&
               app.selected_markers.count(next)) ++next;
        if (next < static_cast<int>(mv.size())) {
            const double ub = (mv[next].time_seconds - eps) - orig_t;
            if (ub < d_max) d_max = ub;
        }
    }
    ok = true;
    return {d_min, d_max};
}

// Shift every selected marker by the clamped delta. Returns whether any
// marker actually moved.
bool GuiWarpMarkersOps::apply_selection_shift(double raw_delta) {
    bool ok = false;
    auto [d_min, d_max] = compute_selection_delta_bounds(ok);
    if (!ok) return false;
    double delta = raw_delta;
    if (delta < d_min) delta = d_min;
    if (delta > d_max) delta = d_max;
    if (delta == 0.0) return false;
    for (int idx : app.selected_markers) {
        GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
        if (!m) continue;
        m->time_seconds += delta;
    }
    return true;
}

// Nudge selected markers by +/- 1 pixel of source time at current zoom.
// direction: -1 for earlier (up/left), +1 for later (down/right).
void GuiWarpMarkersOps::nudge_selected_markers(int direction) {
    if (app.loading || audio.total_frames() <= 0) return;
    // Nudges move the playhead (via sync_playhead_to_last_selected).
    // Stop playback first — Ctrl+Left/Right is the only caller path.
    stop_playback_if_playing();
    if (app.selected_markers.empty()) return;
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const double spp = current_samples_per_pixel(app, audio);
    const double delta_s =
        static_cast<double>(direction) * spp / static_cast<double>(sr);
    if (delta_s == 0.0) return;
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    if (apply_selection_shift(delta_s)) {
        undo.push_undo(std::move(pre_state), OpKind::Move, hint_last);
        selection.sync_playhead_to_last_selected();
        undo.recompute_dirty();
        viewport.invalidate_waveform_area();
        viewport.invalidate_timestamp_area();
    }
}

// `j`: move every selected marker so last_selected lands on the playhead.
// All-or-nothing: if any resulting position would violate monotonicity
// or trim, reject the whole operation with a stderr note.
void GuiWarpMarkersOps::jump_selection_to_playhead() {
    if (app.selected_markers.empty()) return;
    if (app.last_selected_marker < 0) return;
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const auto& mv = app.warpmarkers.markers();
    if (app.last_selected_marker >= static_cast<int>(mv.size())) return;
    const double anchor_t = mv[app.last_selected_marker].time_seconds;
    const double ph_t =
        static_cast<double>(app.playhead_sample) /
        static_cast<double>(sr);
    const double delta = ph_t - anchor_t;
    if (delta == 0.0) return;

    bool ok = false;
    auto [d_min, d_max] = compute_selection_delta_bounds(ok);
    if (!ok || delta < d_min || delta > d_max) {
        std::fprintf(stderr,
            "warptempo_gui: jump rejected: would violate marker "
            "ordering\n");
        return;
    }
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    for (int idx : app.selected_markers) {
        GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
        if (!m) continue;
        m->time_seconds += delta;
    }
    undo.push_undo(std::move(pre_state), OpKind::Move, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}
