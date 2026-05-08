#include "flag_editor.h"

#include "render.h"
#include "text_editor.h"
#include "time_format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

// Translate a click x-coordinate to a byte index into `pending` based
// on the monospace per-character advance and a known text-left x.
// Both `advance > 0` and `text_left_x >= 0` must hold; callers gate
// on those before invoking. The returned index is clamped to
// [0, pending_size].
int byte_index_from_click_x(double click_x, double text_left_x,
                            double advance, int pending_size) {
    const double offset = click_x - text_left_x;
    int idx = static_cast<int>(std::nearbyint(offset / advance));
    return std::clamp(idx, 0, pending_size);
}

} // namespace

// X.7.5b: flag-editor cluster. Method bodies are byte-identical to the
// lambdas they replaced in main.cpp, with these mechanical rewrites:
//
//   push_undo                      → undo.push_undo
//   recompute_dirty                → undo.recompute_dirty
//   invalidate_top_strip           → viewport.invalidate_top_strip
//   invalidate_waveform_area       → viewport.invalidate_waveform_area
//   invalidate_timestamp_area      → viewport.invalidate_timestamp_area
//   clear_hover_popup              → std::function ref (called as f())
//   exit_top_flag_edit_no_commit   → this->exit_top_flag_edit_no_commit
//   build_locked_prefix            → this->build_locked_prefix
//
// Free-function calls (text_editor::*, iter_popup_eligible_marker,
// bpm_popup_eligible_marker, format_iter_bracket_text,
// format_bpm_bracket_text, flag_text_for_marker,
// warpmarkers_internal::parse_single_canonical_line, parse_bpm_bracket,
// effective_disabled) keep their original spelling — the popup helpers
// moved from main.cpp's anonymous namespace into warpmarkers.h alongside
// effective_disabled, so this TU sees them via #include "warpmarkers.h".

// Build the locked-prefix string for `m` — exactly what the canonical
// serializer would write before the pipe character, with the pipe
// included. The editor renders this prefix outside the editable rect
// (left-anchored at the marker column); the pipe is part of the
// prefix but visually anchors to the marker line.
std::string GuiFlagEditor::build_locked_prefix(const GuiWarpMarker& m) {
    std::string out;
    if (m.is_begin_time)    out = "b=";
    else if (m.is_end_time) out = "e=";
    if (m.disabled) out += '#';
    // MM:SS.SSS
    out += format_timestamp(m.time_seconds);
    out += '|';
    return out;
}

void GuiFlagEditor::exit_top_flag_edit_no_commit() {
    if (!text_editor::is_active(app.top_flag_editor)) return;
    text_editor::deactivate(app.top_flag_editor);
    viewport.invalidate_top_strip();
}

void GuiFlagEditor::enter_top_flag_edit(int idx, double click_x) {
    if (idx < 0) return;
    const auto& mv = app.warpmarkers.markers();
    if (idx >= static_cast<int>(mv.size())) return;

    const bool same_target =
        text_editor::is_active(app.top_flag_editor) &&
        app.top_flag_editor.kind ==
            text_editor::Kind::FlagPayload &&
        app.top_flag_editor.target == idx;

    if (same_target) {
        // Re-click on the active editor: update cursor only,
        // preserve pending text and any in-progress state.
        if (click_x >= 0.0) {
            const double advance = monospace_advance();
            const double text_left =
                flag_pending_text_left_x(app, audio, idx);
            if (advance > 0.0 && text_left >= 0.0) {
                app.top_flag_editor.cursor_pos =
                    byte_index_from_click_x(
                        click_x, text_left, advance,
                        static_cast<int>(
                            app.top_flag_editor.pending.size()));
                app.top_flag_editor.selection_anchor = -1;
            }
        }
        viewport.invalidate_top_strip();
        return;
    }

    // Target-switching path. Centralize selection + playhead update
    // here so both call paths (in-edit-active switch and pre-edit
    // plain-flag-click) keep the marker-column outline and the rest
    // of the UI in sync with the new editor target.
    selection.set_single_selection(idx);
    {
        const int sr = audio.sample_rate();
        const int64_t sample = static_cast<int64_t>(std::nearbyint(
            mv[idx].time_seconds * static_cast<double>(sr)));
        viewport.move_playhead_to(sample);
    }

    // Discard any prior edit silently before switching targets.
    if (text_editor::is_active(app.top_flag_editor) &&
        app.top_flag_editor.target != idx) {
        text_editor::deactivate(app.top_flag_editor);
    }
    text_editor::enter(
        app.top_flag_editor, idx,
        this->build_locked_prefix(mv[idx]),
        flag_text_for_marker(mv, idx));

    if (click_x >= 0.0) {
        const double advance = monospace_advance();
        const double text_left =
            flag_pending_text_left_x(app, audio, idx);
        if (advance > 0.0 && text_left >= 0.0) {
            app.top_flag_editor.cursor_pos =
                byte_index_from_click_x(
                    click_x, text_left, advance,
                    static_cast<int>(
                        app.top_flag_editor.pending.size()));
        }
    }

    clear_hover_popup();
    viewport.invalidate_top_strip();
}

// Validate `pending` as a single canonical line and, on success, write
// the parsed marker's fields back onto markers_[idx]. Cascade-renames
// label_def changes onto every other marker that referenced the old
// name. Pushes one undo entry covering all touched markers.
//
// On failure: sets `red`, leaves pending/cursor intact, leaves the
// editor active.
void GuiFlagEditor::commit_top_flag_edit() {
    if (!text_editor::is_active(app.top_flag_editor)) return;
    const int idx = app.top_flag_editor.target;
    const auto& mv_const = app.warpmarkers.markers();
    if (idx < 0 || idx >= static_cast<int>(mv_const.size())) {
        // Editor target became invalid (e.g. file reload). Drop edit.
        this->exit_top_flag_edit_no_commit();
        return;
    }

    const std::string candidate =
        app.top_flag_editor.locked_prefix + app.top_flag_editor.pending;

    GuiWarpMarker parsed;
    std::string err;
    bool ok = warpmarkers_internal::parse_single_canonical_line(
        candidate, parsed, &err);

    // Cross-marker checks (edit target excluded).
    if (ok && !parsed.label_ref.empty()) {
        bool found = false;
        for (int i = 0; i < static_cast<int>(mv_const.size()); ++i) {
            if (i == idx) continue;
            if (mv_const[i].label_def == parsed.label_ref) {
                found = true;
                break;
            }
        }
        if (!found) {
            ok = false;
            err = "reference to undefined label: " + parsed.label_ref;
        }
    }
    if (ok && !parsed.label_def.empty()) {
        for (int i = 0; i < static_cast<int>(mv_const.size()); ++i) {
            if (i == idx) continue;
            if (mv_const[i].label_def == parsed.label_def) {
                ok = false;
                err = "duplicate label definition: " + parsed.label_def;
                break;
            }
        }
    }

    if (!ok) {
        app.top_flag_editor.red = true;
        viewport.invalidate_top_strip();
        std::fprintf(stderr,
            "warptempo_gui: edit rejected: %s\n", err.c_str());
        return;
    }

    // Capture pre-state for undo BEFORE mutating.
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;

    const std::string old_def = mv_const[idx].label_def;
    const std::string new_def = parsed.label_def;

    GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
    if (!m) {
        this->exit_top_flag_edit_no_commit();
        return;
    }

    // Time stays locked; preserve it (parse already produced the
    // same value via the locked prefix, but be explicit).
    const double preserved_time = m->time_seconds;

    // Cache-free: typing `pass` writes inert defaults into
    // tempo_base/tempo_scale; typing an explicit tempo writes the
    // owned value. label_def is independent of tempo source —
    // `pass:LABEL` carries a def at this position while inheriting
    // the tempo from a prior owning marker.
    if (parsed.tempo_inherits) {
        m->tempo_inherits = true;
        m->tempo_base     = 1.0;
        m->tempo_scale    = "1.0000";
        m->label_def      = parsed.label_def;
        m->label_ref.clear();
    } else {
        m->tempo_inherits = false;
        m->tempo_base     = parsed.tempo_base;
        m->tempo_scale    = parsed.tempo_scale;
        m->label_def      = parsed.label_def;
        m->label_ref      = parsed.label_ref;
    }
    m->time_seconds = preserved_time;
    // is_begin_time / is_end_time / disabled live in locked prefix —
    // parse_single_canonical_line populated them; reapply.
    m->is_begin_time = parsed.is_begin_time;
    m->is_end_time   = parsed.is_end_time;
    m->disabled      = parsed.disabled;

    // Cascade rename: if label_def changed and old_def was non-empty,
    // every other marker that referenced old_def gets its ref updated
    // to the new name (or cleared if new_def is empty — the user
    // converted a def to non-def).
    int n_refs_renamed = 0;
    if (!old_def.empty() && old_def != new_def) {
        auto& mv_mut = app.warpmarkers.markers_mut();
        for (int i = 0; i < static_cast<int>(mv_mut.size()); ++i) {
            if (i == idx) continue;
            if (mv_mut[i].label_ref == old_def) {
                mv_mut[i].label_ref = new_def;
                ++n_refs_renamed;
            }
        }
        std::fprintf(stderr,
            "[warptempo_gui] renamed label_def '%s' -> '%s'; "
            "updated %d refs\n",
            old_def.c_str(), new_def.c_str(), n_refs_renamed);
    }

    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    undo.recompute_dirty();

    text_editor::deactivate(app.top_flag_editor);

    viewport.invalidate_waveform_area();
    viewport.invalidate_timestamp_area();
}

// Open an iteration popup edit on `idx`. The seed pending is the
// current popup display ("[]" or "[+0.10,-0.05]") so the user can
// backspace into a valid edit position. Reuses `top_flag_editor`
// state but with Kind::IterationBracket so the editor's keyboard
// vocabulary swaps to `[]+-,.` and digits.
void GuiFlagEditor::enter_iter_edit(int idx, double click_x,
                                    double text_left_x) {
    if (idx < 0) return;
    if (!app.iteration_mode_enabled) return;
    const auto& mv = app.warpmarkers.markers();
    if (idx >= static_cast<int>(mv.size())) return;
    if (!iter_popup_eligible_marker(mv[idx])) return;

    const bool same_target =
        text_editor::is_active(app.top_flag_editor) &&
        app.top_flag_editor.kind ==
            text_editor::Kind::IterationBracket &&
        app.top_flag_editor.target == idx;

    if (same_target) {
        if (click_x >= 0.0 && text_left_x >= 0.0) {
            const double advance = monospace_advance();
            if (advance > 0.0) {
                app.top_flag_editor.cursor_pos =
                    byte_index_from_click_x(
                        click_x, text_left_x, advance,
                        static_cast<int>(
                            app.top_flag_editor.pending.size()));
                app.top_flag_editor.selection_anchor = -1;
            }
        }
        viewport.invalidate_top_strip();
        return;
    }

    // Target-switching path: see enter_top_flag_edit for rationale.
    selection.set_single_selection(idx);
    {
        const int sr = audio.sample_rate();
        const int64_t sample = static_cast<int64_t>(std::nearbyint(
            mv[idx].time_seconds * static_cast<double>(sr)));
        viewport.move_playhead_to(sample);
    }

    if (text_editor::is_active(app.top_flag_editor) &&
        app.top_flag_editor.target != idx) {
        text_editor::deactivate(app.top_flag_editor);
    }
    text_editor::enter(
        app.top_flag_editor, idx,
        /*locked_prefix=*/"",
        /*initial_pending=*/format_iter_bracket_text(mv[idx]),
        text_editor::Kind::IterationBracket);

    if (click_x >= 0.0 && text_left_x >= 0.0) {
        const double advance = monospace_advance();
        if (advance > 0.0) {
            app.top_flag_editor.cursor_pos =
                byte_index_from_click_x(
                    click_x, text_left_x, advance,
                    static_cast<int>(
                        app.top_flag_editor.pending.size()));
        }
    }

    clear_hover_popup();
    viewport.invalidate_top_strip();
}

// Commit the iteration popup's pending buffer. Four accepted forms:
//   1. ""           → iter_start/iter_end := NaN (clear).
//   2. "[]"         → iter_start/iter_end := NaN (clear).
//   3. "[%+0.2f,%+0.2f]" with start <= end → set iter values.
//   4. signed decimal "[+|-]NN[.NN]" → additive offset to tempo_base,
//      clamped to [0.01, 9.99]; iter values cleared.
// Anything else: red flash, stay in edit. Each accepted commit
// pushes one undo entry. Only case 4 affects the on-disk dirty flag
// (iter values are session-only and never serialized).
void GuiFlagEditor::commit_iter_edit() {
    if (!text_editor::is_active(app.top_flag_editor)) return;
    if (app.top_flag_editor.kind !=
            text_editor::Kind::IterationBracket) return;
    const int idx = app.top_flag_editor.target;
    const auto& mv_const = app.warpmarkers.markers();
    if (idx < 0 || idx >= static_cast<int>(mv_const.size())) {
        text_editor::deactivate(app.top_flag_editor);
        viewport.invalidate_top_strip();
        return;
    }
    const std::string& s = app.top_flag_editor.pending;

    bool   clear_iter   = false;
    bool   set_iter     = false;
    double new_start    = 0.0;
    double new_end      = 0.0;
    bool   offset_tempo = false;
    double tempo_delta  = 0.0;

    if (s.empty() || s == "[]") {
        clear_iter = true;
    } else {
        // Case 3: bracketed pair. Strict format — sign, digits, '.',
        // exactly 2 digits — and start <= end.
        auto parse_signed_2dp = [](const std::string& v,
                                   double& out) -> bool {
            if (v.size() < 4) return false;
            if (v[0] != '+' && v[0] != '-') return false;
            const auto dot = v.find('.', 1);
            if (dot == std::string::npos) return false;
            if (dot == 1) return false;
            if (v.size() - dot - 1 != 2) return false;
            for (size_t i = 1; i < v.size(); ++i) {
                if (i == dot) continue;
                if (!std::isdigit(
                        static_cast<unsigned char>(v[i]))) return false;
            }
            try { out = std::stod(v); }
            catch (...) { return false; }
            return true;
        };
        bool tried_pair = false;
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            const std::string inner = s.substr(1, s.size() - 2);
            const auto comma = inner.find(',');
            if (comma != std::string::npos) {
                double pa, pb;
                if (parse_signed_2dp(inner.substr(0, comma), pa) &&
                    parse_signed_2dp(inner.substr(comma + 1), pb) &&
                    pa <= pb) {
                    new_start = pa;
                    new_end   = pb;
                    set_iter  = true;
                }
                tried_pair = true;
            }
        }
        // Case 4: signed decimal additive offset to tempo_base.
        // Recognized only when bracket-pair parse failed; both cases
        // are mutually exclusive by syntax.
        if (!set_iter && !tried_pair) {
            if (s.size() >= 2 && (s[0] == '+' || s[0] == '-')) {
                bool seen_dot = false;
                int  digit_count = 0;
                bool ok = true;
                for (size_t i = 1; i < s.size(); ++i) {
                    const char c = s[i];
                    if (c == '.') {
                        if (seen_dot) { ok = false; break; }
                        seen_dot = true;
                        continue;
                    }
                    if (!std::isdigit(
                            static_cast<unsigned char>(c))) {
                        ok = false; break;
                    }
                    ++digit_count;
                }
                if (ok && digit_count > 0) {
                    try { tempo_delta = std::stod(s); offset_tempo = true; }
                    catch (...) { offset_tempo = false; }
                }
            }
        }
    }

    if (!clear_iter && !set_iter && !offset_tempo) {
        app.top_flag_editor.red = true;
        viewport.invalidate_top_strip();
        std::fprintf(stderr,
            "warptempo_gui: iter edit rejected: invalid syntax: %s\n",
            s.c_str());
        return;
    }

    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;

    GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
    if (!m) {
        text_editor::deactivate(app.top_flag_editor);
        viewport.invalidate_top_strip();
        return;
    }

    bool tempo_changed = false;
    if (offset_tempo) {
        double new_tempo = m->tempo_base + tempo_delta;
        if (new_tempo < 0.01) new_tempo = 0.01;
        if (new_tempo > 9.99) new_tempo = 9.99;
        // Snap to 2 decimals to mirror the owning-marker precision
        // used elsewhere (drop-marker, nudge, etc.).
        new_tempo = std::round(new_tempo * 100.0) / 100.0;
        if (new_tempo != m->tempo_base) {
            m->tempo_base = new_tempo;
            tempo_changed = true;
        }
        m->iter_start = std::numeric_limits<double>::quiet_NaN();
        m->iter_end   = std::numeric_limits<double>::quiet_NaN();
    } else if (set_iter) {
        m->iter_start = new_start;
        m->iter_end   = new_end;
    } else if (clear_iter) {
        m->iter_start = std::numeric_limits<double>::quiet_NaN();
        m->iter_end   = std::numeric_limits<double>::quiet_NaN();
    }

    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    if (tempo_changed) {
        undo.recompute_dirty();
        viewport.invalidate_waveform_area();
        viewport.invalidate_timestamp_area();
    }

    text_editor::deactivate(app.top_flag_editor);
    viewport.invalidate_top_strip();
}

// Bulk-clear the session-only iter values across all warp markers.
// Triggered by Shift+I while iteration mode is on. Single undo
// entry. No-op when no marker carries iter values (avoids a noise
// entry on the undo stack).
void GuiFlagEditor::bulk_clear_iter_values() {
    if (!app.iteration_mode_enabled) return;
    if (app.active_mode != 'W') return;
    auto& mv = app.warpmarkers.markers_mut();
    bool any = false;
    for (const auto& m : mv) {
        if (!std::isnan(m.iter_start) || !std::isnan(m.iter_end)) {
            any = true;
            break;
        }
    }
    if (!any) return;
    std::vector<GuiWarpMarker> pre_state = app.warpmarkers.markers();
    const int              hint_last = app.last_selected_marker;
    for (auto& m : mv) {
        m.iter_start = std::numeric_limits<double>::quiet_NaN();
        m.iter_end   = std::numeric_limits<double>::quiet_NaN();
    }
    undo.push_undo(std::move(pre_state), OpKind::Other, hint_last);
    viewport.invalidate_top_strip();
}

// Open a BPM popup edit on `idx`. Seed pending is the current popup
// text (`"[]"` when blank, else `"<beats>@[<lo>,<hi>]"`). Reuses
// top_flag_editor with Kind::BpmBracket so the keyboard vocabulary
// swaps to digits + `@`/`,`/`[`/`]`.
void GuiFlagEditor::enter_bpm_edit(int idx, double click_x,
                                   double text_left_x) {
    if (idx < 0) return;
    if (!app.bpm_mode_enabled) return;
    const auto& mv = app.warpmarkers.markers();
    if (idx >= static_cast<int>(mv.size())) return;
    if (!bpm_popup_eligible_marker(mv[idx])) return;

    const bool same_target =
        text_editor::is_active(app.top_flag_editor) &&
        app.top_flag_editor.kind ==
            text_editor::Kind::BpmBracket &&
        app.top_flag_editor.target == idx;

    if (same_target) {
        if (click_x >= 0.0 && text_left_x >= 0.0) {
            const double advance = monospace_advance();
            if (advance > 0.0) {
                app.top_flag_editor.cursor_pos =
                    byte_index_from_click_x(
                        click_x, text_left_x, advance,
                        static_cast<int>(
                            app.top_flag_editor.pending.size()));
                app.top_flag_editor.selection_anchor = -1;
            }
        }
        viewport.invalidate_top_strip();
        return;
    }

    // Target-switching path: see enter_top_flag_edit for rationale.
    selection.set_single_selection(idx);
    {
        const int sr = audio.sample_rate();
        const int64_t sample = static_cast<int64_t>(std::nearbyint(
            mv[idx].time_seconds * static_cast<double>(sr)));
        viewport.move_playhead_to(sample);
    }

    if (text_editor::is_active(app.top_flag_editor) &&
        app.top_flag_editor.target != idx) {
        text_editor::deactivate(app.top_flag_editor);
    }
    text_editor::enter(
        app.top_flag_editor, idx,
        /*locked_prefix=*/"",
        /*initial_pending=*/format_bpm_bracket_text(mv[idx]),
        text_editor::Kind::BpmBracket);

    if (click_x >= 0.0 && text_left_x >= 0.0) {
        const double advance = monospace_advance();
        if (advance > 0.0) {
            app.top_flag_editor.cursor_pos =
                byte_index_from_click_x(
                    click_x, text_left_x, advance,
                    static_cast<int>(
                        app.top_flag_editor.pending.size()));
        }
    }

    clear_hover_popup();
    viewport.invalidate_top_strip();
}

// Commit the BPM popup's pending buffer. Strict syntax via
// parse_bpm_bracket. On parse failure the editor stays open with
// a red outline; on success the parsed values are stored on the
// marker (the marker is already the popup owner). Brief X.2: no
// undo entry — BPM values are session-only, treated like view state.
void GuiFlagEditor::commit_bpm_edit() {
    if (!text_editor::is_active(app.top_flag_editor)) return;
    if (app.top_flag_editor.kind !=
            text_editor::Kind::BpmBracket) return;
    const int idx = app.top_flag_editor.target;
    const auto& mv_const = app.warpmarkers.markers();
    if (idx < 0 || idx >= static_cast<int>(mv_const.size())) {
        text_editor::deactivate(app.top_flag_editor);
        viewport.invalidate_top_strip();
        return;
    }
    const std::string& s = app.top_flag_editor.pending;
    int beats = 0, lo = 0, hi = 0;
    if (!parse_bpm_bracket(s, beats, lo, hi)) {
        app.top_flag_editor.red = true;
        viewport.invalidate_top_strip();
        std::fprintf(stderr,
            "warptempo_gui: bpm edit rejected: invalid syntax: %s\n",
            s.c_str());
        return;
    }
    GuiWarpMarker* m = app.warpmarkers.marker_mut(idx);
    if (!m) {
        text_editor::deactivate(app.top_flag_editor);
        viewport.invalidate_top_strip();
        return;
    }
    // Single-owner invariant: clear bpm_is_popup_owner on every other
    // marker before stamping this one. The toggle handler maintains
    // the invariant on mode entry, but the editor can target a
    // different marker than the one originally stamped (via click
    // -switching across popups), so reassert it here.
    auto& mv = app.warpmarkers.markers_mut();
    for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
        if (i == idx) continue;
        if (mv[i].bpm_is_popup_owner) {
            mv[i].bpm_is_popup_owner = false;
            mv[i].bpm_beats          = 0;
            mv[i].bpm_lo             = 0;
            mv[i].bpm_hi             = 0;
        }
    }
    m->bpm_is_popup_owner = true;
    m->bpm_beats     = beats;
    m->bpm_lo        = lo;
    m->bpm_hi        = hi;
    text_editor::deactivate(app.top_flag_editor);
    viewport.invalidate_top_strip();
}

// Bulk-clear every marker's BPM values. Triggered by Shift+M
// (regardless of mode state). Brief X.2: no undo entry; in-memory
// only, no .warpmarkers write.
void GuiFlagEditor::bulk_clear_bpm_values() {
    if (app.active_mode != 'W') return;
    auto& mv = app.warpmarkers.markers_mut();
    bool any = false;
    for (const auto& m : mv) {
        if (m.bpm_is_popup_owner) { any = true; break; }
    }
    if (!any) return;
    for (auto& m : mv) {
        m.bpm_is_popup_owner = false;
        m.bpm_beats          = 0;
        m.bpm_lo             = 0;
        m.bpm_hi             = 0;
    }
    viewport.invalidate_top_strip();
}

// Brief X.2: full mode-on transition for BPM mode. Validates the
// activation gate, toggles iter mode off if active, maintains the
// single-owner invariant, marks the selected marker as the popup
// owner (preserving prior values when re-toggling on the same
// owner), auto-selects the next eligible+enabled marker as the
// visual endpoint cue, and flips the mode flag.
void GuiFlagEditor::enter_bpm_mode() {
    if (app.bpm_mode_enabled) return;
    if (app.active_mode != 'W') return;
    if (app.selected_markers.size() != 1) return;
    const int owner = *app.selected_markers.begin();
    const auto& mv_const = app.warpmarkers.markers();
    if (owner < 0 || owner >= static_cast<int>(mv_const.size())) return;
    if (!bpm_popup_eligible_marker(mv_const[owner])) return;

    if (app.iteration_mode_enabled) {
        app.iteration_mode_enabled = false;
    }

    auto& mv = app.warpmarkers.markers_mut();
    for (int i = 0; i < static_cast<int>(mv.size()); ++i) {
        if (i == owner) continue;
        if (mv[i].bpm_is_popup_owner) {
            mv[i].bpm_is_popup_owner = false;
            mv[i].bpm_beats          = 0;
            mv[i].bpm_lo             = 0;
            mv[i].bpm_hi             = 0;
        }
    }
    // Tag owner with bpm_is_popup_owner=true if not already set so the
    // popup-walk in compute_bpm_popup_hits picks it up. Sentinel-zero
    // values stay zero; format_bpm_bracket_text renders "[]" for
    // that state. Re-toggling on the same owner preserves any
    // previously-committed values (the flag stays true and the
    // values aren't touched).
    if (!mv[owner].bpm_is_popup_owner) {
        mv[owner].bpm_is_popup_owner = true;
        mv[owner].bpm_beats          = 0;
        mv[owner].bpm_lo             = 0;
        mv[owner].bpm_hi             = 0;
    }

    // Auto-select endpoint: next non-disabled marker after owner.
    // Endpoint is purely informational (visual span boundary); pass
    // markers and label_refs are valid endpoints since the popup never
    // lives there. Only effectively-disabled markers are skipped.
    for (int i = owner + 1; i < static_cast<int>(mv.size()); ++i) {
        if (effective_disabled(mv, i)) continue;
        app.selected_markers.insert(i);
        break;
    }

    app.bpm_mode_enabled = true;
    clear_hover_popup();
    viewport.invalidate_top_strip();
}

void GuiFlagEditor::exit_bpm_mode() {
    if (!app.bpm_mode_enabled) return;
    app.bpm_mode_enabled = false;
    viewport.invalidate_top_strip();
}
