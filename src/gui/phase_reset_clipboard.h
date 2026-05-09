#pragma once

#include "warpmarkers.h"

#include <string>
#include <vector>

// Session-only clipboard for the W-mode phase reset propagate feature
// (Ctrl+T copy / Ctrl+Alt+T paste). A copy captures a sequence of named
// warp blocks and the fractional positions of any phase resets that fall
// inside them. Paste walks a destination anchor's named-block sequence
// in lockstep with the clipboard, materializing phase resets at the
// destination's actual durations. Single-slot, in-memory only — never
// persisted to any sidecar, cleared on app exit.

struct ClipboardPlacement {
    double fractional_position = 0.0;  // [0.0, 1.0) into the source block
    bool   disabled            = false;
};

struct ClipboardBlock {
    std::string                     label_name;
    std::vector<ClipboardPlacement> placements;
};

class PhaseResetClipboard {
public:
    void set(std::vector<ClipboardBlock> blocks) {
        blocks_ = std::move(blocks);
    }
    void clear()                                      { blocks_.clear(); }
    bool empty() const                                { return blocks_.empty(); }
    const std::vector<ClipboardBlock>& blocks() const { return blocks_; }

private:
    std::vector<ClipboardBlock> blocks_;
};

// Single accessor that returns a marker's label string regardless of
// whether it's a definition or a reference. Empty when the marker is
// unnamed; block matching at copy/paste time is exact string equality
// on this accessor's return value.
inline const std::string& warp_marker_label_name(const GuiWarpMarker& m) {
    return m.label_def.empty() ? m.label_ref : m.label_def;
}
