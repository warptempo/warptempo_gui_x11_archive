#include "visualizer.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstdio>

void Visualizer::render_eq(const AudioSTFT& stft) {
    int N = stft.N;
    double nyquist = stft.nyquist;
    double bin_hz_width = stft.bin_hz_width;

    std::string png_path = stft.perc_audio_file;
    size_t last_ext = png_path.find_last_of(".");
    if (last_ext != std::string::npos) png_path = png_path.substr(0, last_ext) + ".png";
    else png_path += ".png";

    std::cout << "          -> Rendering 1920x1080 EQ Curve to: " << png_path << "\n";

    auto get_x = [nyquist](double hz) {
        return 1920.0 * (std::log10(hz) - std::log10(10.0)) / (std::log10(nyquist) - std::log10(10.0));
    };
    auto get_y = [](double db) {
        return 540.0 - (std::max(-6.0, std::min(6.0, db)) * 90.0);
    };

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\" width=\"1920\" height=\"1080\" font-family=\"sans-serif\">\n";
    svg << "  <rect width=\"1920\" height=\"1080\" fill=\"#1a1a1a\" />\n";

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

    // Raw delta trace
    svg << "  <polyline fill=\"none\" stroke=\"rgba(180,180,180,0.95)\" stroke-width=\"1\" stroke-linejoin=\"round\" points=\"";
    for (int k = 1; k <= N / 2; ++k) {
        double bin_hz = k * bin_hz_width;
        if (bin_hz < 10.0 || bin_hz > nyquist) continue;
        svg << std::fixed << std::setprecision(2) << get_x(bin_hz) << "," << get_y(stft.raw_delta_db[k]) << " ";
    }
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
