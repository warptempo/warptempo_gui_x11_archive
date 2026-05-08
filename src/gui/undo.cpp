#include "undo.h"

#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

void Undo::recompute_dirty() {
    const auto& h = app.history;
    if (!h.saved_valid) {
        app.warp_dirty      = true;
        app.transient_dirty = true;
    } else if (h.saved_distance == 0) {
        app.warp_dirty      = false;
        app.transient_dirty = false;
    } else if (h.saved_distance < 0) {
        // Saved is `n` undos behind the current cursor. The last n
        // entries of undo_stack moved us from saved baseline to current.
        app.warp_dirty      = false;
        app.transient_dirty = false;
        const int n  = -h.saved_distance;
        const int us = static_cast<int>(h.undo_stack.size());
        for (int i = std::max(0, us - n); i < us; ++i) {
            if (h.undo_stack[i].op_mode == 'T') app.transient_dirty = true;
            else                                app.warp_dirty      = true;
        }
    } else {
        // Saved is `n` redos ahead. The top n entries of redo_stack
        // would, if redone, take us back to the saved state.
        app.warp_dirty      = false;
        app.transient_dirty = false;
        const int n  = h.saved_distance;
        const int rs = static_cast<int>(h.redo_stack.size());
        for (int i = std::max(0, rs - n); i < rs; ++i) {
            if (h.redo_stack[i].op_mode == 'T') app.transient_dirty = true;
            else                                app.warp_dirty      = true;
        }
    }
    app.dirty = app.warp_dirty || app.transient_dirty;
}

void Undo::push_undo(std::vector<GuiWarpMarker> pre_state, OpKind op_kind,
                     int hint_last) {
    UndoEntry e;
    e.snapshot           = std::move(pre_state);
    e.transient_snapshot = app.transientmarkers.markers();
    e.op_kind            = op_kind;
    e.op_mode            = 'W';
    e.hint_last_selected = hint_last;
    app.history.push(std::move(e));
    clear_hover_popup();
}

void Undo::push_undo_transient(std::vector<GuiTransientMarker> pre_state,
                               OpKind op_kind, int hint_last) {
    UndoEntry e;
    e.snapshot           = app.warpmarkers.markers();
    e.transient_snapshot = std::move(pre_state);
    e.op_kind            = op_kind;
    e.op_mode            = 'T';
    e.hint_last_selected = hint_last;
    app.history.push(std::move(e));
    clear_hover_popup();
}

void Undo::push_undo_both(std::vector<GuiWarpMarker> warp_pre,
                          std::vector<GuiTransientMarker> trans_pre,
                          char op_mode, OpKind op_kind, int hint_last) {
    UndoEntry e;
    e.snapshot           = std::move(warp_pre);
    e.transient_snapshot = std::move(trans_pre);
    e.op_kind            = op_kind;
    e.op_mode            = op_mode;
    e.hint_last_selected = hint_last;
    app.history.push(std::move(e));
    clear_hover_popup();
}

void Undo::apply_post_restore_rules_warp(const UndoEntry& entry,
                                         const std::vector<GuiWarpMarker>& before) {
    const auto& after = app.warpmarkers.markers();
    constexpr double kEps = 1e-9;

    std::set<int> target_set;
    bool want_playhead_jump = false;

    if (after.size() > before.size()) {
        std::vector<double> before_times;
        before_times.reserve(before.size());
        for (const auto& m : before) before_times.push_back(m.time_seconds);
        std::sort(before_times.begin(), before_times.end());
        for (size_t i = 0; i < after.size(); ++i) {
            const double t = after[i].time_seconds;
            auto it = std::lower_bound(before_times.begin(),
                                       before_times.end(), t - kEps);
            const bool matched = (it != before_times.end() &&
                                  std::abs(*it - t) < kEps);
            if (!matched) target_set.insert(static_cast<int>(i));
        }
        want_playhead_jump = !target_set.empty();
    } else if (after.size() < before.size()) {
        const int sr = selection.audio.sample_rate();
        if (sr > 0) {
            std::vector<double> after_times;
            after_times.reserve(after.size());
            for (const auto& m : after) after_times.push_back(m.time_seconds);
            std::sort(after_times.begin(), after_times.end());
            double rightmost = 0.0;
            bool   any       = false;
            for (const auto& m : before) {
                const double t = m.time_seconds;
                auto it = std::lower_bound(after_times.begin(),
                                           after_times.end(), t - kEps);
                const bool matched = (it != after_times.end() &&
                                      std::abs(*it - t) < kEps);
                if (!matched && (!any || t > rightmost)) {
                    rightmost = t;
                    any       = true;
                }
            }
            if (any) {
                const int64_t target_sample = static_cast<int64_t>(
                    std::nearbyint(rightmost * static_cast<double>(sr)));
                selection.jump_playhead_to(target_sample);
            }
        }
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        return;
    } else if (entry.op_kind == OpKind::Move) {
        for (size_t i = 0; i < after.size(); ++i) {
            if (std::abs(after[i].time_seconds -
                         before[i].time_seconds) > kEps) {
                target_set.insert(static_cast<int>(i));
            }
        }
        want_playhead_jump = !target_set.empty();
    } else {
        return;
    }

    if (target_set.empty()) return;

    app.selected_markers = target_set;
    if (target_set.count(entry.hint_last_selected)) {
        app.last_selected_marker = entry.hint_last_selected;
    } else {
        app.last_selected_marker = *target_set.rbegin();
    }

    if (!want_playhead_jump) return;
    selection.sync_playhead_to_last_selected();
}

void Undo::apply_post_restore_rules_transient(const UndoEntry& entry,
                                              const std::vector<GuiTransientMarker>& before) {
    const auto& after = app.transientmarkers.markers();
    constexpr double kEps = 1e-9;

    std::set<int> target_set;
    bool want_playhead_jump = false;

    if (after.size() > before.size()) {
        std::vector<double> before_times;
        before_times.reserve(before.size());
        for (const auto& m : before) before_times.push_back(m.time_seconds);
        std::sort(before_times.begin(), before_times.end());
        for (size_t i = 0; i < after.size(); ++i) {
            const double t = after[i].time_seconds;
            auto it = std::lower_bound(before_times.begin(),
                                       before_times.end(), t - kEps);
            const bool matched = (it != before_times.end() &&
                                  std::abs(*it - t) < kEps);
            if (!matched) target_set.insert(static_cast<int>(i));
        }
        want_playhead_jump = !target_set.empty();
    } else if (after.size() < before.size()) {
        std::vector<double> after_times;
        after_times.reserve(after.size());
        for (const auto& m : after) after_times.push_back(m.time_seconds);
        std::sort(after_times.begin(), after_times.end());
        double rightmost = 0.0;
        bool   any       = false;
        for (const auto& m : before) {
            const double t = m.time_seconds;
            auto it = std::lower_bound(after_times.begin(),
                                       after_times.end(), t - kEps);
            const bool matched = (it != after_times.end() &&
                                  std::abs(*it - t) < kEps);
            if (!matched && (!any || t > rightmost)) {
                rightmost = t;
                any       = true;
            }
        }
        if (any) {
            const int sr = selection.audio.sample_rate();
            selection.jump_playhead_to(static_cast<int64_t>(std::nearbyint(
                rightmost * static_cast<double>(sr))));
        }
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        return;
    } else if (entry.op_kind == OpKind::Move) {
        for (size_t i = 0; i < after.size(); ++i) {
            if (std::abs(after[i].time_seconds -
                         before[i].time_seconds) > kEps) {
                target_set.insert(static_cast<int>(i));
            }
        }
        want_playhead_jump = !target_set.empty();
    } else {
        return;
    }

    if (target_set.empty()) return;

    app.selected_markers = target_set;
    if (target_set.count(entry.hint_last_selected)) {
        app.last_selected_marker = entry.hint_last_selected;
    } else {
        app.last_selected_marker = *target_set.rbegin();
    }

    if (!want_playhead_jump) return;
    selection.sync_playhead_to_last_selected();
}

void Undo::do_undo() {
    if (app.history.undo_stack.empty()) return;
    stop_playback_if_playing();
    clear_hover_popup();
    UndoEntry entry = std::move(app.history.undo_stack.back());
    app.history.undo_stack.pop_back();

    UndoEntry redo_entry;
    redo_entry.snapshot           = app.warpmarkers.markers();
    redo_entry.transient_snapshot = app.transientmarkers.markers();
    redo_entry.op_kind            = entry.op_kind;
    redo_entry.op_mode            = entry.op_mode;
    redo_entry.hint_last_selected = entry.hint_last_selected;
    std::vector<GuiWarpMarker>    before_w = redo_entry.snapshot;
    std::vector<GuiTransientMarker> before_t = redo_entry.transient_snapshot;

    app.history.redo_stack.push_back(std::move(redo_entry));
    if (app.history.redo_stack.size() > UndoHistory::kCap) {
        app.history.redo_stack.erase(app.history.redo_stack.begin());
    }
    if (app.history.saved_valid) app.history.saved_distance += 1;

    app.warpmarkers.markers_mut()    = std::move(entry.snapshot);
    app.transientmarkers.markers_mut() = std::move(entry.transient_snapshot);

    // Switch active mode to match the op being undone before applying
    // post-restore rules — selection state is mode-bound, so the rules
    // and the sanitize step must run against the correct list.
    if (entry.op_mode != app.active_mode) {
        // Stash the current selection into the leaving mode's slot,
        // then restore the destination mode's slot.
        ViewState& curtab = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
        if (app.active_mode == 'T') {
            curtab.transient_selected      = app.selected_markers;
            curtab.transient_last_selected = app.last_selected_marker;
            app.selected_markers           = curtab.warp_selected;
            app.last_selected_marker       = curtab.warp_last_selected;
        } else {
            curtab.warp_selected           = app.selected_markers;
            curtab.warp_last_selected      = app.last_selected_marker;
            app.selected_markers           = curtab.transient_selected;
            app.last_selected_marker       = curtab.transient_last_selected;
        }
        app.active_mode = entry.op_mode;
    }

    if (entry.op_mode == 'T') {
        apply_post_restore_rules_transient(entry, before_t);
        selection.sanitize_selection_after_restore(
            static_cast<int>(app.transientmarkers.markers().size()));
    } else {
        apply_post_restore_rules_warp(entry, before_w);
        selection.sanitize_selection_after_restore(
            static_cast<int>(app.warpmarkers.markers().size()));
    }
    recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

void Undo::do_redo() {
    if (app.history.redo_stack.empty()) return;
    stop_playback_if_playing();
    clear_hover_popup();
    UndoEntry entry = std::move(app.history.redo_stack.back());
    app.history.redo_stack.pop_back();

    UndoEntry undo_entry;
    undo_entry.snapshot           = app.warpmarkers.markers();
    undo_entry.transient_snapshot = app.transientmarkers.markers();
    undo_entry.op_kind            = entry.op_kind;
    undo_entry.op_mode            = entry.op_mode;
    undo_entry.hint_last_selected = entry.hint_last_selected;
    std::vector<GuiWarpMarker>    before_w = undo_entry.snapshot;
    std::vector<GuiTransientMarker> before_t = undo_entry.transient_snapshot;

    app.history.undo_stack.push_back(std::move(undo_entry));
    if (app.history.undo_stack.size() > UndoHistory::kCap) {
        app.history.undo_stack.erase(app.history.undo_stack.begin());
    }
    if (app.history.saved_valid) app.history.saved_distance -= 1;

    app.warpmarkers.markers_mut()    = std::move(entry.snapshot);
    app.transientmarkers.markers_mut() = std::move(entry.transient_snapshot);

    if (entry.op_mode != app.active_mode) {
        ViewState& curtab = (app.active_tab == 'B') ? app.tab_b : app.tab_a;
        if (app.active_mode == 'T') {
            curtab.transient_selected      = app.selected_markers;
            curtab.transient_last_selected = app.last_selected_marker;
            app.selected_markers           = curtab.warp_selected;
            app.last_selected_marker       = curtab.warp_last_selected;
        } else {
            curtab.warp_selected           = app.selected_markers;
            curtab.warp_last_selected      = app.last_selected_marker;
            app.selected_markers           = curtab.transient_selected;
            app.last_selected_marker       = curtab.transient_last_selected;
        }
        app.active_mode = entry.op_mode;
    }

    if (entry.op_mode == 'T') {
        apply_post_restore_rules_transient(entry, before_t);
        selection.sanitize_selection_after_restore(
            static_cast<int>(app.transientmarkers.markers().size()));
    } else {
        apply_post_restore_rules_warp(entry, before_w);
        selection.sanitize_selection_after_restore(
            static_cast<int>(app.warpmarkers.markers().size()));
    }
    recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}
