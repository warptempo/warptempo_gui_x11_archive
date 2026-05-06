#include "input_handler.h"

#include "render.h"
#include "render_pipeline.h"
#include "text_editor.h"
#include "warpmarkers.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// X.7.8b-1: keyboard input handler. Method bodies are byte-identical to the
// lambdas they replaced in main.cpp (set_on_key at the original main.cpp:1588;
// run_render_batch at the original main.cpp:1539). The only changes are:
//
//   - Capture-by-reference of `app`, `audio`, `gui`, `playback`, `viewport`,
//     `selection`, `undo`, `warpops`, `transients`, `flag_editor`,
//     `render_view`, `tab_mode`, `clear_hover_popup`,
//     `stop_playback_if_playing`, `save_markers`, `request_close_or_revert`,
//     `prompt_activate_response`, `toggle_playback`, `set_playback_speed` is
//     now reference-member access on `this`. Identifier spelling is identical
//     so nothing else changes inside the bodies.
//   - Forwarder lambdas in main.cpp (do_undo, do_redo, recompute_dirty,
//     push_undo_both, select_*, clear_selection, set_single_selection,
//     toggle_selection_membership, move_playhead_*, zoom_*, scroll_viewport,
//     center_viewport_on_playhead, invalidate_*, trim_*_sample, drop_*,
//     delete_*, force_delete_*, toggle_inherits/disabled/begin_time/end_time,
//     adjust_tempo, clear_trim, nudge_*, jump_*, drop_transient_at_playhead,
//     toggle_transient_*, detect_transients, clear_all_transients,
//     enter_*_edit, commit_*_edit, exit_top_flag_edit_no_commit,
//     bulk_clear_*_values, enter_bpm_mode, exit_bpm_mode,
//     toggle_active_mode, load_render_view_at, restore_source_audio,
//     stash_render_view_selection_to_active_entry,
//     enumerate_render_view_list, write_rendersettings_for) were rewritten
//     to direct method calls on the appropriate operation struct ref.
//   - Free function calls (do_render, iter_popup_eligible_marker,
//     effective_disabled, compute_base_tempo_scale, text_editor::*,
//     std::filesystem::*, std::printf, etc.) keep their original spelling.
//     compute_base_tempo_scale + BaseTempoScale moved out of main.cpp's
//     anonymous namespace into input_handler.h so this TU can reach them;
//     on_key (Ctrl+Alt+M) is the sole caller.

GuiInputHandler::RenderBatchResult
GuiInputHandler::run_render_batch(const std::vector<RenderRequest>& reqs,
                                  const std::string& batch_label) {
    RenderBatchResult result;
    if (reqs.empty()) return result;

    const int total = static_cast<int>(reqs.size());

    app.queue_cancel_requested = false;
    app.queue_running          = true;
    clear_hover_popup();

    for (int i = 0; i < total; ++i) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "%s: rendering %d of %d...",
                      batch_label.c_str(), i + 1, total);
        app.queue_progress_text = buf;
        viewport.invalidate_timestamp_area();
        // First drain surfaces the progress-text paint before the
        // engine starts; otherwise the new "rendering K of N" only
        // appears after the entry completes.
        gui.drain_events();

        if (do_render(reqs[i])) ++result.rendered;

        // Second drain surfaces X events queued during the render —
        // Esc presses, expose events. The cancel flag becomes
        // visible to the next iteration through this drain.
        gui.drain_events();
        if (app.queue_cancel_requested) {
            result.cancelled = true;
            break;
        }
    }

    app.queue_running          = false;
    app.queue_cancel_requested = false;
    // Invalidate the wide bottom-strip rect before clearing the
    // progress text. bottom_strip_wide() reads queue_progress_text;
    // clearing first would shrink the invalidated rect to the narrow
    // timestamp width, leaving the trailing pixels of the final
    // "rendering N of N..." string undamaged.
    viewport.invalidate_timestamp_area();
    app.queue_progress_text.clear();

    return result;
}

void GuiInputHandler::on_key(KeySym keysym, unsigned int mods) {
    if constexpr (kDebugPerf) {
        app.last_input_event_time = std::chrono::steady_clock::now();
    }
    const bool ctrl  = (mods & ControlMask) != 0;
    const bool shift = (mods & ShiftMask)   != 0;
    const bool alt   = (mods & Mod1Mask)    != 0;

    // Bottom-strip prompt owns input while active. Only the prompt's
    // own response keys (case-insensitive) and Esc (rightmost
    // response = Cancel by convention) do anything; everything else
    // is swallowed so marker edits / playback / viewport keys cannot
    // sneak in while the prompt is up.
    if (app.prompt.active) {
        char k = 0;
        if (keysym >= XK_a && keysym <= XK_z) {
            k = static_cast<char>('a' + (keysym - XK_a));
        } else if (keysym >= XK_A && keysym <= XK_Z) {
            k = static_cast<char>('a' + (keysym - XK_A));
        }
        if (keysym == XK_Escape) {
            if (!app.prompt.response_keys.empty()) {
                prompt_activate_response(app.prompt.response_keys.back());
            }
            return;
        }
        if (k != 0) {
            for (char rk : app.prompt.response_keys) {
                if (k == rk) {
                    prompt_activate_response(rk);
                    return;
                }
            }
        }
        return;
    }

    // Blank / loading state: only the quit / close-gesture bindings run;
    // everything else no-ops. Dialog can't fire here because dirty is
    // always false in blank state (history is reset on revert).
    if (app.loading || audio.total_frames() <= 0) {
        if (ctrl && !shift && !alt && keysym == XK_q) {
            request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
        }
        return;
    }

    // V.A1 top-flag editor owns the keyboard while active. Routes here
    // BEFORE queue/drag/playhead Esc handlers so Esc cancels the edit
    // first; Esc with no active edit falls through to the rest.
    if (text_editor::is_active(app.top_flag_editor)) {
        (void)ctrl; (void)alt; // Modifiers swallowed except Shift→colon.
        const auto action = text_editor::handle_key(
            app.top_flag_editor, keysym, mods);
        if (action == text_editor::KeyAction::CommitRequested) {
            if (app.top_flag_editor.kind ==
                    text_editor::Kind::IterationBracket) {
                flag_editor.commit_iter_edit();
            } else if (app.top_flag_editor.kind ==
                    text_editor::Kind::BpmBracket) {
                flag_editor.commit_bpm_edit();
            } else {
                flag_editor.commit_top_flag_edit();
            }
            return;
        }
        if (action == text_editor::KeyAction::CancelRequested) {
            flag_editor.exit_top_flag_edit_no_commit();
            return;
        }
        if (action == text_editor::KeyAction::Consumed) {
            viewport.invalidate_top_strip();
            return;
        }
        // NotConsumed: editor saw nothing useful; fall through is wrong
        // because the editor must own all keys while active. Treat as
        // consumed.
        return;
    }

    // Chunk W: render-view input gate. While render-view is active
    // only keys driving navigation / playback / exit / commit are
    // honored; every authoring key is silently dropped so a stray
    // press can't mutate state through a swapped-out view.
    // Allowlist:
    //   - r (no mods)            → toggle render-view off
    //   - Shift+Left/Right       → previous/next render
    //   - Ctrl+Alt+C             → commit displayed render's markers
    //   - Space                  → playback toggle
    //   - Left/Right (no mods)   → playhead-by-pixel scrub
    //   - Home/End (no mods)     → playhead to trim begin/end
    //   - Esc                    → top-level no-op (chunk Q)
    //   - t (no mods)            → toggle warp/transient sub-view (Brief F)
    //   - Ctrl+Q / Ctrl+W        → close-prompt routing (Brief F)
    //   - Up/Down (no mods)      → zoom in/out (Brief S.2)
    //   - =/- (no mods)          → zoom in/out symbol-key alias (Brief S.2)
    if (app.render_view_enabled) {
        const bool is_r =
            (keysym == XK_r && !ctrl && !shift && !alt);
        const bool is_nav =
            ((keysym == XK_Left || keysym == XK_Right) &&
             shift && !ctrl && !alt);
        const bool is_commit =
            (ctrl && alt && !shift &&
             (keysym == XK_c || keysym == XK_C));
        const bool is_playback = (keysym == XK_space);
        const bool is_scrub =
            ((keysym == XK_Left || keysym == XK_Right) &&
             !ctrl && !shift && !alt);
        const bool is_jump =
            ((keysym == XK_Home || keysym == XK_End) &&
             !ctrl && !shift && !alt);
        const bool is_esc = (keysym == XK_Escape);
        const bool is_sub_view_toggle =
            (keysym == XK_t && !ctrl && !shift && !alt);
        const bool is_ctrl_q =
            (ctrl && !shift && !alt && keysym == XK_q);
        const bool is_ctrl_w =
            (ctrl && !shift && !alt && keysym == XK_w);
        const bool is_zoom =
            ((keysym == XK_Up || keysym == XK_Down) &&
             !ctrl && !shift && !alt);
        const bool is_zoom_symbol =
            ((keysym == XK_equal || keysym == XK_minus) &&
             !ctrl && !shift && !alt);
        if (!(is_r || is_nav || is_commit || is_playback ||
              is_scrub || is_jump || is_esc ||
              is_sub_view_toggle || is_ctrl_q || is_ctrl_w ||
              is_zoom || is_zoom_symbol)) {
            return;
        }
    }

    // Esc during a render-all run requests cancellation between
    // entries. Only fires while the queue walker is active; outside
    // that window Esc retains its other meanings (drag-cancel, etc).
    // Mid-engine Esc presses are queued by X and surface here on the
    // next gui.drain_events() — they take effect after the in-flight
    // entry completes (chunk U does not implement mid-engine cancel).
    if (keysym == XK_Escape && app.queue_running) {
        app.queue_cancel_requested = true;
        return;
    }

    // Escape during a playhead drag ends the gesture at its current
    // position (no restore — the drag already committed its visible
    // progress per motion event, so there's nothing to revert).
    if (keysym == XK_Escape && app.playhead_drag.active) {
        app.playhead_drag = PlayheadDragState{};
        return;
    }

    // Ctrl+Q: quit (via unsaved-work dialog when dirty).
    if (ctrl && !shift && !alt && keysym == XK_q) {
        request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
        return;
    }

    // Ctrl+W: revert to blank state (via unsaved-work dialog when dirty).
    if (ctrl && !shift && !alt && keysym == XK_w) {
        request_close_or_revert(DialogTrigger::REVERT_TO_BLANK);
        return;
    }

    // Ctrl+E: snapshot current authoring state into the in-memory
    // render queue. No disk writes; on-disk authoring files are untouched.
    // Settings are not snapshotted per-entry — the queue walker uses
    // the live settings_passthrough at execution time, mirroring the
    // chunk-U convention. (Chunk W: snapshots moved from disk to memory.)
    if (ctrl && !alt && !shift &&
        (keysym == XK_e || keysym == XK_E)) {
        if (app.source_audio_path.empty()) return;
        AppState::QueuedRender q;
        q.source_audio_path = app.source_audio_path;
        q.markers           = app.warpmarkers.markers();
        q.transients        = app.transientmarkers.markers();
        app.queued_renders.push_back(std::move(q));
        std::fprintf(stderr,
            "warptempo_gui: queued render (%zu in queue)\n",
            app.queued_renders.size());
        return;
    }

    // Ctrl+Alt+R: single render into the source directory using `title`
    // from settings. Mirrors the pre-Chunk-W non-batch path inside
    // do_render: empty batch_folder/batch_basename triggers the
    // engine/limiter-prefix naming. Title-not-set is a hard error
    // surfaced from do_render. Does not consult the in-memory queue and
    // does not write any sidecars beyond the .peaks pyramid that
    // do_render already deposits.
    if (ctrl && alt && !shift &&
        (keysym == XK_r || keysym == XK_R)) {
        if (app.source_audio_path.empty()) return;

        RenderRequest req;
        req.source_audio_path    = app.source_audio_path;
        req.markers              = app.warpmarkers.markers();
        req.transients           = app.transientmarkers.markers();
        req.settings_passthrough = app.settings_passthrough;
        for (const auto& m : app.transientmarkers.markers()) {
            if (m.disabled) continue;
            req.transient_frames.push_back(m.effective_frame());
        }
        // Empty batch_folder/basename selects the source-dir naming
        // convention inside do_render.
        do_render(req);
        return;
    }

    // Ctrl+Alt+E: render the in-memory queue as one batch. Each
    // queued entry produces a sibling .wav (+ .warpmarkers /
    // .transientmarkers when non-empty / .peaks sidecars) inside a fresh
    // batch folder `<source_parent>/renders/<index>_render_all_in_queue/`.
    // The index is one greater than the highest pre-existing batch index
    // in that renders folder (regardless of command tag). Filenames
    // inside the batch are the entry position zero-padded to fit the
    // queue size: 01..10 for 10 entries, 1..7 for 7, 001..100 for 100.
    //
    // Empty queue is a silent no-op (no implicit-batch fallback —
    // single-shot rendering belongs on Ctrl+Alt+R now).
    //
    // Esc between entries drops the remainder. The current render
    // cannot be interrupted (no mid-engine cancellation); its sidecars
    // are written if it succeeds, then the loop exits and the rest of
    // the queue is discarded. The batch folder is left as-is on disk —
    // partial batches just contain fewer files than the queue had.
    // The in-memory queue is cleared after execution whether all
    // entries ran or Esc cut it short.
    if (ctrl && alt && !shift &&
        (keysym == XK_e || keysym == XK_E)) {
        if (app.source_audio_path.empty()) return;
        if (app.queued_renders.empty()) return;

        std::vector<AppState::QueuedRender> entries =
            std::move(app.queued_renders);
        app.queued_renders.clear();

        std::filesystem::path src(app.source_audio_path);
        std::filesystem::path src_parent = src.parent_path();
        if (src_parent.empty()) src_parent = std::filesystem::path(".");
        const std::filesystem::path queue_root = src_parent / "renders";

        // Resolve the next batch index: max+1 over directory entries
        // matching `<digits>_<anything>`. Empty / missing renders/
        // folder seeds index 1.
        int next_index = 1;
        std::error_code ec;
        if (std::filesystem::is_directory(queue_root, ec)) {
            int max_idx = 0;
            for (const auto& de :
                 std::filesystem::directory_iterator(queue_root, ec)) {
                if (!de.is_directory()) continue;
                const std::string name = de.path().filename().string();
                int v = 0;
                size_t i = 0;
                while (i < name.size() &&
                       name[i] >= '0' && name[i] <= '9') {
                    v = v * 10 + (name[i] - '0');
                    ++i;
                }
                if (i == 0 || i >= name.size() || name[i] != '_') continue;
                if (v > max_idx) max_idx = v;
            }
            next_index = max_idx + 1;
        }

        const std::string command_tag = "render_all_in_queue";
        const std::filesystem::path batch_folder =
            queue_root /
            (std::to_string(next_index) + "_" + command_tag);
        std::filesystem::create_directories(batch_folder, ec);
        if (ec) {
            std::fprintf(stderr,
                "warptempo_gui: render-all: could not create '%s': %s\n",
                batch_folder.string().c_str(), ec.message().c_str());
            return;
        }

        // Width-to-fit zero-padding for filename indices. pad_width is
        // computed from the queue size and clamped to a sane upper
        // bound so the snprintf below has a known-bounded output.
        const int total = static_cast<int>(entries.size());
        int pad_width = 1;
        for (int n = total; n >= 10; n /= 10) ++pad_width;
        if (pad_width > 9) pad_width = 9;

        std::vector<RenderRequest> reqs;
        reqs.reserve(total);
        for (int i = 0; i < total; ++i) {
            const auto& q = entries[i];
            char num_buf[16];
            std::snprintf(num_buf, sizeof(num_buf),
                          "%0*d", pad_width, i + 1);
            std::fprintf(stderr,
                "warptempo_gui: rendering entry %d of %d: %s/%s.wav\n",
                i + 1, total,
                batch_folder.filename().string().c_str(), num_buf);

            RenderRequest req;
            req.source_audio_path    = q.source_audio_path;
            req.markers              = q.markers;
            req.transients           = q.transients;
            req.settings_passthrough = app.settings_passthrough;
            for (const auto& m : q.transients) {
                if (m.disabled) continue;
                req.transient_frames.push_back(m.effective_frame());
            }
            req.batch_folder   = batch_folder.string();
            req.batch_basename = num_buf;
            reqs.push_back(std::move(req));
        }

        const auto result = run_render_batch(reqs, "render queue");
        if (result.cancelled) {
            std::fprintf(stderr,
                "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                result.rendered, total);
        } else {
            std::fprintf(stderr,
                "warptempo_gui: rendered %d of %d entries into %s\n",
                result.rendered, total,
                batch_folder.filename().string().c_str());
        }
        return;
    }

    // Brief T: Ctrl+Alt+I renders the Cartesian product of the
    // per-marker iter ranges authored in iteration mode. Output lands
    // in `<source_parent>/renders/<N>_render_iterations/`, with one
    // .wav per cell named `<seq>_<delta_csv>.wav`. The CSV holds the
    // swept markers' deltas in timeline order, formatted `%+0.2f`;
    // markers with no iter range authored are excluded from the CSV
    // and contribute one fixed value (their authored tempo_base) to
    // the product. Per-cell progress and Esc cancellation are handled
    // by run_render_batch. Silent no-op outside iteration mode.
    if (ctrl && alt && !shift &&
        (keysym == XK_i || keysym == XK_I)) {
        if (app.source_audio_path.empty()) return;
        if (!app.iteration_mode_enabled) return;

        // Snapshot markers in timeline order (GuiWarpMarkers guarantees
        // strict-monotonic by time_seconds). For each owning marker
        // build its per-cell delta list: a single 0.0 when no iter
        // range is authored, otherwise integer-cents enumeration from
        // iter_start to iter_end inclusive. Integer-cents avoids the
        // float-accumulation drift a naive `for (d=start; d<=end;
        // d+=0.01)` would suffer across many steps.
        const std::vector<GuiWarpMarker> base_markers =
            app.warpmarkers.markers();
        std::vector<int>                 eligible_indices;
        std::vector<std::vector<double>> per_marker_deltas;
        std::vector<bool>                is_swept;
        for (int i = 0; i < static_cast<int>(base_markers.size()); ++i) {
            const GuiWarpMarker& m = base_markers[i];
            if (!iter_popup_eligible_marker(m)) continue;
            eligible_indices.push_back(i);
            const bool swept =
                !std::isnan(m.iter_start) && !std::isnan(m.iter_end);
            is_swept.push_back(swept);
            std::vector<double> deltas;
            if (swept) {
                const int start_cents = static_cast<int>(
                    std::lround(m.iter_start * 100.0));
                const int end_cents = static_cast<int>(
                    std::lround(m.iter_end * 100.0));
                // commit_iter_edit enforces start <= end, but a stray
                // hand-edit of memory could violate it. Treat that as
                // no sweep rather than producing zero cells.
                if (start_cents > end_cents) {
                    deltas.push_back(0.0);
                    is_swept.back() = false;
                } else {
                    for (int c = start_cents; c <= end_cents; ++c) {
                        deltas.push_back(static_cast<double>(c) / 100.0);
                    }
                }
            } else {
                deltas.push_back(0.0);
            }
            per_marker_deltas.push_back(std::move(deltas));
        }

        bool any_swept = false;
        for (bool s : is_swept) {
            if (s) { any_swept = true; break; }
        }
        if (!any_swept) {
            std::fprintf(stderr,
                "warptempo_gui: render-iterations: no iter ranges "
                "authored; nothing to render\n");
            return;
        }

        size_t total_cells = 1;
        for (const auto& d : per_marker_deltas) total_cells *= d.size();
        if (total_cells == 0) return;

        std::filesystem::path src(app.source_audio_path);
        std::filesystem::path src_parent = src.parent_path();
        if (src_parent.empty()) src_parent = std::filesystem::path(".");
        const std::filesystem::path queue_root = src_parent / "renders";

        // Resolve the next batch index: max+1 over `<digits>_<anything>`
        // entries. Mirrors render_all_in_queue's scanner.
        int next_index = 1;
        std::error_code ec;
        if (std::filesystem::is_directory(queue_root, ec)) {
            int max_idx = 0;
            for (const auto& de :
                 std::filesystem::directory_iterator(queue_root, ec)) {
                if (!de.is_directory()) continue;
                const std::string name = de.path().filename().string();
                int v = 0;
                size_t i = 0;
                while (i < name.size() &&
                       name[i] >= '0' && name[i] <= '9') {
                    v = v * 10 + (name[i] - '0');
                    ++i;
                }
                if (i == 0 || i >= name.size() || name[i] != '_') continue;
                if (v > max_idx) max_idx = v;
            }
            next_index = max_idx + 1;
        }

        const std::string command_tag = "render_iterations";
        const std::filesystem::path batch_folder =
            queue_root /
            (std::to_string(next_index) + "_" + command_tag);
        std::filesystem::create_directories(batch_folder, ec);
        if (ec) {
            std::fprintf(stderr,
                "warptempo_gui: render-iterations: could not create "
                "'%s': %s\n",
                batch_folder.string().c_str(), ec.message().c_str());
            return;
        }

        const int total = static_cast<int>(total_cells);
        int pad_width = 1;
        for (int n = total; n >= 10; n /= 10) ++pad_width;
        if (pad_width > 9) pad_width = 9;

        // Snapshot transients once — every cell shares the same
        // transient configuration, only marker tempo_base values
        // differ across cells.
        const std::vector<GuiTransientMarker> base_transients =
            app.transientmarkers.markers();
        std::vector<int64_t> base_transient_frames;
        for (const auto& t : base_transients) {
            if (t.disabled) continue;
            base_transient_frames.push_back(t.effective_frame());
        }

        // Cartesian product enumeration. `indices[k]` holds the
        // current cell coordinate along the k-th eligible marker
        // (timeline order). Rightmost dimension increments fastest:
        // consecutive cells differ in the last marker's delta first.
        const size_t num_dims = per_marker_deltas.size();
        std::vector<size_t> indices(num_dims, 0);

        std::vector<RenderRequest> reqs;
        reqs.reserve(total);
        for (int cell = 0; cell < total; ++cell) {
            std::string delta_csv;
            for (size_t k = 0; k < num_dims; ++k) {
                if (!is_swept[k]) continue;
                const double d = per_marker_deltas[k][indices[k]];
                char dbuf[16];
                std::snprintf(dbuf, sizeof(dbuf), "%+0.2f", d);
                if (!delta_csv.empty()) delta_csv += ',';
                delta_csv += dbuf;
            }

            char num_buf[16];
            std::snprintf(num_buf, sizeof(num_buf),
                          "%0*d", pad_width, cell + 1);
            std::string basename = num_buf;
            basename += '_';
            basename += delta_csv;

            std::vector<GuiWarpMarker> cell_markers = base_markers;
            for (size_t k = 0; k < num_dims; ++k) {
                const int mi = eligible_indices[k];
                cell_markers[mi].tempo_base =
                    base_markers[mi].tempo_base +
                    per_marker_deltas[k][indices[k]];
                // The engine doesn't consume iter values; clear them
                // so the request is quiet.
                cell_markers[mi].iter_start =
                    std::numeric_limits<double>::quiet_NaN();
                cell_markers[mi].iter_end =
                    std::numeric_limits<double>::quiet_NaN();
            }

            RenderRequest req;
            req.source_audio_path    = app.source_audio_path;
            req.markers              = std::move(cell_markers);
            req.transients           = base_transients;
            req.transient_frames     = base_transient_frames;
            req.settings_passthrough = app.settings_passthrough;
            req.batch_folder         = batch_folder.string();
            req.batch_basename       = std::move(basename);
            reqs.push_back(std::move(req));

            // Increment rightmost dimension; carry left on overflow.
            // The last cell leaves indices in an overflowed state but
            // the loop exits before that's read.
            for (int k = static_cast<int>(num_dims) - 1; k >= 0; --k) {
                ++indices[k];
                if (indices[k] < per_marker_deltas[k].size()) break;
                indices[k] = 0;
            }
        }

        const auto result = run_render_batch(reqs, "render iterations");
        if (result.cancelled) {
            std::fprintf(stderr,
                "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                result.rendered, total);
        } else {
            std::fprintf(stderr,
                "warptempo_gui: rendered %d of %d entries into %s\n",
                result.rendered, total,
                batch_folder.filename().string().c_str());
        }
        return;
    }

    // Brief X.3: Ctrl+Alt+M sweeps every BPM in the popup-owner's
    // [bpm_lo, bpm_hi] range, computing (base_tempo, scale) per cell
    // and rendering one .wav per cell into
    // `<source_parent>/renders/<N>_render_basetempo/`. The scale value
    // is encoded in the filename so Ctrl+Alt+C can later extract and
    // commit it back into source settings_passthrough. Mirrors the
    // iter render handler's structure; the substantive difference is
    // per-cell mutation of settings_passthrough's `scale` entry, in
    // addition to per-cell marker mutation. Silent no-op outside
    // BPM mode / warp / loaded audio / committed popup.
    if (ctrl && alt && !shift &&
        (keysym == XK_m || keysym == XK_M)) {
        if (app.active_mode != 'W') return;
        if (!app.bpm_mode_enabled) return;
        if (app.source_audio_path.empty()) return;
        if (audio.sample_rate() <= 0) return;
        if (audio.total_frames() <= 0) return;

        const std::vector<GuiWarpMarker> base_markers =
            app.warpmarkers.markers();
        int owner_idx = -1;
        for (int i = 0; i < static_cast<int>(base_markers.size()); ++i) {
            if (base_markers[i].bpm_is_popup_owner) {
                owner_idx = i;
                break;
            }
        }
        if (owner_idx < 0) return;
        const GuiWarpMarker& owner = base_markers[owner_idx];
        if (owner.bpm_beats <= 0) return;
        if (owner.bpm_lo    <= 0) return;
        if (owner.bpm_hi    <= 0) return;

        // Find the span endpoint: first effectively-enabled marker
        // after the owner. If none, the span runs to end-of-audio.
        int endpoint_idx = -1;
        for (int i = owner_idx + 1;
             i < static_cast<int>(base_markers.size()); ++i) {
            if (effective_disabled(base_markers, i)) continue;
            endpoint_idx = i;
            break;
        }
        const double audio_total_seconds =
            static_cast<double>(audio.total_frames()) /
            static_cast<double>(audio.sample_rate());
        const double duration_seconds =
            (endpoint_idx >= 0)
                ? (base_markers[endpoint_idx].time_seconds -
                   owner.time_seconds)
                : (audio_total_seconds - owner.time_seconds);
        if (!(duration_seconds > 0.0)) return;

        std::vector<int> bpm_values;
        for (int b = owner.bpm_lo; b <= owner.bpm_hi; ++b) {
            bpm_values.push_back(b);
        }
        if (bpm_values.empty()) return;

        std::filesystem::path src(app.source_audio_path);
        std::filesystem::path src_parent = src.parent_path();
        if (src_parent.empty()) src_parent = std::filesystem::path(".");
        const std::filesystem::path queue_root = src_parent / "renders";

        int next_index = 1;
        std::error_code ec;
        if (std::filesystem::is_directory(queue_root, ec)) {
            int max_idx = 0;
            for (const auto& de :
                 std::filesystem::directory_iterator(queue_root, ec)) {
                if (!de.is_directory()) continue;
                const std::string name = de.path().filename().string();
                int v = 0;
                size_t i = 0;
                while (i < name.size() &&
                       name[i] >= '0' && name[i] <= '9') {
                    v = v * 10 + (name[i] - '0');
                    ++i;
                }
                if (i == 0 || i >= name.size() || name[i] != '_') continue;
                if (v > max_idx) max_idx = v;
            }
            next_index = max_idx + 1;
        }

        const std::string command_tag = "render_basetempo";
        const std::filesystem::path batch_folder =
            queue_root /
            (std::to_string(next_index) + "_" + command_tag);

        int pad_width = 1;
        for (int n = static_cast<int>(bpm_values.size());
             n >= 10; n /= 10) ++pad_width;
        if (pad_width > 9) pad_width = 9;

        const std::vector<GuiTransientMarker> base_transients =
            app.transientmarkers.markers();
        std::vector<int64_t> base_transient_frames;
        for (const auto& t : base_transients) {
            if (t.disabled) continue;
            base_transient_frames.push_back(t.effective_frame());
        }

        std::vector<RenderRequest> reqs;
        reqs.reserve(bpm_values.size());
        int seq = 1;
        for (int bpm : bpm_values) {
            const auto computed = compute_base_tempo_scale(
                duration_seconds, owner.bpm_beats, bpm);
            if (!computed) {
                std::fprintf(stderr,
                    "warptempo_gui: render-basetempo: rejected cell "
                    "bpm=%d (duration=%.6f, beats=%d)\n",
                    bpm, duration_seconds, owner.bpm_beats);
                continue;
            }

            std::vector<GuiWarpMarker> cell_markers = base_markers;
            cell_markers[owner_idx].tempo_base  = computed->base_tempo;
            cell_markers[owner_idx].tempo_scale.clear();

            std::vector<std::pair<std::string, std::string>>
                cell_settings = app.settings_passthrough;
            char scale_buf[32];
            std::snprintf(scale_buf, sizeof(scale_buf),
                          "%.6f", computed->scale);
            bool found_scale = false;
            for (auto& kv : cell_settings) {
                if (kv.first == "scale") {
                    kv.second = scale_buf;
                    found_scale = true;
                    break;
                }
            }
            if (!found_scale) {
                cell_settings.emplace_back("scale", scale_buf);
            }

            char num_buf[16];
            std::snprintf(num_buf, sizeof(num_buf),
                          "%0*d", pad_width, seq);
            char rest_buf[96];
            std::snprintf(rest_buf, sizeof(rest_buf),
                          "_bpm=%d;basetempo=%.2f;scale=%.6f",
                          bpm, computed->base_tempo, computed->scale);
            std::string basename = num_buf;
            basename += rest_buf;

            RenderRequest req;
            req.source_audio_path    = app.source_audio_path;
            req.markers              = std::move(cell_markers);
            req.transients           = base_transients;
            req.transient_frames     = base_transient_frames;
            req.settings_passthrough = std::move(cell_settings);
            req.batch_folder         = batch_folder.string();
            req.batch_basename       = std::move(basename);
            reqs.push_back(std::move(req));
            ++seq;
        }

        if (reqs.empty()) {
            std::fprintf(stderr,
                "warptempo_gui: render-basetempo: no valid cells; "
                "nothing to render\n");
            return;
        }

        std::filesystem::create_directories(batch_folder, ec);
        if (ec) {
            std::fprintf(stderr,
                "warptempo_gui: render-basetempo: could not create "
                "'%s': %s\n",
                batch_folder.string().c_str(), ec.message().c_str());
            return;
        }

        const int total = static_cast<int>(reqs.size());
        const auto result = run_render_batch(reqs, "basetempo");
        if (result.cancelled) {
            std::fprintf(stderr,
                "warptempo_gui: rendered %d of %d entries (cancelled)\n",
                result.rendered, total);
        } else {
            std::fprintf(stderr,
                "warptempo_gui: rendered %d of %d entries into %s\n",
                result.rendered, total,
                batch_folder.filename().string().c_str());
        }
        return;
    }

    // Chunk W: Ctrl+Alt+C commits the displayed render's markers
    // and transients into authoring memory. Single cross-file undo
    // entry; both warp_dirty and transient_dirty are recomputed.
    // After the commit succeeds: render-view exits, the parked
    // source audio is restored, and <source_parent>/renders/ is
    // recursively wiped — by definition the user has chosen one
    // render's parameters as the new baseline, so the prior batch
    // outputs are stale and shouldn't accumulate. Silent no-op
    // outside render-view.
    if (ctrl && alt && !shift &&
        (keysym == XK_c || keysym == XK_C)) {
        if (!app.render_view_enabled) return;
        if (app.render_view_index < 0) return;

        // Addendum 3: app.render_view_markers / _transients are now
        // render-domain (loaded from .renderwarpmarkers /
        // .rendertransientmarkers for display). The commit promotes
        // the render's *source-domain*
        // markers into authoring memory, so reload them from the
        // adjacent .warpmarkers / .transientmarkers sidecars at commit
        // time. Failure to read the source-domain warpmarkers aborts —
        // committing render-domain values into authoring would corrupt
        // the source coordinate system.
        const auto& cur_e =
            app.render_view_list[app.render_view_index];
        std::vector<GuiWarpMarker>    src_warp;
        std::vector<GuiTransientMarker> src_trans;
        {
            const std::filesystem::path wm =
                cur_e.batch_folder / (cur_e.basename + ".warpmarkers");
            GuiWarpMarkers m;
            if (!m.load(wm.string())) {
                std::fprintf(stderr,
                    "warptempo_gui: render-view: commit aborted, failed "
                    "to load %s\n", wm.string().c_str());
                return;
            }
            src_warp = m.markers();
        }
        {
            const std::filesystem::path tm = cur_e.batch_folder /
                (cur_e.basename + ".transientmarkers");
            std::error_code ec;
            if (std::filesystem::exists(tm, ec)) {
                GuiTransientMarkers t;
                if (t.load(tm.string())) {
                    src_trans = t.markers();
                }
                // Load failure: treat as empty transients (the
                // load() call already logged its own diagnostics).
            }
        }

        std::vector<GuiWarpMarker>    warp_pre  = app.warpmarkers.markers();
        std::vector<GuiTransientMarker> trans_pre = app.transientmarkers.markers();
        const int                 hint_last = app.last_selected_marker;

        app.warpmarkers.markers_mut()    = std::move(src_warp);
        app.transientmarkers.markers_mut() = std::move(src_trans);
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        // Brief J.2 Section 4: the active tab's per-mode slots
        // referenced the OLD app.warpmarkers/transients we just
        // replaced. Clear them so restore_source_audio loads
        // empty into the live pair (and so a later mode flip
        // doesn't surface stale indices).
        {
            ViewState& t = (app.active_tab == 'B') ? app.tab_b
                                                   : app.tab_a;
            t.warp_selected.clear();
            t.warp_last_selected      = -1;
            t.transient_selected.clear();
            t.transient_last_selected = -1;
        }

        undo.push_undo_both(std::move(warp_pre), std::move(trans_pre),
                       'W', OpKind::Other, hint_last);
        undo.recompute_dirty();

        // Brief X.3: if the displayed render's basename carries a
        // `scale=<float>` token (BPM-sweep render filenames do; iter
        // and queue render filenames don't), extract the float and
        // overwrite (or append) the `scale` entry in
        // app.settings_passthrough. Settings has no undo by
        // convention; this mutation is permanent until the next
        // Ctrl+S overwrites or the user manually edits the file.
        // Conservative parse: any failure logs and skips, leaving
        // markers+transients commit unaffected.
        {
            const std::string& bn = cur_e.basename;
            const auto sp = bn.find("scale=");
            if (sp != std::string::npos) {
                const size_t value_start = sp + 6;
                size_t value_end = bn.size();
                for (size_t i = value_start; i < bn.size(); ++i) {
                    if (bn[i] == ';') { value_end = i; break; }
                    if (i + 4 <= bn.size() &&
                        bn.compare(i, 4, ".wav") == 0) {
                        value_end = i;
                        break;
                    }
                }
                const std::string val_s =
                    bn.substr(value_start, value_end - value_start);
                double parsed = 0.0;
                bool   ok     = false;
                if (!val_s.empty()) {
                    try {
                        size_t consumed = 0;
                        parsed = std::stod(val_s, &consumed);
                        ok = (consumed == val_s.size()) &&
                             std::isfinite(parsed);
                    } catch (...) {
                        ok = false;
                    }
                }
                if (!ok) {
                    std::fprintf(stderr,
                        "warptempo_gui: render-view: could not parse "
                        "scale from basename '%s'; settings unchanged\n",
                        bn.c_str());
                } else {
                    char fmt_buf[32];
                    std::snprintf(fmt_buf, sizeof(fmt_buf),
                                  "%.6f", parsed);
                    bool found_scale = false;
                    for (auto& kv : app.settings_passthrough) {
                        if (kv.first == "scale") {
                            kv.second = fmt_buf;
                            found_scale = true;
                            break;
                        }
                    }
                    if (!found_scale) {
                        app.settings_passthrough.emplace_back(
                            "scale", fmt_buf);
                    }
                }
            }
        }

        const std::filesystem::path src(app.source_audio_path);
        std::filesystem::path src_parent = src.parent_path();
        if (src_parent.empty()) src_parent = std::filesystem::path(".");
        const std::filesystem::path renders_root =
            src_parent / "renders";

        render_view.restore_source_audio();
        app.render_view_enabled = false;
        app.render_view_list.clear();
        app.render_view_markers.clear();
        app.render_view_transients.clear();
        app.render_view_index             = -1;
        app.render_view_src_F_begin       = 0;
        app.render_view_src_F_end         = 0;
        app.last_render_view_path.clear();

        std::error_code ec;
        if (std::filesystem::is_directory(renders_root, ec)) {
            std::filesystem::remove_all(renders_root, ec);
            if (ec) {
                std::fprintf(stderr,
                    "warptempo_gui: render-view: failed to wipe "
                    "%s: %s\n",
                    renders_root.string().c_str(),
                    ec.message().c_str());
            }
        }

        std::fprintf(stderr,
            "warptempo_gui: render-view: committed render and wiped "
            "renders/\n");
        gui.invalidate_region(0, 0, app.width, app.height);
        return;
    }

    // Ctrl+Shift+Alt+T: clear every transient marker (undoable).
    // Checked before Ctrl+Alt+T so the more-specific binding wins.
    if (ctrl && shift && alt &&
        (keysym == XK_t || keysym == XK_T)) {
        transients.clear_all_transients();
        return;
    }

    // Ctrl+Alt+T: run transient detection (with a confirmation dialog
    // when there's already a prior detection in the list).
    if (ctrl && alt && !shift &&
        (keysym == XK_t || keysym == XK_T)) {
        transients.detect_transients();
        return;
    }

    // Space-bar is modifier-independent.
    if (keysym == XK_space) { toggle_playback(); return; }

    // Shift+<digit> selects a playback speed. Shift+0 is 1.00, Shift+1
    // is 0.10, Shift+9 is 0.90. Applies immediately whether or not
    // playback is active — the audio callback picks up the new atomic
    // on the next buffer.
    if (shift && !ctrl) {
        switch (keysym) {
        case XK_0: set_playback_speed(1.0f); return;
        case XK_1: set_playback_speed(0.1f); return;
        case XK_2: set_playback_speed(0.2f); return;
        case XK_3: set_playback_speed(0.3f); return;
        case XK_4: set_playback_speed(0.4f); return;
        case XK_5: set_playback_speed(0.5f); return;
        case XK_6: set_playback_speed(0.6f); return;
        case XK_7: set_playback_speed(0.7f); return;
        case XK_8: set_playback_speed(0.8f); return;
        case XK_9: set_playback_speed(0.9f); return;
        default: break;
        }
    }

    // Ctrl+Z undo / Ctrl+Shift+Z redo. Placed before the XK_s save
    // handling so modifier dispatch reads left-to-right in the source.
    // Both are silent no-ops when their respective stack is empty.
    if (ctrl && keysym == XK_z) {
        if (shift) undo.do_redo();
        else       undo.do_undo();
        return;
    }

    // `t` (no modifiers) toggles transient mode globally. Brief
    // J.2: render-view shares the global active_mode flag, so a
    // single handler serves both views. Render-view inherits the
    // engine / transients_enabled precondition checks from
    // toggle_active_mode.
    if (keysym == XK_t && !ctrl && !shift && !alt) {
        tab_mode.toggle_active_mode();
        return;
    }

    // V.B `i` (no modifiers) toggles iteration mode in warp. Silent
    // no-op in transient mode (transient flags carry no tempo to
    // iterate). The editor-active branch above already swallows any
    // keystroke while a popup edit is in flight, so this code only
    // runs with no active editor. Toggling repaints the top strip
    // so iteration popups appear or vanish in one frame.
    if (keysym == XK_i && !ctrl && !shift && !alt) {
        if (app.active_mode == 'W') {
            // Brief X.2: mutual exclusion. Toggling iter ON forces
            // BPM mode off; toggling iter OFF leaves BPM untouched.
            const bool turning_on = !app.iteration_mode_enabled;
            if (turning_on && app.bpm_mode_enabled) {
                app.bpm_mode_enabled = false;
            }
            app.iteration_mode_enabled = !app.iteration_mode_enabled;
            clear_hover_popup();
            viewport.invalidate_top_strip();
        }
        return;
    }
    // V.B Shift+I: bulk-clear every marker's iter values AND exit
    // iteration mode in one keystroke ("stop authoring this mode").
    // Only fires while iteration mode is on; otherwise silent no-op.
    if (keysym == XK_i && !ctrl && shift && !alt) {
        if (app.active_mode == 'W' && app.iteration_mode_enabled) {
            flag_editor.bulk_clear_iter_values();
            app.iteration_mode_enabled = false;
            viewport.invalidate_top_strip();
        }
        return;
    }

    // Brief X.2 `m` (no modifiers): toggle BPM mode in warp. Silent
    // no-op in transient mode. Mutual exclusion with iter mode is
    // handled inside enter_bpm_mode.
    if (keysym == XK_m && !ctrl && !shift && !alt) {
        if (app.active_mode == 'W') {
            if (app.bpm_mode_enabled) {
                flag_editor.exit_bpm_mode();
            } else {
                flag_editor.enter_bpm_mode();
            }
        }
        return;
    }
    // Brief X.2 Shift+M: bulk-clear every marker's BPM values AND
    // exit BPM mode in one keystroke ("stop authoring this mode").
    // Only fires while BPM mode is on; otherwise silent no-op.
    if (keysym == XK_m && !ctrl && shift && !alt) {
        if (app.active_mode == 'W' && app.bpm_mode_enabled) {
            flag_editor.bulk_clear_bpm_values();
            flag_editor.exit_bpm_mode();
        }
        return;
    }

    // Chunk W: plain `r` toggles render analysis mode. Source audio
    // must be loaded; otherwise silent no-op (nothing to base the
    // renders folder lookup on). Toggle-on enumerates the renders
    // folder and loads either the last-displayed render (if its
    // path is still in the list) or the first entry; an empty
    // enumeration aborts the toggle. Iteration mode is forcibly
    // disabled on entry per the chunk W brief; the prior value is
    // not restored on toggle-off — the user re-enables it
    // explicitly if desired.
    if (keysym == XK_r && !ctrl && !shift && !alt) {
        if (app.source_audio_path.empty()) return;
        if (app.loading) return;
        if (!app.render_view_enabled) {
            std::vector<AppState::RenderViewEntry> list =
                render_view.enumerate_render_view_list();
            if (list.empty()) {
                std::fprintf(stderr,
                    "warptempo_gui: render-view: no renders found "
                    "under %s/renders/\n",
                    std::filesystem::path(app.source_audio_path)
                        .parent_path().string().c_str());
                return;
            }
            // Brief F Section 4: migrate persisted selection from
            // the prior render-view session (still on the old
            // app.render_view_list) into the freshly enumerated
            // list, keyed by wav_path. Entries that disappeared
            // since last session simply lose their persisted state;
            // newly added entries start with default-empty
            // persistence (no match → load_render_view_at clears).
            if (!app.render_view_list.empty()) {
                std::map<std::string,
                    AppState::RenderViewEntry*> prior;
                for (auto& pe : app.render_view_list) {
                    prior[pe.wav_path.string()] = &pe;
                }
                for (auto& ne : list) {
                    auto it = prior.find(ne.wav_path.string());
                    if (it == prior.end()) continue;
                    const auto& src = *it->second;
                    ne.state           = src.state;
                    ne.persisted_size  = src.persisted_size;
                    ne.persisted_mtime = src.persisted_mtime;
                }
            }
            int target = 0;
            if (!app.last_render_view_path.empty()) {
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i].wav_path.string() ==
                        app.last_render_view_path) {
                        target = static_cast<int>(i);
                        break;
                    }
                }
            }
            app.render_view_src_sr    = audio.sample_rate();
            app.render_view_src_total = audio.total_frames();
            app.render_view_list      = std::move(list);
            app.iteration_mode_enabled = false;
            // Brief X.2: BPM mode is force-off on render-view entry,
            // mirroring iter. Stored values persist (in-memory only,
            // never serialized) and re-appear if the user toggles
            // BPM mode back on after exiting render-view.
            app.bpm_mode_enabled       = false;
            app.render_view_enabled    = true;
            // Brief J.2: render-view shares the global active_mode
            // flag, so the user's chosen mode carries across the
            // view-domain transition without per-entry restore.
            if (!render_view.load_render_view_at(target)) {
                app.render_view_enabled = false;
                app.render_view_list.clear();
            }
        } else {
            // Capture the just-viewed render's zoom/viewport/playhead
            // before restoring source-audio state. Not done on the
            // Ctrl+Alt+C commit path — the renders folder is wiped
            // immediately after commit, so the write would be lost.
            if (app.render_view_index >= 0 &&
                app.render_view_index <
                    static_cast<int>(app.render_view_list.size())) {
                render_view.write_rendersettings_for(
                    app.render_view_list[app.render_view_index]);
            }
            // Brief F Section 4: stash the live selection onto
            // the active entry so the next toggle-on can restore
            // it (gated by the wav's stat tuple still matching).
            // render_view_list is intentionally NOT cleared here
            // — re-entry migrates its persisted_* fields into the
            // freshly enumerated list.
            render_view.stash_render_view_selection_to_active_entry();
            render_view.restore_source_audio();
            app.render_view_enabled = false;
            app.render_view_markers.clear();
            app.render_view_transients.clear();
            app.render_view_index             = -1;
            app.render_view_src_F_begin       = 0;
            app.render_view_src_F_end         = 0;
        }
        return;
    }

    // XLookupKeysym with index 0 returns the unshifted keysym, so a
    // Shift+letter press arrives as the lowercase XK_* with ShiftMask in
    // mods — disambiguate via the `shift` bool, not uppercase keysyms.
    if (keysym == XK_s) {
        if (ctrl)                          save_markers();
        else if (app.active_mode == 'T')   transients.drop_transient_at_playhead();
        else if (shift)                    warpops.drop_inherit_marker_at_playhead();
        else                               warpops.drop_marker_at_playhead();
        return;
    }
    // Shift+P: toggle inherit (warp only). Plain `p` is unbound.
    if (keysym == XK_p && !ctrl && !alt && shift) {
        if (app.active_mode == 'T') return;
        warpops.toggle_inherits();
        return;
    }
    // Ctrl+D: toggle disabled (warp + transient). Plain `d` and Shift+D are unbound.
    if (keysym == XK_d && ctrl && !alt && !shift) {
        if (app.active_mode == 'T') transients.toggle_transient_disabled();
        else                        warpops.toggle_disabled();
        return;
    }
    if (keysym == XK_Delete && !ctrl) {
        if (app.active_mode == 'T') {
            transients.delete_selected_transient();
            return;
        }
        if (shift) warpops.force_delete_selected_marker();
        else       warpops.delete_selected_marker();
        return;
    }

    // Ctrl+Tab toggles A/B navigational tabs. Stops playback, saves
    // current viewport/zoom/playhead to the leaving tab, restores the
    // target tab. Does not mark the document dirty.
    if (ctrl && !shift && keysym == XK_Tab) {
        tab_mode.switch_active_tab_to(app.active_tab == 'A' ? 'B' : 'A');
        return;
    }

    if (keysym == XK_Tab && !shift) { selection.select_next_marker(); return; }
    if (keysym == XK_Tab && shift)  { selection.select_prev_marker(); return; }
    if (keysym == XK_ISO_Left_Tab)  { selection.select_prev_marker(); return; }

    // Tempo nudge. Ctrl+Up / Ctrl+Down only. Bare `=` / `-` were the
    // previous binding; they now zoom (see below) so the keyboard has
    // a symbol-key alias for the bare Up/Down zoom chord.
    if (ctrl && !shift && !alt && keysym == XK_Up) {
        warpops.adjust_tempo(+0.01); return;
    }
    if (ctrl && !shift && !alt && keysym == XK_Down) {
        warpops.adjust_tempo(-0.01); return;
    }
    if (keysym == XK_equal && !shift && !ctrl && !alt) {
        viewport.zoom_in(); return;
    }
    if (keysym == XK_minus && !shift && !ctrl && !alt) {
        viewport.zoom_out(); return;
    }

    // `l` (no modifier) clears any b= / e= flags. `Shift+L` clears the
    // selection set (UI-only — no dirty, no playhead move).
    if (keysym == XK_l && !ctrl) {
        if (shift) selection.clear_selection();
        else       warpops.clear_trim();
        return;
    }

    // `j` jumps the selected set to the playhead, anchored on
    // last_selected_marker. All-or-nothing clamp check.
    if (keysym == XK_j && !shift && !ctrl) {
        if (app.active_mode == 'T') transients.jump_transient_selection_to_playhead();
        else                        warpops.jump_selection_to_playhead();
        return;
    }

    // Chunk W: Shift+Left / Shift+Right navigates the render-view
    // list with wraparound. Active only when render_view_enabled is
    // true; in source-view these chords fall through to the normal
    // playhead-by-pixel handler in the switch below. Wraparound
    // mirrors the brief: Shift+Right past the end loops to index 0,
    // Shift+Left before index 0 loops to the last entry.
    if (app.render_view_enabled && shift && !ctrl && !alt &&
        (keysym == XK_Left || keysym == XK_Right)) {
        const int n = static_cast<int>(app.render_view_list.size());
        if (n <= 0) return;
        int next = app.render_view_index;
        if (keysym == XK_Left)  next = (next - 1 + n) % n;
        else                    next = (next + 1) % n;
        // Capture the outgoing render's live zoom/viewport/playhead
        // before swapping.
        if (app.render_view_index >= 0 &&
            app.render_view_index <
                static_cast<int>(app.render_view_list.size())) {
            render_view.write_rendersettings_for(
                app.render_view_list[app.render_view_index]);
        }
        // Brief F Section 4: stash the outgoing entry's
        // selection so re-navigating back later (in the same
        // session) restores it. load_render_view_at then loads
        // the destination's own persisted state if its stat tuple
        // still matches; otherwise leaves selection empty.
        render_view.stash_render_view_selection_to_active_entry();
        render_view.load_render_view_at(next);
        return;
    }

    // Ctrl+Left / Ctrl+Right: nudge selected markers by one pixel.
    if (ctrl && !shift && keysym == XK_Left) {
        if (app.active_mode == 'T') transients.nudge_selected_transients(-1);
        else                        warpops.nudge_selected_markers(-1);
        return;
    }
    if (ctrl && !shift && keysym == XK_Right) {
        if (app.active_mode == 'T') transients.nudge_selected_transients(+1);
        else                        warpops.nudge_selected_markers(+1);
        return;
    }

    // Bare-key dispatch. Every modifier-gated handler above this point
    // returns on match, so by the time we reach here, any modifier being
    // held means the chord had no binding and should be a silent no-op
    // — never fall through into a bare binding (e.g. Ctrl+Shift+Alt+E
    // must not toggle end-time via XK_e).
    if (!ctrl && !shift && !alt) {
        switch (keysym) {
        case XK_Escape: /* top-level Escape is a no-op (chunk Q) */ break;
        case XK_Left:   stop_playback_if_playing();
                        viewport.move_playhead_pixels(-1);         break;
        case XK_Right:  stop_playback_if_playing();
                        viewport.move_playhead_pixels(+1);         break;
        case XK_Up:     viewport.zoom_in();                        break;
        case XK_Down:   viewport.zoom_out();                       break;
        case XK_f: {
            const bool was_off = !app.follow_mode;
            app.follow_mode = !app.follow_mode;
            if (was_off && app.follow_mode &&
                playback.is_playing()) {
                playback.resync_predictor();
            }
            break;
        }
        case XK_c:      viewport.center_viewport_on_playhead();    break;
        case XK_Home:   stop_playback_if_playing();
                        viewport.move_playhead_to(viewport.trim_begin_sample()); break;
        case XK_End:    stop_playback_if_playing();
                        viewport.move_playhead_to(viewport.trim_end_sample() - 1); break;
        case XK_b:      if (app.active_mode == 'T') transients.toggle_transient_begin_time();
                        else                        warpops.toggle_begin_time();
                        break;
        case XK_e:      if (app.active_mode == 'T') transients.toggle_transient_end_time();
                        else                        warpops.toggle_end_time();
                        break;
        // TODO: growing binding set will want an in-GUI help overlay.
        default: break;
        }
    }
}
