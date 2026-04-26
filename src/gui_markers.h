#pragma once

#include <string>
#include <vector>

// One warp marker, the GUI's authoring view. Three independent state axes:
//
//   1. Tempo source. `tempo_inherits == false`: this marker owns its tempo
//      (`tempo_base` is the numeric value). `tempo_inherits == true`: the
//      presentation tempo is inherited from the nearest earlier owning
//      marker; `tempo_base`/`tempo_scale` then serve as a cache of the
//      remembered owned value (so toggling inherit off restores what was
//      there before).
//
//   2. Label relationship. At most one of `label_def` and `label_ref` is
//      non-empty. `label_def` marks a label origin; `label_ref` cites one.
//
//   3. Disabled flag. Allowed on any marker. A disabled marker has its
//      tempo contribution silenced; trim flags (`b=`/`e=`) are still
//      honored at render time (see chunk U patch 2). When the disabled
//      marker is a `label_def`, all `label_ref` markers pointing to it
//      are also treated as disabled (cascade). The cascade rule applies
//      only to label_def markers; a disabled non-label-def is locally
//      disabled and does not propagate.
struct GuiMarker {
    double time_seconds = 0.0;

    bool        tempo_inherits = false;
    double      tempo_base     = 1.0;
    std::string tempo_scale;

    std::string label_def;
    std::string label_ref;

    bool disabled      = false;
    bool is_begin_time = false;
    bool is_end_time   = false;
};

struct GuiMarkerError {
    int         line_number;   // 1-based; 0 means "file-level, no line"
    std::string message;
};

class GuiMarkers {
public:
    // Parses `path`. On success, populates markers() and returns true. On
    // failure, errors() lists what went wrong (continues parsing after the
    // first error so the caller sees the full set) and markers() is empty.
    // A missing file is reported via errors() and returns false; no throw.
    bool load(const std::string& path);

    // Writes the canonical form to `path`. Atomic: writes to
    // <path>.tmp, fsyncs, then renames. Preserves existing permissions or
    // uses 0644 if the file is new. Returns true on success.
    bool save(const std::string& path) const;

    const std::vector<GuiMarker>&       markers() const { return markers_; }
    const std::vector<GuiMarkerError>&  errors()  const { return errors_; }

    // True if load() observed content that the canonical save() would
    // discard: comments, blank lines, indented lines, freeform trailing
    // text, or ditto tempos.
    bool had_nonstandard_content() const { return had_nonstandard_content_; }

    // Inserts `m` at the position that preserves strict-monotonic order by
    // time_seconds. Returns the insertion index.
    int insert_marker(GuiMarker m);

    // Removes the marker at `index`. No-op if out of range.
    void remove_marker(int index);

    // Mutable accessor for keyboard/mouse toggles that edit a single marker
    // in place without changing its time (so list order is preserved).
    GuiMarker* marker_mut(int index) {
        if (index < 0 || index >= static_cast<int>(markers_.size())) return nullptr;
        return &markers_[index];
    }

    // Bulk-mutable accessor. Callers must not reorder by time_seconds; the
    // class assumes strict-monotonic order. Exposed for operations that
    // twiddle a flag across many markers at once (e.g. clearing all
    // is_begin_time/is_end_time markers).
    std::vector<GuiMarker>& markers_mut() { return markers_; }

    void clear() {
        markers_.clear();
        errors_.clear();
        had_nonstandard_content_ = false;
    }

private:
    std::vector<GuiMarker>       markers_;
    std::vector<GuiMarkerError>  errors_;
    bool                         had_nonstandard_content_ = false;
};
