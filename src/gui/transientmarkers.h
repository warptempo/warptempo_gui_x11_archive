#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One transient marker, the GUI's authoring view: a transient authored at
// `src_frame`, with an optional `disabled` flag and optional cross-file
// `b=`/`e=` trim flags (`is_begin_time` / `is_end_time`, mirroring the S.1
// system shared with warp markers).
struct GuiTransientMarker {
    int64_t src_frame     = 0;
    bool    disabled      = false;
    bool    is_begin_time = false;
    bool    is_end_time   = false;

    int64_t effective_frame() const { return src_frame; }
};

struct GuiTransientMarkerError {
    int         line_number;   // 1-based; 0 means "file-level, no line"
    std::string message;
};

class GuiTransientMarkers {
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

    // Static variant for callers that hold a raw GuiTransientMarker vector (e.g.
    // the render pipeline writing per-render sidecars). Same on-disk format
    // and dedup behavior as the instance method.
    static bool save(const std::string& path,
                     const std::vector<GuiTransientMarker>& markers);

    // Best-effort unlink. Returns true on success or if the file didn't
    // already exist; false on any other error. Used by the empty-list save
    // path so an emptied transient list removes the on-disk sibling file.
    bool delete_file(const std::string& path) const;

    const std::vector<GuiTransientMarker>&        markers() const { return markers_; }
    const std::vector<GuiTransientMarkerError>&   errors()  const { return errors_; }

    // True if load() observed content that the canonical save() would
    // discard: comments or blank lines.
    bool had_nonstandard_content() const { return had_nonstandard_content_; }
    void clear_nonstandard_flag() { had_nonstandard_content_ = false; }

    // Inserts `m` at the position that preserves ascending effective_frame()
    // order. Returns the insertion index. Equal-frame collisions are
    // accepted at insert time (the user may transit through them via
    // nudge); save dedups them.
    int insert_marker(GuiTransientMarker m);

    // Removes the marker at `index`. No-op if out of range.
    void remove_marker(int index);

    GuiTransientMarker* marker_mut(int index) {
        if (index < 0 || index >= static_cast<int>(markers_.size())) return nullptr;
        return &markers_[index];
    }

    std::vector<GuiTransientMarker>& markers_mut() { return markers_; }

    void clear() {
        markers_.clear();
        errors_.clear();
        had_nonstandard_content_ = false;
    }

private:
    std::vector<GuiTransientMarker>      markers_;
    std::vector<GuiTransientMarkerError> errors_;
    bool                           had_nonstandard_content_ = false;
};
