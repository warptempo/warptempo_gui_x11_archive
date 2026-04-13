// eq_analysis.cpp — Standalone EQ analysis binary.
// Reads source and target WAV files, computes per-direction PSD delta,
// and renders a dual-panel diagnostic PNG. No gain is applied.
//
// Usage: eq_analysis <source_audio> <timemap_file> <target_audio> [output_png]
//
// output_png defaults to <target_audio stem>_eq.png
//
// Dependencies: libsndfile, FFTW3, ImageMagick (magick).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <fftw3.h>
#include <sndfile.h>

// Reuse the timemap helpers from the production container.
// stft_container.h defines TimeMapSegment, get_alpha, map_source_to_target,
// princarg, and AudioSTFT (which we do not instantiate here).
#include "stft_container.h"

// ============================================================
// Local constants (mirroring the former stft_container.h values)
// ============================================================
static constexpr int    CURVE_RESOLUTION         = 500;
static constexpr double MIN_STRETCH_SEC          = 5.0;
static constexpr int    TOP_STRETCH_REGIONS      = 10;
static constexpr double SLOWDOWN_FALLBACK_THRESHOLD = 0.10;
static constexpr double UNITY_EPSILON            = 1e-6;
static constexpr int    DEFAULT_N                = 4096;

struct Point { double x, y; };

// ============================================================
// Stretch region (source-frame space)
// ============================================================
struct StretchRegion {
    size_t src_start;
    size_t src_end;
    double mean_alpha;
    double duration_sec;
};

// ============================================================
// Lightweight analysis state
// ============================================================
struct AnalysisState {
    SF_INFO src_info{}, tgt_info_sf{};
    SNDFILE* src_snd  = nullptr;
    SNDFILE* tgt_snd  = nullptr;

    int    N             = DEFAULT_N;
    int    R_s           = 0;
    int    channels      = 0;
    double nyquist       = 0.0;
    double bin_hz_width  = 0.0;
    size_t target_total_frames = 0;

    std::vector<TimeMapSegment> timemap;
    std::vector<int64_t>        frame_map;
    std::vector<double>         window;

    double* fft_in    = nullptr;
    fftw_complex* fft_out = nullptr;
    fftw_plan plan_fwd{};

    // Results
    std::vector<double> raw_delta_db_speedup, raw_delta_db_slowdown;
    std::vector<Point>  smoothed_curve_speedup, smoothed_curve_slowdown;
    double alpha_ref_speedup  = 1.0;
    double alpha_ref_slowdown = 1.0;
    bool   has_slowdown_frames = false;

    std::string output_png;

    void init() {
        R_s = N / 4;
        window.resize(N);
        for (int n = 0; n < N; ++n)
            window[n] = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)));

        fft_in  = fftw_alloc_real(N);
        fft_out = fftw_alloc_complex(N / 2 + 1);
        plan_fwd = fftw_plan_dft_r2c_1d(N, fft_in, fft_out, FFTW_ESTIMATE);
    }

    void cleanup() {
        fftw_destroy_plan(plan_fwd);
        fftw_free(fft_in);
        fftw_free(fft_out);
        if (src_snd) sf_close(src_snd);
        if (tgt_snd) sf_close(tgt_snd);
    }

    // Mirrors AudioSTFT::generate_frame_map — iterates source in R_s hops.
    std::vector<int64_t> generate_frame_map() const {
        std::vector<int64_t> fmap;
        double t_a = -(double)N / 2.0;
        size_t t_s = 0;
        int    idx = 0;
        while (t_s < target_total_frames) {
            double alpha = get_alpha(t_s, timemap);
            double R_a   = R_s / alpha;
            if (idx > 0) t_a += R_a;
            fmap.push_back(static_cast<int64_t>(std::llround(t_a)));
            t_s += R_s;
            idx++;
        }
        return fmap;
    }
};

// ============================================================
// Region detection (adapted from EQMatcher::process)
// ============================================================
static void detect_regions(const AnalysisState& st,
                            std::vector<StretchRegion>& speedup_out,
                            std::vector<StretchRegion>& slowdown_out)
{
    const double sample_rate = static_cast<double>(st.src_info.samplerate);

    struct SegInfo {
        size_t src_start, src_end;
        double alpha;
        double duration_sec;
    };

    std::vector<SegInfo> segs;
    for (size_t i = 0; i + 1 < st.timemap.size(); ++i) {
        const auto& a = st.timemap[i];
        const auto& b = st.timemap[i + 1];
        if (b.src_frame <= a.src_frame) continue;
        double src_dur = static_cast<double>(b.src_frame - a.src_frame);
        double tgt_dur = static_cast<double>(b.tgt_frame - a.tgt_frame);
        segs.push_back({a.src_frame, b.src_frame,
                        tgt_dur / src_dur,
                        src_dur / sample_rate});
    }

    auto direction = [](double a) -> int {
        if (a < 1.0 - UNITY_EPSILON) return -1;
        if (a > 1.0 + UNITY_EPSILON) return  1;
        return 0;
    };

    struct Building {
        int    dir = 0;
        size_t src_start = 0, src_end = 0;
        double alpha_dur_sum = 0.0;
        double dur_sum = 0.0;
    } cur;

    auto flush = [&]() {
        if (cur.dir == 0 || cur.dur_sum < 1e-9) return;
        StretchRegion r{cur.src_start, cur.src_end,
                        cur.alpha_dur_sum / cur.dur_sum, cur.dur_sum};
        if (cur.dir == -1) speedup_out.push_back(r);
        else               slowdown_out.push_back(r);
    };

    for (const auto& s : segs) {
        int d = direction(s.alpha);
        if (d != cur.dir) {
            flush();
            cur.dir           = d;
            cur.src_start     = s.src_start;
            cur.src_end       = s.src_end;
            cur.alpha_dur_sum = s.alpha * s.duration_sec;
            cur.dur_sum       = s.duration_sec;
        } else {
            cur.src_end        = s.src_end;
            cur.alpha_dur_sum += s.alpha * s.duration_sec;
            cur.dur_sum       += s.duration_sec;
        }
    }
    flush();

    auto filter_and_rank = [](std::vector<StretchRegion>& v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [](const StretchRegion& r) {
                                   return r.duration_sec < MIN_STRETCH_SEC;
                               }),
                v.end());
        std::sort(v.begin(), v.end(),
                  [](const StretchRegion& a, const StretchRegion& b) {
                      return a.duration_sec > b.duration_sec;
                  });
        if (static_cast<int>(v.size()) > TOP_STRETCH_REGIONS)
            v.resize(TOP_STRETCH_REGIONS);
    };
    filter_and_rank(speedup_out);
    filter_and_rank(slowdown_out);
}

// ============================================================
// PSD accumulation + Gaussian smoothing (adapted from eq_matcher.cpp)
// Target is read from tgt_snd using map_source_to_target alignment.
// ============================================================
static void accumulate_and_smooth(
        AnalysisState& st,
        const std::vector<StretchRegion>& regions,
        std::vector<double>& raw_delta_out,
        std::vector<Point>&  smoothed_out,
        double& alpha_ref_out)
{
    const int    N          = st.N;
    const int    R_s        = st.R_s;
    const int    channels   = st.channels;
    const int    K          = N / 2 + 1;
    const int    num_frames = static_cast<int>(st.frame_map.size());

    std::vector<double> src_psd(K, 0.0);
    std::vector<double> tgt_psd(K, 0.0);
    size_t total_windows = 0;
    double alpha_sum     = 0.0;

    std::vector<float> src_chunk(N * channels, 0.0f);
    std::vector<float> tgt_chunk(N * channels, 0.0f);

    for (int m = 0; m < num_frames; ++m) {
        int64_t src_pos = st.frame_map[m];
        if (src_pos < 0 || src_pos >= st.src_info.frames) continue;

        // Check membership in any qualifying region
        bool in_region = false;
        for (const auto& r : regions) {
            if (static_cast<size_t>(src_pos) >= r.src_start &&
                static_cast<size_t>(src_pos) <  r.src_end) {
                in_region = true;
                break;
            }
        }
        if (!in_region) continue;

        // Target position via timemap
        double tgt_pos_d = map_source_to_target(static_cast<size_t>(src_pos), st.timemap);
        int64_t tgt_pos  = static_cast<int64_t>(std::llround(tgt_pos_d));
        if (tgt_pos < 0 || tgt_pos + N > st.tgt_info_sf.frames) continue;

        // Read source chunk
        sf_seek(st.src_snd, src_pos, SEEK_SET);
        sf_readf_float(st.src_snd, src_chunk.data(), N);

        // Read target chunk
        sf_seek(st.tgt_snd, tgt_pos, SEEK_SET);
        sf_readf_float(st.tgt_snd, tgt_chunk.data(), N);

        for (int ch = 0; ch < channels; ++ch) {
            // Source FFT
            for (int i = 0; i < N; ++i)
                st.fft_in[i] = src_chunk[i * channels + ch] * st.window[i];
            fftw_execute(st.plan_fwd);
            for (int k = 1; k < K; ++k)
                src_psd[k] += st.fft_out[k][0] * st.fft_out[k][0]
                            + st.fft_out[k][1] * st.fft_out[k][1];

            // Target FFT
            for (int i = 0; i < N; ++i)
                st.fft_in[i] = tgt_chunk[i * channels + ch] * st.window[i];
            fftw_execute(st.plan_fwd);
            for (int k = 1; k < K; ++k)
                tgt_psd[k] += st.fft_out[k][0] * st.fft_out[k][0]
                            + st.fft_out[k][1] * st.fft_out[k][1];
        }

        alpha_sum += get_alpha(static_cast<size_t>(m) * R_s, st.timemap);
        total_windows++;
    }

    std::cout << "  -> " << total_windows << " PSD windows accumulated.\n";
    alpha_ref_out = (total_windows > 0) ? (alpha_sum / total_windows) : 1.0;

    const double count = static_cast<double>(total_windows * channels);
    raw_delta_out.assign(K, 0.0);
    if (count > 0) {
        for (int k = 1; k < K; ++k) {
            if (src_psd[k] > 0 && tgt_psd[k] > 0)
                raw_delta_out[k] = 10.0 * std::log10(src_psd[k] / count)
                                 - 10.0 * std::log10(tgt_psd[k] / count);
        }
    }

    // Gaussian smoothing (1/3 octave sigma)
    const double nyquist       = st.nyquist;
    const double bin_hz_width  = st.bin_hz_width;
    const double log_min       = std::log10(10.0);
    const double log_max       = std::log10(nyquist);
    const double sigma_octaves = 1.0 / 3.0 / 2.355;

    smoothed_out.clear();
    for (int i = 0; i < CURVE_RESOLUTION; ++i) {
        double hz   = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
        double wsum = 0.0, dsum = 0.0;
        for (int k = 1; k < K; ++k) {
            double bin_hz = k * bin_hz_width;
            if (bin_hz < 10.0) continue;
            double w = std::exp(-0.5 * std::pow(std::log2(bin_hz / hz) / sigma_octaves, 2.0));
            if (w > 0.001) { wsum += w; dsum += w * raw_delta_out[k]; }
        }
        smoothed_out.push_back({hz, (wsum > 0.0) ? (dsum / wsum) : 0.0});
    }

    // Nyquist taper
    for (auto& pt : smoothed_out) {
        if (pt.x > nyquist - 20.0)
            pt.y *= std::max(0.0, std::min(1.0, (nyquist - pt.x) / 20.0));
    }
}

// ============================================================
// SVG panel rendering (adapted from visualizer.cpp)
// ============================================================
static std::string render_panel(
        int N, double nyquist, double bin_hz_width,
        const std::vector<double>& raw_delta,
        const std::vector<Point>&  smoothed,
        const std::string& alpha_label,
        double alpha_val)
{
    auto get_x = [nyquist](double hz) {
        return 1920.0 * (std::log10(hz) - std::log10(10.0)) / (std::log10(nyquist) - std::log10(10.0));
    };
    auto get_y = [](double db) {
        return 540.0 - (std::max(-6.0, std::min(6.0, db)) * 90.0);
    };

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\""
        << " width=\"1920\" height=\"1080\" font-family=\"sans-serif\">\n";
    svg << "  <rect width=\"1920\" height=\"1080\" fill=\"#1e1e1e\" />\n";

    // dB grid
    for (int db = -6; db <= 6; ++db) {
        double y = get_y(db);
        if (db == 0) {
            svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1920\" y2=\"" << y
                << "\" stroke=\"#ffffff\" stroke-width=\"2\" opacity=\"0.6\" />\n";
            svg << "  <text x=\"10\" y=\"" << (y - 5)
                << "\" fill=\"#ffffff\" font-size=\"16\" font-weight=\"bold\">0 dB</text>\n";
        } else {
            svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1920\" y2=\"" << y
                << "\" stroke=\"#888888\" stroke-width=\"1\" stroke-dasharray=\"4\" opacity=\"0.3\" />\n";
            std::string sign = (db > 0) ? "+" : "";
            svg << "  <text x=\"10\" y=\"" << (y - 5)
                << "\" fill=\"#888888\" font-size=\"14\">" << sign << db << " dB</text>\n";
        }
    }

    // Frequency grid
    double freqs[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    for (double f : freqs) {
        if (f > nyquist) continue;
        double x = get_x(f);
        svg << "  <line x1=\"" << x << "\" y1=\"0\" x2=\"" << x << "\" y2=\"1080\""
            << " stroke=\"#888888\" stroke-width=\"1\" opacity=\"0.15\" />\n";
        std::string lbl = (f >= 1000) ? std::to_string((int)(f / 1000)) + "k"
                                      : std::to_string((int)f);
        svg << "  <text x=\"" << x << "\" y=\"1060\""
            << " fill=\"#888888\" font-size=\"16\" text-anchor=\"middle\">" << lbl << "</text>\n";
    }

    // Raw delta trace (gray thin)
    if (!raw_delta.empty()) {
        svg << "  <polyline fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1\""
            << " stroke-linejoin=\"round\" opacity=\"0.25\" points=\"";
        for (int k = 1; k <= N / 2; ++k) {
            double bin_hz = k * bin_hz_width;
            if (bin_hz < 10.0 || bin_hz > nyquist) continue;
            svg << std::fixed << std::setprecision(2)
                << get_x(bin_hz) << "," << get_y(raw_delta[k]) << " ";
        }
        svg << "\" />\n";
    }

    // Smoothed curve (cyan thick)
    if (!smoothed.empty()) {
        svg << "  <polyline fill=\"none\" stroke=\"#00ffff\" stroke-width=\"4\""
            << " stroke-linejoin=\"round\" opacity=\"0.9\" points=\"";
        for (const auto& pt : smoothed)
            svg << std::fixed << std::setprecision(2)
                << get_x(pt.x) << "," << get_y(pt.y) << " ";
        svg << "\" />\n";
    }

    // Alpha annotation — top right
    svg << "  <text x=\"1912\" y=\"20\" fill=\"#aaaaaa\" font-size=\"16\""
        << " text-anchor=\"end\">" << alpha_label << " = "
        << std::fixed << std::setprecision(4) << alpha_val << "</text>\n";

    svg << "</svg>\n";
    return svg.str();
}

// ============================================================
// PNG rendering (adapted from Visualizer::render_eq)
// ============================================================
static void render_png(AnalysisState& st)
{
    const std::string& png_path = st.output_png;

    // Strip .png suffix to get a stem for temp files
    std::string stem = png_path;
    if (stem.size() >= 4 && stem.substr(stem.size() - 4) == ".png")
        stem = stem.substr(0, stem.size() - 4);

    auto write_svg = [](const std::string& path, const std::string& content) -> bool {
        std::ofstream f(path);
        if (!f) return false;
        f << content;
        return true;
    };
    auto convert = [](const std::string& svg, const std::string& png) -> bool {
        std::string cmd = "magick \"" + svg + "\" -density 144 \"" + png + "\" 2>/dev/null";
        return system(cmd.c_str()) == 0;
    };

    if (!st.has_slowdown_frames) {
        std::cout << "  -> Rendering 1920x1080 PNG (speedup only): " << png_path << "\n";
        std::string svg_path = stem + "_eq.svg";
        write_svg(svg_path, render_panel(
                st.N, st.nyquist, st.bin_hz_width,
                st.raw_delta_db_speedup, st.smoothed_curve_speedup,
                "alpha_ref_speedup", st.alpha_ref_speedup));
        if (!convert(svg_path, png_path))
            std::cerr << "  -> Warning: SVG conversion failed.\n";
        std::remove(svg_path.c_str());
        return;
    }

    std::cout << "  -> Rendering 1920x2160 PNG (dual panel): " << png_path << "\n";

    std::string svg_speedup  = stem + "_eq_speedup.svg";
    std::string svg_slowdown = stem + "_eq_slowdown.svg";
    std::string png_speedup  = stem + "_eq_speedup.png";
    std::string png_slowdown = stem + "_eq_slowdown.png";

    write_svg(svg_speedup, render_panel(
            st.N, st.nyquist, st.bin_hz_width,
            st.raw_delta_db_speedup, st.smoothed_curve_speedup,
            "alpha_ref_speedup", st.alpha_ref_speedup));
    write_svg(svg_slowdown, render_panel(
            st.N, st.nyquist, st.bin_hz_width,
            st.raw_delta_db_slowdown, st.smoothed_curve_slowdown,
            "alpha_ref_slowdown", st.alpha_ref_slowdown));

    if (!convert(svg_speedup, png_speedup) || !convert(svg_slowdown, png_slowdown))
        std::cerr << "  -> Warning: SVG conversion failed for one or both panels.\n";

    std::string stitch = "magick \"" + png_speedup + "\" \"" + png_slowdown
                       + "\" -append \"" + png_path + "\" 2>/dev/null";
    if (system(stitch.c_str()) != 0)
        std::cerr << "  -> Warning: Could not stitch panels.\n";

    std::remove(svg_speedup.c_str());
    std::remove(svg_slowdown.c_str());
    std::remove(png_speedup.c_str());
    std::remove(png_slowdown.c_str());
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <source_audio> <timemap_file> <target_audio> [output_png]\n";
        return 1;
    }

    const std::string src_path = argv[1];
    const std::string map_path = argv[2];
    const std::string tgt_path = argv[3];

    // Derive default output PNG name
    std::string default_png = tgt_path;
    {
        auto dot = default_png.rfind('.');
        if (dot != std::string::npos) default_png = default_png.substr(0, dot);
        default_png += "_eq.png";
    }
    const std::string out_png = (argc >= 5) ? argv[4] : default_png;

    // --------------------------------------------------------
    // Parse timemap
    // --------------------------------------------------------
    AnalysisState st;
    st.output_png = out_png;

    {
        std::ifstream f(map_path);
        if (!f) {
            std::cerr << "Error: Cannot open timemap: " << map_path << "\n";
            return 1;
        }
        size_t sf, tf;
        while (f >> sf >> tf) st.timemap.push_back({sf, tf});
    }
    if (st.timemap.size() < 2) {
        std::cerr << "Error: Timemap must have at least two entries.\n";
        return 1;
    }

    // --------------------------------------------------------
    // Open source audio
    // --------------------------------------------------------
    st.src_info.format = 0;
    st.src_snd = sf_open(src_path.c_str(), SFM_READ, &st.src_info);
    if (!st.src_snd) {
        std::cerr << "Error: Cannot open source audio: " << src_path << "\n";
        return 1;
    }

    // --------------------------------------------------------
    // Open target audio
    // --------------------------------------------------------
    st.tgt_info_sf.format = 0;
    st.tgt_snd = sf_open(tgt_path.c_str(), SFM_READ, &st.tgt_info_sf);
    if (!st.tgt_snd) {
        std::cerr << "Error: Cannot open target audio: " << tgt_path << "\n";
        sf_close(st.src_snd);
        return 1;
    }

    // Channel counts must match (we read both files with the same channel stride)
    if (st.src_info.channels != st.tgt_info_sf.channels) {
        std::cerr << "Error: Source and target have different channel counts ("
                  << st.src_info.channels << " vs " << st.tgt_info_sf.channels << ").\n";
        st.cleanup();
        return 1;
    }

    st.channels     = st.src_info.channels;
    st.nyquist      = st.src_info.samplerate / 2.0;
    st.bin_hz_width = static_cast<double>(st.src_info.samplerate) / st.N;
    st.target_total_frames = st.timemap.back().tgt_frame + st.N;

    // --------------------------------------------------------
    // Init FFTW + generate frame map
    // --------------------------------------------------------
    st.init();
    st.frame_map = st.generate_frame_map();
    std::cout << "[eq_analysis] " << st.frame_map.size() << " frames, "
              << "N=" << st.N << ", sr=" << st.src_info.samplerate << " Hz\n";

    // --------------------------------------------------------
    // Region detection
    // --------------------------------------------------------
    std::vector<StretchRegion> speedup_regions, slowdown_regions;
    detect_regions(st, speedup_regions, slowdown_regions);
    std::cout << "  Speedup regions: " << speedup_regions.size()
              << "  Slowdown regions: " << slowdown_regions.size() << "\n";

    // --------------------------------------------------------
    // Slowdown fallback check
    // --------------------------------------------------------
    const double src_total_sec = st.src_info.frames / static_cast<double>(st.src_info.samplerate);
    double slowdown_total_sec  = 0.0;
    for (const auto& r : slowdown_regions) slowdown_total_sec += r.duration_sec;
    st.has_slowdown_frames = (slowdown_total_sec / src_total_sec >= SLOWDOWN_FALLBACK_THRESHOLD);
    if (!st.has_slowdown_frames)
        std::cout << "  Slowdown content < 10% of source — single-panel output.\n";

    // --------------------------------------------------------
    // PSD accumulation — speedup
    // --------------------------------------------------------
    if (!speedup_regions.empty()) {
        std::cout << "  [Speedup] accumulating PSD...\n";
        accumulate_and_smooth(st, speedup_regions,
                              st.raw_delta_db_speedup,
                              st.smoothed_curve_speedup,
                              st.alpha_ref_speedup);
        std::cout << "  Speedup: alpha_ref=" << st.alpha_ref_speedup << "\n";
    } else {
        std::cerr << "  [WARN] No qualifying speedup regions.\n";
    }

    // --------------------------------------------------------
    // PSD accumulation — slowdown
    // --------------------------------------------------------
    if (st.has_slowdown_frames && !slowdown_regions.empty()) {
        std::cout << "  [Slowdown] accumulating PSD...\n";
        accumulate_and_smooth(st, slowdown_regions,
                              st.raw_delta_db_slowdown,
                              st.smoothed_curve_slowdown,
                              st.alpha_ref_slowdown);
        std::cout << "  Slowdown: alpha_ref=" << st.alpha_ref_slowdown << "\n";
    }

    // --------------------------------------------------------
    // Fallback copies
    // --------------------------------------------------------
    if (!st.has_slowdown_frames && !st.smoothed_curve_speedup.empty()) {
        st.raw_delta_db_slowdown   = st.raw_delta_db_speedup;
        st.smoothed_curve_slowdown = st.smoothed_curve_speedup;
        st.alpha_ref_slowdown      = st.alpha_ref_speedup;
    }
    if (speedup_regions.empty() && !st.smoothed_curve_slowdown.empty()) {
        std::cerr << "  [WARN] No qualifying speedup regions — using slowdown curve as reverse fallback.\n";
        st.raw_delta_db_speedup   = st.raw_delta_db_slowdown;
        st.smoothed_curve_speedup = st.smoothed_curve_slowdown;
        st.alpha_ref_speedup      = st.alpha_ref_slowdown;
    }
    if (st.smoothed_curve_speedup.empty() && st.smoothed_curve_slowdown.empty()) {
        std::cerr << "  [WARN] No qualifying stretch regions — nothing to plot.\n";
        st.cleanup();
        return 1;
    }

    // --------------------------------------------------------
    // Render PNG
    // --------------------------------------------------------
    render_png(st);

    st.cleanup();
    std::cout << "[eq_analysis] Done.\n";
    return 0;
}
