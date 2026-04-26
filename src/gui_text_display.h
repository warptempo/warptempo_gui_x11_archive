#pragma once

#include "gui_render.h"

#include <cairo/cairo.h>
#include <string>

// Reusable text-display primitive. Draws a short string of text positioned
// relative to an anchor rectangle, in a monospace font, with a customizable
// color tint. Pure rendering: no input, no timing, no state mutation. The
// caller manages visibility timing, anchor computation, color choice, and
// region invalidation.
//
// First use: V.A3b's hover popup over pass / label_ref flag rects. Future
// uses include settings dialogs and other hover hints.

namespace gui_text_display {

// Note: not named Above/Below because X11's Xlib.h pollutes the global
// namespace with `#define Above` / `#define Below` (stack-mode constants),
// which mangle the enum name at preprocessor time in any TU that includes
// both this header and Xlib.h.
enum class Position {
    Top,
    Bottom,
};

struct State {
    // Anchor rectangle in screen coordinates. Popup is positioned relative
    // to this rect's top edge (Position::Top) or bottom edge
    // (Position::Bottom).
    GuiRect     anchor          = {0, 0, 0, 0};

    // Content string. Typically short (a tempo display in V.A3b's case).
    std::string content;

    // Caller-driven visibility flag. When false, render() is a no-op.
    bool        visible         = false;

    // Tint applied to the content text. Caller picks something visually
    // distinct from the surrounding rect's normal text color.
    GuiColor    color           = {1.0, 1.0, 1.0};

    // Where to place the popup relative to the anchor.
    Position    position        = Position::Top;
};

// Draw the popup. No-op if `s.visible` is false, the anchor has zero area,
// or the content is empty. Uses the same monospace font face as the rest of
// the GUI, at `font_size` pixels. Cairo state is saved/restored — callers
// can rely on no leakage of font face / source color.
void render(cairo_t* cr, const State& s, double font_size);

} // namespace gui_text_display
