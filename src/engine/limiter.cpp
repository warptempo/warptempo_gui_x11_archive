#include "limiter.h"
#include "synthesis.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

// Two sample-coordinate systems are in play:
//   - post_trim (output/meas_ola coords): sample 0 is the first sample emitted
//     by synthesize_full after the N/2 OLA ramp-up is discarded.
//   - pre_trim  (virtual OLA coords): sample 0 is the very first OLA
//     accumulation position; frame m contributes to samples [m*R_s, m*R_s + N).
// Relationship: pre_trim_sample = post_trim_sample + N/2.
// Rule: any function that maps between sample-index and frame-index works in
// pre_trim coords. Convert at the boundary.

namespace {
constexpr int    RESCAN_HALF_WIDTH_FRAMES = 100;  // base radius (frames) for rescan region
constexpr int    MIN_PEAK_EDGE_MARGIN     = 25;   // minimum frames between any peak and region edge
constexpr int    PEAK_DEDUP_RADIUS        = 4;    // per-channel minimum sample gap between peaks
constexpr int    MAX_REFINEMENT_TRIES     = 3;    // extra attempts past the first
constexpr int    MAX_CLAMP_REDIST_TRIES   = 8;    // safety cap on apply_update inner loop
constexpr double DIAG_FLOOR_DB            = 12.0; // reduction (dB) that fills diag floor

struct Peak {
    int64_t sample_idx;     // position in OLA output (post-trim coords)
    int     ch;
    int     sign;           // +1 or -1
    double  magnitude;      // |measured value|
    double  original_mag;   // magnitude at first measurement (for diag amplitude)
};

static inline int64_t pre_trim(int64_t post_trim_sample, int N) {
    return post_trim_sample + N / 2;
}

// IFFT of single band (all other bins zeroed) with gain applied, windowed and /N.
static void band_ifft(AudioSTFT& stft, const std::complex<float>* cached_spec,
                       int band, double gain, std::vector<double>& out_buf) {
    const int N = stft.N;
    const int K = N / 2 + 1;
    const auto& b2b = stft.bin_to_band;
    #pragma omp parallel for
    for (int k = 0; k < K; ++k) {
        if (b2b[k] == band) {
            stft.ifft_in[k][0] = cached_spec[k].real() * gain;
            stft.ifft_in[k][1] = cached_spec[k].imag() * gain;
        } else {
            stft.ifft_in[k][0] = 0.0;
            stft.ifft_in[k][1] = 0.0;
        }
    }
    fftw_execute(stft.plan_inv);
    const double inv_N = 1.0 / N;
    #pragma omp parallel for
    for (int n = 0; n < N; ++n)
        out_buf[n] = stft.ifft_out[n] * inv_N * stft.synth_window[n];
}

// Full-spectrum IFFT with per-band gain from attenuation_map[frame_idx].
static void full_ifft_with_map(AudioSTFT& stft, const std::complex<float>* cached_spec,
                                int frame_idx, std::vector<double>& out_buf) {
    const int N = stft.N;
    const int K = N / 2 + 1;
    const auto& b2b = stft.bin_to_band;
    const double* row = stft.attenuation_map[frame_idx].data();
    #pragma omp parallel for
    for (int k = 0; k < K; ++k) {
        double g = row[b2b[k]];
        stft.ifft_in[k][0] = cached_spec[k].real() * g;
        stft.ifft_in[k][1] = cached_spec[k].imag() * g;
    }
    fftw_execute(stft.plan_inv);
    const double inv_N = 1.0 / N;
    #pragma omp parallel for
    for (int n = 0; n < N; ++n)
        out_buf[n] = stft.ifft_out[n] * inv_N * stft.synth_window[n];
}

// At 75% overlap (R_s = N/4), exactly 4 frames contribute to each output sample.
// `s` must be in pre-trim coords.
static void contributing_frames(int64_t s, int R_s, int N, int num_frames,
                                 std::vector<int>& out) {
    out.clear();
    int overlap = N / R_s;
    int m_hi = static_cast<int>(s / R_s);
    if (m_hi >= num_frames) m_hi = num_frames - 1;
    int m_lo = m_hi - (overlap - 1);
    if (m_lo < 0) m_lo = 0;
    for (int m = m_lo; m <= m_hi; ++m) {
        int64_t fs = static_cast<int64_t>(m) * R_s;
        if (fs <= s && s < fs + N) out.push_back(m);
    }
}

// Scan [n_start, n_end) for one channel. Emits one Peak per above-ceiling cluster
// (contiguous above-ceiling run within PEAK_DEDUP_RADIUS samples).
static void find_peaks_in_range(const float* ola, int64_t n_start, int64_t n_end,
                                 int channels, int ch, double ceiling,
                                 std::vector<Peak>& out_peaks) {
    int64_t best_idx = -1;
    double best_mag = 0.0;
    int    best_sign = 0;
    int64_t last_above = std::numeric_limits<int64_t>::min() / 2;

    auto flush_cluster = [&]() {
        if (best_idx >= 0) {
            Peak p;
            p.sample_idx = best_idx;
            p.ch = ch;
            p.sign = best_sign;
            p.magnitude = best_mag;
            p.original_mag = best_mag;
            out_peaks.push_back(p);
            best_idx = -1;
            best_mag = 0.0;
            best_sign = 0;
        }
    };

    for (int64_t n = n_start; n < n_end; ++n) {
        float v = ola[n * channels + ch];
        double a = std::abs(v);
        if (a > ceiling) {
            if (best_idx >= 0 && n - last_above > PEAK_DEDUP_RADIUS) flush_cluster();
            if (best_idx < 0 || a > best_mag) {
                best_idx = n;
                best_mag = a;
                best_sign = (v >= 0.0f) ? 1 : -1;
            }
            last_above = n;
        }
    }
    flush_cluster();
}

// Rescan: re-synthesize OLA over [pre_trim_s_start, pre_trim_s_end) using cached
// spectra and current attenuation map. Parameters are in pre-trim coords, and the
// returned slice's index 0 corresponds to pre-trim position s_start (== post-trim
// position s_start - N/2). Writes interleaved floats into dest.
static void rescan_region(AudioSTFT& stft,
                           const std::complex<float>* cached_spectra,
                           int64_t s_start, int64_t s_end,
                           std::vector<float>& dest) {
    const int N = stft.N;
    const int R_s = stft.R_s;
    const int channels = stft.channels;
    const int K = N / 2 + 1;
    const int num_frames = static_cast<int>(stft.frame_map.size());
    const size_t slice_len = static_cast<size_t>(s_end - s_start);

    std::vector<std::vector<double>> local_ola(channels, std::vector<double>(slice_len, 0.0));
    std::vector<double> frame_time(N);

    int overlap = N / R_s;
    int m_hi = static_cast<int>((s_end - 1) / R_s);
    if (m_hi >= num_frames) m_hi = num_frames - 1;
    int m_lo = static_cast<int>(s_start / R_s) - (overlap - 1);
    if (m_lo < 0) m_lo = 0;

    for (int m = m_lo; m <= m_hi; ++m) {
        int64_t frame_start = static_cast<int64_t>(m) * R_s;
        if (frame_start + N <= s_start) continue;
        if (frame_start >= s_end) break;
        for (int ch = 0; ch < channels; ++ch) {
            const std::complex<float>* spec = cached_spectra
                + (static_cast<size_t>(m) * channels + ch) * K;
            full_ifft_with_map(stft, spec, m, frame_time);
            for (int n = 0; n < N; ++n) {
                int64_t out_idx = frame_start + n - s_start;
                if (out_idx < 0 || out_idx >= static_cast<int64_t>(slice_len)) continue;
                local_ola[ch][out_idx] += frame_time[n];
            }
        }
    }

    dest.assign(slice_len * channels, 0.0f);
    for (size_t n = 0; n < slice_len; ++n)
        for (int ch = 0; ch < channels; ++ch)
            dest[n * channels + ch] = static_cast<float>(local_ola[ch][n]);
}

}  // anonymous namespace

void Limiter::process(AudioSTFT& stft) {
    auto& lp = stft.limiter_params;

    if (!lp.enabled) {
        std::cout << "[Pass 2/3] Limiter.......................... disabled\n";
        return;
    }

    const int N          = stft.N;
    const int R_s        = stft.R_s;
    const int channels   = stft.channels;
    const int K          = N / 2 + 1;
    const int num_frames = static_cast<int>(stft.frame_map.size());
    const int num_bands  = stft.num_bands;

    const double ceiling = std::pow(10.0, lp.ceiling_dbfs / 20.0);
    const double tol_amp_hi = std::pow(10.0, (lp.ceiling_dbfs + lp.tolerance_db) / 20.0);
    const double tol_amp_lo = std::pow(10.0, (lp.ceiling_dbfs - lp.tolerance_db) / 20.0);
    const double band3_hi   = std::pow(10.0, (lp.ceiling_dbfs + 3.0 * lp.tolerance_db) / 20.0);
    const double band3_lo   = std::pow(10.0, (lp.ceiling_dbfs - 3.0 * lp.tolerance_db) / 20.0);

    // Matches synthesize_full's post-trim output length (see stft_container.h).
    const int64_t total_samples =
        static_cast<int64_t>(num_frames) * R_s + N / 2 - R_s;
    const size_t total_out_samples =
        static_cast<size_t>(total_samples) * channels;

    // -- Measurement synthesis --
    std::vector<float> meas_ola(total_out_samples, 0.0f);
    std::vector<std::complex<float>> cached_spectra(
        static_cast<size_t>(num_frames) * channels * K);

    size_t mem_pos = 0;
    auto write_mem = [&](const float* buf, size_t n_frames) {
        size_t n_samples = n_frames * channels;
        if (mem_pos + n_samples > meas_ola.size())
            n_samples = meas_ola.size() - mem_pos;
        std::memcpy(meas_ola.data() + mem_pos, buf, n_samples * sizeof(float));
        mem_pos += n_samples;
    };
    Synthesis::synthesize_full(stft,
                                cached_spectra.data(), write_mem,
                                /*show_progress=*/false,
                                /*pass_label=*/"");

    // -- Peak detection (all channels) --
    std::vector<Peak> queue;
    for (int ch = 0; ch < channels; ++ch)
        find_peaks_in_range(meas_ola.data(), 0, total_samples, channels, ch,
                             ceiling, queue);

    if (queue.empty()) {
        std::cout << "[Pass 2/3] Limiter.......................... 0 peaks, no attenuation required\n";
        return;
    }

    auto cmp_desc = [](const Peak& a, const Peak& b) {
        if (a.magnitude != b.magnitude) return a.magnitude > b.magnitude;
        return a.sample_idx < b.sample_idx;
    };
    std::sort(queue.begin(), queue.end(), cmp_desc);

    std::vector<Peak>   resolved;
    std::vector<double> reduction_db_list;
    resolved.reserve(queue.size());
    reduction_db_list.reserve(queue.size());

    std::vector<double> frame_time_buf(N);
    std::vector<float>  rescan_slice;
    std::vector<int>    frames_cov;

    int iterations = 0;

    auto within_tol  = [&](double v) { double a = std::abs(v); return a >= tol_amp_lo && a <= tol_amp_hi; };
    auto within_3tol = [&](double v) { double a = std::abs(v); return a >= band3_lo && a <= band3_hi; };

    while (!queue.empty()) {
        Peak peak = queue.front();
        queue.erase(queue.begin());
        ++iterations;

        const int64_t peak_pre = pre_trim(peak.sample_idx, N);

        contributing_frames(peak_pre, R_s, N, num_frames, frames_cov);
        if (frames_cov.empty()) continue;

        // Compute c_identity[i][b] = IFFT of band b of cached spectrum at identity attenuation,
        // sampled at local offset = peak_sample - m*R_s (pre-trim), windowed and /N.
        std::vector<double> c_id(frames_cov.size() * num_bands, 0.0);
        for (size_t i = 0; i < frames_cov.size(); ++i) {
            int m = frames_cov[i];
            int64_t local = peak_pre - static_cast<int64_t>(m) * R_s;
            if (local < 0 || local >= N) continue;
            const std::complex<float>* spec =
                cached_spectra.data() + (static_cast<size_t>(m) * channels + peak.ch) * K;
            for (int b = 0; b < num_bands; ++b) {
                band_ifft(stft, spec, b, 1.0, frame_time_buf);
                c_id[i * num_bands + b] = frame_time_buf[local];
            }
        }

        // Snapshot current attenuation for contributing frames/bands (pre-attempt base).
        std::vector<std::vector<double>> snapshot(frames_cov.size(),
            std::vector<double>(num_bands, 0.0));
        for (size_t i = 0; i < frames_cov.size(); ++i) {
            int m = frames_cov[i];
            for (int b = 0; b < num_bands; ++b)
                snapshot[i][b] = stft.attenuation_map[m][b];
        }

        // Analytic current peak value (should match measured within rounding).
        auto eval_map = [&](const std::vector<std::vector<double>>& smap) {
            double y = 0.0;
            for (size_t i = 0; i < frames_cov.size(); ++i)
                for (int b = 0; b < num_bands; ++b)
                    y += smap[i][b] * c_id[i * num_bands + b];
            return y;
        };
        double ref_val = eval_map(snapshot);
        if (std::abs(ref_val) <= ceiling) {
            // Already conforming under current map (neighbor attenuations resolved this
            // peak indirectly). Credit the full reduction from original_mag to |ref_val|
            // so the diag WAV reflects the real work done across iterations.
            double reduction_db = 0.0;
            if (std::abs(ref_val) > 1e-30 && peak.original_mag > 1e-30)
                reduction_db = 20.0 * std::log10(peak.original_mag / std::abs(ref_val));
            if (reduction_db < 0.0) reduction_db = 0.0;
            resolved.push_back(peak);
            reduction_db_list.push_back(reduction_db);
            continue;
        }
        double ref_sign = (ref_val >= 0.0) ? 1.0 : -1.0;

        // Apply proportional update with signed target. Operates on `base` snapshot,
        // writes result into `out`. r = target/current clamped to [0,1].
        //
        // Inner loop redistributes the reduction budget when bands would clamp at
        // zero: a pair whose tentative factor goes negative is frozen at 0, its
        // unrealized share is removed from the active pool, and the remaining
        // unfrozen pairs take on a larger share. Iterates until no new pair clamps
        // (or every pair is frozen). Without this, a single dominant band in one
        // frame stalls the outer loop — the peak comes down partway, gets re-queued
        // by the rescan, and the main loop burns iterations re-hitting it.
        auto apply_update = [&](const std::vector<std::vector<double>>& base,
                                 double current_val, double target_val,
                                 std::vector<std::vector<double>>& out) {
            out = base;
            if (std::abs(current_val) < 1e-30) return;
            double r = target_val / current_val;
            if (r < 0.0) r = 0.0;
            if (r > 1.0) r = 1.0;

            const size_t n_pairs = frames_cov.size() * static_cast<size_t>(num_bands);
            std::vector<double> contrib(n_pairs, 0.0);
            std::vector<unsigned char> frozen(n_pairs, 0);
            double total_mag = 0.0;
            for (size_t i = 0; i < frames_cov.size(); ++i)
                for (int b = 0; b < num_bands; ++b) {
                    double c = std::abs(base[i][b] * c_id[i * num_bands + b]);
                    contrib[i * num_bands + b] = c;
                    total_mag += c;
                }
            if (total_mag < 1e-30) return;

            const double one_minus_r = 1.0 - r;
            double active_total_mag    = total_mag;
            double clamped_contrib_sum = 0.0;
            double one_minus_r_adj     = one_minus_r;

            for (int inner = 0; inner < MAX_CLAMP_REDIST_TRIES; ++inner) {
                bool any_new_clamp = false;
                for (size_t p = 0; p < n_pairs; ++p) {
                    if (frozen[p]) continue;
                    double tentative = 1.0 - one_minus_r_adj * contrib[p] / active_total_mag;
                    if (tentative < 0.0) {
                        frozen[p] = 1;
                        clamped_contrib_sum += contrib[p];
                        active_total_mag    -= contrib[p];
                        any_new_clamp = true;
                    }
                }
                if (!any_new_clamp) break;
                if (active_total_mag < 1e-30) break;
                one_minus_r_adj =
                    (one_minus_r * total_mag - clamped_contrib_sum) / active_total_mag;
            }

            for (size_t i = 0; i < frames_cov.size(); ++i)
                for (int b = 0; b < num_bands; ++b) {
                    size_t p = i * num_bands + b;
                    double factor;
                    if (frozen[p] || active_total_mag < 1e-30) {
                        factor = 0.0;
                    } else {
                        factor = 1.0 - one_minus_r_adj * contrib[p] / active_total_mag;
                        if (factor < 0.0) factor = 0.0;
                    }
                    out[i][b] = base[i][b] * factor;
                }
        };

        // First attempt: aim at ceiling.
        std::vector<std::vector<double>> attempt_map;
        apply_update(snapshot, ref_val, ref_sign * ceiling, attempt_map);
        double attempt_val = eval_map(attempt_map);

        std::vector<std::vector<double>> best_map = attempt_map;
        double best_val = attempt_val;
        auto dist_to_ceiling = [&](double v) { return std::abs(std::abs(v) - ceiling); };

        // Refinement if outside 3*tol band.
        if (!(within_tol(attempt_val) || within_3tol(attempt_val))) {
            double last_val = attempt_val;
            double last_target_mag = ceiling;
            for (int t = 0; t < MAX_REFINEMENT_TRIES; ++t) {
                if (std::abs(last_val) < 1e-30) break;
                // Multiplicative target correction: if landed |v|, scale target by ceiling/|v|.
                double new_target_mag = last_target_mag * (ceiling / std::abs(last_val));
                if (new_target_mag < ceiling * 0.01) new_target_mag = ceiling * 0.01;
                apply_update(snapshot, ref_val, ref_sign * new_target_mag, attempt_map);
                double v = eval_map(attempt_map);
                if (dist_to_ceiling(v) < dist_to_ceiling(best_val)) {
                    best_val = v;
                    best_map = attempt_map;
                }
                last_val = v;
                last_target_mag = new_target_mag;
                if (within_tol(v)) break;
            }
        }

        // Commit best map.
        for (size_t i = 0; i < frames_cov.size(); ++i) {
            int m = frames_cov[i];
            for (int b = 0; b < num_bands; ++b)
                stft.attenuation_map[m][b] = best_map[i][b];
        }

        double reduction_db = 0.0;
        if (std::abs(best_val) > 1e-30 && peak.original_mag > 1e-30)
            reduction_db = 20.0 * std::log10(peak.original_mag / std::abs(best_val));
        if (reduction_db < 0.0) reduction_db = 0.0;

        // -- Rescan region --
        int64_t reg_start = peak.sample_idx - static_cast<int64_t>(RESCAN_HALF_WIDTH_FRAMES) * R_s;
        int64_t reg_end   = peak.sample_idx + static_cast<int64_t>(RESCAN_HALF_WIDTH_FRAMES) * R_s;
        if (reg_start < 0) reg_start = 0;
        if (reg_end > total_samples) reg_end = total_samples;

        bool extended = true;
        while (extended) {
            extended = false;
            for (const auto& q : queue) {
                if (q.sample_idx < reg_start || q.sample_idx >= reg_end) continue;
                int64_t need = static_cast<int64_t>(MIN_PEAK_EDGE_MARGIN) * R_s;
                if (q.sample_idx - reg_start < need) {
                    int64_t new_start = q.sample_idx - need;
                    if (new_start < 0) new_start = 0;
                    if (new_start < reg_start) { reg_start = new_start; extended = true; }
                }
                if (reg_end - 1 - q.sample_idx < need) {
                    int64_t new_end = q.sample_idx + need + 1;
                    if (new_end > total_samples) new_end = total_samples;
                    if (new_end > reg_end) { reg_end = new_end; extended = true; }
                }
            }
        }

        rescan_region(stft, cached_spectra.data(),
                      pre_trim(reg_start, N), pre_trim(reg_end, N),
                      rescan_slice);

        for (int64_t n = reg_start; n < reg_end; ++n) {
            size_t src_off = static_cast<size_t>(n - reg_start) * channels;
            size_t dst_off = static_cast<size_t>(n) * channels;
            for (int ch = 0; ch < channels; ++ch)
                meas_ola[dst_off + ch] = rescan_slice[src_off + ch];
        }

        // Remove queued peaks in region (carry their original_mag for matching).
        std::vector<Peak> carried;
        carried.reserve(queue.size());
        queue.erase(std::remove_if(queue.begin(), queue.end(),
            [&](const Peak& q) {
                bool inside = (q.sample_idx >= reg_start && q.sample_idx < reg_end);
                if (inside) carried.push_back(q);
                return inside;
            }), queue.end());

        // Re-scan region for new peaks across all channels.
        std::vector<Peak> region_peaks;
        for (int ch = 0; ch < channels; ++ch)
            find_peaks_in_range(meas_ola.data(), reg_start, reg_end, channels, ch,
                                 ceiling, region_peaks);

        for (auto& np : region_peaks) {
            for (const auto& op : carried) {
                if (op.ch == np.ch &&
                    std::llabs(op.sample_idx - np.sample_idx) <= PEAK_DEDUP_RADIUS) {
                    np.original_mag = op.original_mag;
                    break;
                }
            }
            queue.push_back(np);
        }
        std::sort(queue.begin(), queue.end(), cmp_desc);

        resolved.push_back(peak);
        reduction_db_list.push_back(reduction_db);
    }

    std::cout << "[Pass 2/3] Limiter.......................... "
              << resolved.size() << " peaks, " << iterations
              << " iterations, done\n";

    // -- Optional diagnostic WAV --
    if (lp.diag) {
        std::string diag_path = stft.output_audio_file;
        auto dot = diag_path.find_last_of('.');
        if (dot != std::string::npos) diag_path.insert(dot, "-limiter-diag");
        else                          diag_path += "-limiter-diag";

        SF_INFO dinfo = stft.src_info;
        dinfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        dinfo.channels = 1;
        SNDFILE* dsnd = sf_open(diag_path.c_str(), SFM_WRITE, &dinfo);
        if (!dsnd) {
            std::cerr << "  ! could not create limiter diag file '" << diag_path << "'\n";
        } else {
            std::vector<float> dbuf(static_cast<size_t>(total_samples), 0.0f);
            auto poke = [&](int64_t pos, float amp) {
                if (pos < 0 || pos >= total_samples) return;
                dbuf[static_cast<size_t>(pos)] = amp;
            };
            for (size_t i = 0; i < resolved.size(); ++i) {
                double red = reduction_db_list[i];
                double scale = red / DIAG_FLOOR_DB;
                if (scale > 1.0) scale = 1.0;
                if (scale < 0.0) scale = 0.0;
                float s = static_cast<float>(scale);
                int64_t pos = resolved[i].sample_idx;
                poke(pos - 1, -0.5f * s);
                poke(pos,     -1.0f * s);
                poke(pos + 1, -0.5f * s);
            }
            sf_writef_float(dsnd, dbuf.data(), static_cast<sf_count_t>(total_samples));
            sf_close(dsnd);
        }
    }
}
