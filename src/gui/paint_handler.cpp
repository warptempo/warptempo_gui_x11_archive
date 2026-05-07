#include "paint_handler.h"

#include "render.h"
#include "text_display.h"
#include "text_editor.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// X.7.8a: paint cluster. Method bodies are byte-identical to the lambdas
// they replaced in main.cpp (set_on_redraw at the original main.cpp:999
// and set_on_resize at the original main.cpp:1892). The only changes are:
//
//   - Capture-by-reference of `app`, `audio`, `playback`, `wf_cache`,
//     `gui` is now reference-member access on `this`. Identifier spelling
//     is identical so nothing else changes inside the bodies.
//   - `bottom_strip_wide()` (the old lambda capture) is replaced with the
//     free-function form `bottom_strip_wide(app)` declared in app_state.h.
//
// IterPopupHit / BpmPopupHit and the compute_*_popup_hits helpers below
// were also extracted out of main.cpp's anonymous namespace because paint
// uses them and other (non-paint) main.cpp callsites reach them through
// the same paint_handler.h include.

// -- compute_iter_popup_hits / compute_bpm_popup_hits --------------------
//
// Bodies copied verbatim from the original main.cpp anonymous-namespace
// definitions; the only change is removing `inline` (these now have
// external linkage as paint_handler.cpp is their sole TU of definition).

std::vector<IterPopupHit> compute_iter_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<IterPopupHit> out;
    auto rects = compute_flag_hit_rects(
        cr, top_strip_area, markers,
        viewport_start_sample, viewport_end_sample,
        sample_rate, font_size);
    if (rects.empty()) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    // The widest possible iteration text drives a uniform hit-rect width
    // so popups don't visibly jiggle in size as values change. Matches
    // the [%+0.2f,%+0.2f] format with single-digit integer parts.
    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "[+0.00,+0.00]", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    // Greedy left-to-right elision over popup positions. Brief Y.4 sub-bug
    // B: collision is computed against the popup's actual painted-text
    // width plus 2 * kFlagInnerPadPx — i.e., the on-screen extent of the
    // bg-fill rect, not the uniform [+0.00,+0.00] hit_rect.w. The hit_rect
    // stays uniform-width so click targets are stable as values change;
    // pack and paint are separate concerns. With this rule, two adjacent
    // owning markers whose painted popup texts (e.g. "[ ]") don't actually
    // overlap will both render, even if their uniform hit rects do — which
    // matches the flag pack in iterate_visible_flags. No editor exemption.
    const double pop_pad = 4.0;
    double rightmost_right_edge = -1e18;
    for (const auto& r : rects) {
        const int idx = r.marker_index;
        if (idx < 0 || idx >= static_cast<int>(markers.size())) continue;
        if (!iter_popup_eligible_marker(markers[idx])) continue;
        IterPopupHit h;
        h.marker_index = idx;
        h.flag_rect.x = static_cast<int>(std::lround(r.x));
        h.flag_rect.y = static_cast<int>(std::lround(r.y));
        h.flag_rect.w = static_cast<int>(std::lround(r.w));
        h.flag_rect.h = static_cast<int>(std::lround(r.h));
        h.text = format_iter_bracket_text(markers[idx]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, h.text.c_str(), &ext);
        const int popup_w =
            static_cast<int>(std::ceil(uniform_ext.x_advance + 2 * hl_pad));
        const int popup_h = h.flag_rect.h;
        h.hit_rect.x = h.flag_rect.x;
        h.hit_rect.y = h.flag_rect.y -
            static_cast<int>(std::lround(kIterPopupVerticalGapPx)) -
            popup_h;
        h.hit_rect.w = popup_w;
        h.hit_rect.h = popup_h;

        // Pack collision uses the painted-extent width (matches the bg-
        // fill rect from sub-bug A), not h.hit_rect.w. By construction
        // the pack rule and the visual occlusion rule agree.
        const double pack_w = ext.x_advance + 2.0 * hl_pad;
        const double left = static_cast<double>(h.hit_rect.x);
        if (left < rightmost_right_edge + pop_pad) continue;
        rightmost_right_edge = left + pack_w;
        out.push_back(h);
    }
    cairo_restore(cr);
    return out;
}

std::vector<BpmPopupHit> compute_bpm_popup_hits(
    cairo_t* cr,
    GuiRect top_strip_area,
    const std::vector<GuiWarpMarker>& markers,
    long long viewport_start_sample,
    long long viewport_end_sample,
    int sample_rate,
    double font_size) {
    std::vector<BpmPopupHit> out;
    auto rects = compute_flag_hit_rects(
        cr, top_strip_area, markers,
        viewport_start_sample, viewport_end_sample,
        sample_rate, font_size);
    if (rects.empty()) return out;

    cairo_save(cr);
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);
    cairo_text_extents_t uniform_ext;
    cairo_text_extents(cr, "99@[999,999]", &uniform_ext);
    const double hl_pad = kFlagInnerPadPx;

    const double pop_pad = 4.0;
    double rightmost_right_edge = -1e18;
    for (const auto& r : rects) {
        const int idx = r.marker_index;
        if (idx < 0 || idx >= static_cast<int>(markers.size())) continue;
        if (!bpm_popup_eligible_marker(markers[idx])) continue;
        if (!markers[idx].bpm_is_popup_owner) continue;
        BpmPopupHit h;
        h.marker_index = idx;
        h.flag_rect.x = static_cast<int>(std::lround(r.x));
        h.flag_rect.y = static_cast<int>(std::lround(r.y));
        h.flag_rect.w = static_cast<int>(std::lround(r.w));
        h.flag_rect.h = static_cast<int>(std::lround(r.h));
        h.text = format_bpm_bracket_text(markers[idx]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, h.text.c_str(), &ext);
        const int popup_w =
            static_cast<int>(std::ceil(uniform_ext.x_advance + 2 * hl_pad));
        const int popup_h = h.flag_rect.h;
        h.hit_rect.x = h.flag_rect.x;
        h.hit_rect.y = h.flag_rect.y -
            static_cast<int>(std::lround(kIterPopupVerticalGapPx)) -
            popup_h;
        h.hit_rect.w = popup_w;
        h.hit_rect.h = popup_h;

        const double pack_w = ext.x_advance + 2.0 * hl_pad;
        const double left = static_cast<double>(h.hit_rect.x);
        if (left < rightmost_right_edge + pop_pad) continue;
        rightmost_right_edge = left + pack_w;
        out.push_back(h);
    }
    cairo_restore(cr);
    return out;
}

// -- GuiPaintHandler::on_redraw ------------------------------------------

void GuiPaintHandler::on_redraw(cairo_t* cr, int x, int y, int w, int h) {
    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();

    if constexpr (kDebugPerf) perf_counters::reset();

    double t_waveform_ms = 0.0;
    double t_markers_ms  = 0.0;
    double t_flags_ms    = 0.0;
    double t_playhead_ms = 0.0;
    double t_ts_ms       = 0.0;
    double t_dirty_ms    = 0.0;
    double t_flush_ms    = 0.0;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);

    render_background(cr, x, y, w, h);

    if (app.loading) {
        const int bar_y = app.height - kProgressBarHeight;
        render_progress_bar(cr, 0, bar_y, app.width, kProgressBarHeight,
                            app.load_progress);
    } else if (audio.total_frames() > 0) {
        const GuiRect area       = waveform_area(app);
        const GuiRect top_strip  = top_strip_area(app);
        const GuiRect exposed{x, y, w, h};
        const double  spp        = current_samples_per_pixel(app, audio);
        const int64_t vp_start   = app.viewport_start_sample;
        const int64_t vp_end     = vp_start +
            static_cast<int64_t>(std::llround(spp * area.w));
        const int     sr         = audio.sample_rate();

        // In render-view the audio buffer is already render-domain
        // (trim already baked in at render time, b=/e= flags stripped
        // from the .renderwarpmarkers/.rendertransientmarkers
        // sidecars). The
        // source's authoring markers carry b=/e= in source-frame
        // coordinates that don't map onto the rendered audio's
        // timeline, so feeding them to compute_trim_samples here
        // produces a patchy color split. Use the render-view markers
        // instead, which collapse to [0, total_frames] and dim
        // nothing.
        const auto trim = app.render_view_enabled
            ? compute_trim_samples(
                  app.render_view_markers, app.render_view_transients,
                  sr, audio.total_frames())
            : compute_trim_samples(
                  app.warpmarkers.markers(), app.transientmarkers.markers(),
                  sr, audio.total_frames());
        const int64_t trim_begin = trim.first;
        const int64_t trim_end   = trim.second;
        const TrimRange trim_struct{trim_begin, trim_end};

        const int rc = audio.render_channels();
        {
            const auto wf0 = clock::now();

            // Cache surface lifecycle: (re)create when dimensions don't
            // match the current waveform area. Size mismatch implies a
            // window resize; content is stale regardless.
            if (!wf_cache.surface ||
                wf_cache.width  != area.w ||
                wf_cache.height != area.h) {
                wf_cache.destroy_surface();
                if (area.w > 0 && area.h > 0) {
                    wf_cache.surface = cairo_image_surface_create(
                        CAIRO_FORMAT_ARGB32, area.w, area.h);
                    wf_cache.width  = area.w;
                    wf_cache.height = area.h;
                    wf_cache.dirty  = true;
                }
            }

            // Cache invalidation: any change to the inputs of
            // render_waveform forces a re-render. Checked here (not at
            // mutation sites) so new mutation paths can never forget.
            if (wf_cache.surface &&
                (wf_cache.fp_audio_gen  != app.audio_generation ||
                 wf_cache.fp_vp_start   != vp_start             ||
                 wf_cache.fp_vp_end     != vp_end               ||
                 wf_cache.fp_trim_begin != trim_begin           ||
                 wf_cache.fp_trim_end   != trim_end             ||
                 wf_cache.fp_area_w     != area.w               ||
                 wf_cache.fp_area_h     != area.h)) {
                wf_cache.dirty = true;
            }

            if (wf_cache.surface && wf_cache.dirty) {
                cairo_t* ccr = cairo_create(wf_cache.surface);
                // Clear to transparent — the pixmap's background fill
                // shows through wherever the waveform strokes don't paint.
                cairo_save(ccr);
                cairo_set_operator(ccr, CAIRO_OPERATOR_CLEAR);
                cairo_paint(ccr);
                cairo_restore(ccr);
                const GuiRect cache_area{0, 0, area.w, area.h};
                if (rc == 1) {
                    render_waveform(ccr, cache_area, audio, 0,
                                    vp_start, vp_end,
                                    trim_begin, trim_end,
                                    kWaveform, dim(kWaveform));
                } else if (rc >= 2) {
                    const int ch_h = (cache_area.h - kChannelGapPx) / 2;
                    const GuiRect ch0{0, 0, cache_area.w, ch_h};
                    const GuiRect ch1{0, ch_h + kChannelGapPx,
                                      cache_area.w, ch_h};
                    render_waveform(ccr, ch0, audio, 0,
                                    vp_start, vp_end,
                                    trim_begin, trim_end,
                                    kWaveform, dim(kWaveform));
                    render_waveform(ccr, ch1, audio, 1,
                                    vp_start, vp_end,
                                    trim_begin, trim_end,
                                    kWaveform, dim(kWaveform));
                }
                cairo_destroy(ccr);
                wf_cache.fp_audio_gen  = app.audio_generation;
                wf_cache.fp_vp_start   = vp_start;
                wf_cache.fp_vp_end     = vp_end;
                wf_cache.fp_trim_begin = trim_begin;
                wf_cache.fp_trim_end   = trim_end;
                wf_cache.fp_area_w     = area.w;
                wf_cache.fp_area_h     = area.h;
                wf_cache.dirty = false;
            }

            // Blit the cache into the pixmap, clipped to the exposed
            // rect's intersection with the waveform area. Cairo handles
            // the intersection via the outer clip plus this inner clip.
            if (wf_cache.surface && rects_intersect(exposed, area)) {
                cairo_save(cr);
                cairo_rectangle(cr, area.x, area.y, area.w, area.h);
                cairo_clip(cr);
                cairo_set_source_surface(cr, wf_cache.surface,
                                         area.x, area.y);
                cairo_paint(cr);
                cairo_restore(cr);
            }

            const auto wf1 = clock::now();
            t_waveform_ms =
                std::chrono::duration<double, std::milli>(wf1 - wf0).count();
        }

        // Markers: vertical lines in the waveform area, beneath the
        // playhead. Cairo's outer clip confines painting to `exposed`.
        if (rects_intersect(exposed, area) ||
            rects_intersect(exposed, top_strip)) {
            const auto m0 = clock::now();
            if (app.render_view_enabled) {
                // Render-view: dark blue base, sky-tint when selected.
                // The render's warpmarkers list is strict-monotonic on
                // time_seconds (engine-written), so render_markers'
                // usual ordering assumption holds. Selection is
                // visual-only — it does not flow into commit.
                // Brief F Section 3: when sub-mode is 'T', paint the
                // render's transient list using the transient renderer
                // (matches source-view's transient appearance).
                if (app.active_mode == 'T') {
                    render_transient_markers(
                        cr, area, app.render_view_transients,
                        vp_start, vp_end, sr,
                        trim_struct);
                } else {
                    render_markers(cr, area, app.render_view_markers,
                                   vp_start, vp_end, sr,
                                   trim_struct);
                }
            } else if (app.active_mode == 'T') {
                render_transient_markers(
                    cr, area, app.transientmarkers.markers(),
                    vp_start, vp_end, sr,
                    trim_struct);
            } else {
                render_markers(cr, area, app.warpmarkers.markers(),
                               vp_start, vp_end, sr,
                               trim_struct);
            }
            const auto m1 = clock::now();
            t_markers_ms =
                std::chrono::duration<double, std::milli>(m1 - m0).count();
        }

        // Brief E: precompute the playhead's pixel column so flag
        // renderers can light the outline of the marker the playhead
        // sits on. Same value is reused by render_playhead below.
        const double px_x = playhead_pixel_x(app, audio);

        // Flag annotations in the top strip.
        if (rects_intersect(exposed, top_strip)) {
            const auto f0 = clock::now();
            if (app.render_view_enabled) {
                // Render-view: dark-blue flags, no editor overlay.
                // Selection is visual-only (sky-tint on selected,
                // dark-blue otherwise). Iteration popups are
                // suppressed by the iteration_mode_enabled toggle
                // being forced false on entry to render-view.
                // Sub-mode 'T' (transients): paint via
                // render_transient_flags from app.render_view_transients
                // (no popups; transient markers are not popup-eligible).
                if (app.active_mode != 'T') {
                render_flags(cr, top_strip, app.render_view_markers,
                             vp_start, vp_end, sr,
                             kFlagFontSize,
                             app.selected_markers,
                             trim_struct,
                             px_x,
                             FlagEditorOverlay{});

                // V.A3b hover popup paint, render-view variant.
                // Mirrors the source-view branch below but reads
                // from app.render_view_markers and uses the cached
                // source sample rate (the render's audio sr is
                // typically equal but the brief specifies source-
                // axis presentation).
                if (app.hover_popup.visible) {
                    const auto& mv = app.render_view_markers;
                    const int hidx = app.hover_popup.marker_index;
                    const bool eligible =
                        (hidx >= 0 &&
                         hidx < static_cast<int>(mv.size()) &&
                         (mv[hidx].tempo_inherits ||
                          !mv[hidx].label_ref.empty()) &&
                         !app.hover_popup.cached_text.empty());
                    if (eligible) {
                        auto rects = compute_flag_hit_rects(
                            cr, top_strip, mv,
                            vp_start, vp_end, sr, kFlagFontSize);
                        GuiRect anchor{0, 0, 0, 0};
                        for (const auto& r : rects) {
                            if (r.marker_index == hidx) {
                                anchor.x = static_cast<int>(
                                    std::lround(r.x)) +
                                    static_cast<int>(kFlagInnerPadPx);
                                anchor.y = static_cast<int>(
                                    std::lround(r.y));
                                anchor.w = static_cast<int>(
                                    std::lround(r.w));
                                anchor.h = static_cast<int>(
                                    std::lround(r.h));
                                break;
                            }
                        }
                        if (anchor.w > 0 && anchor.h > 0) {
                            const int64_t pos = static_cast<int64_t>(
                                std::llround(
                                    mv[hidx].time_seconds *
                                    static_cast<double>(sr)));
                            const bool oot =
                                marker_out_of_trim(pos, trim_struct);
                            text_display::State td;
                            td.anchor   = anchor;
                            td.content  = app.hover_popup.cached_text;
                            td.visible  = true;
                            td.color    = oot ? dim(kText) : kText;
                            td.position =
                                text_display::Position::Top;
                            text_display::render(cr, td,
                                                     kFlagFontSize);
                        }
                    }
                }
                } else {
                    render_transient_flags(
                        cr, top_strip, app.render_view_transients,
                        vp_start, vp_end, sr,
                        kFlagFontSize,
                        app.selected_markers,
                        trim_struct,
                        px_x);
                }
            } else if (app.active_mode == 'T') {
                render_transient_flags(
                    cr, top_strip, app.transientmarkers.markers(),
                    vp_start, vp_end, sr,
                    kFlagFontSize,
                    app.selected_markers,
                    trim_struct,
                    px_x);
            } else {
                FlagEditorOverlay overlay;
                // Only the V.A1 FlagPayload kind paints into the flag
                // rect; the V.B IterationBracket kind owns the popup
                // above the rect and leaves the flag's normal text
                // alone. When the iter popup is the focused editor
                // target, the flag rect below must suppress its
                // last-selected highlight (V.B Addendum 2).
                if (text_editor::is_active(app.top_flag_editor) &&
                    app.top_flag_editor.kind ==
                        text_editor::Kind::FlagPayload) {
                    overlay.marker_index   = app.top_flag_editor.target;
                    overlay.pending        = app.top_flag_editor.pending;
                    overlay.cursor_pos     = app.top_flag_editor.cursor_pos;
                    overlay.is_red         = app.top_flag_editor.red;
                    overlay.cursor_visible =
                        text_editor::cursor_visible_now(
                            app.top_flag_editor);
                    overlay.has_selection =
                        text_editor::has_selection(
                            app.top_flag_editor);
                    overlay.selection_start =
                        text_editor::selection_start(
                            app.top_flag_editor);
                    overlay.selection_end =
                        text_editor::selection_end(
                            app.top_flag_editor);
                } else if (text_editor::is_active(app.top_flag_editor) &&
                           app.top_flag_editor.kind ==
                               text_editor::Kind::IterationBracket) {
                    overlay.popup_editor_target =
                        app.top_flag_editor.target;
                } else if (text_editor::is_active(app.top_flag_editor) &&
                           app.top_flag_editor.kind ==
                               text_editor::Kind::BpmBracket) {
                    // Brief X.2: same flag-rect highlight suppression
                    // as iter — the popup above owns the highlight.
                    // Modes are mutually exclusive so the shared
                    // popup_editor_target channel is safe.
                    overlay.popup_editor_target =
                        app.top_flag_editor.target;
                }
                render_flags(cr, top_strip, app.warpmarkers.markers(),
                             vp_start, vp_end, sr,
                             kFlagFontSize,
                             app.selected_markers,
                             trim_struct,
                             px_x,
                             overlay);

                // V.A3b hover popup. Drawn on top of the flag strip,
                // strictly after render_flags. Motion + tick already
                // gate visibility; redraw just paints what state says.
                // The popup text was precomputed at hover-entry into
                // `app.hover_popup.cached_text` so this redraw branch
                // doesn't have to repeat the parser-mirroring math.
                if (app.hover_popup.visible &&
                    !app.iteration_mode_enabled) {
                    const auto& mv = app.warpmarkers.markers();
                    const int hidx = app.hover_popup.marker_index;
                    const bool eligible =
                        (hidx >= 0 &&
                         hidx < static_cast<int>(mv.size()) &&
                         (mv[hidx].tempo_inherits ||
                          !mv[hidx].label_ref.empty()) &&
                         !app.hover_popup.cached_text.empty());
                    if (eligible) {
                        auto rects = compute_flag_hit_rects(
                            cr, top_strip, mv,
                            vp_start, vp_end, sr, kFlagFontSize);
                        GuiRect anchor{0, 0, 0, 0};
                        for (const auto& r : rects) {
                            if (r.marker_index == hidx) {
                                // anchor.x is the flag rect's text-origin
                                // (rect's geometric x + render_flags' hl_pad
                                // = kFlagInnerPadPx), so the popup's
                                // leading character sits at the same column
                                // as the flag's leading character.
                                anchor.x = static_cast<int>(std::lround(r.x)) +
                                           static_cast<int>(kFlagInnerPadPx);
                                anchor.y = static_cast<int>(std::lround(r.y));
                                anchor.w = static_cast<int>(std::lround(r.w));
                                anchor.h = static_cast<int>(std::lround(r.h));
                                break;
                            }
                        }
                        if (anchor.w > 0 && anchor.h > 0) {
                            const int64_t pos = static_cast<int64_t>(
                                std::llround(
                                    mv[hidx].time_seconds *
                                    static_cast<double>(sr)));
                            const bool oot =
                                marker_out_of_trim(pos, trim_struct);
                            text_display::State td;
                            td.anchor   = anchor;
                            td.content  = app.hover_popup.cached_text;
                            td.visible  = true;
                            td.color    = oot ? dim(kText) : kText;
                            td.position =
                                text_display::Position::Top;
                            text_display::render(cr, td,
                                                     kFlagFontSize);
                        }
                    }
                }

                // V.B iteration popups. Persistent per-flag annotations
                // when iteration mode is on. Each owning marker gets a
                // popup above its flag rect; pass markers and label_ref
                // markers are excluded (no own tempo to vary). When the
                // top_flag_editor is active in IterationBracket kind on
                // marker `T`, popup `T` paints the editor's pending
                // text (with the [] brackets visible during edit) and
                // a 1-px cursor at cursor_pos; other popups paint
                // their formatted iter text normally.
                if (app.iteration_mode_enabled) {
                    const auto& mv = app.warpmarkers.markers();
                    auto hits = compute_iter_popup_hits(
                        cr, top_strip, mv,
                        vp_start, vp_end, sr, kFlagFontSize);
                    const bool editor_on_iter =
                        text_editor::is_active(app.top_flag_editor) &&
                        app.top_flag_editor.kind ==
                            text_editor::Kind::IterationBracket;
                    // Brief Y.5: paint hits in REVERSE so the leftmost
                    // popup paints last (on top). The compute_iter_popup
                    // _hits pack walks left-to-right and elides right-of-
                    // collision popups; reverse paint makes the editor's
                    // widening pending text occlude its right neighbor
                    // rather than vice-versa, matching the leftmost-wins
                    // pack rule. In static (non-edit) states the bg-fills
                    // are kBackground and text rects don't overlap, so
                    // the pixels are identical to the previous order.
                    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                        const auto& h = *it;
                        // Anchor for text_display: x at the flag's
                        // text-origin (flag.x + kFlagInnerPadPx, mirrors
                        // hover popup), y/w/h from the flag rect itself.
                        GuiRect anchor{
                            h.flag_rect.x +
                                static_cast<int>(kFlagInnerPadPx),
                            h.flag_rect.y,
                            h.flag_rect.w,
                            h.flag_rect.h
                        };
                        const int64_t pos = static_cast<int64_t>(
                            std::llround(
                                mv[h.marker_index].time_seconds *
                                static_cast<double>(sr)));
                        const bool oot =
                            marker_out_of_trim(pos, trim_struct);
                        if (editor_on_iter &&
                            app.top_flag_editor.target == h.marker_index) {
                            // Editor branch: state 2/3 of the three-state
                            // model. Background fills with kAccent on
                            // parse failure, otherwise kMarker; text and
                            // (blink-gated) cursor in kText. Out-of-trim
                            // wraps every color in dim() uniformly.
                            const std::string& pending =
                                app.top_flag_editor.pending;
                            cairo_save(cr);
                            cairo_select_font_face(cr, "monospace",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
                            cairo_set_font_size(cr, kFlagFontSize);
                            cairo_text_extents_t pext;
                            cairo_text_extents(cr, pending.c_str(), &pext);
                            cairo_text_extents_t uext;
                            cairo_text_extents(cr, "[+0.00,+0.00]", &uext);
                            const double hl_pad = kFlagInnerPadPx;
                            const double bg_w =
                                pext.x_advance + 2.0 * hl_pad;
                            const double bg_x =
                                static_cast<double>(anchor.x) - hl_pad;
                            const double bg_y =
                                static_cast<double>(h.hit_rect.y);
                            const double bg_h =
                                static_cast<double>(h.hit_rect.h);
                            // Brief Y.4 sub-bug A: opaque canvas-bg
                            // fill under the popup text, drawn before
                            // the colored outline below. Width tracks
                            // pending text + 2 * kFlagInnerPadPx, so
                            // it occludes neighbor popup text once
                            // pending widens past the original [ ].
                            render_flag_text_bg_fill(cr,
                                static_cast<double>(anchor.x),
                                pext.x_advance, bg_y, bg_h);
                            GuiColor bg_col = app.top_flag_editor.red
                                ? kAccent : kMarker;
                            if (oot) bg_col = dim(bg_col);
                            cairo_set_source_rgb(cr,
                                bg_col.r, bg_col.g, bg_col.b);
                            const double sx = std::round(bg_x) + 0.5;
                            const double sy = std::round(bg_y) + 0.5;
                            const int sw = static_cast<int>(
                                std::round(bg_w));
                            const int sh = static_cast<int>(
                                std::round(bg_h));
                            cairo_set_line_width(cr, 1.0);
                            cairo_rectangle(cr, sx, sy,
                                static_cast<double>(sw),
                                static_cast<double>(sh));
                            cairo_stroke(cr);

                            const double baseline_y =
                                static_cast<double>(anchor.y)
                              - kIterPopupVerticalGapPx
                              - kIterPopupVPadExtraPx
                              - (uext.height + uext.y_bearing);
                            const GuiColor txt = oot ? dim(kText) : kText;
                            cairo_set_source_rgb(cr,
                                txt.r, txt.g, txt.b);
                            cairo_move_to(cr,
                                static_cast<double>(anchor.x), baseline_y);
                            cairo_show_text(cr, pending.c_str());

                            // Brief seven: selection swap over the
                            // selected substring. Same shape as the
                            // flag-payload site in render.cpp; the
                            // re-paint color is kBackground (the
                            // canvas-bg used by render_flag_text_bg_fill).
                            if (text_editor::has_selection(
                                    app.top_flag_editor)) {
                                const int sel_a = text_editor::selection_start(
                                    app.top_flag_editor);
                                const int sel_b = text_editor::selection_end(
                                    app.top_flag_editor);
                                cairo_text_extents_t a_ext;
                                cairo_text_extents(cr,
                                    pending.substr(0,
                                        static_cast<size_t>(sel_a)).c_str(),
                                    &a_ext);
                                cairo_text_extents_t b_ext;
                                cairo_text_extents(cr,
                                    pending.substr(0,
                                        static_cast<size_t>(sel_b)).c_str(),
                                    &b_ext);
                                const double hi_x =
                                    static_cast<double>(anchor.x) +
                                    a_ext.x_advance;
                                const double hi_w =
                                    b_ext.x_advance - a_ext.x_advance;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_rectangle(cr, hi_x, bg_y,
                                                hi_w, bg_h);
                                cairo_fill(cr);
                                const GuiColor bg_swap =
                                    oot ? dim(kBackground) : kBackground;
                                cairo_set_source_rgb(cr,
                                    bg_swap.r, bg_swap.g, bg_swap.b);
                                cairo_move_to(cr, hi_x, baseline_y);
                                cairo_show_text(cr,
                                    pending.substr(
                                        static_cast<size_t>(sel_a),
                                        static_cast<size_t>(sel_b - sel_a))
                                        .c_str());
                            }

                            if (text_editor::cursor_visible_now(
                                    app.top_flag_editor)) {
                                std::string left = pending.substr(
                                    0, static_cast<size_t>(
                                        app.top_flag_editor.cursor_pos));
                                cairo_text_extents_t lext;
                                cairo_text_extents(cr, left.c_str(), &lext);
                                const double cx =
                                    static_cast<double>(anchor.x) +
                                    lext.x_advance;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_set_line_width(cr, 1.0);
                                cairo_move_to(cr, cx, bg_y);
                                cairo_line_to(cr, cx, bg_y + bg_h);
                                cairo_stroke(cr);
                            }
                            cairo_restore(cr);
                        } else {
                            // Brief Y.4 sub-bug A: paint the canvas-bg
                            // fill under the popup text before the
                            // text is drawn. In the static (non-edit)
                            // case the fill matches strip-clear
                            // exactly, so pixels are identical to
                            // today; the fill does the occlusion
                            // work only when an adjacent popup is
                            // being edited and grows over this one.
                            cairo_save(cr);
                            cairo_select_font_face(cr, "monospace",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
                            cairo_set_font_size(cr, kFlagFontSize);
                            cairo_text_extents_t hext;
                            cairo_text_extents(cr, h.text.c_str(), &hext);
                            render_flag_text_bg_fill(cr,
                                static_cast<double>(anchor.x),
                                hext.x_advance,
                                static_cast<double>(h.hit_rect.y),
                                static_cast<double>(h.hit_rect.h));
                            cairo_restore(cr);

                            text_display::State td;
                            td.anchor   = anchor;
                            td.content  = h.text;
                            td.visible  = true;
                            td.color    = oot ? dim(kText) : kText;
                            td.position =
                                text_display::Position::Top;
                            text_display::render(cr, td,
                                                     kFlagFontSize);
                        }
                    }
                }

                // Brief X.2 BPM popups. Parallel to the iter block
                // above; mutually exclusive with iteration mode (only
                // one mode's popups paint at a time). At most one
                // marker has bpm_is_popup_owner=true so hits is normally
                // a single entry, but the reverse-paint and
                // bg-fill-under-text patterns mirror iter for
                // consistency.
                if (app.bpm_mode_enabled) {
                    const auto& mv = app.warpmarkers.markers();
                    auto hits = compute_bpm_popup_hits(
                        cr, top_strip, mv,
                        vp_start, vp_end, sr, kFlagFontSize);
                    const bool editor_on_bpm =
                        text_editor::is_active(app.top_flag_editor) &&
                        app.top_flag_editor.kind ==
                            text_editor::Kind::BpmBracket;
                    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
                        const auto& h = *it;
                        GuiRect anchor{
                            h.flag_rect.x +
                                static_cast<int>(kFlagInnerPadPx),
                            h.flag_rect.y,
                            h.flag_rect.w,
                            h.flag_rect.h
                        };
                        const int64_t pos = static_cast<int64_t>(
                            std::llround(
                                mv[h.marker_index].time_seconds *
                                static_cast<double>(sr)));
                        const bool oot =
                            marker_out_of_trim(pos, trim_struct);
                        if (editor_on_bpm &&
                            app.top_flag_editor.target == h.marker_index) {
                            const std::string& pending =
                                app.top_flag_editor.pending;
                            cairo_save(cr);
                            cairo_select_font_face(cr, "monospace",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
                            cairo_set_font_size(cr, kFlagFontSize);
                            cairo_text_extents_t pext;
                            cairo_text_extents(cr, pending.c_str(), &pext);
                            cairo_text_extents_t uext;
                            cairo_text_extents(cr, "99@[999,999]", &uext);
                            const double hl_pad = kFlagInnerPadPx;
                            const double bg_w =
                                pext.x_advance + 2.0 * hl_pad;
                            const double bg_x =
                                static_cast<double>(anchor.x) - hl_pad;
                            const double bg_y =
                                static_cast<double>(h.hit_rect.y);
                            const double bg_h =
                                static_cast<double>(h.hit_rect.h);
                            render_flag_text_bg_fill(cr,
                                static_cast<double>(anchor.x),
                                pext.x_advance, bg_y, bg_h);
                            GuiColor bg_col = app.top_flag_editor.red
                                ? kAccent : kMarker;
                            if (oot) bg_col = dim(bg_col);
                            cairo_set_source_rgb(cr,
                                bg_col.r, bg_col.g, bg_col.b);
                            const double sx = std::round(bg_x) + 0.5;
                            const double sy = std::round(bg_y) + 0.5;
                            const int sw = static_cast<int>(
                                std::round(bg_w));
                            const int sh = static_cast<int>(
                                std::round(bg_h));
                            cairo_set_line_width(cr, 1.0);
                            cairo_rectangle(cr, sx, sy,
                                static_cast<double>(sw),
                                static_cast<double>(sh));
                            cairo_stroke(cr);

                            const double baseline_y =
                                static_cast<double>(anchor.y)
                              - kIterPopupVerticalGapPx
                              - kIterPopupVPadExtraPx
                              - (uext.height + uext.y_bearing);
                            const GuiColor txt = oot ? dim(kText) : kText;
                            cairo_set_source_rgb(cr,
                                txt.r, txt.g, txt.b);
                            cairo_move_to(cr,
                                static_cast<double>(anchor.x), baseline_y);
                            cairo_show_text(cr, pending.c_str());

                            // Brief seven: selection swap. Mirrors the
                            // iter-popup and flag-payload sites.
                            if (text_editor::has_selection(
                                    app.top_flag_editor)) {
                                const int sel_a = text_editor::selection_start(
                                    app.top_flag_editor);
                                const int sel_b = text_editor::selection_end(
                                    app.top_flag_editor);
                                cairo_text_extents_t a_ext;
                                cairo_text_extents(cr,
                                    pending.substr(0,
                                        static_cast<size_t>(sel_a)).c_str(),
                                    &a_ext);
                                cairo_text_extents_t b_ext;
                                cairo_text_extents(cr,
                                    pending.substr(0,
                                        static_cast<size_t>(sel_b)).c_str(),
                                    &b_ext);
                                const double hi_x =
                                    static_cast<double>(anchor.x) +
                                    a_ext.x_advance;
                                const double hi_w =
                                    b_ext.x_advance - a_ext.x_advance;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_rectangle(cr, hi_x, bg_y,
                                                hi_w, bg_h);
                                cairo_fill(cr);
                                const GuiColor bg_swap =
                                    oot ? dim(kBackground) : kBackground;
                                cairo_set_source_rgb(cr,
                                    bg_swap.r, bg_swap.g, bg_swap.b);
                                cairo_move_to(cr, hi_x, baseline_y);
                                cairo_show_text(cr,
                                    pending.substr(
                                        static_cast<size_t>(sel_a),
                                        static_cast<size_t>(sel_b - sel_a))
                                        .c_str());
                            }

                            if (text_editor::cursor_visible_now(
                                    app.top_flag_editor)) {
                                std::string left = pending.substr(
                                    0, static_cast<size_t>(
                                        app.top_flag_editor.cursor_pos));
                                cairo_text_extents_t lext;
                                cairo_text_extents(cr, left.c_str(), &lext);
                                const double cx =
                                    static_cast<double>(anchor.x) +
                                    lext.x_advance;
                                cairo_set_source_rgb(cr,
                                    txt.r, txt.g, txt.b);
                                cairo_set_line_width(cr, 1.0);
                                cairo_move_to(cr, cx, bg_y);
                                cairo_line_to(cr, cx, bg_y + bg_h);
                                cairo_stroke(cr);
                            }
                            cairo_restore(cr);
                        } else {
                            cairo_save(cr);
                            cairo_select_font_face(cr, "monospace",
                                CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
                            cairo_set_font_size(cr, kFlagFontSize);
                            cairo_text_extents_t hext;
                            cairo_text_extents(cr, h.text.c_str(), &hext);
                            render_flag_text_bg_fill(cr,
                                static_cast<double>(anchor.x),
                                hext.x_advance,
                                static_cast<double>(h.hit_rect.y),
                                static_cast<double>(h.hit_rect.h));
                            cairo_restore(cr);

                            text_display::State td;
                            td.anchor   = anchor;
                            td.content  = h.text;
                            td.visible  = true;
                            td.color    = oot ? dim(kText) : kText;
                            td.position =
                                text_display::Position::Top;
                            text_display::render(cr, td,
                                                     kFlagFontSize);
                        }
                    }
                }
            }
            const auto f1 = clock::now();
            t_flags_ms =
                std::chrono::duration<double, std::milli>(f1 - f0).count();
        }

        // Playhead drawn last so its stem and triangle paint over any
        // marker connector pixels they share a column with — the brief
        // mandates the playhead never be occluded by marker stems or
        // flag annotations. The triangle indicator lives in the top
        // strip, so render whenever either the waveform or top strip is
        // exposed; otherwise a flag-strip-only repaint would erase the
        // triangle.
        if (rects_intersect(exposed, area) ||
            rects_intersect(exposed, top_strip)) {
            const auto p0 = clock::now();
            render_playhead(cr, area, px_x, kPlayhead,
                            gui.playhead_triangle_surface());
            const auto p1 = clock::now();
            t_playhead_ms =
                std::chrono::duration<double, std::milli>(p1 - p0).count();
        }

        // Bottom strip: either the prompt overlay (when active) or
        // the regular elements (timestamp / tab letter / dirty / render
        // -view filename). The prompt is modal — while active, it
        // owns the strip and the regular elements are not visible.
        const GuiRect ts = timestamp_invalidate_rect(
            app.height, app.width, bottom_strip_wide(app));
        if (rects_intersect(exposed, ts)) {
            const int baseline_y = app.height - kTimestampBaselineFromBottom;
            if (app.prompt.active) {
                cairo_save(cr);
                cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                cairo_select_font_face(cr, "monospace",
                                       CAIRO_FONT_SLANT_NORMAL,
                                       CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, 14.0);
                cairo_move_to(cr, kTimestampPadX, baseline_y);
                cairo_show_text(cr, app.prompt.text.c_str());
                cairo_text_extents_t pext;
                cairo_text_extents(cr, app.prompt.text.c_str(), &pext);
                const double label_gap = kTabLetterGapPx * 2.0;
                double cursor_x = static_cast<double>(kTimestampPadX) +
                                  pext.x_advance + label_gap;
                for (const auto& label : app.prompt.response_labels) {
                    cairo_move_to(cr, cursor_x, baseline_y);
                    cairo_show_text(cr, label.c_str());
                    cairo_text_extents_t lext;
                    cairo_text_extents(cr, label.c_str(), &lext);
                    cursor_x += lext.x_advance + label_gap;
                }
                cairo_restore(cr);
            } else if (!app.queue_progress_text.empty()) {
                cairo_save(cr);
                cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                cairo_select_font_face(cr, "monospace",
                                       CAIRO_FONT_SLANT_NORMAL,
                                       CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size(cr, 14.0);
                cairo_move_to(cr, kTimestampPadX, baseline_y);
                cairo_show_text(cr, app.queue_progress_text.c_str());
                cairo_restore(cr);
            } else {
                // In source-view, sr is the loaded file's sample rate
                // and playhead_sample is in source-frames. In render
                // -view the active `audio` is the render, so its sr
                // is what the engine wrote out — but the playhead is
                // in render-frame coords. Render-view timestamp is
                // render-domain (zero at render sample 0); source-time
                // and render-time advance at different rates because
                // of warping, so the same arithmetic suffices.
                double seconds = 0.0;
                if (sr > 0) {
                    seconds = static_cast<double>(app.playhead_sample) /
                              static_cast<double>(sr);
                }
                {
                    const auto s0 = clock::now();
                    render_timestamp(cr, kTimestampPadX, baseline_y,
                                     seconds, kText);
                    const auto s1 = clock::now();
                    t_ts_ms =
                        std::chrono::duration<double, std::milli>(s1 - s0).count();
                }

                // A/B tab letter between timestamp and dirty indicator.
                // Same font/size/color as the timestamp; no background.
                // Suppressed in render-view since the Tab key is gated
                // out there and the letter would carry no meaning.
                const double tw = measure_timestamp_width(cr, seconds);
                double right_after_letter =
                    static_cast<double>(kTimestampPadX) + tw;
                if (!app.render_view_enabled) {
                    const double letter_x =
                        static_cast<double>(kTimestampPadX) + tw +
                        kTabLetterGapPx;
                    const char letter_buf[2] = { app.active_tab, '\0' };
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_text_extents_t ext;
                    cairo_text_extents(cr, letter_buf, &ext);
                    cairo_move_to(cr, letter_x, baseline_y);
                    cairo_show_text(cr, letter_buf);
                    right_after_letter = letter_x + ext.x_advance;
                    cairo_restore(cr);
                }

                if (app.dirty) {
                    const auto d0 = clock::now();
                    const double cx = right_after_letter + kTabLetterGapPx;
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_move_to(cr, cx, baseline_y);
                    cairo_show_text(cr, "*");
                    cairo_restore(cr);
                    const auto d1 = clock::now();
                    t_dirty_ms =
                        std::chrono::duration<double, std::milli>(d1 - d0).count();
                }

                // Chunk W: render-view filename. Right-aligned in the
                // bottom strip so it doesn't conflict with the
                // timestamp / tab letter / dirty indicator on the left.
                if (app.render_view_enabled &&
                    app.render_view_index >= 0 &&
                    app.render_view_index <
                        static_cast<int>(app.render_view_list.size())) {
                    const auto& e =
                        app.render_view_list[app.render_view_index];
                    const std::string label =
                        e.batch_folder.filename().string() + "/" +
                        e.basename + ".wav";
                    cairo_save(cr);
                    cairo_set_source_rgb(cr, kText.r, kText.g, kText.b);
                    cairo_select_font_face(cr, "monospace",
                                           CAIRO_FONT_SLANT_NORMAL,
                                           CAIRO_FONT_WEIGHT_NORMAL);
                    cairo_set_font_size(cr, 14.0);
                    cairo_text_extents_t ext;
                    cairo_text_extents(cr, label.c_str(), &ext);
                    const double rx = static_cast<double>(app.width) -
                                      static_cast<double>(kTimestampPadX) -
                                      ext.x_advance;
                    cairo_move_to(cr, rx, baseline_y);
                    cairo_show_text(cr, label.c_str());
                    cairo_restore(cr);
                }
            }
        }
    }

    cairo_restore(cr);

    // Force any pending Cairo ops out to the X server so the flush cost
    // is captured here rather than attributed elsewhere. The subsequent
    // flush in GuiX11::dispatch_event is a cheap no-op.
    {
        const auto fl0 = clock::now();
        cairo_surface_flush(cairo_get_target(cr));
        const auto fl1 = clock::now();
        t_flush_ms =
            std::chrono::duration<double, std::milli>(fl1 - fl0).count();
    }

    const auto t_end = clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(t_end - t_start).count();

    if constexpr (kDebugPerf) {
        if (elapsed_ms > 3.0) {
            double e2e_ms = -1.0;
            if (app.last_input_event_time.time_since_epoch().count() != 0) {
                e2e_ms = std::chrono::duration<double, std::milli>(
                    t_end - app.last_input_event_time).count();
            }
            std::fprintf(stderr,
                "[dbg perf] total=%.2f ms waveform=%.2f markers=%.2f "
                "flags=%.2f playhead=%.2f ts=%.2f dirty=%.2f flush=%.2f "
                "pixel_area=%dx%d wf_cols=%d wf_pyramid_samples=%d "
                "flag_measure=%d flag_drawn=%d flag_elided=%d "
                "e2e=%.2f\n",
                elapsed_ms, t_waveform_ms, t_markers_ms, t_flags_ms,
                t_playhead_ms, t_ts_ms, t_dirty_ms, t_flush_ms,
                w, h,
                perf_counters::wf_cols, perf_counters::wf_pyramid_samples,
                perf_counters::flag_measure, perf_counters::flag_drawn,
                perf_counters::flag_elided,
                e2e_ms);
        }
    }

    if constexpr (kDebugPerf) {
        if (elapsed_ms > app.stats_max_redraw_ms)
            app.stats_max_redraw_ms = elapsed_ms;
        if (elapsed_ms > 1.0) app.stats_over_1ms_count++;
        const double since_last = std::chrono::duration<double>(
            t_end - app.stats_last_report).count();
        if (since_last >= 1.0) {
            if (app.stats_max_redraw_ms > 1.0) {
                std::fprintf(stderr,
                    "[warptempo_gui] redraw max=%.2fms in last %.1fs "
                    "(%d redraws > 1ms)\n",
                    app.stats_max_redraw_ms, since_last,
                    app.stats_over_1ms_count);
            }
            app.stats_max_redraw_ms = 0.0;
            app.stats_over_1ms_count = 0;
            app.stats_last_report = t_end;
        }
    }
}

// -- GuiPaintHandler::on_resize ------------------------------------------

void GuiPaintHandler::on_resize(int w, int h) {
    app.width  = w;
    app.height = h;
    if (app.loading || audio.total_frames() <= 0) return;

    // A numeric zoom level may have been valid at the old width but show
    // more samples than the file at the new width — promote to fit-file.
    const int max_num = max_valid_numeric_level(
        waveform_area(app).w, audio.total_frames(), audio.sample_rate());
    if (app.zoom_level != kFitFileLevel) {
        if (max_num < 0 || app.zoom_level > max_num) {
            app.zoom_level = kFitFileLevel;
            app.viewport_start_sample = 0;
            if (playback.is_playing()) playback.resync_predictor();
        }
    }
    clamp_viewport_start(app, audio);
}
