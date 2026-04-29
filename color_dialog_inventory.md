# Color and Dialog Architecture Inventory

Phase 1 of Brief G + Addendum 1. Recon only — no source changes.

Working tree scanned: `src/` (10 GUI translation units, ~10k LOC).
The scan covers `src/*.{cpp,h}`; `miniaudio.h` was excluded from color-name
matches (its only `*0.70` matches are FFT constants, unrelated).

## Section 1: Color constant uses

All color constants are defined in `src/gui_main.cpp`. Inventory below
enumerates every consumption site. A "consumer" is any source location
naming the constant (whether to pass into a renderer, set a Cairo source,
or alias another constant).

### Constant definitions (where each name lives)

| File:line | Constant | Value |
|---|---|---|
| gui_main.cpp:46 | `kWaveformColor` | {0.55, 0.75, 0.90} |
| gui_main.cpp:47 | `kWaveformDimColor` | {0.30, 0.38, 0.45} |
| gui_main.cpp:48 | `kPlayheadColor` | {0.95, 0.85, 0.35} |
| gui_main.cpp:49 | `kTimestampColor` | {0.80, 0.80, 0.82} |
| gui_main.cpp:50 | `kMarkerColor` | {0.85, 0.82, 0.78} |
| gui_main.cpp:51 | `kMarkerDimColor` | {0.45, 0.42, 0.40} |
| gui_main.cpp:52 | `kSelectedColor` | alias of `kPlayheadColor` |
| gui_main.cpp:53 | `kFlagHighlightColor` | {0.30, 0.28, 0.22} |
| gui_main.cpp:54 | `kDirtyColor` | {0.90, 0.65, 0.35} |
| gui_main.cpp:63 | `kRenderViewMarkerColor` | {0.30, 0.45, 0.85} |
| gui_main.cpp:64 | `kRenderViewMarkerSelectedColor` | {0.55, 0.78, 1.00} |
| gui_main.cpp:72 | `kTransientColor` | {0.85, 0.45, 0.95} |
| gui_main.cpp:73 | `kTransientDimColor` | {0.42, 0.22, 0.47} |
| gui_main.cpp:74 | `kTransientSelectedColor` | {0.95, 0.65, 1.00} |
| gui_main.cpp:75 | `kTransientHighlightColor` | {0.40, 0.20, 0.45} |
| gui_main.cpp:76 | `kTransientPlayheadColor` | alias of `kPlayheadColor` |
| gui_main.cpp:82 | `kDialogTextColor` | alias of `kMarkerColor` |
| gui_main.cpp:83 | `kDialogPanelColor` | alias of `kWaveformDimColor` |
| gui_main.cpp:84 | `kDialogButtonColor` | alias of `kFlagHighlightColor` |
| gui_main.cpp:85 | `kDialogFocusColor` | {0.55, 0.45, 0.20} |

### Per-call-site mapping table

Proposed-new-color column uses the brief's vocabulary: `kBackground`,
`kWaveform`, `kMarker`, `kPlayhead`, `kAccent`, `kText`, and
`dim(c)` for the derivation. Ambiguous mappings are flagged with **AMB**.

| File:line | Constant consumed | Render site | Proposed new color |
|---|---|---|---|
| gui_main.cpp:1512 | `kWaveformColor`, `kWaveformDimColor` | `render_waveform` (mono) — bright (in trim) and dim (out of trim) waveform fill | `kWaveform`, `dim(kWaveform)` |
| gui_main.cpp:1521 | `kWaveformColor`, `kWaveformDimColor` | `render_waveform` (stereo channel 0) | `kWaveform`, `dim(kWaveform)` |
| gui_main.cpp:1525 | `kWaveformColor`, `kWaveformDimColor` | `render_waveform` (stereo channel 1) | `kWaveform`, `dim(kWaveform)` |
| gui_main.cpp:1568 | `kRenderViewMarkerColor` | `render_markers` enabled_color (render-view branch) | `kAccent` (**AMB** — render-view today is hue-distinct from authoring; under the new palette render-view markers either reuse `kMarker` or get repurposed onto `kAccent`. Architect must decide.) |
| gui_main.cpp:1569 | `kRenderViewMarkerColor` | `render_markers` disabled_color (render-view branch) | same as above; under unified rule disabled = `dim(<base>)`. **AMB** |
| gui_main.cpp:1570 | `kRenderViewMarkerSelectedColor` | `render_markers` selected_color (render-view branch) | `kPlayhead` (selection in authoring is yellow today; render-view's sky-tint should collapse to the same selected color under the new palette). **AMB** if architect wants render-view to remain visually distinct. |
| gui_main.cpp:1577 | `kTransientColor` | `render_transient_markers` enabled_color | `kMarker` (**AMB** — magenta-vs-yellow distinction collapses; verify architect intent) |
| gui_main.cpp:1577 | `kTransientDimColor` | `render_transient_markers` disabled_color | `dim(kMarker)` |
| gui_main.cpp:1578 | `kTransientSelectedColor` | `render_transient_markers` selected_color | `kPlayhead` (selection unifies across modes) |
| gui_main.cpp:1584 | `kMarkerColor` | `render_markers` enabled_color (warp branch) | `kMarker` |
| gui_main.cpp:1584 | `kMarkerDimColor` | `render_markers` disabled_color (warp branch) | `dim(kMarker)` |
| gui_main.cpp:1585 | `kSelectedColor` | `render_markers` selected_color (warp branch) | `kPlayhead` |
| gui_main.cpp:1602–1605 | `kRenderViewMarkerColor`, `kTransientPlayheadColor`, `kPlayheadColor` | `render_playhead` color selection per active mode | `kPlayhead` for warp/transient; **AMB** render-view (currently uses dark blue marker color so the playhead doesn't clash with the read-only marker hue — collapsing to `kPlayhead` would re-introduce the very clash the existing code avoids) |
| gui_main.cpp:1623–1626 | `kRenderViewMarkerColor` (×2), `kRenderViewMarkerSelectedColor`, `kFlagHighlightColor` | `render_flags` (render-view) — enabled / disabled / selected / highlight | enabled+disabled→**AMB** (see 1568–1570); selected→`kPlayhead`; highlight→**Three-state model**: highlight is the selected-bg fill which under the new rule is `kMarker` (the three-state spec uses `kMarker` for the selected background fill regardless of which mode is active) |
| gui_main.cpp:1670 | `kRenderViewMarkerColor` | `gui_text_display::State::color` for render-view hover popup text | `kText` (popup text under three-state rule is plain text). **AMB** if architect wants render-view popups to remain hue-distinct. |
| gui_main.cpp:1682 | `kTransientColor` | `render_transient_flags` enabled_color | `kText` (flag text not selected → `kText` per three-state rule) |
| gui_main.cpp:1682 | `kTransientDimColor` | `render_transient_flags` disabled_color | `dim(kText)` |
| gui_main.cpp:1683 | `kTransientSelectedColor` | `render_transient_flags` selected_color | `kText` (selected text on `kMarker` fill is still `kText`) |
| gui_main.cpp:1683 | `kTransientHighlightColor` | `render_transient_flags` highlight_color (last-selected bg fill) | `kMarker` |
| gui_main.cpp:1713 | `kMarkerColor` | `render_flags` enabled_color (warp) | `kText` |
| gui_main.cpp:1713 | `kMarkerDimColor` | `render_flags` disabled_color (warp) | `dim(kText)` |
| gui_main.cpp:1714 | `kSelectedColor` | `render_flags` selected_color (warp) | `kText` (under three-state, selected text is still `kText` — the `kMarker` fill underneath communicates selection, not a color shift on the glyphs) |
| gui_main.cpp:1714 | `kFlagHighlightColor` | `render_flags` highlight_color (warp last-selected bg fill) | `kMarker` |
| gui_main.cpp:1760 | `kMarkerColor` | `gui_text_display::State::color` for warp hover popup text | `kText` |
| gui_main.cpp:1831–1834 | inline `GuiColor{0.75, 0.20, 0.18}` (parse-fail red) and `kFlagHighlightColor` | iter popup edit-state bg rect | `kAccent` for parse-fail, `kMarker` for normal-edit fill |
| gui_main.cpp:1853 | `kMarkerColor` (`.r/.g/.b`) | iter popup edit-state pending text fill | `kText` |
| gui_main.cpp:1881 | `kMarkerColor` | iter popup non-edit text (normal popup text) | `kText` |
| gui_main.cpp:1920 | `kTimestampColor` | `render_timestamp` (bottom strip MM:SS.mmm) | `kText` |
| gui_main.cpp:1939–1940 | `kTimestampColor` (`.r/.g/.b`) | A/B tab letter glyph in bottom strip | `kText` |
| gui_main.cpp:1959 | `kDirtyColor` | `render_dirty_indicator` (bottom strip dot) | `kAccent` (**AMB** — under new palette, "unsaved" warning could stay on `kAccent` for visibility, or move to a derivation. The brief lists no dedicated dirty color so default to `kAccent`.) |
| gui_main.cpp:1980–1982 | `kRenderViewMarkerColor` (`.r/.g/.b`) | render-view filename mnemonic in bottom strip (right-aligned) | `kText` (**AMB** — currently dark blue to mirror the read-only mode visual; the new palette has no mode-specific text color) |
| gui_main.cpp:2011–2012 | `kDialogTextColor`, `kDialogPanelColor`, `kDialogButtonColor`, `kDialogFocusColor` | `render_dialog` paint (text / panel bg / button bg / focused-button bg) | **N/A — dialog is being eliminated entirely.** All four constants and their aliasing definitions (gui_main.cpp:82–85) become dead code. |

### Out-of-tree color references

These are not consumption sites of the named `k…Color` constants, but
they are direct RGB/RGBA literals that paint window chrome and would
plausibly be candidates for the new palette substitution:

| File:line | Literal | Render site | Proposed new color |
|---|---|---|---|
| gui_render.cpp:117 | `0.10, 0.10, 0.12` | `render_background` (window-wide background fill, painted before everything else) | `kBackground` (this is the canonical site for `kBackground` itself — a literal here today) |
| gui_render.cpp:130 | `0.35, 0.35, 0.40` | `render_progress_bar` (loading progress bar fill) | `dim(kText)` or **AMB** — the brief doesn't specify a progress-bar treatment. |
| gui_render.cpp:973 | `0.0, 0.0, 0.0` (rgba @ 0.55) | `render_dialog` dim overlay | **N/A — dialog removed.** |
| gui_render.h:115 | `0.75, 0.20, 0.18` (default of `FlagEditorOverlay::red_color`) | parse-failure background fill on flag rect | `kAccent` (the addendum's parse-fail-replaces-fill rule maps directly here) |
| gui_main.cpp:1833 | inline `GuiColor{0.75, 0.20, 0.18}` (iter popup edit) | parse-failure background fill on iter popup edit | `kAccent` (same rule, second site — note the duplication: this literal and the FlagEditorOverlay default are the same red value, separately encoded) |

### Ambiguities flagged

1. **Render-view marker color collapse.** `kRenderViewMarkerColor` and
   `kRenderViewMarkerSelectedColor` exist specifically to keep the
   read-only render-view mode hue-distinct from authoring. The new
   palette has no concept of mode-keyed color, so collapsing them
   removes that visual cue. Architect should decide whether render-view
   reuses authoring colors directly, or whether one of the bases (e.g.
   `kAccent`) gets repurposed as "render-view tint."

2. **Transient mode color collapse.** Same shape as render-view: the
   magenta family is hue-distinct from warp yellow on purpose. Brief
   does not specify whether transient mode keeps a distinguishing color
   or merges with warp.

3. **Render-view playhead.** Today the playhead in render-view paints
   in `kRenderViewMarkerColor` (dark blue) — this was a deliberate
   inline override at gui_main.cpp:1602–1605 to avoid clashing with
   warp yellow. Under the new palette, every playhead becomes
   `kPlayhead`, but the rationale for the override would need to be
   revisited (would `kPlayhead` and the new render-view marker color
   still be visually distinct?).

4. **Dirty indicator.** `kDirtyColor` (orange) is unique to the bottom
   strip and conveys "unsaved changes." Brief lists no dirty-specific
   base. Default mapping is `kAccent`, but architect may want a
   dedicated treatment.

5. **Selected text color.** Brief specifies "selected without editor: `kMarker`
   background, `kText` text." This means `render_flags` and
   `render_transient_flags` no longer take a `selected_color` parameter
   distinct from the enabled color — text always paints `kText`. The
   call signature itself changes; this is a structural simplification
   the implementation brief should plan for, not just a swap.

6. **Selected marker stem color.** `render_markers` and
   `render_transient_markers` currently paint the *stem* in
   `selected_color` (yellow). Brief's three-state rule applies to "flag
   and popup text boxes" — does the same rule apply to marker stems,
   or do stems keep `selected_color`? The brief does not address stems
   directly. Stems aren't text, so the three-state model doesn't apply
   — but if `kSelectedColor` is removed in favor of the
   `kMarker`-fill-on-selected rule, the stem needs some color when
   selected. Default mapping is `kPlayhead` but the architect should confirm.

## Section 2: Flag and popup rendering sites

### `render_flags` definition and call sites

- **Definition:** `gui_render.cpp:436` (declaration `gui_render.h:127`).
  Signature takes `enabled_color`, `disabled_color`, `selected_color`,
  `highlight_color`, plus the `FlagEditorOverlay` for the V.A1 editor
  state. Internal logic at gui_render.cpp:495 selects between drawing
  the editor's red bg, the highlight bg (last-selected), or no bg;
  text color logic at gui_render.cpp:514–518 picks
  selected/disabled/enabled and applies the inline `*0.70` darkening
  for selected-and-disabled.
- **Call site (warp authoring):** gui_main.cpp:1711.
- **Call site (render-view):** gui_main.cpp:1621.

Currently renders: text + optional bg rect (highlight on last-selected,
red on parse-fail editor) + optional 1-px cursor (when editor active
and `cursor_visible`). Has out-of-trim awareness: **no.** Has color
logic that won't fit the new three-state model: **yes** — the
selected-and-disabled `*0.70` darkening at gui_render.cpp:517 is a hue
shift of `selected_color`; under the new model selected paints
`kText` on `kMarker` fill regardless of disabled state, with `dim()`
applied uniformly only on the out-of-trim predicate.

### `render_transient_flags` definition and call sites

- **Definition:** `gui_render.cpp:738` (declaration `gui_render.h:173`).
  Same signature shape as `render_flags` minus the `FlagEditorOverlay`
  — transients have no in-place flag editor.
- **Call site:** gui_main.cpp:1679.

Currently renders: text + optional last-selected bg rect. Has
out-of-trim awareness: **no.** Has color logic that won't fit: **yes**
— same selected-and-disabled `*0.70` darkening at gui_render.cpp:789.

### Hover popup rendering (V.A3b)

- **Render-view branch:** gui_main.cpp:1638–1677. Anchors popup off
  `compute_flag_hit_rects` and paints via `gui_text_display::render`
  with `td.color = kRenderViewMarkerColor`.
- **Warp branch:** gui_main.cpp:1726–1767. Same anchor logic, paints
  with `td.color = kMarkerColor`.
- **Helper module:** `gui_text_display::render` lives at
  `src/gui_text_display.cpp:15`. The popup paints text only — no
  background rect, no cursor. Color comes from the `State::color`
  field set by the caller.

Currently renders: text only (no bg, no cursor). The popup is purely
informational — there is no "selected" state and no "editor engaged"
state for hover popups. Out-of-trim awareness: **no.** Color logic
that won't fit: **none directly** — but under the three-state rule,
the popup needs to be re-classified. The brief says hover popups
follow the same three-state rule. A hover popup is by definition
"not selected" (no selection state on a hover surface), so it would
always render text-only with no bg fill — current behavior. The text
color changes from a mode-tinted `kRenderViewMarkerColor` /
`kMarkerColor` to `kText`.

### Iteration bracket popup rendering (V.B)

- **Helper:** `compute_iter_popup_hits` at gui_main.cpp:175 (definition
  spans 175–228). Computes per-popup `flag_rect` + `hit_rect` + `text`.
- **Edit-state paint:** gui_main.cpp:1797–1875. Inline-painted (does
  not go through `gui_text_display::render`). Draws bg rect (red on
  parse-fail per inline `GuiColor{0.75, 0.20, 0.18}` at line 1833,
  else `kFlagHighlightColor`), then pending text in `kMarkerColor`,
  then a 1-px cursor.
- **Non-edit paint:** gui_main.cpp:1876–1886. Goes through
  `gui_text_display::render` with `td.color = kMarkerColor`.
- **Call site (only during iteration mode in warp authoring):**
  gui_main.cpp:1778. Suppressed in render-view (the
  `iteration_mode_enabled` toggle is forced false on entry).

Currently renders: depends on state. Non-edit = text only. Edit = bg
rect + text + cursor. The two paint paths use different code (inline
vs `gui_text_display::render`); under the new three-state model this
divergence becomes a candidate for unification — both states need bg
fill rules now (selected w/o editor = `kMarker` fill; selected w/
editor engaged = same fill + cursor).

Out-of-trim awareness: **no.** Color logic that won't fit: the
duplicate red literal at gui_main.cpp:1833 is the same value as
`FlagEditorOverlay::red_color`'s default at gui_render.h:115.
Implementation should consolidate both into the single
`kAccent`-driven path.

### Top-flag editor overlay (`gui_text_editor` ↔ `render_flags`)

- **Editor state struct:** `gui_text_editor::State` (declared in
  gui_text_editor.h, definition in gui_text_editor.cpp). Stateful:
  target marker index, kind (FlagPayload | IterationBracket), pending
  text, cursor position, red flag, blink epoch.
- **Bridge into render_flags:** gui_main.cpp:1688–1710. When the
  editor's kind is `FlagPayload`, its state is copied into a
  `FlagEditorOverlay` and passed to `render_flags`. When the kind is
  `IterationBracket`, only the `iter_editor_target` is plumbed in (so
  the flag rect's last-selected highlight is suppressed — V.B
  Addendum 2); the actual edit paint happens up at the iteration
  popup paint path described above.
- **Editor paint logic in `render_flags`:** gui_render.cpp:495–544.
  Bg fill chooses between `editor.red_color` (parse-fail) and
  `highlight_color` (normal edit); text uses the same color-pick
  logic as non-edit; cursor is a 1-px vertical bar at byte offset
  `cursor_pos` when `cursor_visible`.

Currently renders: text + bg rect (red on parse-fail, highlight
otherwise) + 1-px cursor. Maps cleanly onto the new three-state model:
"selected with editor engaged" = `kMarker` fill + `kText` text +
blinking cursor; parse-failure = `kAccent` fill + same text/cursor.

Out-of-trim awareness: **no.**

### Other top-strip text

The top strip contains only flags (and their popups). The bottom strip
houses timestamp / tab letter / dirty indicator / render-view filename
— covered in section 4 below.

The playhead's inverted-triangle indicator (`render_playhead` at
gui_render.cpp:259–267) is drawn into the top-strip pixels but is not
text and not flag-related. It paints in the playhead `color`
parameter.

## Section 3: Modal dialog sites

### Dialog state

- **State struct:** `DialogState` at gui_main.cpp:491–498 (`active`,
  `focused_button`, `trigger`, `prompt_text`, `button_labels`).
- **Trigger enum:** `DialogTrigger` at gui_main.cpp:480–484. Three
  values: `CLOSE_WINDOW`, `REVERT_TO_BLANK`, `DETECT_TRANSIENTS`.
- **Live state instance:** `app.dialog` (member of `AppState`).

### Dialog rendering

- **Layout helper:** `compute_dialog_layout` at gui_render.cpp:906–957
  (declared gui_render.h:240).
- **Paint helper:** `render_dialog` at gui_render.cpp:959–1027
  (declared gui_render.h:249).
- **Call site:** gui_main.cpp:2002–2013 (inside the redraw lambda,
  painted last so it overlays everything).

### Dialog input handling

- **Keyboard:** gui_main.cpp:5371–5406. Top-of-`set_on_key` filter:
  `if (app.dialog.active)` block consumes Escape (= last button →
  Cancel by convention), Enter / KP_Enter / Space (= focused button),
  Left / Shift+Tab / ISO_Left_Tab (= focus prev), Right / Tab (= focus
  next); all other keys are swallowed.
- **Mouse:** gui_main.cpp:6131–6152 (button press), gui_main.cpp:6498
  (button release returns early), gui_main.cpp:6516 (motion clears
  hover popup and returns). Button-press recomputes layout and
  hit-tests each button rect; click outside any button is a no-op.

### Per-dialog details

#### Unsaved-work dialog (CLOSE_WINDOW / REVERT_TO_BLANK)

- **Open site:** `open_unsaved_dialog` lambda at gui_main.cpp:4249–4258.
- **Invocation:** `request_close_or_revert` lambda at
  gui_main.cpp:4321–4325 (gates the trigger on `app.dirty`). Bound to:
  - Ctrl+Q (gui_main.cpp:5510) → `CLOSE_WINDOW`
  - Ctrl+Shift+R (gui_main.cpp:5516) → `REVERT_TO_BLANK`
  - WM-close callback (gui_main.cpp:6121) → `CLOSE_WINDOW`
- **Prompt:** `"Unsaved changes. Save before continuing?"`.
- **Buttons:** `{"Save", "Discard", "Cancel"}` (focus default = 0).
- **Activation handler:** `dialog_activate_button` lambda at
  gui_main.cpp:4282–4316. Save (0) calls `save_markers()` then
  `proceed_with_trigger`; Discard (1) calls `proceed_with_trigger`
  directly; Cancel (2) closes the dialog.
- **Save-failure path:** gui_main.cpp:4298–4302 mutates
  `prompt_text` to `"Save failed."` and re-invalidates without
  closing — the dialog persists in this error state until the user
  picks another button.
- **Keyboard shortcuts:** Esc → Cancel (last button); Enter / KP_Enter
  / Space → focused button; Tab / Right → focus next; Shift+Tab /
  Left / ISO_Left_Tab → focus previous.

#### Re-detect transients dialog (DETECT_TRANSIENTS)

- **Open site:** `open_detect_confirm_dialog` lambda at
  gui_main.cpp:4260–4272.
- **Invocation:** `detect_transients` lambda at gui_main.cpp:4433. The
  dialog only opens when prior detection exists (gui_main.cpp:4438+);
  if no prior detection, `run_detect_now` fires immediately. Bound to
  Ctrl+Alt+T (per the lambda's "Ctrl+Alt+T entry point" comment at
  4430–4432; the actual key dispatch is elsewhere in `set_on_key`).
- **Prompt:** `"Re-detect transients? Existing detection will be
  replaced."`.
- **Buttons:** `{"Detect", "Cancel"}` (focus default = 1, the safe
  Cancel).
- **Activation handler:** same `dialog_activate_button` lambda at
  gui_main.cpp:4282. Trigger-keyed branch at 4285–4292: button 0 →
  close + `proceed_with_trigger` (which calls `run_detect_now`);
  button 1 → close.
- **Keyboard shortcuts:** same as the unsaved-work dialog (Esc →
  Cancel, Enter → focused button, Tab/Shift+Tab → focus cycle).

### Other modal dialogs

**No other dialogs exist.** Only `DialogTrigger` values are
`CLOSE_WINDOW`, `REVERT_TO_BLANK`, `DETECT_TRANSIENTS`. The
welcome-back doc's reference to "transient-detection-already-exists"
maps directly onto the DETECT_TRANSIENTS trigger above. There is no
separate transient-related dialog.

## Section 4: Bottom strip current layout

All bottom-strip rendering is at gui_main.cpp:1895–1996 (inside the
redraw lambda, gated by `rects_intersect(exposed, ts)`).

Layout, left to right:

| Element | File:line | Coordinates | Notes |
|---|---|---|---|
| Timestamp `MM:SS.mmm` | gui_main.cpp:1919 | x = `kTimestampPadX` (=8), baseline y = `app.height - kTimestampBaselineFromBottom` (=12 from bottom) | Rendered via `render_timestamp` (gui_render.cpp:271). Always present. |
| A/B tab letter | gui_main.cpp:1934–1950 | x = padX + timestamp_width + `kTabLetterGapPx` (=10), same baseline y | Single character (`app.active_tab`). Suppressed when `app.render_view_enabled` (line 1933 check). |
| Dirty indicator | gui_main.cpp:1959 | cx = letter_right + `kTabLetterGapPx` + 3, cy = baseline_y - 5 | A 3-px-radius filled circle (`render_dirty_indicator`). Only present when `app.dirty`. |
| Render-view filename | gui_main.cpp:1970–1995 | Right-aligned: rx = `app.width - kTimestampPadX - text_width`, baseline y same | Format: `<batch_folder_name>/<basename>.wav`. Only present when `app.render_view_enabled` and `render_view_index` is in range. |

Bottom-strip total height: `kBottomStripRatio * window_height`
(default 10%, gui_main.cpp:44). Invalidate region:
`timestamp_invalidate_rect` at gui_main.cpp:995–998 returns
`{0, height-30, 200, 30}` — only the left 200 px of the bottom strip
is treated as "the timestamp area" for invalidation purposes. The
right-aligned render-view filename lives outside this rect, which
means a redraw triggered solely by a `timestamp_invalidate_rect`
expose will not repaint the filename. (Noted as ambient observation;
not a brief target.)

The free space available between the dirty indicator (or letter, when
not dirty) and the render-view filename / right edge is the natural
home for the new bottom-strip command-input prompt. No element
currently occupies that center span.

## Section 5: Color helper functions

There is no existing `dim()` function. The current "dim" derivation is
either:

1. **Hand-rolled per-constant Dim variants** (definitions at
   gui_main.cpp:47, 51, 73 — `kWaveformDimColor`, `kMarkerDimColor`,
   `kTransientDimColor`). Each is a separate constant with separate
   RGB values.
2. **Inline `*0.70` darkening at four sites** for the
   selected-and-disabled case:
   - `gui_render.cpp:363` — `render_markers` (warp), selected pass.
   - `gui_render.cpp:517` — `render_flags` (warp), text fill.
   - `gui_render.cpp:723` — `render_transient_markers`, selected pass.
   - `gui_render.cpp:789` — `render_transient_flags`, text fill.

All four `*0.70` sites have the identical body:
`c.r *= 0.70; c.g *= 0.70; c.b *= 0.70;` applied to a local `GuiColor c`
that was just assigned `selected_color`. Under the new scheme they
either disappear (if selection is purely about bg fill, with text
always at `kText`) or fold into the unified `dim(c)` helper.

The hand-rolled Dim variants disappear under the new scheme — every
"dim" rendering goes through `dim(c)` instead of consuming a separate
constant.

## Section 6: Unanticipated findings

### F1 — Two parse-failure red literals, hard-coded separately

The "parse failure" background color appears as the same RGB literal
`{0.75, 0.20, 0.18}` in two places:
- `FlagEditorOverlay::red_color` default at `gui_render.h:115`.
- Inline `GuiColor{0.75, 0.20, 0.18}` at `gui_main.cpp:1833` for the
  iteration popup edit-state bg.

Neither is named in the architect's list of constants. Both should
collapse to a single `kAccent` reference in the implementation. Not
scope creep — they are the parse-fail behavior the brief already
calls out — but the architect's list was incomplete here.

### F2 — `render_progress_bar` paints with a hard-coded grey

`render_progress_bar` at `gui_render.cpp:123–134` uses a literal
`{0.35, 0.35, 0.40}` with no named constant. The progress bar appears
during file load. Brief does not specify a treatment for this. Default
mapping = `dim(kText)` or similar; architect should decide.

### F3 — `render_background` defines `kBackground` implicitly

`render_background` at `gui_render.cpp:115–121` paints
`{0.10, 0.10, 0.12}` directly. There is no named `kBackgroundColor`
constant today — the value lives inline in the renderer. The new
palette has `kBackground` as a base; this line is the canonical site
that becomes the literal value of the new constant.

### F4 — Dialog colors include a non-aliased value

`kDialogFocusColor` at gui_main.cpp:85 (`{0.55, 0.45, 0.20}`) is the
only Dialog color that is *not* an alias of another constant (Text,
Panel, Button all alias `kMarkerColor`/`kWaveformDimColor`/
`kFlagHighlightColor`). Under the dialog-removal plan, this constant
becomes dead code along with the others; noted because the brief did
not flag this asymmetry.

### F5 — Render-view selection is visual-only

`render_view_selected_markers` and
`render_view_last_selected_marker` exist (gui_main.cpp branches at
1571–1572, 1628–1629) and use the same color logic as authoring
selection but against a separate selection set. The render-view UX
explicitly does not commit selection to the underlying file —
selection in render-view is purely a visual cue (highlighting). Under
the new three-state model, render-view selected flags would still
fill `kMarker` and render text in `kText`. The behavioral
distinction between authoring-selected (mutable) and read-only
render-view selected (visual-only) is preserved by the
`render_view_*` field plumbing, not by color.

### F6 — Hover popup uses `gui_text_display::render`; iter popup
**only partially** uses the same helper.

`compute_iter_popup_hits` at gui_main.cpp:175 sets up the hit/flag
geometry, but the actual edit-state paint at gui_main.cpp:1797–1875
inlines its own `cairo_*` calls instead of going through
`gui_text_display::render`. The non-edit branch *does* go through the
helper. This split would be a natural consolidation candidate during
phase 2, since the new three-state rule requires the bg-fill path to
be uniform across "selected" and "selected with editor engaged."

### F7 — Hover popup behavior under the three-state rule is unclear

The brief's three-state rule applies to "flag and popup text boxes."
Hover popups are popups but they have no concept of "selected" — they
appear on dwell, disappear on motion. Under the new rule a hover
popup would always be in state 1 ("not selected — text in `kText`,
no bg"). That collapses cleanly. Iteration popups, by contrast, *do*
have a selected/editor-engaged state and need all three states. The
implementation brief should make this distinction explicit so a
future contributor doesn't try to give hover popups a bg fill.

### F8 — Time budget

The inventory took roughly 90 minutes of focused exploration. No
significant time sink. Section 6 surfaced eight findings (F1–F8) but
none of them rise to "scope creep that needs another inventory pass"
— they're clarifications and consolidations the architect can fold
into brief H directly.

## Section 7: Out-of-trim dimming consolidation

### Trim predicate

`compute_trim_samples` at `gui_main.cpp:834–882` is the single source
of truth for the `[trim_begin, trim_end]` boundary, computed from
`b=` / `e=` flags on warp markers and transients. The redraw path
calls it once per frame at `gui_main.cpp:1455–1463` (with a render-view
vs. authoring-mode branch) and stores the resulting `trim_begin` /
`trim_end` int64_t pair for that frame.

Predicate per the brief's notation:
```
out_of_trim(marker) = (marker.time_seconds * sample_rate < trim_begin)
                    || (marker.time_seconds * sample_rate >= trim_end)
```

For warp markers the predicate is identical across all rendering
sites — `time_seconds * sample_rate` is the canonical source-frame
of the marker.

For transients the marker position is `effective_frame()` (an int64_t
already in source frames), not `time_seconds * sample_rate`. The
predicate body is structurally identical but reads from a different
field. Both expressions reduce to "is the marker's source-frame
position inside `[trim_begin, trim_end)`?" — the comparison logic
itself is uniform.

### Sites currently consuming the in-trim/out-of-trim distinction

The trim distinction is currently applied to **the waveform fill only**.

- **`render_waveform`** at `gui_render.cpp:136–234` takes
  `trim_begin_sample` / `trim_end_sample` and switches between
  `bright_color` and `dim_color` per pixel column based on the
  midpoint of that column's source-sample range
  (gui_render.cpp:208–210). Out-of-trim → dim.

That is the only place the trim boundary affects rendering today.

### Sites that do NOT currently consume the trim distinction (but need to under the new rule)

| Site | File:line | Currently has trim awareness? |
|---|---|---|
| `render_markers` (warp marker stems) | gui_render.cpp:302–376 | No |
| `render_transient_markers` | gui_render.cpp:668–736 | No |
| `render_flags` (warp flag text + bg + cursor) | gui_render.cpp:436–550 | No |
| `render_transient_flags` (transient flag text + bg) | gui_render.cpp:738–798 | No |
| Iter popup edit-state paint | gui_main.cpp:1797–1875 (inline) | No |
| Iter popup non-edit paint | gui_main.cpp:1876–1886 (`gui_text_display::render`) | No |
| Hover popup paint (warp) | gui_main.cpp:1726–1767 | No |
| Hover popup paint (render-view) | gui_main.cpp:1638–1677 | No |

### API plumbing required

**Yes, plumbing required at every site above.** None of the marker /
flag / popup renderers currently receive `trim_begin` / `trim_end`.
The implementation brief must add either:

(a) Two new parameters (`trim_begin_sample`, `trim_end_sample`) to the
five public renderer signatures
(`render_markers`, `render_transient_markers`, `render_flags`,
`render_transient_flags`, plus the popup paint paths in gui_main.cpp);
or

(b) A small struct (e.g. `TrimRange { int64_t begin; int64_t end; }`)
threaded through the same call sites.

Both `compute_iter_popup_hits` and `compute_flag_hit_rects` currently
do not need trim awareness (they are pure geometry), but the iter
popup paint code at gui_main.cpp:1797–1888 would need the trim
boundary to apply `dim()` to the popup's bg/text/cursor when its
underlying marker is out of trim.

### Predicate-uniformity check

The predicate "is this marker in or out of trim" expresses identically
across **warp marker** sites (using `time_seconds * sample_rate`).
Transient sites use `effective_frame()` instead of
`time_seconds * sample_rate` — the underlying representation differs
because `GuiTransient` stores frames directly while `GuiMarker`
stores seconds. The implementation brief can either:

- Plumb a small inline helper that takes `(int64_t pos_in_source_frames,
  int64_t trim_begin, int64_t trim_end)` and have each renderer
  compute `pos_in_source_frames` from its own native field; or
- Split into two helpers (`marker_out_of_trim` and
  `transient_out_of_trim`) that each take the right native type.

Either form keeps the comparison logic identical; the only friction
is the differing native fields on `GuiMarker` vs `GuiTransient`.

### Interactions to watch for

- **Selected-and-disabled hue shift** at gui_render.cpp:363, 517,
  723, 789 (the `*0.70` sites). Under the unified rule, "disabled"
  is no longer modeled as a hue shift on `selected_color`; instead,
  out-of-trim drives a uniform `dim()` and the existing
  effective_disabled (label-cascade) and `m.disabled` predicates
  drive a different (or possibly combined) treatment. The
  implementation brief should clarify whether out-of-trim and
  disabled stack (e.g. an out-of-trim and disabled marker:
  `dim(dim(c))`?) or supersede each other.

- **Render-view dimming**. Render-view's
  `compute_trim_samples` call at gui_main.cpp:1456–1458 always
  returns `[0, total_frames]` (the markers list collapses b=/e=),
  so out-of-trim is trivially never true in render-view. Applying
  the new uniform rule there is a no-op — good — but the implementation
  shouldn't accidentally introduce dimming where none existed.

- **Hover popup over an out-of-trim marker**. Currently rendered the
  same as in-trim. Under the new rule the popup text would render
  through `dim(kText)`. This is the brief's intent
  ("every visual element belonging to that marker") but worth
  noting — the hover popup is already a low-emphasis affordance, so
  dimming it further may make it unreadable. Architect should
  confirm.

- **Iter popup over an out-of-trim marker**. Same concern as the
  hover popup, with the additional wrinkle that the iter popup has
  three states. Under the new rule the bg fill (when selected) and
  the text and the cursor (when editor engaged) all dim. The
  parse-failure `kAccent` fill also dims. This is implementable
  without surprise, but the visual end result on an out-of-trim
  parse-failure cursor-blinking iter popup needs to be considered
  for legibility.

### Implementability summary

The unified rule is implementable. The only structural change is
plumbing trim boundaries through the five renderer signatures + the
two inline popup paint paths. No site has color flow that would fight
the `dim()` derivation — the existing
`*0.70`-darkening code is the closest analog, and it is being
removed anyway.

Open question for the architect: does **disabled** (label-cascade or
locally-set-disabled) also dim under the new rule, or is dimming
strictly tied to out-of-trim? Under the current code, disabled is
the only path to dim coloring on markers. Under the brief, the
out-of-trim predicate replaces or supplements disabled. Resolving
this is required before brief H can specify the final per-site
mapping.
