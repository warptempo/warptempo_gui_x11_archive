#include "visualizer.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstdio>

void Visualizer::render_eq(const AudioSTFT& stft) {
    int N = stft.N;
    double nyquist = stft.nyquist;
    double bin_hz_width = stft.bin_hz_width;

    std::string png_path = stft.tgt_audio_file;
    size_t last_ext = png_path.find_last_of(".");
    if (last_ext != std::string::npos) png_path = png_path.substr(0, last_ext) + "_eq.png";
    else png_path += "_eq.png";

    std::cout << "          -> Rendering 1920x1080 EQ Curve to: " << png_path << "\n";

    auto get_x = [nyquist](double hz) {
        return 1920.0 * (std::log10(hz) - std::log10(10.0)) / (std::log10(nyquist) - std::log10(10.0));
    };
    auto get_y = [](double db) {
        return 540.0 - (std::max(-6.0, std::min(6.0, db)) * 90.0);
    };

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\" width=\"1920\" height=\"1080\" font-family=\"sans-serif\">\n";
    svg << "  <rect width=\"1920\" height=\"1080\" fill=\"#1e1e1e\" />\n";

    for (int db = -6; db <= 6; ++db) {
        double y = get_y(db);
        if (db == 0) {
            svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1920\" y2=\"" << y << "\" stroke=\"#ffffff\" stroke-width=\"2\" opacity=\"0.6\" />\n";
            svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#ffffff\" font-size=\"16\" font-weight=\"bold\">0 dB</text>\n";
        } else {
            svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1920\" y2=\"" << y << "\" stroke=\"#888888\" stroke-width=\"1\" stroke-dasharray=\"4\" opacity=\"0.3\" />\n";
            std::string sign = (db > 0) ? "+" : "";
            svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#888888\" font-size=\"14\">" << sign << db << " dB</text>\n";
        }
    }

    double freqs[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    for (double f : freqs) {
        if (f > nyquist) continue;
        double x = get_x(f);
        svg << "  <line x1=\"" << x << "\" y1=\"0\" x2=\"" << x << "\" y2=\"1080\" stroke=\"#888888\" stroke-width=\"1\" opacity=\"0.15\" />\n";
        std::string label = (f >= 1000) ? std::to_string((int)(f / 1000)) + "k" : std::to_string((int)f);
        svg << "  <text x=\"" << x << "\" y=\"1060\" fill=\"#888888\" font-size=\"16\" text-anchor=\"middle\">" << label << "</text>\n";
    }

    double x_thresh = get_x(stft.low_threshold_hz);
    svg << "  <line x1=\"" << x_thresh << "\" y1=\"0\" x2=\"" << x_thresh << "\" y2=\"1080\" stroke=\"#ffffff\" stroke-width=\"1.5\" stroke-dasharray=\"5,5\" opacity=\"0.7\" />\n";
    svg << "  <text x=\"" << (x_thresh + 5) << "\" y=\"110\" fill=\"#ffffff\" font-size=\"14\" opacity=\"0.9\">Low Threshold ("
        << std::fixed << std::setprecision(1) << stft.low_threshold_hz << " Hz)</text>\n";

    // Raw delta trace
    svg << "  <polyline fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1\" stroke-linejoin=\"round\" opacity=\"0.25\" points=\"";
    for (int k = 1; k <= N / 2; ++k) {
        double bin_hz = k * bin_hz_width;
        if (bin_hz < 10.0 || bin_hz > nyquist) continue;
        svg << std::fixed << std::setprecision(2) << get_x(bin_hz) << "," << get_y(stft.raw_delta_db[k]) << " ";
    }
    svg << "\" />\n";

    // Smoothed curve
    svg << "  <polyline fill=\"none\" stroke=\"#00ffff\" stroke-width=\"4\" stroke-linejoin=\"round\" opacity=\"0.9\" points=\"";
    for (const auto& pt : stft.smoothed_curve)
        svg << std::fixed << std::setprecision(2) << get_x(pt.x) << "," << get_y(pt.y) << " ";
    svg << "\" />\n</svg>\n";

    std::string cmd = "magick svg:- -density 144 \"" + png_path + "\" 2>/dev/null || rsvg-convert -f png -o \"" + png_path + "\"";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (pipe) {
        fwrite(svg.str().c_str(), 1, svg.str().length(), pipe);
        pclose(pipe);
    } else {
        std::cerr << "          -> Warning: Could not execute 'magick' or 'rsvg-convert' to generate PNG.\n";
    }
}

void Visualizer::render_dynamics(const AudioSTFT& stft) {
    std::cout << "          -> Rendering Dynamics GR Plot...\n";

    int total_analysis_frames = stft.total_analysis_frames;

    std::string dyn_png_path = stft.tgt_audio_file;
    size_t dyn_last_ext = dyn_png_path.find_last_of(".");
    dyn_png_path = (dyn_last_ext != std::string::npos)
                       ? dyn_png_path.substr(0, dyn_last_ext) + "_dynamics.png"
                       : dyn_png_path + "_dynamics.png";

    double render_range_db = -2.0;
    auto get_dyn_y = [render_range_db](double db) { return 100.0 + (db / render_range_db) * 880.0; };

    std::ostringstream dyn_svg;
    dyn_svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\" width=\"1920\" height=\"1080\">\n";
    dyn_svg << "  <rect width=\"1920\" height=\"1080\" fill=\"#1e1e1e\" />\n";

    for (double db = 0.0; db >= render_range_db; db -= 0.5) {
        double y = get_dyn_y(db);
        dyn_svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1920\" y2=\"" << y << "\" stroke=\"#ffffff\" stroke-width=\"1\" opacity=\"0.3\" />\n";
        dyn_svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#ffffff\" font-size=\"14\">" << db << " dB</text>\n";
    }

    const std::vector<double>* trajs[3] = {&stft.S_traj_L, &stft.S_traj_M, &stft.S_traj_H};
    std::string colors[3] = {"#ff5555", "#55ff55", "#5555ff"};

    for (int b = 0; b < 3; ++b) {
        dyn_svg << "  <polyline fill=\"none\" stroke=\"" << colors[b] << "\" stroke-width=\"2\" points=\"";
        for (int m = 0; m < total_analysis_frames; m += 10) {
            double mean_db = 20.0 * std::log10((*trajs[b])[m]);
            mean_db = std::max(render_range_db, mean_db);
            double x = 1920.0 * ((double)m / total_analysis_frames);
            double y = get_dyn_y(mean_db);
            dyn_svg << x << "," << y << " ";
        }
        dyn_svg << "\" />\n";
    }
    dyn_svg << "</svg>\n";

    std::string dyn_cmd = "magick svg:- -density 144 \"" + dyn_png_path + "\" 2>/dev/null || rsvg-convert -f png -o \"" + dyn_png_path + "\"";
    FILE* dyn_pipe = popen(dyn_cmd.c_str(), "w");
    if (dyn_pipe) {
        fwrite(dyn_svg.str().c_str(), 1, dyn_svg.str().length(), dyn_pipe);
        pclose(dyn_pipe);
    }
}
