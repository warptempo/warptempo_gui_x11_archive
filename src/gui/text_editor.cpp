#include "text_editor.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>

namespace text_editor {

namespace {

// Vocabulary closed set, no whitespace, no pipe (pipe is in the locked
// prefix). Letters lowercase only — uppercase swallowed. Map an X11
// keysym (paired with mods) to the literal char to insert; returns 0
// for "not a printable in this vocabulary". `kind` selects between the
// flag-payload, iteration-bracket, and BPM-bracket alphabets.
char keysym_to_char(unsigned long keysym, unsigned int mods, Kind kind) {
    const bool shift = (mods & ShiftMask) != 0;

    // Digits are common to all kinds.
    if (keysym >= XK_0 && keysym <= XK_9 && !shift) {
        // Brief X.2: Shift+2 → '@' for BpmBracket only; the digit branch
        // already filters out shift, so the BpmBracket-specific block
        // below is what handles the shifted form.
        return static_cast<char>('0' + (keysym - XK_0));
    }
    // Decimal point is accepted by FlagPayload and IterationBracket but
    // NOT BpmBracket (strict integer-only).
    if (keysym == XK_period && !shift && kind != Kind::BpmBracket) return '.';

    if (kind == Kind::IterationBracket) {
        // Brackets, comma, signed-number prefixes. No letters, no `*`,
        // no `:` — those would be syntactically invalid in the
        // iteration popup payload.
        if (keysym == XK_bracketleft  && !shift) return '[';
        if (keysym == XK_bracketright && !shift) return ']';
        if (keysym == XK_comma        && !shift) return ',';
        if (keysym == XK_minus        && !shift) return '-';
        if (keysym == XK_plus)                   return '+';
        // US layout: Shift+= produces +.
        if (keysym == XK_equal && shift)         return '+';
        return 0;
    }

    if (kind == Kind::BpmBracket) {
        // Brief X.2: digits, `@`, `,`, `[`, `]`. No letters, no signs,
        // no decimals.
        if (keysym == XK_bracketleft  && !shift) return '[';
        if (keysym == XK_bracketright && !shift) return ']';
        if (keysym == XK_comma        && !shift) return ',';
        if (keysym == XK_at)                     return '@';
        // US layout: Shift+2 produces @.
        if (keysym == XK_2 && shift)             return '@';
        return 0;
    }

    // FlagPayload kind.
    if (keysym >= XK_a && keysym <= XK_z && !shift) {
        return static_cast<char>('a' + (keysym - XK_a));
    }
    if (keysym == XK_asterisk)            return '*';
    if (keysym == XK_colon)               return ':';
    // US layout: Shift+; gives :. Treat as colon.
    if (keysym == XK_semicolon && shift)  return ':';
    // Shift+8 also produces asterisk on US layouts. Let it through.
    if (keysym == XK_8 && shift)          return '*';
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

KeyAction handle_key(State& s, unsigned long keysym, unsigned int mods) {
    if (!is_active(s)) return KeyAction::NotConsumed;

    const bool ctrl  = (mods & ControlMask) != 0;
    const bool shift = (mods & ShiftMask) != 0;

    if (keysym == XK_Escape) {
        return KeyAction::CancelRequested;
    }
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        return KeyAction::CommitRequested;
    }

    // Ctrl+A: select-all. Sits above the printable-detect path so the
    // `a` keysym doesn't fall through and insert a literal 'a'.
    if (ctrl && (keysym == XK_a || keysym == XK_A)) {
        if (!s.pending.empty()) {
            s.selection_anchor = 0;
            s.cursor_pos = static_cast<int>(s.pending.size());
            touch_blink(s);
        }
        return KeyAction::Consumed;
    }

    // Cursor motion. Shift extends a selection from an anchor; bare
    // motion collapses any existing selection to the corresponding edge.
    if (keysym == XK_Left) {
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
    if (keysym == XK_Right) {
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
    if (keysym == XK_Home) {
        if (shift) {
            if (s.selection_anchor < 0) s.selection_anchor = s.cursor_pos;
        } else {
            s.selection_anchor = -1;
        }
        s.cursor_pos = 0;
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (keysym == XK_End) {
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
    if (keysym == XK_BackSpace) {
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
    if (keysym == XK_Delete) {
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
    const char ch = keysym_to_char(keysym, mods, s.kind);
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
