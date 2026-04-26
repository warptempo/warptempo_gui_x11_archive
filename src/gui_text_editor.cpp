#include "gui_text_editor.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>

namespace gui_text_editor {

namespace {

// Vocabulary closed set, no whitespace, no pipe (pipe is in the locked
// prefix). Letters lowercase only — uppercase swallowed. Map an X11
// keysym (paired with mods) to the literal char to insert; returns 0
// for "not a printable in this vocabulary".
char keysym_to_char(unsigned long keysym, unsigned int mods) {
    const bool shift = (mods & ShiftMask) != 0;
    // Digits.
    if (keysym >= XK_0 && keysym <= XK_9 && !shift) {
        return static_cast<char>('0' + (keysym - XK_0));
    }
    // Letters a-z (lowercase only — Shift+letter is rejected).
    if (keysym >= XK_a && keysym <= XK_z && !shift) {
        return static_cast<char>('a' + (keysym - XK_a));
    }
    // Punctuation glyphs in the vocabulary.
    if (keysym == XK_period && !shift)    return '.';
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

} // namespace

void deactivate(State& s) {
    s.target        = -1;
    s.pending.clear();
    s.locked_prefix.clear();
    s.cursor_pos    = 0;
    s.red           = false;
}

void enter(State& s, int target,
           std::string locked_prefix,
           std::string initial_pending) {
    s.target        = target;
    s.locked_prefix = std::move(locked_prefix);
    s.pending       = std::move(initial_pending);
    s.cursor_pos    = static_cast<int>(s.pending.size());
    s.red           = false;
    touch_blink(s);
}

KeyAction handle_key(State& s, unsigned long keysym, unsigned int mods) {
    if (!is_active(s)) return KeyAction::NotConsumed;

    if (keysym == XK_Escape) {
        return KeyAction::CancelRequested;
    }
    if (keysym == XK_Return || keysym == XK_KP_Enter) {
        return KeyAction::CommitRequested;
    }

    // Cursor motion.
    if (keysym == XK_Left) {
        if (s.cursor_pos > 0) s.cursor_pos--;
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (keysym == XK_Right) {
        if (s.cursor_pos < static_cast<int>(s.pending.size())) s.cursor_pos++;
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (keysym == XK_Home) {
        s.cursor_pos = 0;
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (keysym == XK_End) {
        s.cursor_pos = static_cast<int>(s.pending.size());
        touch_blink(s);
        return KeyAction::Consumed;
    }

    // Editing.
    if (keysym == XK_BackSpace) {
        if (s.cursor_pos > 0) {
            s.pending.erase(static_cast<size_t>(s.cursor_pos - 1), 1);
            s.cursor_pos--;
            s.red = false;
        }
        touch_blink(s);
        return KeyAction::Consumed;
    }
    if (keysym == XK_Delete) {
        if (s.cursor_pos < static_cast<int>(s.pending.size())) {
            s.pending.erase(static_cast<size_t>(s.cursor_pos), 1);
            s.red = false;
        }
        touch_blink(s);
        return KeyAction::Consumed;
    }

    // Printable insertion (length-capped).
    const char ch = keysym_to_char(keysym, mods);
    if (ch != 0) {
        if (static_cast<int>(s.pending.size()) >= kMaxPendingChars) {
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

} // namespace gui_text_editor
