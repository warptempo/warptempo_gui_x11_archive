# Render Path

## Top-level render function

`gui.set_on_redraw(...)` in `src/gui_main.cpp` (~line 656). The lambda is
invoked by `GuiX11::dispatch_event` in `src/gui_x11.cpp` for every `Expose`
event, which carries the damaged rectangle `(x, y, w, h)`. Drawing goes into
an off-screen pixmap-backed Cairo surface; `GuiX11::dispatch_event` then
flushes the surface and blits the damaged rectangle to the window via
`XCopyArea`.

## Region culling

Exists, but coarsely. The lambda:

1. Clips the Cairo context to `(x, y, w, h)` — any draw outside that box is
   discarded by Cairo.
2. Uses `rects_intersect(exposed, <subsystem_area>)` as an early-out guard
   around `render_markers`, `render_playhead`, `render_flags`, and the
   timestamp / dirty-indicator block. If the damaged rect doesn't overlap
   the subsystem's area the call is skipped entirely.
3. For the waveform, `render_waveform_exposed` intersects the damaged rect
   with each channel's rect and derives a sub-viewport so `render_waveform`
   only iterates the exposed columns.

What does **not** get culled: `render_markers` and `render_flags` always
iterate *all* markers when their area overlaps the damaged rect — they
don't skip markers whose x is outside the exposed horizontal band. So a
1-pixel playhead invalidation in a stereo viewport still walks the full
marker list twice (unselected enabled + unselected disabled) plus each
selected marker.

## Per-redraw Cairo / X11 calls

Per Expose: no new surface creation, no new Cairo context — the pixmap
surface is reused for the life of the window, recreated only on
`ConfigureNotify` resize. The redraw does `cairo_save/clip/restore`, calls
each `render_*` helper (each of which does its own `cairo_save/restore`
plus `cairo_stroke`/`cairo_fill`/`cairo_show_text`), then returns. After
the lambda, `GuiX11::dispatch_event` calls `cairo_surface_flush(surface_)`
and `XCopyArea(pixmap_ → win_)` for the damaged rect.

## Invalidation flow: input event → pixels

`set_on_key` / `set_on_button_press` / `set_on_motion` handlers mutate
`AppState` and call one or more `GuiX11::invalidate_region(x, y, w, h)`
calls. `invalidate_region` synthesizes an `Expose` event via `XSendEvent` +
`XFlush` — it is *not* queued until after the current handler returns. The
event is dispatched by the next `XNextEvent` in `GuiX11::run`'s main loop.

Example (`=` key → tempo nudge): `on_key` → `adjust_tempo` →
`invalidate_marker_column` (per selected marker) + `invalidate_top_strip`
+ `invalidate_dirty_and_timestamp`. Each is a separate `XSendEvent`, so
multiple `Expose` events arrive; the redraw lambda fires once per
`Expose`, each with its own damaged rect. There is **no coalescing** of
multiple invalidations in one frame — if the handler calls
`invalidate_region` three times, the redraw lambda runs three times.

Motion during Ctrl+drag: each `MotionNotify` → `on_motion` →
`invalidate_waveform_area` (full top + waveform) + possibly
`invalidate_playhead_columns` + `invalidate_timestamp_area`. Paint runs
once per motion event, not per frame — so a burst of motion events all
paint.
