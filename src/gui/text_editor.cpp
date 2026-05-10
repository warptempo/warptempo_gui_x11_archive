#include "text_editor.h"

#include <algorithm>

namespace text_editor {

namespace {

// Vocabulary closed set, no whitespace, no pipe (pipe is in the locked
// prefix). Letters lowercase only — uppercase swallowed (the platform
// boundary already case-folds, so only the lowercase form arrives). Map
// a GuiKey (paired with mods) to the literal char to insert; returns 0
// for "not a printable in this vocabulary". `kind` selects between the
// flag-payload, iteration-bracket, and BPM-bracket alphabets.
char keysym_to_char(GuiKey key, GuiInputState mods, Kind kind) {
    const bool shift = mods.shift;

    // Digits are common to all kinds.
    if (key >= GuiKeys::Digit0 && key <= GuiKeys::Digit9 && !shift) {
        // Brief X.2: Shift+2 → '@' for BpmBracket only; the digit branch
        // already filters out shift, so the BpmBracket-specific block
        // below is what handles the shifted form.
        return static_cast<char>('0' + (key - GuiKeys::Digit0));
    }
    // Decimal point is accepted by FlagPayload and IterationBracket but
    // NOT BpmBracket (strict integer-only).
    if (key == GuiKeys::Period && !shift && kind != Kind::BpmBracket) return '.';

    if (kind == Kind::IterationBracket) {
        // Brackets, comma, signed-number prefixes. No letters, no `*`,
        // no `:` — those would be syntactically invalid in the
        // iteration popup payload.
        if (key == GuiKeys::BracketLeft  && !shift) return '[';
        if (key == GuiKeys::BracketRight && !shift) return ']';
        if (key == GuiKeys::Comma        && !shift) return ',';
        if (key == GuiKeys::Minus        && !shift) return '-';
        if (key == GuiKeys::Plus)                   return '+';
        // US layout: Shift+= produces +.
        if (key == GuiKeys::Equal && shift)         return '+';
        return 0;
    }

    if (kind == Kind::BpmBracket) {
        // Brief X.2: digits, `@`, `,`, `[`, `]`. No letters, no signs,
        // no decimals.
        if (key == GuiKeys::BracketLeft  && !shift) return '[';
        if (key == GuiKeys::BracketRight && !shift) return ']';
        if (key == GuiKeys::Comma        && !shift) return ',';
        if (key == GuiKeys::At)                     return '@';
        // US layout: Shift+2 produces @.
        if (key == GuiKeys::Digit2 && shift)        return '@';
        return 0;
    }

    // FlagPayload kind.
    if (key >= GuiKeys::A && key <= GuiKeys::Z && !shift) {
        return static_cast<char>('a' + (key - GuiKeys::A));
    }
    if (key == GuiKeys::Asterisk)            return '*';
    if (key == GuiKeys::Colon)               return ':';
    // US layout: Shift+; gives :. Treat as colon.
    if (key == GuiKeys::Semicolon && shift)  return ':';
    // Shift+8 also produces asterisk on US layouts. Let it through.
    if (key == GuiKeys::Digit8 && shift)     return '*';
    return 0;
}

void touch_blink(State& s) {
    s.blink_epoch = std::chrono::steady_clock::now();
}

// Remove the selected range from `pending`, place the cursor at the
// selection start, and clear the anchor. No-op if no selection.
void erase_selection(State& s) {
    if (!has_selection(s)) return;
    const int a = selection_start(s);
    const int b = selection_end(s);
    s.pending.erase(static_cast<size_t>(a),
                    static_cast<size_t>(b - a));
    s.cursor_pos = a;
    s.selection_anchor = -1;
}

} // namespace

void deactivate(State& s) {
    s.target            = -1;
    s.kind              = Kind::FlagPayload;
    s.pending.clear();
    s.locked_prefix.clear();
    s.cursor_pos        = 0;
    s.selection_anchor  = -1;
    s.red               = false;
}

void enter(State& s, int target,
           std::string locked_prefix,
           std::string initial_pending,
           Kind kind) {
    s.target            = target;
    s.kind              = kind;
    s.locked_prefix     = std::move(locked_prefix);
    s.pending           = std::move(initial_pending);
    s.cursor_pos        = static_cast<int>(s.pending.size());
    s.selection_anchor  = -1;
    s.red               = false;
    touch_blink(s);
}

KeyAction handle_key(State& s, GuiKey key, GuiInputState mods) {
    if (!is_active(s)) return KeyAction::NotConsumed;

    const bool ctrl  = mods.ctrl;
    const bool shift = mods.shift;

    if (key == GuiKeys::Escape) {
        return KeyAction::CancelRequested;
    }
    if (key == GuiKeys::Return || key == GuiKeys::KpEnter) {
        return KeyAction::CommitRequested;
    }

    // Ctrl+A: select-all. Sits above the printable-detect path so the
    // `a` key doesn't fall through and insert a literal 'a'.
    if (ctrl && key == GuiKeys::A) {
        if (!s.pending.empty()) {
            s.selection_anchor = 0;
            s.cursor_pos = static_cast<int>(s.pending.size());
            touch_blink(s);
        }
        return KeyAction::Consumed;
    }

    // Cursor motion. Shift extends a selection from an anchor; bare
    // motion collapses any existing selection to the corresponding edge.
    if (key == GuiKeys::Left) {
        if (shift) {
            if (s.selection_anchor < 0) s.selection_anchor = s.cursor_pos;
            if (s.cursor_pos > 0) s.cursor_pos--;
        } else {
            if (has_selection(s)) {
                s.cursor_pos = selection_start(s);
            } else if (s.cursor_pos > 0) {
                s.cursor_pos--;
            }
            s.selection_anchor = -1;
        }
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (key == GuiKeys::Right) {
        if (shift) {
            if (s.selection_anchor < 0) s.selection_anchor = s.cursor_pos;
            if (s.cursor_pos < static_cast<int>(s.pending.size())) {
                s.cursor_pos++;
            }
        } else {
            if (has_selection(s)) {
                s.cursor_pos = selection_end(s);
            } else if (s.cursor_pos <
                       static_cast<int>(s.pending.size())) {
                s.cursor_pos++;
            }
            s.selection_anchor = -1;
        }
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (key == GuiKeys::Home) {
        if (shift) {
            if (s.selection_anchor < 0) s.selection_anchor = s.cursor_pos;
        } else {
            s.selection_anchor = -1;
        }
        s.cursor_pos = 0;
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (key == GuiKeys::End) {
        if (shift) {
            if (s.selection_anchor < 0) s.selection_anchor = s.cursor_pos;
        } else {
            s.selection_anchor = -1;
        }
        s.cursor_pos = static_cast<int>(s.pending.size());
        touch_blink(s);
        return KeyAction::Consumed;
    }

    // Editing.
    if (key == GuiKeys::BackSpace) {
        if (has_selection(s)) {
            erase_selection(s);
            s.red = false;
        } else if (s.cursor_pos > 0) {
            s.pending.erase(static_cast<size_t>(s.cursor_pos - 1), 1);
            s.cursor_pos--;
            s.red = false;
        }
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (key == GuiKeys::Delete) {
        if (has_selection(s)) {
            erase_selection(s);
            s.red = false;
        } else if (s.cursor_pos < static_cast<int>(s.pending.size())) {
            s.pending.erase(static_cast<size_t>(s.cursor_pos), 1);
            s.red = false;
        }
        touch_blink(s);
        return KeyAction::Consumed;
    }

    // Printable insertion (length-capped). BpmBracket gets a tighter cap
    // than the default (brief X.2): the strict format `<beats>@[<lo>,<hi>]`
    // tops out at 12 chars, so 13 leaves one char of typo slack.
    const char ch = keysym_to_char(key, mods, s.kind);
    if (ch != 0) {
        const int cap = (s.kind == Kind::BpmBracket)
            ? kMaxPendingCharsBpm : kMaxPendingChars;
        // Replace-on-type: erase before the cap check so the typed
        // char can land inside the cap when a max-length pending is
        // entirely selected.
        if (has_selection(s)) {
            erase_selection(s);
        }
        if (static_cast<int>(s.pending.size()) >= cap) {
            // Silent swallow at cap.
            return KeyAction::Consumed;
        }
        s.pending.insert(static_cast<size_t>(s.cursor_pos), 1, ch);
        s.cursor_pos++;
        s.red = false;
        touch_blink(s);
        return KeyAction::Consumed;
    }

    // Any other key while editing: swallow (the editor owns the keyboard).
    return KeyAction::Consumed;
}

bool cursor_visible_now(const State& s) {
    if (!is_active(s)) return false;
    using namespace std::chrono;
    const auto now = steady_clock::now();
    const auto ms  = duration_cast<milliseconds>(now - s.blink_epoch).count();
    // 1000ms period: visible for the first 500ms, hidden for the next 500ms.
    return ((ms / 500) % 2) == 0;
}

bool blink_period_milliseconds(int& out_ms) {
    out_ms = 500;
    return true;
}

} // namespace text_editor
