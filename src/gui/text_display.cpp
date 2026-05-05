#include "text_display.h"

namespace text_display {

namespace {
// Pixel gap between the popup baseline and the anchor edge.
constexpr double kVerticalGapPx = 4.0;
} // namespace

void render(cairo_t* cr, const State& s, double font_size) {
    if (!s.visible) return;
    if (s.anchor.w <= 0 || s.anchor.h <= 0) return;
    if (s.content.empty()) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, s.content.c_str(), &ext);

    // The popup's text-left aligns with anchor.x. The caller decides what
    // that x represents — for the V.A3b hover popup it is the anchored
    // rect's text-origin so popup and rect text share a column.
    const double text_left = static_cast<double>(s.anchor.x);

    double baseline_y;
    if (s.position == Position::Top) {
        // Place baseline so the descent of the text sits `kVerticalGapPx`
        // above the top edge of the anchor, with kVPadExtraPx of additional
        // clearance for the popup's own bottom inner padding.
        baseline_y = static_cast<double>(s.anchor.y)
                   - kVerticalGapPx
                   - kVPadExtraPx
                   - (ext.height + ext.y_bearing);
    } else {
        // Bottom: top of glyph cluster sits `kVerticalGapPx` plus the
        // popup's own top inner padding below the bottom edge of the anchor.
        baseline_y = static_cast<double>(s.anchor.y + s.anchor.h)
                   + kVerticalGapPx
                   + kVPadExtraPx
                   - ext.y_bearing;
    }

    cairo_set_source_rgb(cr, s.color.r, s.color.g, s.color.b);
    cairo_move_to(cr, text_left, baseline_y);
    cairo_show_text(cr, s.content.c_str());

    cairo_restore(cr);
}

} // namespace text_display
