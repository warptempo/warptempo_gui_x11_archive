#include "visualizer.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <algorithm>

void Visualizer::render_eq(const AudioSTFT& stft) {
    int N = stft.N;
    double nyquist      = stft.nyquist;
    double bin_hz_width = stft.bin_hz_width;

    std::string png_path = stft.tgt_audio_file + "_eq.png";
    std::cout << "          -> Rendering 1920x2160 dual-panel EQ Curve to: " << png_path << "\n";

    // Each panel is 1920×1080; total image is 1920×2160.
    constexpr double W = 1920.0;
    constexpr double PH = 1080.0;  // per-panel height

    auto get_x = [&](double hz) -> double {
        return W * (std::log10(hz) - std::log10(10.0)) / (std::log10(nyquist) - std::log10(10.0));
    };

    // Compute dynamic y-axis range from both curves
    double max_abs_db = 1.0;
    for (const auto& pt : stft.smoothed_curve_speedup)
        max_abs_db = std::max(max_abs_db, std::abs(pt.y));
    for (const auto& pt : stft.smoothed_curve_slowdown)
        max_abs_db = std::max(max_abs_db, std::abs(pt.y));
    double y_range = max_abs_db * 1.15;  // 15% headroom

    // Map dB → pixel y within a panel (y_offset = top of panel in SVG coords)
    auto get_y = [&](double db, double y_offset) -> double {
        double center = y_offset + PH / 2.0;
        return center - (std::max(-y_range, std::min(y_range, db)) / y_range) * (PH / 2.0 * 0.85);
    };

    // Frequency grid labels
    double freqs[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};

    // dB grid lines to draw (based on y_range)
    std::vector<int> db_marks;
    {
        int step = (y_range > 4.0) ? 2 : 1;
        for (int v = -static_cast<int>(std::ceil(y_range)); v <= static_cast<int>(std::ceil(y_range)); v += step)
            db_marks.push_back(v);
    }

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << W << " " << 2 * PH
        << "\" width=\"" << W << "\" height=\"" << 2 * PH << "\" font-family=\"sans-serif\">\n";
    svg << "  <rect width=\"" << W << "\" height=\"" << 2 * PH << "\" fill=\"#1e1e1e\" />\n";

    // Panel separator line
    svg << "  <line x1=\"0\" y1=\"" << PH << "\" x2=\"" << W << "\" y2=\"" << PH
        << "\" stroke=\"#444444\" stroke-width=\"2\" />\n";

    // Helper: draw one panel
    auto draw_panel = [&](const std::vector<double>& raw_delta,
                          const std::vector<Point>& smoothed,
                          double y_offset,
                          const std::string& label,
                          double alpha_ref,
                          bool is_fallback)
    {
        // Background (already filled by outer rect, just draw separator region hint)
        // dB grid
        for (int db : db_marks) {
            double y = get_y(static_cast<double>(db), y_offset);
            if (db == 0) {
                svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"" << W << "\" y2=\"" << y
                    << "\" stroke=\"#ffffff\" stroke-width=\"2\" opacity=\"0.6\" />\n";
                svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#ffffff\" font-size=\"16\" font-weight=\"bold\">0 dB</text>\n";
            } else {
                svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"" << W << "\" y2=\"" << y
                    << "\" stroke=\"#888888\" stroke-width=\"1\" stroke-dasharray=\"4\" opacity=\"0.3\" />\n";
                std::string sign = (db > 0) ? "+" : "";
                svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#888888\" font-size=\"14\">"
                    << sign << db << " dB</text>\n";
            }
        }

        // Frequency grid
        for (double f : freqs) {
            if (f > nyquist) continue;
            double x = get_x(f);
            svg << "  <line x1=\"" << x << "\" y1=\"" << y_offset
                << "\" x2=\"" << x << "\" y2=\"" << (y_offset + PH)
                << "\" stroke=\"#888888\" stroke-width=\"1\" opacity=\"0.15\" />\n";
            std::string lbl = (f >= 1000) ? std::to_string((int)(f / 1000)) + "k" : std::to_string((int)f);
            svg << "  <text x=\"" << x << "\" y=\"" << (y_offset + PH - 20)
                << "\" fill=\"#888888\" font-size=\"16\" text-anchor=\"middle\">" << lbl << "</text>\n";
        }

        // HPF marker
        if (stft.hpf_hz > 0.0) {
            double x_hpf = get_x(stft.hpf_hz);
            svg << "  <line x1=\"" << x_hpf << "\" y1=\"" << y_offset
                << "\" x2=\"" << x_hpf << "\" y2=\"" << (y_offset + PH)
                << "\" stroke=\"#ffffff\" stroke-width=\"1.5\" stroke-dasharray=\"5,5\" opacity=\"0.7\" />\n";
            svg << "  <text x=\"" << (x_hpf + 5) << "\" y=\"" << (y_offset + 110)
                << "\" fill=\"#ffffff\" font-size=\"14\" opacity=\"0.9\">HPF ("
                << std::fixed << std::setprecision(1) << stft.hpf_hz << " Hz)</text>\n";

            // HPF slope curve
            svg << "  <polyline fill=\"none\" stroke=\"#ff9955\" stroke-width=\"2\""
                << " stroke-linejoin=\"round\" opacity=\"0.8\" points=\"";
            double log_min = std::log10(10.0), log_max = std::log10(nyquist);
            double prev_f = 0.0;
            for (int i = 0; i < CURVE_RESOLUTION; ++i) {
                double f = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
                if (prev_f < stft.hpf_hz && f >= stft.hpf_hz)
                    svg << std::fixed << std::setprecision(2)
                        << get_x(stft.hpf_hz) << "," << get_y(-6.02, y_offset) << " ";
                double r8  = std::pow(f / stft.hpf_hz, 8.0);
                double hdb = 20.0 * std::log10(std::max(r8 / (1.0 + r8), 1e-10));
                svg << std::fixed << std::setprecision(2) << get_x(f) << "," << get_y(hdb, y_offset) << " ";
                prev_f = f;
            }
            svg << "\" />\n";
        }

        // Raw delta trace (gray thin)
        if (!raw_delta.empty()) {
            svg << "  <polyline fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1\""
                << " stroke-linejoin=\"round\" opacity=\"0.25\" points=\"";
            for (int k = 1; k <= N / 2; ++k) {
                double bin_hz = k * bin_hz_width;
                if (bin_hz < 10.0 || bin_hz > nyquist) continue;
                svg << std::fixed << std::setprecision(2)
                    << get_x(bin_hz) << "," << get_y(raw_delta[k], y_offset) << " ";
            }
            svg << "\" />\n";
        }

        // Smoothed curve (cyan thick)
        if (!smoothed.empty()) {
            svg << "  <polyline fill=\"none\" stroke=\"#00ffff\" stroke-width=\"4\""
                << " stroke-linejoin=\"round\" opacity=\"0.9\" points=\"";
            for (const auto& pt : smoothed)
                svg << std::fixed << std::setprecision(2)
                    << get_x(pt.x) << "," << get_y(pt.y, y_offset) << " ";
            svg << "\" />\n";
        }

        // Panel annotations
        double ann_y = y_offset + 60;
        svg << "  <text x=\"40\" y=\"" << ann_y << "\" fill=\"#ffffff\" font-size=\"22\""
            << " font-weight=\"bold\">" << label << "</text>\n";
        svg << "  <text x=\"40\" y=\"" << (ann_y + 30) << "\" fill=\"#aaaaaa\" font-size=\"16\">"
            << "α_ref = " << std::fixed << std::setprecision(4) << alpha_ref << "</text>\n";
        if (is_fallback) {
            svg << "  <text x=\"40\" y=\"" << (ann_y + 56) << "\" fill=\"#ffaa44\" font-size=\"14\""
                << " font-style=\"italic\">Fallback: insufficient slowdown material</text>\n";
        }
    };

    // Top panel: speedup
    draw_panel(stft.raw_delta_db_speedup, stft.smoothed_curve_speedup,
               0.0, "Speedup (\xce\xb1 < 1, get_alpha coords)", stft.alpha_ref_speedup, false);

    // Bottom panel: slowdown
    draw_panel(stft.raw_delta_db_slowdown, stft.smoothed_curve_slowdown,
               PH, "Slowdown (\xce\xb1 > 1, get_alpha coords)", stft.alpha_ref_slowdown,
               !stft.has_slowdown_frames);

    svg << "</svg>\n";

    std::string cmd = "magick svg:- -density 144 \"" + png_path
                    + "\" 2>/dev/null || rsvg-convert -f png -o \"" + png_path + "\"";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (pipe) {
        fwrite(svg.str().c_str(), 1, svg.str().length(), pipe);
        pclose(pipe);
    } else {
        std::cerr << "          -> Warning: Could not execute 'magick' or 'rsvg-convert' to generate PNG.\n";
    }
}
