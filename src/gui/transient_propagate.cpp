#include "transient_propagate.h"

#include "transient_clipboard.h"
#include "transientmarkers.h"
#include "warpmarkers.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

// One named block resolved from a warp-marker walk. `label` is the
// owning marker's label name (empty markers don't produce entries);
// `start` and `end` are absolute seconds from the marker and its
// immediate successor.
struct DestBlock {
    std::string label;
    double      start;
    double      end;
};

// Walk the warp marker list across [from_idx, to_idx_exclusive),
// returning the named blocks in order. A block's extent runs from
// its owning marker to the next warp marker in the list; the final
// marker in the warp list has no successor and contributes no block.
// Markers without a label name also contribute no entry.
std::vector<DestBlock> walk_named_blocks(
    const std::vector<GuiWarpMarker>& mv,
    int from_idx, int to_idx_exclusive) {
    std::vector<DestBlock> out;
    const int n = static_cast<int>(mv.size());
    if (from_idx < 0)        from_idx = 0;
    if (to_idx_exclusive > n) to_idx_exclusive = n;
    for (int i = from_idx; i < to_idx_exclusive; ++i) {
        if (i + 1 >= n) break;  // no next marker → no extent
        const std::string& name = warp_marker_label_name(mv[i]);
        if (name.empty()) continue;
        const double start = mv[i].time_seconds;
        const double end   = mv[i + 1].time_seconds;
        out.push_back(DestBlock{name, start, end});
    }
    return out;
}

}  // namespace

void TransientPropagate::copy_from_selection() {
    const auto& mv = app.warpmarkers.markers();
    const auto& tv = app.transientmarkers.markers();
    if (app.selected_markers.size() != 2) return;

    auto it = app.selected_markers.begin();
    const int first_idx = *it++;
    const int last_idx  = *it;
    if (first_idx < 0 || last_idx < 0) return;
    const int n = static_cast<int>(mv.size());
    if (first_idx >= n || last_idx >= n) return;
    if (first_idx >= last_idx) return;

    // The closing boundary is the time of the last selected marker —
    // its block is excluded. The walk inspects markers in
    // [first_idx, last_idx) and uses each marker's next-time as the
    // block's end. The last_idx marker's time is the end-extent for
    // a named block at index last_idx - 1 (provided by walk_named_blocks's
    // i+1 indexing into mv).
    std::vector<DestBlock> src_blocks =
        walk_named_blocks(mv, first_idx, last_idx);

    std::vector<ClipboardBlock> clipboard_blocks;
    clipboard_blocks.reserve(src_blocks.size());
    for (const auto& b : src_blocks) {
        ClipboardBlock cb;
        cb.label_name = b.label;
        const double duration = b.end - b.start;
        if (duration <= 0.0) {
            clipboard_blocks.push_back(std::move(cb));
            continue;
        }
        for (const auto& t : tv) {
            const double t_time = t.time_seconds;
            if (t_time < b.start) continue;
            if (t_time >= b.end)  continue;
            ClipboardPlacement p;
            p.fractional_position = (t_time - b.start) / duration;
            p.disabled            = t.disabled;
            cb.placements.push_back(p);
        }
        clipboard_blocks.push_back(std::move(cb));
    }

    app.transient_clipboard.set(std::move(clipboard_blocks));
}

void TransientPropagate::open_paste_confirmation() {
    if (app.transient_clipboard.empty()) return;
    if (app.selected_markers.size() != 1) return;
    const int anchor = *app.selected_markers.begin();
    const int n = static_cast<int>(app.warpmarkers.markers().size());
    if (anchor < 0 || anchor >= n) return;

    app.pending_paste_anchor   = anchor;
    app.prompt.active          = true;
    app.prompt.text            =
        "Paste transients into matching blocks? "
        "Existing transients in matched ranges will be cleared.";
    app.prompt.response_keys   = {'y', '\x1b'};
    app.prompt.response_labels = {"[Y]es", "[Esc]"};
    app.prompt.trigger         = DialogTrigger::PASTE_CONFIRM;
    viewport.invalidate_all();
}

void TransientPropagate::paste_apply() {
    const int anchor = app.pending_paste_anchor;
    app.pending_paste_anchor = -1;
    if (app.transient_clipboard.empty()) return;
    const auto& mv = app.warpmarkers.markers();
    const int n = static_cast<int>(mv.size());
    if (anchor < 0 || anchor >= n) return;

    std::vector<DestBlock> dest_blocks =
        walk_named_blocks(mv, anchor, n);

    const auto& clip_blocks = app.transient_clipboard.blocks();

    // Lockstep walk; stop on the first name divergence.
    const size_t pair_count = std::min(clip_blocks.size(), dest_blocks.size());
    size_t matched = 0;
    for (; matched < pair_count; ++matched) {
        if (clip_blocks[matched].label_name != dest_blocks[matched].label) break;
    }
    if (matched == 0) return;

    std::vector<GuiTransientMarker> pre_state =
        app.transientmarkers.markers();
    const int hint_last = app.last_selected_marker;

    auto& out = app.transientmarkers.markers_mut();

    // Per-block clear of destination transients inside [start, end).
    for (size_t i = 0; i < matched; ++i) {
        const double start = dest_blocks[i].start;
        const double end   = dest_blocks[i].end;
        out.erase(std::remove_if(out.begin(), out.end(),
            [start, end](const GuiTransientMarker& m) {
                return m.time_seconds >= start && m.time_seconds < end;
            }), out.end());
    }

    // Per-block materialization. Insert via the monotonic-insertion
    // path so list ordering and any defensive dedup behave identically
    // to manual authoring.
    for (size_t i = 0; i < matched; ++i) {
        const double dst_start = dest_blocks[i].start;
        const double dst_dur   = dest_blocks[i].end - dst_start;
        if (dst_dur <= 0.0) continue;
        for (const auto& p : clip_blocks[i].placements) {
            GuiTransientMarker nm;
            nm.time_seconds = dst_start + p.fractional_position * dst_dur;
            nm.disabled     = p.disabled;
            app.transientmarkers.insert_marker(std::move(nm));
        }
    }

    undo.push_undo_transient(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();
    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}
