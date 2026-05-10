#include "input_handler.h"

#include "render.h"
#include "render_pipeline.h"
#include "text_editor.h"
#include "warpmarkers.h"

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
//     `selection`, `undo`, `warpops`, `phase resets`, `flag_editor`,
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
//     adjust_tempo, clear_trim, nudge_*, jump_*, drop_phase_reset_at_playhead,
//     toggle_phase_reset_*, detect_phase_resets, clear_all_phase_resets,
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

void GuiInputHandler::on_key(GuiKey key, GuiInputState mods) {
    if constexpr (kDebugPerf) {
        app.last_input_event_time = std::chrono::steady_clock::now();
    }
    const bool ctrl  = mods.ctrl;
    const bool shift = mods.shift;
    const bool alt   = mods.alt;

    // Bottom-strip prompt owns input while active. Only the prompt's
    // own response keys do anything; everything else is swallowed so
    // marker edits / playback / viewport keys cannot sneak in while
    // the prompt is up. Letter keys are case-insensitive; Delete and
    // Escape map to sentinel chars '\x7f' and '\x1b' so they participate
    // in the same vector<char> match as letter responses.
    if (app.prompt.active) {
        char k = 0;
        if (key >= GuiKeys::A && key <= GuiKeys::Z) {
            k = static_cast<char>('a' + (key - GuiKeys::A));
        } else if (key == GuiKeys::Delete) {
            k = '\x7f';
        } else if (key == GuiKeys::Escape) {
            k = '\x1b';
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
        if (ctrl && !shift && !alt && key == GuiKeys::Q) {
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
            app.top_flag_editor, key, mods);
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
    //   - t (no mods)            → toggle warp/phase reset sub-view (Brief F)
    //   - Ctrl+Q / Ctrl+W        → close-prompt routing (Brief F)
    //   - Up/Down (no mods)      → zoom in/out (Brief S.2)
    //   - =/- (no mods)          → zoom in/out symbol-key alias (Brief S.2)
    //   - f (no mods)          → follow mode toggle
    if (app.render_view_enabled) {
        const bool is_r =
            (key == GuiKeys::R && !ctrl && !shift && !alt);
        const bool is_nav =
            ((key == GuiKeys::Left || key == GuiKeys::Right) &&
             shift && !ctrl && !alt);
        const bool is_commit =
            (ctrl && alt && !shift &&
             key == GuiKeys::C);
        const bool is_playback = (key == GuiKeys::Space);
        const bool is_scrub =
            ((key == GuiKeys::Left || key == GuiKeys::Right) &&
             !ctrl && !shift && !alt);
        const bool is_jump =
            ((key == GuiKeys::Home || key == GuiKeys::End) &&
             !ctrl && !shift && !alt);
        const bool is_esc = (key == GuiKeys::Escape);
        const bool is_sub_view_toggle =
            (key == GuiKeys::P && !ctrl && !shift && !alt);
        const bool is_ctrl_q =
            (ctrl && !shift && !alt && key == GuiKeys::Q);
        const bool is_ctrl_w =
            (ctrl && !shift && !alt && key == GuiKeys::W);
        const bool is_zoom =
            ((key == GuiKeys::Up || key == GuiKeys::Down) &&
             !ctrl && !shift && !alt);
        const bool is_zoom_symbol =
            ((key == GuiKeys::Equal || key == GuiKeys::Minus) &&
             !ctrl && !shift && !alt);
        const bool is_follow =
            (key == GuiKeys::F && !ctrl && !shift && !alt);
        if (!(is_r || is_nav || is_commit || is_playback ||
              is_scrub || is_jump || is_esc ||
              is_sub_view_toggle || is_ctrl_q || is_ctrl_w ||
              is_zoom || is_zoom_symbol || is_follow)) {
            return;
        }
    }

    // Esc during a render-all run requests cancellation between
    // entries. Only fires while the queue walker is active; outside
    // that window Esc retains its other meanings (drag-cancel, etc).
    // Mid-engine Esc presses are queued by X and surface here on the
    // next gui.drain_events() — they take effect after the in-flight
    // entry completes (chunk U does not implement mid-engine cancel).
    if (key == GuiKeys::Escape && app.queue_running) {
        app.queue_cancel_requested = true;
        return;
    }

    // Escape during a playhead drag ends the gesture at its current
    // position (no restore — the drag already committed its visible
    // progress per motion event, so there's nothing to revert).
    if (key == GuiKeys::Escape && app.playhead_drag.active) {
        app.playhead_drag = PlayheadDragState{};
        return;
    }

    // Ctrl+Q: quit (via unsaved-work dialog when dirty).
    if (ctrl && !shift && !alt && key == GuiKeys::Q) {
        request_close_or_revert(DialogTrigger::CLOSE_WINDOW);
        return;
    }

    // Ctrl+W: revert to blank state (via unsaved-work dialog when dirty).
    if (ctrl && !shift && !alt && key == GuiKeys::W) {
        request_close_or_revert(DialogTrigger::REVERT_TO_BLANK);
        return;
    }

    // Ctrl+E: snapshot current authoring state into the in-memory
    // render queue. No disk writes; on-disk authoring files are untouched.
    // Settings are not snapshotted per-entry — the queue walker uses
    // the live settings_passthrough at execution time, mirroring the
    // chunk-U convention. (Chunk W: snapshots moved from disk to memory.)
    if (ctrl && !alt && !shift &&
        key == GuiKeys::E) {
        if (app.source_audio_path.empty()) return;
        AppState::QueuedRender q;
        q.source_audio_path = app.source_audio_path;
        q.markers           = app.warpmarkers.markers();
        q.phase_resets        = app.phase_reset_markers.markers();
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
        key == GuiKeys::R) {
        if (app.source_audio_path.empty()) return;

        RenderRequest req;
        req.source_audio_path    = app.source_audio_path;
        req.markers              = app.warpmarkers.markers();
        req.phase_resets           = app.phase_reset_markers.markers();
        req.settings_passthrough = app.settings_passthrough;
        for (const auto& m : app.phase_reset_markers.markers()) {
            if (m.disabled) continue;
            req.phase_reset_frames.push_back(static_cast<int64_t>(
                std::nearbyint(m.time_seconds *
                               static_cast<double>(audio.sample_rate()))));
        }
        // Empty batch_folder/basename selects the source-dir naming
        // convention inside do_render.
        do_render(req);
        return;
    }

    // Ctrl+Alt+E: render the in-memory queue as one batch. Each
    // queued entry produces a sibling .wav (+ .warpmarkers /
    // .phaseresetmarkers when non-empty / .peaks sidecars) inside a fresh
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
        key == GuiKeys::E) {
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
            req.phase_resets           = q.phase_resets;
            req.settings_passthrough = app.settings_passthrough;
            for (const auto& m : q.phase_resets) {
                if (m.disabled) continue;
                req.phase_reset_frames.push_back(static_cast<int64_t>(
                    std::nearbyint(m.time_seconds *
                                   static_cast<double>(audio.sample_rate()))));
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
        key == GuiKeys::I) {
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

        // Snapshot phase resets once — every cell shares the same
        // phase reset configuration, only marker tempo_base values
        // differ across cells.
        const std::vector<GuiPhaseResetMarker> base_phase_resets =
            app.phase_reset_markers.markers();
        std::vector<int64_t> base_phase_reset_frames;
        for (const auto& t : base_phase_resets) {
            if (t.disabled) continue;
            base_phase_reset_frames.push_back(static_cast<int64_t>(
                std::nearbyint(t.time_seconds *
                               static_cast<double>(audio.sample_rate()))));
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
            req.phase_resets           = base_phase_resets;
            req.phase_reset_frames     = base_phase_reset_frames;
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
        key == GuiKeys::M) {
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

        const std::vector<GuiPhaseResetMarker> base_phase_resets =
            app.phase_reset_markers.markers();
        std::vector<int64_t> base_phase_reset_frames;
        for (const auto& t : base_phase_resets) {
            if (t.disabled) continue;
            base_phase_reset_frames.push_back(static_cast<int64_t>(
                std::nearbyint(t.time_seconds *
                               static_cast<double>(audio.sample_rate()))));
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
            req.phase_resets           = base_phase_resets;
            req.phase_reset_frames     = base_phase_reset_frames;
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
    // and phase resets into authoring memory. Single cross-file undo
    // entry; both warp_dirty and phase_reset_dirty are recomputed.
    // After the commit succeeds: render-view exits, the parked
    // source audio is restored, and <source_parent>/renders/ is
    // recursively wiped — by definition the user has chosen one
    // render's parameters as the new baseline, so the prior batch
    // outputs are stale and shouldn't accumulate. Silent no-op
    // outside render-view.
    if (ctrl && alt && !shift &&
        key == GuiKeys::C) {
        if (!app.render_view_enabled) return;
        if (app.render_view_index < 0) return;

        // Addendum 3: app.render_view_markers / _phase_resets are now
        // render-domain (loaded from .renderwarpmarkers /
        // .renderphaseresetmarkers for display). The commit promotes
        // the render's *source-domain*
        // markers into authoring memory, so reload them from the
        // adjacent .warpmarkers / .phaseresetmarkers sidecars at commit
        // time. Failure to read the source-domain warpmarkers aborts —
        // committing render-domain values into authoring would corrupt
        // the source coordinate system.
        const auto& cur_e =
            app.render_view_list[app.render_view_index];
        std::vector<GuiWarpMarker>    src_warp;
        std::vector<GuiPhaseResetMarker> src_trans;
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
                (cur_e.basename + ".phaseresetmarkers");
            std::error_code ec;
            if (std::filesystem::exists(tm, ec)) {
                GuiPhaseResetMarkers t;
                if (t.load(tm.string())) {
                    src_trans = t.markers();
                }
                // Load failure: treat as empty phase resets (the
                // load() call already logged its own diagnostics).
            }
        }

        std::vector<GuiWarpMarker>    warp_pre  = app.warpmarkers.markers();
        std::vector<GuiPhaseResetMarker> trans_pre = app.phase_reset_markers.markers();
        const int                 hint_last = app.last_selected_marker;

        app.warpmarkers.markers_mut()    = std::move(src_warp);
        app.phase_reset_markers.markers_mut() = std::move(src_trans);
        app.selected_markers.clear();
        app.last_selected_marker = -1;
        // Brief J.2 Section 4: the active tab's per-mode slots
        // referenced the OLD app.warpmarkers/phase resets we just
        // replaced. Clear them so restore_source_audio loads
        // empty into the live pair (and so a later mode flip
        // doesn't surface stale indices).
        {
            ViewState& t = (app.active_tab == 'B') ? app.tab_b
                                                   : app.tab_a;
            t.warp_selected.clear();
            t.warp_last_selected      = -1;
            t.phase_reset_selected.clear();
            t.phase_reset_last_selected = -1;
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
        // markers+phase resets commit unaffected.
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
        app.render_view_phase_resets.clear();
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

    // Space-bar is modifier-independent.
    if (key == GuiKeys::Space) { toggle_playback(); return; }

    // Shift+<digit> selects a playback speed. Shift+0 is 1.00, Shift+1
    // is 0.10, Shift+9 is 0.90. Applies immediately whether or not
    // playback is active — the audio callback picks up the new atomic
    // on the next buffer.
    if (shift && !ctrl) {
        switch (key) {
        case GuiKeys::Digit0: set_playback_speed(1.0f); return;
        case GuiKeys::Digit1: set_playback_speed(0.1f); return;
        case GuiKeys::Digit2: set_playback_speed(0.2f); return;
        case GuiKeys::Digit3: set_playback_speed(0.3f); return;
        case GuiKeys::Digit4: set_playback_speed(0.4f); return;
        case GuiKeys::Digit5: set_playback_speed(0.5f); return;
        case GuiKeys::Digit6: set_playback_speed(0.6f); return;
        case GuiKeys::Digit7: set_playback_speed(0.7f); return;
        case GuiKeys::Digit8: set_playback_speed(0.8f); return;
        case GuiKeys::Digit9: set_playback_speed(0.9f); return;
        default: break;
        }
    }

    // Ctrl+Z undo / Ctrl+Shift+Z redo. Placed before the GuiKeys::S save
    // handling so modifier dispatch reads left-to-right in the source.
    // Both are silent no-ops when their respective stack is empty.
    if (ctrl && key == GuiKeys::Z) {
        if (shift) undo.do_redo();
        else       undo.do_undo();
        return;
    }

    // Ctrl+P: copy phase reset placements from a two-warp-marker
    // selection into the session clipboard. W-mode only; phase reset
    // mode is a silent no-op. Off-count selection in W-mode emits a
    // one-line stderr nudge.
    if (key == GuiKeys::P && ctrl && !shift && !alt) {
        if (app.active_mode != 'W') return;
        if (app.selected_markers.size() != 2) {
            std::fprintf(stderr,
                "warptempo_gui: phase_reset copy: select exactly two warp "
                "markers\n");
            return;
        }
        phase_reset_propagate.copy_from_selection();
        return;
    }

    // Ctrl+Alt+P: paste clipboard phase resets onto the destination
    // anchored at the single selected warp marker. W-mode only; phase
    // reset mode is a silent no-op. Empty clipboard is a silent no-op.
    // Opens a confirmation prompt before any mutation.
    if (key == GuiKeys::P && ctrl && !shift && alt) {
        if (app.active_mode != 'W') return;
        if (app.phase_reset_clipboard.empty()) return;
        if (app.selected_markers.size() != 1) {
            std::fprintf(stderr,
                "warptempo_gui: phase_reset paste: select exactly one warp "
                "marker\n");
            return;
        }
        phase_reset_propagate.open_paste_confirmation();
        return;
    }

    // `p` (no modifiers) toggles phase reset mode globally. Brief
    // J.2: render-view shares the global active_mode flag, so a
    // single handler serves both views. Render-view inherits the
    // engine precondition check from toggle_active_mode.
    if (key == GuiKeys::P && !ctrl && !shift && !alt) {
        tab_mode.toggle_active_mode();
        return;
    }

    // V.B `i` (no modifiers) toggles iteration mode in warp. Silent
    // no-op in phase reset mode (phase reset flags carry no tempo to
    // iterate). The editor-active branch above already swallows any
    // keystroke while a popup edit is in flight, so this code only
    // runs with no active editor. Toggling repaints the top strip
    // so iteration popups appear or vanish in one frame.
    if (key == GuiKeys::I && !ctrl && !shift && !alt) {
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
    if (key == GuiKeys::I && !ctrl && shift && !alt) {
        if (app.active_mode == 'W' && app.iteration_mode_enabled) {
            flag_editor.bulk_clear_iter_values();
            app.iteration_mode_enabled = false;
            viewport.invalidate_top_strip();
        }
        return;
    }

    // Brief X.2 `m` (no modifiers): toggle BPM mode in warp. Silent
    // no-op in phase reset mode. Mutual exclusion with iter mode is
    // handled inside enter_bpm_mode.
    if (key == GuiKeys::M && !ctrl && !shift && !alt) {
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
    if (key == GuiKeys::M && !ctrl && shift && !alt) {
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
    if (key == GuiKeys::R && !ctrl && !shift && !alt) {
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
            app.render_view_phase_resets.clear();
            app.render_view_index             = -1;
            app.render_view_src_F_begin       = 0;
            app.render_view_src_F_end         = 0;
        }
        return;
    }

    // The platform boundary case-folds letters and delivers the
    // unshifted GuiKey, so a Shift+letter press arrives as the lowercase
    // GuiKeys::* with mods.shift set — disambiguate via the `shift` bool.
    if (key == GuiKeys::S) {
        if (ctrl)                          save_markers();
        else if (app.active_mode == 'P')   phase_resets.drop_phase_reset_at_playhead();
        else if (shift)                    warpops.drop_inherit_marker_at_playhead();
        else                               warpops.drop_marker_at_playhead();
        return;
    }
    // Shift+P: toggle inherit (warp only).
    if (key == GuiKeys::P && !ctrl && !alt && shift) {
        if (app.active_mode == 'P') return;
        warpops.toggle_inherits();
        return;
    }
    // Ctrl+D: toggle disabled (warp + phase reset). Plain `d` and Shift+D are unbound.
    if (key == GuiKeys::D && ctrl && !alt && !shift) {
        if (app.active_mode == 'P') phase_resets.toggle_phase_reset_disabled();
        else                        warpops.toggle_disabled();
        return;
    }
    if (key == GuiKeys::Delete && !ctrl) {
        if (app.active_mode == 'P') {
            phase_resets.delete_selected_phase_reset();
            return;
        }
        if (shift) warpops.force_delete_selected_marker();
        else       warpops.delete_selected_marker();
        return;
    }

    // Ctrl+Tab toggles A/B navigational tabs. Stops playback, saves
    // current viewport/zoom/playhead to the leaving tab, restores the
    // target tab. Does not mark the document dirty.
    if (ctrl && !shift && key == GuiKeys::Tab) {
        tab_mode.switch_active_tab_to(app.active_tab == 'A' ? 'B' : 'A');
        return;
    }

    if (key == GuiKeys::Tab && !shift) { selection.select_next_marker(); return; }
    if (key == GuiKeys::Tab && shift)  { selection.select_prev_marker(); return; }
    if (key == GuiKeys::IsoLeftTab)  { selection.select_prev_marker(); return; }

    // Tempo nudge. Ctrl+Up / Ctrl+Down only. Bare `=` / `-` were the
    // previous binding; they now zoom (see below) so the keyboard has
    // a symbol-key alias for the bare Up/Down zoom chord.
    if (ctrl && !shift && !alt && key == GuiKeys::Up) {
        warpops.adjust_tempo(+0.01); return;
    }
    if (ctrl && !shift && !alt && key == GuiKeys::Down) {
        warpops.adjust_tempo(-0.01); return;
    }
    if (key == GuiKeys::Equal && !shift && !ctrl && !alt) {
        viewport.zoom_in(); return;
    }
    if (key == GuiKeys::Minus && !shift && !ctrl && !alt) {
        viewport.zoom_out(); return;
    }

    // `l` (no modifier) clears any b= / e= flags. `Shift+L` clears the
    // selection set (UI-only — no dirty, no playhead move).
    if (key == GuiKeys::L && !ctrl) {
        if (shift) selection.clear_selection();
        else       warpops.clear_trim();
        return;
    }

    // `j` jumps the selected set to the playhead, anchored on
    // last_selected_marker. All-or-nothing clamp check.
    if (key == GuiKeys::J && !shift && !ctrl) {
        if (app.active_mode == 'P') phase_resets.jump_phase_reset_selection_to_playhead();
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
        (key == GuiKeys::Left || key == GuiKeys::Right)) {
        const int n = static_cast<int>(app.render_view_list.size());
        if (n <= 0) return;
        int next = app.render_view_index;
        if (key == GuiKeys::Left)  next = (next - 1 + n) % n;
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
    if (ctrl && !shift && key == GuiKeys::Left) {
        if (app.active_mode == 'P') phase_resets.nudge_selected_phase_resets(-1);
        else                        warpops.nudge_selected_markers(-1);
        return;
    }
    if (ctrl && !shift && key == GuiKeys::Right) {
        if (app.active_mode == 'P') phase_resets.nudge_selected_phase_resets(+1);
        else                        warpops.nudge_selected_markers(+1);
        return;
    }

    // Bare-key dispatch. Every modifier-gated handler above this point
    // returns on match, so by the time we reach here, any modifier being
    // held means the chord had no binding and should be a silent no-op
    // — never fall through into a bare binding (e.g. Ctrl+Shift+Alt+E
    // must not toggle end-time via GuiKeys::E).
    if (!ctrl && !shift && !alt) {
        switch (key) {
        case GuiKeys::Escape: /* top-level Escape is a no-op (chunk Q) */ break;
        case GuiKeys::Left:   stop_playback_if_playing();
                        viewport.move_playhead_pixels(-1);         break;
        case GuiKeys::Right:  stop_playback_if_playing();
                        viewport.move_playhead_pixels(+1);         break;
        case GuiKeys::Up:     viewport.zoom_in();                        break;
        case GuiKeys::Down:   viewport.zoom_out();                       break;
        case GuiKeys::F: {
            const bool was_off = !app.follow_mode;
            app.follow_mode = !app.follow_mode;
            if (was_off && app.follow_mode &&
                playback.is_playing()) {
                playback.resync_predictor();
            }
            break;
        }
        case GuiKeys::C:      viewport.apply_zoom_change(0);
                        viewport.center_viewport_on_playhead();    break;
        case GuiKeys::Home:   stop_playback_if_playing();
                        viewport.move_playhead_to(viewport.trim_begin_sample()); break;
        case GuiKeys::End:    stop_playback_if_playing();
                        viewport.move_playhead_to(viewport.trim_end_sample() - 1); break;
        // b / e set the warp trim begin / end flags. In W-mode they target
        // the selected warp marker (toggle-off on re-press, auto-replace,
        // auto-swap on cross, refuse equal-frame). In P-mode they target
        // the nearest warp marker at or before / at or after the selected
        // phase reset's time, leaving the phase reset selection untouched —
        // a one-keystroke "ensure trim contains this phase reset" gesture.
        // Both modes require exactly one selection.
        case GuiKeys::B:
            if (app.active_mode == 'P') warpops.set_begin_from_phase_reset_selection();
            else                        warpops.toggle_begin_time();
            break;
        case GuiKeys::E:
            if (app.active_mode == 'P') warpops.set_end_from_phase_reset_selection();
            else                        warpops.toggle_end_time();
            break;
        // TODO: growing binding set will want an in-GUI help overlay.
        default: break;
        }
    }
}

// X.7.8b-2: shared wheel handler. Verbatim from the lambda at the original
// main.cpp:1444 — only difference is the captured viewport / playhead
// helpers now resolve through this struct's reference members.
void GuiInputHandler::handle_wheel(unsigned int button,
                                   bool ctrl, bool alt,
                                   bool inside_waveform, bool inside_top) {
    if (!inside_waveform && !inside_top) return;
    if (ctrl && alt) {
        const int64_t step = std::max<int64_t>(
            1, samples_visible(app, audio) / 50);
        viewport.scroll_viewport(button == 4 ? -step : +step);
        return;
    }
    if (ctrl) {
        stop_playback_if_playing();
        viewport.move_playhead_pixels(button == 4 ? -1 : +1);
        return;
    }
    if (alt) {
        const int64_t step = std::max<int64_t>(
            1, samples_visible(app, audio) / 10);
        viewport.scroll_viewport(button == 4 ? -step : +step);
        return;
    }
    if (button == 4) viewport.zoom_out();
    else             viewport.zoom_in();
}

// X.7.8b-2: button-press handler. Verbatim from the lambda at the original
// main.cpp:1483; the captured operation-struct lambdas (begin_drag,
// drop_marker, drop_phase_reset_at_position, set_single_selection, etc.)
// are rewritten to direct method calls on the appropriate operation
// struct ref. The four hit_test_* lambdas are now free functions taking
// (app, audio, ...) explicit args. The handle_wheel lambda is now a
// private method on this struct.
void GuiInputHandler::on_button_press(unsigned int button, int x, int y,
                                      GuiInputState mods) {
    if constexpr (kDebugPerf) {
        app.last_input_event_time = std::chrono::steady_clock::now();
    }
    // Prompt-modal input handling: while the bottom-strip prompt is
    // active, all mouse events are swallowed. Responses go through
    // the keyboard.
    if (app.prompt.active) return;
    if (app.loading || audio.total_frames() <= 0) return;
    const GuiRect area = waveform_area(app);
    const GuiRect top  = top_strip_area(app);
    const bool inside_waveform =
        x >= area.x && x < area.x + area.w &&
        y >= area.y && y < area.y + area.h;
    const bool inside_top =
        x >= top.x && x < top.x + top.w &&
        y >= top.y && y < top.y + top.h;
    const bool ctrl  = mods.ctrl;
    const bool shift = mods.shift;
    const bool alt   = mods.alt;

    // Defensive: a second press during a drag is ignored (left button
    // should still be held down for a drag to exist).
    if (app.drag.active) return;

    // Chunk W: render-view mouse gate. Left-click on a marker line
    // (in the waveform area) or a flag rect (in the top strip)
    // toggles selection and jumps the playhead to the marker;
    // left-click elsewhere in the waveform area positions the
    // playhead (with playback stop) and clears the selection unless
    // Shift is held. All wheel chords (zoom, Alt/Ctrl+Alt pan,
    // Ctrl+wheel playhead-move) are pure viewport / playhead ops and
    // pass through unchanged. Drag-create and top-strip playhead
    // movement are silent no-ops so the read-only invariant on
    // marker state is preserved. Hover-popup motion still runs in
    // the motion handler against render_view_markers.
    if (app.render_view_enabled) {
        if (button == 4 || button == 5) {
            handle_wheel(button, ctrl, alt,
                         inside_waveform, inside_top);
            return;
        }
        if (button != 1) return;
        // Brief F Section 3: in phase reset sub-view, top-strip clicks
        // are silent no-ops (phase resets have no flag rects). Bail
        // before hit-testing so we don't attempt selection bookkeeping
        // on a non-existent flag pack.
        if (app.active_mode == 'P' && inside_top) return;
        // Brief six: capture playback / playhead state at press entry
        // so the four playhead-moving gestures below can reseek the
        // audio device to the new position when playback is active,
        // keeping audio alive across a click. The reseek is gated on
        // sample != playhead_at_click_entry so a marker-click on the
        // marker the playhead is already on doesn't tear down and
        // restart the audio device for nothing.
        const bool was_playing_rv =
            (inside_waveform || inside_top) && playback.is_playing();
        const int64_t playhead_at_click_entry_rv = app.playhead_sample;
        int hit = -1;
        if (inside_waveform)  hit = hit_test_marker_line(app, audio, x);
        else if (inside_top)  hit = hit_test_flag(app, audio, x, y);
        else                  return;
        // Brief J.2 Section 3: live selection lives in the global
        // pair regardless of view domain. active_mode tells us
        // which marker list the indices map to.
        const bool sub_t = (app.active_mode == 'P');
        std::set<int>& sel = app.selected_markers;
        int& last_sel      = app.last_selected_marker;
        const int n = sub_t
            ? static_cast<int>(app.render_view_phase_resets.size())
            : static_cast<int>(app.render_view_markers.size());
        if (hit >= 0 && hit < n) {
            if (shift) {
                auto it = sel.find(hit);
                if (it == sel.end()) {
                    sel.insert(hit);
                    last_sel = hit;
                } else {
                    sel.erase(it);
                    if (last_sel == hit) {
                        last_sel = sel.empty()
                            ? -1
                            : *sel.rbegin();
                    }
                }
            } else {
                sel.clear();
                sel.insert(hit);
                last_sel = hit;
            }
            gui.invalidate_region(0, 0, app.width, app.height);
            const int sr = audio.sample_rate();
            int64_t sample;
            if (sub_t) {
                sample = static_cast<int64_t>(std::nearbyint(
                    app.render_view_phase_resets[hit].time_seconds *
                    static_cast<double>(sr)));
            } else {
                sample = static_cast<int64_t>(std::nearbyint(
                    app.render_view_markers[hit].time_seconds *
                    static_cast<double>(sr)));
            }
            viewport.move_playhead_to(sample);
            // Brief six: keep playback alive across a marker-line /
            // top-strip flag click in render-view. The reseek replaces
            // the previous unconditional press-time stop. Skip when
            // the click landed on the marker the playhead was already
            // on — the move was a no-op, so the reseek would be too.
            if (was_playing_rv && sample != playhead_at_click_entry_rv) {
                playback.play(sample, viewport.trim_end_sample());
            }
            // Brief F Section 2: any waveform-area press starts a
            // playhead-drag gesture. Top-strip flag-click does not.
            if (inside_waveform) {
                app.playhead_drag.active = true;
                app.playhead_drag.was_playing_at_press = was_playing_rv;
                app.playhead_drag.press_marker_idx = hit;
            }
            return;
        }
        // Empty-space click in the waveform area: clear the active
        // sub-view's selection (unless Shift) and move the playhead.
        // Brief F Section 2: also start a playhead-drag gesture so
        // the motion handler's snap logic kicks in.
        if (inside_waveform) {
            if (!shift &&
                (!sel.empty() || last_sel != -1)) {
                sel.clear();
                last_sel = -1;
                gui.invalidate_region(0, 0, app.width, app.height);
            }
            const double spp = current_samples_per_pixel(app, audio);
            int rel = x - area.x;
            if (rel < 0) rel = 0;
            if (rel >= area.w) rel = area.w - 1;
            const int64_t sample =
                app.viewport_start_sample +
                static_cast<int64_t>(std::nearbyint(rel * spp));
            viewport.move_playhead_to(sample);
            // Brief six: empty-space click in render-view keeps
            // playback alive by reseeking to the new position.
            if (was_playing_rv && sample != playhead_at_click_entry_rv) {
                playback.play(sample, viewport.trim_end_sample());
            }
            app.playhead_drag.active = true;
            app.playhead_drag.was_playing_at_press = was_playing_rv;
            app.playhead_drag.press_marker_idx = -1;
        }
        return;
    }

    if (button == 1) {
        // Brief six: the chunk-P-patch-1 unconditional stop is now
        // overridden for waveform clicks. A click in the waveform area
        // that reseats the playhead keeps playback alive — the press
        // sites below reseek the audio device to the new position. A
        // drag (button-1 motion observed while the gesture is active)
        // converts the gesture into a scrub and stops playback once
        // from the on_motion handler. Top-strip clicks still stop
        // playback unconditionally: anything in the top strip could
        // open an editor, so the conservative scope is "stop on any
        // top-strip click." The mental model: attention is signalled
        // by Space (or transport keys), not by mouse press.
        const bool was_playing =
            (inside_waveform || inside_top) && playback.is_playing();
        const int64_t playhead_at_click_entry = app.playhead_sample;
        if (inside_top && was_playing) stop_playback_if_playing();

        // V.A1 / V.B editor: mouse handling.
        //   click inside top strip on the editing target: re-position
        //     cursor at the clicked byte (handled inside enter_*_edit)
        //   click inside top strip on a different popup/flag: switch
        //     target (iter popup wins over the flag below it when
        //     iteration mode is on)
        //   click anywhere else: exit edit (no commit), then fall
        //     through so the click routes through normal handling.
        if (text_editor::is_active(app.top_flag_editor)) {
            if (inside_top) {
                double iter_text_left = -1.0;
                const int iter_hit = hit_test_iter_popup(
                    app, audio, x, y, &iter_text_left);
                if (iter_hit >= 0) {
                    flag_editor.enter_iter_edit(
                        iter_hit, static_cast<double>(x),
                        iter_text_left);
                    return;
                }
                double bpm_text_left = -1.0;
                const int bpm_hit = hit_test_bpm_popup(
                    app, audio, x, y, &bpm_text_left);
                if (bpm_hit >= 0) {
                    flag_editor.enter_bpm_edit(
                        bpm_hit, static_cast<double>(x),
                        bpm_text_left);
                    return;
                }
                const int hit_now = hit_test_flag(app, audio, x, y);
                if (hit_now >= 0 && app.active_mode != 'P') {
                    flag_editor.enter_top_flag_edit(
                        hit_now, static_cast<double>(x));
                    return;
                }
                // Top strip click that isn't on a popup or flag: exit
                // and fall through to normal handling.
                flag_editor.exit_top_flag_edit_no_commit();
            } else {
                flag_editor.exit_top_flag_edit_no_commit();
                // Fall through so the click can drive a playhead
                // drag, marker click, etc.
            }
        }

        // Detect double-click from timing + position deltas.
        const auto now = std::chrono::steady_clock::now();
        const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - app.last_click_time).count();
        const bool is_double =
            !app.last_click_consumed &&
            dt_ms <= kDoubleClickMs &&
            std::abs(x - app.last_click_x) <= kDoubleClickPixels &&
            std::abs(y - app.last_click_y) <= kDoubleClickPixels;

        // A double-click in the waveform area creates a new marker at
        // the click position (not the playhead). In warp mode, drops a
        // warp marker (Shift forces inherit). In phase reset mode, drops
        // a phase reset (Shift is ignored — no inherit concept). The
        // first single-click already moved the playhead via the
        // playhead-drag-press logic below.
        if (is_double && inside_waveform && !ctrl) {
            const double spp = current_samples_per_pixel(app, audio);
            const int click_rel_x = x - area.x;
            const int sr = audio.sample_rate();
            const int64_t sample = app.viewport_start_sample +
                static_cast<int64_t>(std::nearbyint(click_rel_x * spp));
            const double t = (sr > 0)
                ? static_cast<double>(sample) / static_cast<double>(sr)
                : 0.0;
            if (app.active_mode == 'P') {
                phase_resets.drop_phase_reset_at_position(t);
            } else {
                warpops.drop_marker(t, /*inherit=*/shift);
            }
            // Consume this click so a triple-click doesn't double-fire.
            app.last_click_consumed = true;
            return;
        }

        // Store this click for the next one to compare against.
        app.last_click_time     = now;
        app.last_click_x        = x;
        app.last_click_y        = y;
        app.last_click_consumed = false;

        // Iteration popup click takes priority over flag click when
        // iteration mode is on. The popup sits above the flag rect so
        // their hit zones don't overlap, but checking the popup first
        // makes the intent unambiguous when the flag-strip extents
        // change shape.
        if (inside_top && !ctrl) {
            double iter_text_left = -1.0;
            const int iter_hit = hit_test_iter_popup(
                app, audio, x, y, &iter_text_left);
            if (iter_hit >= 0) {
                flag_editor.enter_iter_edit(
                    iter_hit, static_cast<double>(x), iter_text_left);
                return;
            }
            double bpm_text_left = -1.0;
            const int bpm_hit = hit_test_bpm_popup(
                app, audio, x, y, &bpm_text_left);
            if (bpm_hit >= 0) {
                flag_editor.enter_bpm_edit(
                    bpm_hit, static_cast<double>(x), bpm_text_left);
                return;
            }
        }

        // Consolidated hit-test across waveform (marker line) and top
        // strip (flag rect). A flag click behaves exactly like a click
        // on its marker line.
        int hit = -1;
        bool in_click_region = false;
        if (inside_waveform) {
            hit = hit_test_marker_line(app, audio, x);
            in_click_region = true;
        } else if (inside_top) {
            hit = hit_test_flag(app, audio, x, y);
            in_click_region = true;
        }

        if (!in_click_region) return;

        if (ctrl) {
            // Ctrl branch: marker-reposition drag or no-op on empty.
            if (hit >= 0) {
                // begin_drag preserves the multi-selection if `hit` is in
                // it, else collapses to just `hit`. Motion decides whether
                // it actually becomes a drag vs. a plain click.
                warpops.begin_drag(hit, x);
            }
            // else: Ctrl+press on empty space is a silent no-op.
            return;
        }

        // Non-Ctrl: plain or Shift press. In the waveform area this
        // starts a playhead-drag gesture. In the top strip (flag click)
        // a warp-mode flag click enters the V.A1 text editor; in
        // phase reset mode we keep the legacy select-on-click behavior.
        if (inside_top) {
            if (hit >= 0) {
                if (app.active_mode != 'P' && !shift) {
                    // V.A1: plain click on a warp flag enters edit
                    // mode. enter_top_flag_edit owns the selection +
                    // playhead update on its target-switching path,
                    // so this site just delegates. Shift+click keeps
                    // the legacy multi-select toggle below.
                    flag_editor.enter_top_flag_edit(
                        hit, static_cast<double>(x));
                    return;
                }
                if (shift) selection.toggle_selection_membership(hit);
                else       selection.set_single_selection(hit);
                const int sr = audio.sample_rate();
                int64_t sample;
                if (app.active_mode == 'P') {
                    sample = static_cast<int64_t>(std::nearbyint(
                        app.phase_reset_markers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
                } else {
                    sample = static_cast<int64_t>(std::nearbyint(
                        app.warpmarkers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
                }
                viewport.move_playhead_to(sample);
            }
            return;
        }

        // Waveform-area press: start playhead drag gesture.
        {
            const int sr = audio.sample_rate();
            if (hit >= 0) {
                // Press on a marker (within 3px).
                if (!shift) {
                    selection.set_single_selection(hit);
                } else {
                    // Shift+press on marker: toggles membership in the
                    // selection, last_selected repaired by the helper.
                    // Plain press collapses to single selection.
                    selection.toggle_selection_membership(hit);
                }
                int64_t sample;
                if (app.active_mode == 'P') {
                    sample = static_cast<int64_t>(std::nearbyint(
                        app.phase_reset_markers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
                } else {
                    sample = static_cast<int64_t>(std::nearbyint(
                        app.warpmarkers.markers()[hit].time_seconds *
                        static_cast<double>(sr)));
                }
                viewport.move_playhead_to(sample);
                // Brief six: marker-line click in the waveform area
                // keeps playback alive by reseeking the audio device
                // to the marker's sample. Skip the reseek when the
                // click landed on the marker the playhead was already
                // on — it'd be a no-op move + a wasted teardown.
                if (was_playing && sample != playhead_at_click_entry) {
                    playback.play(sample, viewport.trim_end_sample());
                }
                app.playhead_drag.active = true;
                app.playhead_drag.was_playing_at_press = was_playing;
                app.playhead_drag.press_marker_idx = hit;
            } else {
                // Press on empty waveform.
                const double spp = current_samples_per_pixel(app, audio);
                const int click_rel_x = x - area.x;
                if (click_rel_x < 0 || click_rel_x >= area.w) {
                    if (!shift) selection.clear_selection();
                    return;
                }
                const int64_t sample = app.viewport_start_sample +
                    static_cast<int64_t>(std::nearbyint(click_rel_x * spp));
                if (!shift) selection.clear_selection();
                viewport.move_playhead_to(sample);
                // Brief six: empty-space click in the waveform area
                // keeps playback alive by reseeking to the click
                // position.
                if (was_playing && sample != playhead_at_click_entry) {
                    playback.play(sample, viewport.trim_end_sample());
                }
                app.playhead_drag.active = true;
                app.playhead_drag.was_playing_at_press = was_playing;
                app.playhead_drag.press_marker_idx = -1;
            }
        }
    } else if (button == 4 || button == 5) {
        handle_wheel(button, ctrl, alt,
                     inside_waveform, inside_top);
    }
}

// X.7.8b-2: button-release handler. Verbatim from the lambda at the
// original main.cpp:1835; commit_drag and set_single_selection are
// rewritten to direct method calls on warpops / selection respectively.
void GuiInputHandler::on_button_release(unsigned int button, int /*x*/,
                                        int /*y*/, GuiInputState mods) {
    if (app.prompt.active) return;
    if (button != 1) return;
    if (app.playhead_drag.active) {
        // Brief F Section 1: if the playhead snapped onto a marker
        // during the drag, commit selection on release. Plain release
        // sets the snapped marker as the single selection; Shift
        // release adds it to the existing set without removing
        // anything. Off-marker release leaves selection alone.
        const bool shift = mods.shift;
        const int  sr    = audio.sample_rate();
        int snapped = -1;
        if (sr > 0) {
            const int64_t ph = app.playhead_sample;
            if (app.render_view_enabled) {
                if (app.active_mode == 'P') {
                    const auto& mv = app.render_view_phase_resets;
                    for (size_t i = 0; i < mv.size(); ++i) {
                        const int64_t s = static_cast<int64_t>(
                            std::nearbyint(mv[i].time_seconds *
                                         static_cast<double>(sr)));
                        if (s == ph) {
                            snapped = static_cast<int>(i);
                            break;
                        }
                    }
                } else {
                    const auto& mv = app.render_view_markers;
                    for (size_t i = 0; i < mv.size(); ++i) {
                        const int64_t s = static_cast<int64_t>(
                            std::nearbyint(mv[i].time_seconds *
                                         static_cast<double>(sr)));
                        if (s == ph) {
                            snapped = static_cast<int>(i);
                            break;
                        }
                    }
                }
            } else if (app.active_mode == 'P') {
                const auto& mv = app.phase_reset_markers.markers();
                for (size_t i = 0; i < mv.size(); ++i) {
                    const int64_t s = static_cast<int64_t>(
                        std::nearbyint(mv[i].time_seconds *
                                     static_cast<double>(sr)));
                    if (s == ph) {
                        snapped = static_cast<int>(i);
                        break;
                    }
                }
            } else {
                const auto& mv = app.warpmarkers.markers();
                for (size_t i = 0; i < mv.size(); ++i) {
                    const int64_t s = static_cast<int64_t>(
                        std::nearbyint(mv[i].time_seconds *
                                     static_cast<double>(sr)));
                    if (s == ph) {
                        snapped = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
        if (snapped >= 0) {
            if (snapped == app.playhead_drag.press_marker_idx) {
                // Press already committed selection for this marker; release
                // is from a click-without-drag, not a drag-completion. Skip
                // the snap-action so press/release don't overlap destructively
                // (specifically, shift+click-remove must not be re-added by
                // the release).
                app.playhead_drag = PlayheadDragState{};
                return;
            }
            if (app.render_view_enabled) {
                // Brief J.2 Section 3: render-view writes the
                // global live pair. active_mode tells us which
                // marker list the indices map to.
                if (shift) {
                    app.selected_markers.insert(snapped);
                    app.last_selected_marker = snapped;
                } else {
                    app.selected_markers.clear();
                    app.selected_markers.insert(snapped);
                    app.last_selected_marker = snapped;
                }
                gui.invalidate_region(0, 0, app.width, app.height);
            } else if (shift) {
                app.selected_markers.insert(snapped);
                app.last_selected_marker = snapped;
                viewport.invalidate_top_strip();
            } else {
                selection.set_single_selection(snapped);
            }
        }
        app.playhead_drag = PlayheadDragState{};
        return;
    }
    if (!app.drag.active) return;
    warpops.commit_drag();
}

// X.7.8b-3: motion handler. Verbatim from the lambda at the original
// main.cpp:1319; the operation-struct method calls (apply_drag_motion,
// commit_drag, move_playhead_to, invalidate_top_strip,
// invalidate_timestamp_area, invalidate_playhead_columns) are rewritten
// to direct method calls on warpops / viewport. popup_eligible_marker
// (now in app_state.{h,cpp}) takes `app` as its first argument; the
// remaining free function calls (hit_test_marker_line, hit_test_flag,
// compute_hover_popup_text, waveform_area, current_samples_per_pixel,
// playhead_pixel_x, text_editor::is_active) keep their original spelling.
void GuiInputHandler::on_motion(int mouse_x, int mouse_y, GuiInputState mods) {
    if constexpr (kDebugPerf) {
        app.last_input_event_time = std::chrono::steady_clock::now();
    }
    // V.A3b Addendum 3: record latest cursor coords so viewport
    // mutators can re-evaluate hover at the cursor's last position.
    app.last_mouse_x = mouse_x;
    app.last_mouse_y = mouse_y;
    if (app.prompt.active) {
        clear_hover_popup();
        return;
    }
    // Chunk W: render-view motion handler. Brief F Section 2 adds
    // playhead-drag snap support: when a drag is in flight, snap the
    // playhead to the visible sub-view's markers (3px epsilon),
    // matching source-view's gesture. Otherwise run hover popup
    // detection against render_view_markers (suppressed in phase reset
    // sub-view because hit_test_flag short-circuits to -1).
    if (app.render_view_enabled) {
        if (app.playhead_drag.active) {
            clear_hover_popup();
            if (!mods.primary_button_held) {
                app.playhead_drag = PlayheadDragState{};
                return;
            }
            // Brief six: the first drag-motion event after a press
            // during playback stops playback once. Click-only gestures
            // (press, no motion before release) keep playback alive
            // via the press-site reseek; once motion fires, the
            // gesture has become a scrub and continuous reseeking
            // would stutter badly.
            if (app.playhead_drag.was_playing_at_press &&
                !app.playhead_drag.drag_motion_stopped_audio) {
                stop_playback_if_playing();
                app.playhead_drag.drag_motion_stopped_audio = true;
            }
            const int sr = audio.sample_rate();
            if (sr <= 0) return;
            const GuiRect area = waveform_area(app);
            const double spp = current_samples_per_pixel(app, audio);
            if (spp <= 0.0) return;
            const int hit = hit_test_marker_line(app, audio, mouse_x);
            int64_t new_playhead;
            if (hit >= 0) {
                if (app.active_mode == 'P') {
                    new_playhead = static_cast<int64_t>(std::nearbyint(
                        app.render_view_phase_resets[hit].time_seconds *
                        static_cast<double>(sr)));
                } else {
                    new_playhead = static_cast<int64_t>(std::nearbyint(
                        app.render_view_markers[hit].time_seconds *
                        static_cast<double>(sr)));
                }
            } else {
                int rel = mouse_x - area.x;
                if (rel < 0) rel = 0;
                if (rel >= area.w) rel = area.w - 1;
                new_playhead = app.viewport_start_sample +
                    static_cast<int64_t>(std::nearbyint(rel * spp));
            }
            if (new_playhead != app.playhead_sample) {
                viewport.move_playhead_to(new_playhead);
            }
            return;
        }
        const int hit = hit_test_flag(app, audio, mouse_x, mouse_y);
        if (hit != app.hover_popup.marker_index) {
            if (app.hover_popup.visible) viewport.invalidate_top_strip();
            app.hover_popup.marker_index = hit;
            app.hover_popup.visible      = false;
            app.hover_popup.entry_time   =
                std::chrono::steady_clock::now();
            app.hover_popup.cached_text =
                popup_eligible_marker(app, hit)
                    ? compute_hover_popup_text(
                          app.render_view_markers, hit,
                          app.render_view_src_sr)
                    : std::string();
        }
        return;
    }
    if (app.playhead_drag.active) {
        clear_hover_popup();
        // Left button must still be held; if not, the release was lost —
        // terminate the drag. Modifier changes mid-drag are ignored.
        if (!mods.primary_button_held) {
            app.playhead_drag = PlayheadDragState{};
            return;
        }
        // Brief six: the first drag-motion event after a press during
        // playback stops playback once. See render-view branch above
        // for the full rationale; this is the source-view mirror.
        if (app.playhead_drag.was_playing_at_press &&
            !app.playhead_drag.drag_motion_stopped_audio) {
            stop_playback_if_playing();
            app.playhead_drag.drag_motion_stopped_audio = true;
        }
        const int sr = audio.sample_rate();
        if (sr <= 0) return;
        const GuiRect area = waveform_area(app);
        const double spp = current_samples_per_pixel(app, audio);
        if (spp <= 0.0) return;

        // Marker snap test — uses the same 3px epsilon as marker hit-test.
        // Selection is fixed at press time and is NOT mutated here; the
        // snap is purely a playhead-positioning magnet.
        const int hit = hit_test_marker_line(app, audio, mouse_x);
        int64_t new_playhead;
        if (hit >= 0) {
            if (app.active_mode == 'P') {
                new_playhead = static_cast<int64_t>(std::nearbyint(
                    app.phase_reset_markers.markers()[hit].time_seconds *
                    static_cast<double>(sr)));
            } else {
                new_playhead = static_cast<int64_t>(std::nearbyint(
                    app.warpmarkers.markers()[hit].time_seconds *
                    static_cast<double>(sr)));
            }
        } else {
            // No marker within epsilon: playhead follows cursor freely.
            int rel = mouse_x - area.x;
            if (rel < 0) rel = 0;
            if (rel >= area.w) rel = area.w - 1;
            new_playhead = app.viewport_start_sample +
                static_cast<int64_t>(std::nearbyint(rel * spp));
        }

        if (new_playhead != app.playhead_sample) {
            viewport.move_playhead_to(new_playhead);
        }
        return;
    }
    if (!app.drag.active) {
        // No active gesture: run hover-popup detection. Only in warp
        // mode, with no editor, no dialog (already returned), no drag,
        // and not while iteration mode owns the popup space.
        // The dwell timer is started/restarted on every transition into
        // an eligible rect; the on_tick handler flips visibility.
        if (app.active_mode == 'W' &&
            !app.iteration_mode_enabled &&
            !text_editor::is_active(app.top_flag_editor) &&
            !app.queue_running) {
            const int hit = hit_test_flag(app, audio, mouse_x, mouse_y);
            if (hit != app.hover_popup.marker_index) {
                if (app.hover_popup.visible) viewport.invalidate_top_strip();
                app.hover_popup.marker_index = hit;
                app.hover_popup.visible      = false;
                app.hover_popup.entry_time   =
                    std::chrono::steady_clock::now();
                // Precompute popup text at rect-entry so the
                // delay-completion paint doesn't repeat the math.
                app.hover_popup.cached_text =
                    popup_eligible_marker(app, hit)
                        ? compute_hover_popup_text(
                              app.warpmarkers.markers(), hit,
                              audio.sample_rate())
                        : std::string();
            }
        } else {
            clear_hover_popup();
        }
        return;
    }
    // A drag is active — drop any pending popup.
    clear_hover_popup();
    // Left button must still be held down — otherwise release was lost.
    if (!mods.primary_button_held) {
        warpops.commit_drag();
        return;
    }
    const int sr = audio.sample_rate();
    if (sr <= 0) return;
    const GuiRect area = waveform_area(app);
    const double spp = current_samples_per_pixel(app, audio);
    const double sr_d = static_cast<double>(sr);
    const double vp_time = static_cast<double>(app.viewport_start_sample) / sr_d;
    const double mouse_time = vp_time +
        static_cast<double>(mouse_x - area.x) * spp / sr_d;
    warpops.apply_drag_motion(mouse_time - app.drag.anchor_mouse_time_seconds);

    // Track the playhead with the grabbed marker. The drag applies a
    // uniform delta across the dragging set, so the hit marker's
    // post-motion time matches the user's cursor intent. Viewport is
    // deliberately not followed — the user can pan manually if the
    // drag runs past the edge.
    const int hit_idx = app.drag.hit_marker;
    const bool phase_reset_drag = (app.drag.drag_mode == 'P');
    const int n = phase_reset_drag
        ? static_cast<int>(app.phase_reset_markers.markers().size())
        : static_cast<int>(app.warpmarkers.markers().size());
    if (hit_idx >= 0 && hit_idx < n) {
        int64_t ph;
        if (phase_reset_drag) {
            ph = static_cast<int64_t>(std::nearbyint(
                app.phase_reset_markers.markers()[hit_idx].time_seconds * sr_d));
        } else {
            ph = static_cast<int64_t>(std::nearbyint(
                app.warpmarkers.markers()[hit_idx].time_seconds * sr_d));
        }
        if (ph != app.playhead_sample) {
            const double old_px = playhead_pixel_x(app, audio);
            app.playhead_sample = ph;
            if (playback.is_playing()) playback.resync_predictor();
            const double new_px = playhead_pixel_x(app, audio);
            viewport.invalidate_playhead_columns(old_px, new_px);
            viewport.invalidate_timestamp_area();
        }
    }
}
