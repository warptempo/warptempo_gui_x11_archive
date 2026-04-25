#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One transient marker, the GUI's authoring view.
//
// `is_inserted == true`: a manually authored transient at `src_frame`. On
// disk: `<src_frame> I` (with optional `b=`/`e=` prefix and `#` for disabled).
//
// `is_inserted == false`: a detected transient. `src_frame` is the immutable
// detection anchor (used by the merge step to match across re-detections).
// If `has_displacement`, the marker's effective on-screen position is
// `displaced_frame`; otherwise it is `src_frame`. On disk:
// `<src_frame> D` for plain detected, `<src_frame> D <displaced_frame>` for
// detected-with-nudge (D*).
//
// `disabled` is freely toggleable on any transient (no label_def constraint
// like warp markers have). `is_begin_time` and `is_end_time` mirror the
// cross-file trim flag system from S.1: a `b=` or `e=` flag may live on
// either a warp marker or a transient marker, and the engine consumes one
// of each across both files.
struct GuiTransient {
    int64_t src_frame        = 0;     // anchor for D, position for I
    bool    is_inserted      = true;  // false = D, true = I
    bool    disabled         = false;
    bool    is_begin_time    = false;
    bool    is_end_time      = false;
    bool    has_displacement = false; // only meaningful when !is_inserted
    int64_t displaced_frame  = 0;     // user-nudged position (D*)

    // Returns the marker's effective on-screen position. For I and plain D,
    // this is src_frame. For D-with-displacement, this is displaced_frame.
    int64_t effective_frame() const {
        return has_displacement ? displaced_frame : src_frame;
    }
};

struct GuiTransientError {
    int         line_number;   // 1-based; 0 means "file-level, no line"
    std::string message;
};

class GuiTransients {
public:
    // Parses `path`. On success, populates markers() and returns true. On
    // failure, errors() lists what went wrong (continues parsing after the
    // first error so the caller sees the full set) and markers() is empty.
    // A missing file is reported via errors() and returns false; no throw.
    bool load(const std::string& path);

    // Writes the canonical form to `path`. Atomic: writes to <path>.tmp,
    // fsyncs, then renames. Preserves existing permissions or uses 0644 if
    // the file is new. Returns true on success. Save dedups duplicate
    // effective_frame()s silently and emits a one-line stderr notice if any
    // rows were dropped — mid-edit drag gestures may transit through
    // duplicate states, so we don't error in the GUI for them.
    bool save(const std::string& path) const;

    // Best-effort unlink. Returns true on success or if the file didn't
    // already exist; false on any other error. Used by the empty-list save
    // path so an emptied transient list removes the on-disk sibling file.
    bool delete_file(const std::string& path) const;

    const std::vector<GuiTransient>&        markers() const { return markers_; }
    const std::vector<GuiTransientError>&   errors()  const { return errors_; }

    // True if load() observed content that the canonical save() would
    // discard: comments or blank lines.
    bool had_nonstandard_content() const { return had_nonstandard_content_; }
    void clear_nonstandard_flag() { had_nonstandard_content_ = false; }

    // Inserts `m` at the position that preserves ascending effective_frame()
    // order. Returns the insertion index. Equal-frame collisions are
    // accepted at insert time (the user may transit through them via
    // nudge); save dedups them.
    int insert_marker(GuiTransient m);

    // Removes the marker at `index`. No-op if out of range.
    void remove_marker(int index);

    GuiTransient* marker_mut(int index) {
        if (index < 0 || index >= static_cast<int>(markers_.size())) return nullptr;
        return &markers_[index];
    }

    std::vector<GuiTransient>& markers_mut() { return markers_; }

    void clear() {
        markers_.clear();
        errors_.clear();
        had_nonstandard_content_ = false;
    }

private:
    std::vector<GuiTransient>      markers_;
    std::vector<GuiTransientError> errors_;
    bool                           had_nonstandard_content_ = false;
};
