#pragma once

#include <chrono>
#include <functional>
#include <string>

// Generic in-place text editor for a small constrained vocabulary, used
// by the V.A1 top-flag editor. State is a POD-ish struct so callers can
// keep it inline on AppState. Validation, commit semantics, and visual
// rendering are the caller's concerns; this module only handles the
// keyboard-driven mutation of `pending` + cursor state and exposes hooks
// for blink and the parse-failure red flash.
//
// Reuse: V.A3 hover popups and V.B bottom-flag iteration syntax will
// supply different validators and writers but reuse this state shape and
// keystroke routing.

namespace text_editor {

// Maximum characters allowed in `pending`. Mirrors the longest valid
// payload `1.23*1.2345:a.aa` (16 chars). Insertions past this cap are
// silently swallowed (no red, no flash). Backspace/Delete/cursor moves
// remain available so an over-cap pending (loaded from a hand-edited
// file) can be trimmed back to canonical form.
constexpr int kMaxPendingChars = 16;
// Brief X.2 BPM popup. Cap matches the brief's per-Kind tightening for
// `<beats>@[<lo>,<hi>]` editing.
constexpr int kMaxPendingCharsBpm = 13;

// Vocabulary the editor accepts on the keyboard. Different call sites
// edit different payload shapes; the kind selects which keys produce a
// printable character. V.A1's flag editor uses FlagPayload (digits,
// letters, `.`, `*`, `:`); V.B's iteration popup uses IterationBracket
// (digits, `.`, `+`, `-`, `,`, `[`, `]`); brief X.2's BPM popup uses
// BpmBracket (digits, `@`, `,`, `[`, `]`).
enum class Kind {
    FlagPayload,
    IterationBracket,
    BpmBracket,
};

// State for a single editable rect.
struct State {
    // Identifier of the entity being edited. -1 means "not editing".
    // The caller decides what this means (a marker index in V.A1).
    int target = -1;

    // Vocabulary discriminator. The caller sets this in `enter()` and
    // the keystroke handler routes printable detection accordingly.
    Kind kind = Kind::FlagPayload;

    // Editable text — currently the canonical post-pipe payload.
    std::string pending;

    // Frozen prefix (rendered to the left of `pending`) carrying time
    // and metadata flags. Display-only; not editable.
    std::string locked_prefix;

    // Byte index into `pending`. Clamped to [0, pending.size()].
    int cursor_pos = 0;

    // True after a failed commit. Cleared by any keystroke that mutates
    // `pending`.
    bool red = false;

    // Cursor blink: monotonic timestamp at which the cursor became
    // visible. The renderer tests `(now - blink_epoch) % period < period/2`
    // to decide whether to draw the bar this frame.
    std::chrono::steady_clock::time_point blink_epoch =
        std::chrono::steady_clock::now();
};

inline bool is_active(const State& s) { return s.target >= 0; }

// Reset `s` so `is_active` returns false.
void deactivate(State& s);

// Begin editing `target` with the given locked prefix and seed pending.
// Cursor lands at end of pending. `kind` selects the vocabulary the
// keystroke handler will accept while this editor is active.
void enter(State& s, int target,
           std::string locked_prefix,
           std::string initial_pending,
           Kind kind = Kind::FlagPayload);

// Apply a key event to the editor. Returns true if the key was consumed
// — the caller should NOT route a consumed key to other handlers.
//
// Special return-value semantics for commit / discard:
//   - consumed_commit: editor wants the caller to validate-and-commit.
//   - consumed_cancel: editor wants the caller to discard (Esc).
//
// Pure printable characters and motion keys are handled internally and
// reported as plain `consumed`.
//
// Keysyms are X11 KeySym values; mods uses Xlib's modifier mask layout.
enum class KeyAction {
    NotConsumed,
    Consumed,
    CommitRequested,
    CancelRequested,
};

KeyAction handle_key(State& s, unsigned long keysym, unsigned int mods);

// Render-side helper: returns true if the cursor should be drawn this
// frame. Period is 1000ms (~500ms on, ~500ms off) and resets at every
// `pending` mutation so the cursor stays visible immediately after a
// keystroke.
bool cursor_visible_now(const State& s);

// True if `now - s.blink_epoch` straddled a half-period since the last
// call. Convenience for renderers that want to know whether to schedule
// the next paint. Always returns true on the first call after a state
// change.
bool blink_period_milliseconds(int& out_ms);

} // namespace text_editor
