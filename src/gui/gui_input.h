#pragma once
#include <cstdint>

// Platform-neutral keyboard / mouse input types. Backends translate native
// events at their boundary into these so the rest of the GUI never sees
// platform-specific types. Numeric values mirror the universal keysym
// numbering shared by xkbcommon and the X Window System (xkbcommon was
// derived from that table), so each backend only needs a near-zero-cost
// cast plus a small case-fold for letters.
using GuiKey = uint32_t;

namespace GuiKeys {
    // Letters (X11 lowercase keysym values; case-folded at platform boundary).
    constexpr GuiKey A = 0x0061; constexpr GuiKey B = 0x0062;
    constexpr GuiKey C = 0x0063; constexpr GuiKey D = 0x0064;
    constexpr GuiKey E = 0x0065; constexpr GuiKey F = 0x0066;
    constexpr GuiKey G = 0x0067; constexpr GuiKey H = 0x0068;
    constexpr GuiKey I = 0x0069; constexpr GuiKey J = 0x006a;
    constexpr GuiKey K = 0x006b; constexpr GuiKey L = 0x006c;
    constexpr GuiKey M = 0x006d; constexpr GuiKey N = 0x006e;
    constexpr GuiKey O = 0x006f; constexpr GuiKey P = 0x0070;
    constexpr GuiKey Q = 0x0071; constexpr GuiKey R = 0x0072;
    constexpr GuiKey S = 0x0073; constexpr GuiKey T = 0x0074;
    constexpr GuiKey U = 0x0075; constexpr GuiKey V = 0x0076;
    constexpr GuiKey W = 0x0077; constexpr GuiKey X = 0x0078;
    constexpr GuiKey Y = 0x0079; constexpr GuiKey Z = 0x007a;

    // Digits.
    constexpr GuiKey Digit0 = 0x0030; constexpr GuiKey Digit1 = 0x0031;
    constexpr GuiKey Digit2 = 0x0032; constexpr GuiKey Digit3 = 0x0033;
    constexpr GuiKey Digit4 = 0x0034; constexpr GuiKey Digit5 = 0x0035;
    constexpr GuiKey Digit6 = 0x0036; constexpr GuiKey Digit7 = 0x0037;
    constexpr GuiKey Digit8 = 0x0038; constexpr GuiKey Digit9 = 0x0039;

    // Punctuation (X11 keysym values).
    constexpr GuiKey Space        = 0x0020;
    constexpr GuiKey Asterisk     = 0x002a;
    constexpr GuiKey Plus         = 0x002b;
    constexpr GuiKey Comma        = 0x002c;
    constexpr GuiKey Minus        = 0x002d;
    constexpr GuiKey Period       = 0x002e;
    constexpr GuiKey Colon        = 0x003a;
    constexpr GuiKey Semicolon    = 0x003b;
    constexpr GuiKey Equal        = 0x003d;
    constexpr GuiKey At           = 0x0040;
    constexpr GuiKey BracketLeft  = 0x005b;
    constexpr GuiKey BracketRight = 0x005d;

    // Navigation, editing, control.
    constexpr GuiKey BackSpace  = 0xff08;
    constexpr GuiKey Tab        = 0xff09;
    constexpr GuiKey Return     = 0xff0d;
    constexpr GuiKey Escape     = 0xff1b;
    constexpr GuiKey Home       = 0xff50;
    constexpr GuiKey Left       = 0xff51;
    constexpr GuiKey Up         = 0xff52;
    constexpr GuiKey Right      = 0xff53;
    constexpr GuiKey Down       = 0xff54;
    constexpr GuiKey End        = 0xff57;
    constexpr GuiKey IsoLeftTab = 0xfe20;
    constexpr GuiKey KpEnter    = 0xff8d;
    constexpr GuiKey Delete     = 0xffff;
}

struct GuiInputState {
    bool ctrl                 = false;
    bool shift                = false;
    bool alt                  = false;
    bool primary_button_held  = false;
};

enum class GuiMouseButton {
    Left,
    Middle,
    Right,
    WheelUp,
    WheelDown
};
