#pragma once

#include <limits>
#include <string>
#include <vector>

// One warp marker, the GUI's authoring view. Three independent state axes:
//
//   1. Tempo source. `tempo_inherits == false`: this marker owns its tempo
//      (`tempo_base` is the numeric value). `tempo_inherits == true` (a
//      "pass" marker): the presentation tempo is resolved live by walking
//      backward through the marker list to the nearest owning marker.
//      `tempo_base`/`tempo_scale` carry inert defaults (1.0 / "1.0000")
//      that are never read while the marker is inheriting.
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

    // V.B iteration mode. Session-only render-parameter scratchpad: never
    // serialized, lost on app close, populated and edited inline via the
    // iteration popup that appears above each owning marker's flag rect
    // when iteration mode is on. NaN means "blank" (popup shows []); when
    // set, both are non-NaN and iter_start <= iter_end.
    double iter_start = std::numeric_limits<double>::quiet_NaN();
    double iter_end   = std::numeric_limits<double>::quiet_NaN();

    // Brief X.2 BPM mode. Session-only authoring state for basetempo-scale
    // sweeps; never serialized, lost on app close. At most one marker at a
    // time has bpm_is_popup_owner=true (invariant maintained by the `m`
    // toggle handler). "Committed" is implicit: bpm_beats > 0 means the
    // owner has authored a value (parser guarantees all three of bpm_beats,
    // bpm_lo, bpm_hi are set together). The value form is
    // "<beats>@[<lo>,<hi>]" with positive integers and lo <= hi.
    // Math/render is X.3.
    bool bpm_is_popup_owner = false;
    int  bpm_beats          = 0;
    int  bpm_lo             = 0;
    int  bpm_hi             = 0;
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

    // Static variant for callers that hold a raw GuiMarker vector (e.g. the
    // render pipeline writing per-render sidecars). Same on-disk format as
    // the instance method.
    static bool save(const std::string& path,
                     const std::vector<GuiMarker>& markers);

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

// True if the marker at `idx` should render as disabled. Per chunk U
// patch 3, `disabled` is allowed on any marker — a locally set flag
// always counts. For an active (non-locally-disabled) `label_ref`, the
// cascade rule applies: the ref inherits its target label_def's
// disabled state.
bool effective_disabled(const std::vector<GuiMarker>& markers, int idx);

namespace gui_markers_internal {

// Parse one canonical new-format line into a GuiMarker. Used by the GUI
// editor's commit path. Performs only line-local validation (format,
// whitespace rejection, payload structure) — cross-marker rules (label_ref
// existence, label_def uniqueness, time monotonicity) are the caller's
// responsibility. On `pass`, tempo_base/tempo_scale are populated with
// inert defaults (1.0 / "1.0000") — there is no cache to preserve.
bool parse_single_canonical_line(
    const std::string& raw_line,
    GuiMarker& out,
    std::string* error_out);

} // namespace gui_markers_internal
