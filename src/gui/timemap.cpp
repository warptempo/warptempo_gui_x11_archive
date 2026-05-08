#include "timemap.h"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <sndfile.h>

namespace {

double effective_tempo(const MarkerForRender& m) {
    double v = m.tempo_base;
    if (!m.tempo_scale.empty()) {
        try {
            v *= std::stod(m.tempo_scale);
        } catch (...) {
            return 0.0;
        }
    }
    return v;
}

struct LabelCacheEntry {
    double      delta_tgt   = 0.0;
    double      tempo_base  = 1.0;
    std::string tempo_scale;
};

}  // namespace

bool build_timemaps(const TimemapBuildInput& in, TimemapBuildResult& out) {
    out = TimemapBuildResult{};

    const auto&  markers      = in.markers;
    const double scale        = in.scale;
    const long   sample_rate  = in.sample_rate;
    const long   total_frames = in.total_frames;

    if (sample_rate <= 0 || total_frames <= 0) {
        std::cerr << "warptempo_gui: timemap error: invalid source audio metadata\n";
        return false;
    }

    // Trim range extraction. A b= / e= marker is a real marker in the
    // stream (it contributes to the timemap math); the flag only governs
    // the post-pass filter.
    bool   has_begin    = false;
    bool   has_end      = false;
    double begin_sec    = 0.0;
    double end_sec      = 0.0;
    for (const auto& m : markers) {
        if (m.is_begin_time) { has_begin = true; begin_sec = m.time_seconds; }
        if (m.is_end_time)   { has_end   = true; end_sec   = m.time_seconds; }
    }
    for (const auto& t : in.transient_trim_flags) {
        if (t.is_begin_time && !has_begin) {
            has_begin = true; begin_sec = t.time_seconds;
        }
        if (t.is_end_time && !has_end) {
            has_end = true; end_sec = t.time_seconds;
        }
    }
    if (has_begin && begin_sec <= 0.0) {
        std::cerr << "warptempo_gui: timemap error: begin time cannot be 00:00.000\n";
        return false;
    }

    // Pass 1: accumulate per-label deltas so forward-declared references
    // receive the correct duration when encountered in Pass 2.
    std::map<std::string, LabelCacheEntry> label_cache;

    double src_f_prev = 0.0;
    double tgt_f_prev = 0.0;

    for (size_t i = 0; i < markers.size(); ++i) {
        double src_frame = (i + 1 < markers.size())
            ? markers[i + 1].time_seconds * static_cast<double>(sample_rate)
            : static_cast<double>(total_frames);

        if (src_frame > static_cast<double>(total_frames)) {
            std::cerr << "warptempo_gui: timemap error: marker time exceeds source length\n";
            return false;
        }
        if (src_frame - src_f_prev < 1.0) {
            std::cerr << "warptempo_gui: timemap error: marker segment < 1 frame at index "
                      << i << "\n";
            return false;
        }

        const auto& m = markers[i];
        double current_tgt = tgt_f_prev;
        const bool is_numeric   = m.label_ref.empty();
        const bool is_label_ref = !m.label_ref.empty();

        if (is_numeric) {
            double tempo_val = effective_tempo(m);
            if (tempo_val > 9.99) {
                std::cerr << "warptempo_gui: timemap error: tempo > 9.99 at marker "
                          << i << "\n";
                return false;
            }
            if (tempo_val <= 0.0) {
                std::cerr << "warptempo_gui: timemap error: tempo <= 0 at marker "
                          << i << "\n";
                return false;
            }

            double delta_src = src_frame - src_f_prev;
            double delta_tgt = delta_src / (tempo_val * scale);
            current_tgt += delta_tgt;

            if (!m.label_def.empty()) {
                if (label_cache.count(m.label_def)) {
                    std::cerr << "warptempo_gui: timemap error: duplicate label definition '"
                              << m.label_def << "'\n";
                    return false;
                }
                LabelCacheEntry e;
                e.delta_tgt   = delta_tgt;
                e.tempo_base  = m.tempo_base;
                e.tempo_scale = m.tempo_scale;
                label_cache[m.label_def] = e;
            }
            tgt_f_prev = current_tgt;
        }
        (void)is_label_ref;  // computed only for Pass 2

        src_f_prev = src_frame;
    }

    // Pass 2: emit standard timemap + midi tempomap entries.
    out.standard.push_back({0, 0});

    src_f_prev = 0.0;
    tgt_f_prev = 0.0;
    double last_valid_multiplier = 1.0;

    for (size_t i = 0; i < markers.size(); ++i) {
        double src_frame = (i + 1 < markers.size())
            ? markers[i + 1].time_seconds * static_cast<double>(sample_rate)
            : static_cast<double>(total_frames);

        const auto& m = markers[i];
        double target_frame = 0.0;

        if (!m.label_ref.empty()) {
            auto it = label_cache.find(m.label_ref);
            if (it == label_cache.end()) {
                std::cerr << "warptempo_gui: timemap error: undefined label reference '"
                          << m.label_ref << "'\n";
                return false;
            }
            const LabelCacheEntry& lbl = it->second;
            target_frame = tgt_f_prev + lbl.delta_tgt;

            double base_val = lbl.tempo_base;
            double unadj    = tgt_f_prev + ((src_frame - src_f_prev) / (base_val * scale));
            double multiplier = (unadj - tgt_f_prev) / (target_frame - tgt_f_prev);

            double final_multiplier = multiplier;
            if (!lbl.tempo_scale.empty()) {
                double s_val = 0.0;
                try { s_val = std::stod(lbl.tempo_scale); }
                catch (...) { s_val = 0.0; }
                final_multiplier = s_val * multiplier;
            }
            if (final_multiplier > 9.9999) {
                std::cerr << "warptempo_gui: timemap error: label final multiplier > 9.9999 at marker "
                          << i << " (label '" << m.label_ref << "')\n";
                return false;
            }
        } else {
            double tempo_val = effective_tempo(m);
            double delta_src = src_frame - src_f_prev;
            target_frame = tgt_f_prev + (delta_src / (tempo_val * scale));
        }

        double seg_src_dur = src_frame - src_f_prev;
        double seg_tgt_dur = target_frame - tgt_f_prev;
        if (seg_tgt_dur > 0.000001) {
            double effective_multiplier = seg_src_dur / seg_tgt_dur;
            last_valid_multiplier = effective_multiplier;
            double seg_start_time = tgt_f_prev / static_cast<double>(sample_rate);
            out.midi.push_back({seg_start_time, effective_multiplier});
        }

        size_t sf = static_cast<size_t>(std::llrint(src_frame));
        size_t tf = static_cast<size_t>(std::llrint(target_frame));
        out.standard.push_back({sf, tf});

        src_f_prev = src_frame;
        tgt_f_prev = target_frame;
    }

    double final_tgt_sec = tgt_f_prev / static_cast<double>(sample_rate);
    out.midi.push_back({final_tgt_sec, last_valid_multiplier});

    // Trim post-pass. Shifts both standard and midi vectors to begin at the
    // trim start; drops entries outside [begin_frame, end_frame].
    if (has_begin || has_end) {
        long begin_frame = has_begin
            ? static_cast<long>(std::llrint(begin_sec * sample_rate))
            : 0;
        long end_frame   = has_end
            ? static_cast<long>(std::llrint(end_sec   * sample_rate))
            : total_frames;

        std::vector<TimemapSegment> std_shifted;
        long begin_tgt = -1;
        long end_tgt   = -1;
        for (const auto& seg : out.standard) {
            long sf = static_cast<long>(seg.src_frame);
            long tf = static_cast<long>(seg.tgt_frame);
            if (sf >= begin_frame && sf <= end_frame) {
                if (begin_tgt == -1) begin_tgt = tf;
                end_tgt = tf;
                std_shifted.push_back({
                    static_cast<size_t>(sf - begin_frame),
                    static_cast<size_t>(tf - begin_tgt)
                });
            }
        }
        out.standard = std::move(std_shifted);

        if (begin_tgt != -1) {
            double begin_tgt_sec = static_cast<double>(begin_tgt) / sample_rate;
            double end_tgt_sec   = (end_tgt != -1)
                ? (static_cast<double>(end_tgt) / sample_rate)
                : (static_cast<double>(total_frames) / sample_rate);

            std::vector<TempomapEntry> midi_shifted;
            double active_multiplier = 1.0;
            bool   start_point_written = false;
            for (const auto& e : out.midi) {
                if (e.target_time_sec < begin_tgt_sec) {
                    active_multiplier = e.multiplier;
                } else if (e.target_time_sec <= end_tgt_sec) {
                    if (!start_point_written) {
                        if (e.target_time_sec > begin_tgt_sec) {
                            midi_shifted.push_back({0.0, active_multiplier});
                        }
                        start_point_written = true;
                    }
                    midi_shifted.push_back({e.target_time_sec - begin_tgt_sec, e.multiplier});
                    active_multiplier = e.multiplier;
                }
            }
            if (!start_point_written) {
                midi_shifted.push_back({0.0, active_multiplier});
            }
            out.midi = std::move(midi_shifted);
        }

        out.trimmed          = true;
        out.trim_begin_frame = static_cast<size_t>(begin_frame);
        out.trim_end_frame   = static_cast<size_t>(end_frame);
    }

    return true;
}

bool write_trimmed_wav(const std::string& src_path,
                       const std::string& out_path,
                       size_t begin_frame,
                       size_t end_frame) {
    if (end_frame <= begin_frame) {
        std::cerr << "warptempo_gui: trim error: end_frame <= begin_frame\n";
        return false;
    }

    SF_INFO src_info{};
    src_info.format = 0;
    SNDFILE* src = sf_open(src_path.c_str(), SFM_READ, &src_info);
    if (!src) {
        std::cerr << "warptempo_gui: trim error: could not open source '" << src_path << "'\n";
        return false;
    }
    if (static_cast<sf_count_t>(end_frame) > src_info.frames) {
        sf_close(src);
        std::cerr << "warptempo_gui: trim error: end_frame exceeds source length\n";
        return false;
    }
    if (sf_seek(src, static_cast<sf_count_t>(begin_frame), SEEK_SET) < 0) {
        sf_close(src);
        std::cerr << "warptempo_gui: trim error: sf_seek failed\n";
        return false;
    }

    SF_INFO out_info = src_info;
    out_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* dst = sf_open(out_path.c_str(), SFM_WRITE, &out_info);
    if (!dst) {
        sf_close(src);
        std::cerr << "warptempo_gui: trim error: could not create output '" << out_path << "'\n";
        return false;
    }

    const size_t kChunk = 65536;
    std::vector<float> buf(kChunk * static_cast<size_t>(src_info.channels));
    size_t remaining = end_frame - begin_frame;
    while (remaining > 0) {
        sf_count_t want = static_cast<sf_count_t>(std::min(kChunk, remaining));
        sf_count_t got  = sf_readf_float(src, buf.data(), want);
        if (got <= 0) break;
        if (sf_writef_float(dst, buf.data(), got) != got) {
            sf_close(src);
            sf_close(dst);
            std::cerr << "warptempo_gui: trim error: short write\n";
            return false;
        }
        remaining -= static_cast<size_t>(got);
    }

    sf_close(src);
    sf_close(dst);
    return true;
}
