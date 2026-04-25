#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One transient marker, the GUI's authoring view (chunk S.2.2).
//
// S.2.2 only authors `inserted` transients — `is_inserted` is always true and
// the only on-disk status code is `I`. S.3 will extend with detected and
// nudged variants (which is why `is_inserted` is stored explicitly rather
// than implicit in the type).
//
// `disabled` is freely toggleable on any transient (no label_def constraint
// like warp markers have). `is_begin_time` and `is_end_time` mirror the
// cross-file trim flag system from S.1: a `b=` or `e=` flag may live on
// either a warp marker or a transient marker, and the engine consumes one
// of each across both files.
struct GuiTransient {
    int64_t src_frame     = 0;
    bool    is_inserted   = true;
    bool    disabled      = false;
    bool    is_begin_time = false;
    bool    is_end_time   = false;
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
    // src_frames silently and emits a one-line stderr notice if any rows
    // were dropped — mid-edit drag gestures may transit through duplicate
    // states, so we don't error in the GUI for them.
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

    // Inserts `m` at the position that preserves ascending src_frame order.
    // Returns the insertion index. Equal-frame collisions are accepted at
    // insert time (the user may transit through them via nudge); save
    // dedups them.
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
