#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <sndfile.h>
#include <ebur128.h>
#include <fftw3.h>
#include <cstdio>

// --- Configuration Constants ---
const double ATTACK_TOLERANCE_LU = 3.0;      
const double RELEASE_TOLERANCE_LU = 8.0;     
const double MIN_GAP_SEC = 0.5;              
const double MIN_PHRASE_SEC = 5.0;           
const size_t TOP_X_CHUNKS = 10;               
const int CURVE_RESOLUTION = 500; 

// --- Data Structures ---
struct TimeMapSegment {
    size_t src_frame;
    size_t tgt_frame;
};

struct AcousticBlock {
    size_t start_frame;
    size_t end_frame;
    double duration_sec;
};

struct Point {
    double x, y;
};

// --- DSP Helpers ---
double princarg(double phase) {
    return phase - 2.0 * M_PI * std::floor((phase + M_PI) / (2.0 * M_PI));
}

double get_alpha(size_t t_s, const std::vector<TimeMapSegment>& map) {
    if (map.empty()) return 1.0;
    if (t_s <= map.front().tgt_frame) return 1.0;
    for (size_t i = 0; i < map.size() - 1; ++i) {
        if (t_s >= map[i].tgt_frame && t_s < map[i+1].tgt_frame) {
            double tgt_dur = static_cast<double>(map[i+1].tgt_frame - map[i].tgt_frame);
            double src_dur = static_cast<double>(map[i+1].src_frame - map[i].src_frame);
            return tgt_dur / src_dur;
        }
    }
    return 1.0; 
}

double map_source_to_target(size_t src_frame, const std::vector<TimeMapSegment>& map) {
    if (map.empty()) return static_cast<double>(src_frame);
    if (src_frame <= map.front().src_frame) return map.front().tgt_frame;
    for (size_t i = 0; i < map.size() - 1; ++i) {
        if (src_frame >= map[i].src_frame && src_frame < map[i+1].src_frame) {
            double src_dur = static_cast<double>(map[i+1].src_frame - map[i].src_frame);
            double tgt_dur = static_cast<double>(map[i+1].tgt_frame - map[i].tgt_frame);
            double offset = static_cast<double>(src_frame - map[i].src_frame);
            return map[i].tgt_frame + (offset * (tgt_dur / src_dur));
        }
    }
    const auto& last = map.back();
    if (src_frame >= last.src_frame) {
        return last.tgt_frame + (src_frame - last.src_frame); 
    }
    return 0.0;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <source_audio> <timemap_file> <target_audio> "
                  << "[N=3328] [low_thresh_hz=50.0]\n"
                  << "       [thresh_L/M/H=-25,-20,-25] [knee=6.0] [atten_max=-24.0]\n"
                  << "       [tauB_L/M/H=50,30,20] [tauF_L/M/H=250,150,80] [depth_L/M/H=1.0,1.0,1.0]\n"
                  << "       [xover_L,H=120,3500]\n";
        return 1;
    }

    std::string src_audio_file = argv[1];
    std::string map_file = argv[2];
    std::string tgt_audio_file = argv[3];
    
    int N = (argc >= 5) ? std::stoi(argv[4]) : 3328;
    double low_threshold_hz = (argc >= 6) ? std::stod(argv[5]) : 50.0;

    // V2 Dynamics Parameters (with defaults)
    double thresh_low = -25.0, thresh_mid = -20.0, thresh_high = -25.0;
    double knee_low = 6.0, knee_mid = 6.0, knee_high = 6.0;
    double atten_low = -24.0, atten_mid = -24.0, atten_high = -24.0;
    double tau_B_low = 50.0, tau_B_mid = 30.0, tau_B_high = 20.0;
    double tau_F_low = 250.0, tau_F_mid = 150.0, tau_F_high = 80.0;

    // DEPTHS (Ratio Overdrive. 1.0 = 100% cut, 2.0 = exponential overdrive)
    double depth_low = 1.0, depth_mid = 1.0, depth_high = 1.0;
    
    // CROSSOVERS (Defaults: 120Hz and 3.5kHz)
    double xover_low = 120.0, xover_high = 3500.0;

    // CLI Parsing

    // CLI Parsing
    if (argc >= 7) std::sscanf(argv[6], "%lf,%lf,%lf", &thresh_low, &thresh_mid, &thresh_high);
    if (argc >= 8) knee_low = knee_mid = knee_high = std::stod(argv[7]); 
    if (argc >= 9) atten_low = atten_mid = atten_high = std::stod(argv[8]); 
    if (argc >= 10) std::sscanf(argv[9], "%lf,%lf,%lf", &tau_B_low, &tau_B_mid, &tau_B_high);
    if (argc >= 11) std::sscanf(argv[10], "%lf,%lf,%lf", &tau_F_low, &tau_F_mid, &tau_F_high);
    if (argc >= 12) std::sscanf(argv[11], "%lf,%lf,%lf", &depth_low, &depth_mid, &depth_high);
    if (argc >= 13) std::sscanf(argv[12], "%lf,%lf", &xover_low, &xover_high);
    
    if (N % 4 != 0) {
        std::cerr << "Error: N must be divisible by 4.\n";
        return 1;
    }
    int R_s = N / 4;

    // 1. Parse Timemap
    std::vector<TimeMapSegment> timemap;
    std::ifstream file(map_file);
    if (!file.is_open()) { std::cerr << "Error: Could not open timemap.\n"; return 1; }
    size_t src_f, tgt_f;
    while (file >> src_f >> tgt_f) timemap.push_back({src_f, tgt_f});
    file.close();

    // 2. Open Source Audio
    SF_INFO src_info; src_info.format = 0;
    SNDFILE* src_snd = sf_open(src_audio_file.c_str(), SFM_READ, &src_info);
    if (!src_snd) { 
        std::cerr << "Error: Could not open Source file: '" << src_audio_file << "'\n";
        return 1; 
    }
    int channels = src_info.channels;
    double nyquist = src_info.samplerate / 2.0;
    double bin_hz_width = static_cast<double>(src_info.samplerate) / N;
    size_t target_total_frames = timemap.back().tgt_frame + N;

    // 3. FFTW Setup
    std::vector<double> window(N), synth_window(N);
    for (int n = 0; n < N; ++n) {
        window[n] = 0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1)));
        synth_window[n] = window[n] / 1.5; 
    }
    double* fft_in = fftw_alloc_real(N);
    fftw_complex* fft_out = fftw_alloc_complex(N / 2 + 1);
    fftw_plan plan_fwd = fftw_plan_dft_r2c_1d(N, fft_in, fft_out, FFTW_ESTIMATE);
    fftw_complex* ifft_in = fftw_alloc_complex(N / 2 + 1);
    double* ifft_out = fftw_alloc_real(N);
    fftw_plan plan_inv = fftw_plan_dft_c2r_1d(N, ifft_in, ifft_out, FFTW_ESTIMATE);

    // ========================================================================
    // PASS 1: The Dry Pass (Synthesis to Virtual Memory Buffer)
    // ========================================================================
    std::cout << "[Pass 1] Executing Dry Pass (N=" << N << ") -> Virtual Memory Buffer...\n";
    std::vector<float> virtual_tgt_buf;
    virtual_tgt_buf.reserve(target_total_frames * channels);
    
    std::vector<std::vector<double>> phi_prev(channels, std::vector<double>(N / 2 + 1, 0.0));
    std::vector<std::vector<double>> theta_prev(channels, std::vector<double>(N / 2 + 1, 0.0));
    std::vector<std::vector<double>> overlap_add(channels, std::vector<double>(N, 0.0));
    std::vector<float> read_buf(N * channels, 0.0f);

    double t_a = -(double)N / 2.0; 
    long t_a_rounded_prev = std::round(t_a);
    size_t t_s = 0; 
    int frame_idx = 0;
    int frames_to_skip = N / 2; 

    while (t_s < target_total_frames) {
        double alpha = get_alpha(t_s, timemap);
        double R_a = R_s / alpha;
        if (frame_idx > 0) t_a += R_a;
        long t_a_rounded = std::round(t_a);
        long R_a_actual = t_a_rounded - t_a_rounded_prev;

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded < src_info.frames) {
            sf_seek(src_snd, std::max(0L, t_a_rounded), SEEK_SET);
            sf_readf_float(src_snd, read_buf.data(), N);
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n) fft_in[n] = read_buf[n * channels + ch] * window[n];
            fftw_execute(plan_fwd);
            
            std::vector<double> M(N / 2 + 1, 0.0), phi(N / 2 + 1, 0.0), theta(N / 2 + 1, 0.0);
            for (int k = 0; k <= N / 2; ++k) {
                M[k] = std::sqrt(fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);
                phi[k] = std::atan2(fft_out[k][1], fft_out[k][0]);
            }

            if (frame_idx == 0) {
                for (int k = 0; k <= N / 2; ++k) theta[k] = phi[k];
            } else {
                std::vector<int> peaks;
                for (int k = 1; k < N / 2; ++k) {
                    if (M[k] > M[k-1] && M[k] > M[k+1]) peaks.push_back(k);
                }
                if (peaks.empty()) peaks.push_back(N/4); 

                for (int p : peaks) {
                    double omega_p = 2.0 * M_PI * p / N;
                    theta[p] = theta_prev[ch][p] + (omega_p + princarg(phi[p] - phi_prev[ch][p] - omega_p * R_a_actual) / R_a_actual) * R_s;
                }

                size_t peak_idx = 0;
                for (int k = 0; k <= N / 2; ++k) {
                    if (peak_idx < peaks.size() - 1 && std::abs(k - peaks[peak_idx + 1]) < std::abs(k - peaks[peak_idx])) peak_idx++;
                    int p = peaks[peak_idx];
                    if (k != p) theta[k] = theta[p] + phi[k] - phi[p];
                }
            }

            for (int k = 0; k <= N / 2; ++k) {
                phi_prev[ch][k] = phi[k]; theta_prev[ch][k] = theta[k];
                ifft_in[k][0] = M[k] * std::cos(theta[k]);
                ifft_in[k][1] = M[k] * std::sin(theta[k]);
            }
            fftw_execute(plan_inv);

            for (int n = 0; n < N; ++n) overlap_add[ch][n] += (ifft_out[n] / N) * synth_window[n];
        }

        int write_offset = 0, write_len = R_s;
        if (frames_to_skip > 0) {
            if (frames_to_skip >= write_len) { frames_to_skip -= write_len; write_len = 0; } 
            else { write_offset = frames_to_skip; write_len -= frames_to_skip; frames_to_skip = 0; }
        }
        for (int n = write_offset; n < write_offset + write_len; ++n) {
            for (int ch = 0; ch < channels; ++ch) virtual_tgt_buf.push_back(static_cast<float>(overlap_add[ch][n]));
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) overlap_add[ch][n] = overlap_add[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n) overlap_add[ch][n] = 0.0;
        }
        t_s += R_s; t_a_rounded_prev = t_a_rounded; frame_idx++;
    }

    // ========================================================================
    // PASS 2: EQ Match (Static Spectral Analysis - Original Exact Block)
    // ========================================================================
    std::cout << "\n[Pass 2] Profiling Source Acoustics & Virtual PSD Delta...\n";
    ebur128_state* st = ebur128_init(channels, src_info.samplerate, EBUR128_MODE_M | EBUR128_MODE_I);
    
    std::vector<float> ebur_buf(4096 * channels);
    sf_seek(src_snd, 0, SEEK_SET);
    while (sf_readf_float(src_snd, ebur_buf.data(), 4096) > 0) ebur128_add_frames_float(st, ebur_buf.data(), 4096);
    double src_global_lufs; ebur128_loudness_global(st, &src_global_lufs);
    double attack_thresh = src_global_lufs - ATTACK_TOLERANCE_LU;
    double release_thresh = src_global_lufs - RELEASE_TOLERANCE_LU;

    std::vector<AcousticBlock> blocks;
    bool is_loud = false; size_t current_start = 0; size_t current_frame = 0;
    sf_seek(src_snd, 0, SEEK_SET);
    while (true) {
        sf_count_t read_count = sf_readf_float(src_snd, ebur_buf.data(), 4096);
        if (read_count <= 0) break;
        ebur128_add_frames_float(st, ebur_buf.data(), read_count);
        current_frame += read_count;
        double momentary; ebur128_loudness_momentary(st, &momentary);
        if (!is_loud && momentary >= attack_thresh) { is_loud = true; current_start = current_frame; }
        else if (is_loud && momentary < release_thresh) {
            is_loud = false; blocks.push_back({current_start, current_frame, 0.0});
        }
    }
    if (is_loud) blocks.push_back({current_start, current_frame, 0.0});
    ebur128_destroy(&st);
    
    std::vector<AcousticBlock> merged_blocks;
    size_t min_gap = MIN_GAP_SEC * src_info.samplerate;
    for (const auto& b : blocks) {
        if (merged_blocks.empty()) merged_blocks.push_back(b);
        else {
            if ((b.start_frame - merged_blocks.back().end_frame) < min_gap) merged_blocks.back().end_frame = b.end_frame;
            else merged_blocks.push_back(b);
        }
    }

    std::vector<AcousticBlock> final_blocks;
    for (auto& b : merged_blocks) {
        b.duration_sec = static_cast<double>(b.end_frame - b.start_frame) / src_info.samplerate;
        if (b.duration_sec >= MIN_PHRASE_SEC) final_blocks.push_back(b);
    }
    std::sort(final_blocks.begin(), final_blocks.end(), [](const AcousticBlock& a, const AcousticBlock& b) { return a.duration_sec > b.duration_sec; });
    if (final_blocks.size() > TOP_X_CHUNKS) final_blocks.resize(TOP_X_CHUNKS);

    std::vector<double> src_psd_linked(N / 2 + 1, 0.0);
    std::vector<double> tgt_psd_linked(N / 2 + 1, 0.0);
    size_t total_psd_windows = 0;
    std::vector<float> src_chunk(N * channels), tgt_chunk(N * channels);
    
    for (const auto& b : final_blocks) {
        size_t s_frame = b.start_frame;
        while (s_frame + N <= b.end_frame) {
            size_t t_start = static_cast<size_t>(map_source_to_target(s_frame, timemap));
            
            sf_seek(src_snd, s_frame, SEEK_SET);
            sf_readf_float(src_snd, src_chunk.data(), N);
            
            if (t_start + N <= virtual_tgt_buf.size() / channels) {
                std::copy(virtual_tgt_buf.begin() + t_start * channels, 
                          virtual_tgt_buf.begin() + (t_start + N) * channels, tgt_chunk.begin());
            }

            for (int ch = 0; ch < channels; ++ch) {
                for (int i = 0; i < N; ++i) fft_in[i] = src_chunk[i * channels + ch] * window[i];
                fftw_execute(plan_fwd);
                for (int k = 1; k <= N / 2; ++k) src_psd_linked[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);

                for (int i = 0; i < N; ++i) fft_in[i] = tgt_chunk[i * channels + ch] * window[i];
                fftw_execute(plan_fwd);
                for (int k = 1; k <= N / 2; ++k) tgt_psd_linked[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);
            }
            total_psd_windows++;
            s_frame += N / 2;
        }
    }

    std::vector<double> raw_delta_db(N / 2 + 1, 0.0);
    double total_measurements = static_cast<double>(total_psd_windows * channels);

    for (int k = 1; k <= N / 2; ++k) {
        if (src_psd_linked[k] > 0 && tgt_psd_linked[k] > 0) {
            raw_delta_db[k] = 10.0 * std::log10(src_psd_linked[k] / total_measurements) 
                            - 10.0 * std::log10(tgt_psd_linked[k] / total_measurements);
        }
    }

    std::vector<Point> smoothed_curve;
    double log_min = std::log10(10.0), log_max = std::log10(nyquist);
    double sigma_octaves = 1.0 / 3.0 / 2.355;
    for (int i = 0; i < CURVE_RESOLUTION; ++i) {
        double current_hz = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
        double weight_sum = 0.0, delta_sum = 0.0;
        for (int k = 1; k <= N / 2; ++k) {
            double bin_hz = k * bin_hz_width;
            if (bin_hz < 10.0) continue;
            double weight = std::exp(-0.5 * std::pow(std::log2(bin_hz / current_hz) / sigma_octaves, 2.0));
            if (weight > 0.001) { weight_sum += weight; delta_sum += weight * raw_delta_db[k]; }
        }
        smoothed_curve.push_back({current_hz, (weight_sum > 0.0) ? (delta_sum / weight_sum) : 0.0});
    }

    int num_pts = smoothed_curve.size();
    std::vector<double> drop_slope(num_pts - 1, 0.0);
    for (int i = 0; i < num_pts - 1; ++i) drop_slope[i] = (smoothed_curve[i].y - smoothed_curve[i+1].y) / std::log2(smoothed_curve[i+1].x / smoothed_curve[i].x);

    bool in_fall = false, crossed_threshold = false;
    double current_steepest_slope = 0.0; int current_inflection_idx = -1;
    for (int i = num_pts - 2; i >= 0; --i) {
        if (drop_slope[i] < 0.0) {
            if (!in_fall) { in_fall = true; crossed_threshold = false; current_steepest_slope = 0.0; current_inflection_idx = -1; }
            if (drop_slope[i] < current_steepest_slope) { current_steepest_slope = drop_slope[i]; current_inflection_idx = i; }
            if (smoothed_curve[i].x <= low_threshold_hz) crossed_threshold = true;
        } else if (drop_slope[i] > 0.0) {
            if (in_fall && crossed_threshold && current_inflection_idx != -1) {
                double x_trig = smoothed_curve[current_inflection_idx].x, y_trig = smoothed_curve[current_inflection_idx].y;
                for (int j = current_inflection_idx - 1; j >= 0; --j) smoothed_curve[j].y = y_trig + (current_steepest_slope * std::log2(x_trig / smoothed_curve[j].x));
                break;
            }
            in_fall = false; crossed_threshold = false;
        }
    }

    for (auto& pt : smoothed_curve) {
        if (pt.x > nyquist - 20.0) pt.y *= std::max(0.0, std::min(1.0, (nyquist - pt.x) / 20.0));
    }

    auto get_interp_db = [&](double hz) {
        if (hz <= smoothed_curve.front().x) return smoothed_curve.front().y;
        if (hz >= smoothed_curve.back().x) return smoothed_curve.back().y;
        auto it = std::lower_bound(smoothed_curve.begin(), smoothed_curve.end(), hz, [](const Point& p, double f){ return p.x < f; });
        auto prev = it - 1;
        return prev->y + ((hz - prev->x) / (it->x - prev->x)) * (it->y - prev->y);
    };

    std::vector<double> multiplier_array(N / 2 + 1, 1.0);
    for (int k = 1; k <= N / 2; ++k) {
        double target_db = get_interp_db(k * bin_hz_width);
        multiplier_array[k] = std::pow(10.0, target_db / 20.0);
    }
    std::cout << "          -> Target curve baked into M[k] multiplier array.\n";
    
    // --- 2D. High-Resolution PNG Rendering ---
    std::string png_path = tgt_audio_file;
    size_t last_ext = png_path.find_last_of(".");
    if (last_ext != std::string::npos) png_path = png_path.substr(0, last_ext) + "_eq.png";
    else png_path += "_eq.png";

    std::cout << "          -> Rendering 1920x1080 EQ Curve to: " << png_path << "\n";
    
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\" width=\"1920\" height=\"1080\" font-family=\"sans-serif\">\n";
    svg << "  <rect width=\"1920\" height=\"1080\" fill=\"#1e1e1e\" />\n";

    auto get_x = [nyquist](double hz) { return 1920.0 * (std::log10(hz) - std::log10(10.0)) / (std::log10(nyquist) - std::log10(10.0)); };
    auto get_y = [](double db) { return 540.0 - (std::max(-6.0, std::min(6.0, db)) * 90.0); };

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
        std::string label = (f >= 1000) ? std::to_string((int)(f/1000)) + "k" : std::to_string((int)f);
        svg << "  <text x=\"" << x << "\" y=\"1060\" fill=\"#888888\" font-size=\"16\" text-anchor=\"middle\">" << label << "</text>\n";
    }

    double x_thresh = get_x(low_threshold_hz);
    svg << "  <line x1=\"" << x_thresh << "\" y1=\"0\" x2=\"" << x_thresh << "\" y2=\"1080\" stroke=\"#ffffff\" stroke-width=\"1.5\" stroke-dasharray=\"5,5\" opacity=\"0.7\" />\n";
    svg << "  <text x=\"" << (x_thresh + 5) << "\" y=\"110\" fill=\"#ffffff\" font-size=\"14\" opacity=\"0.9\">Low Threshold (" << std::fixed << std::setprecision(1) << low_threshold_hz << " Hz)</text>\n";
    
    svg << "  <polyline fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1\" stroke-linejoin=\"round\" opacity=\"0.25\" points=\"";
    for (int k = 1; k <= N / 2; ++k) {
        double bin_hz = k * bin_hz_width;
        if (bin_hz < 10.0 || bin_hz > nyquist) continue;
        svg << std::fixed << std::setprecision(2) << get_x(bin_hz) << "," << get_y(raw_delta_db[k]) << " ";
    }
    svg << "\" />\n";
    
    svg << "  <polyline fill=\"none\" stroke=\"#00ffff\" stroke-width=\"4\" stroke-linejoin=\"round\" opacity=\"0.9\" points=\"";
    for (const auto& pt : smoothed_curve) svg << std::fixed << std::setprecision(2) << get_x(pt.x) << "," << get_y(pt.y) << " ";
    svg << "\" />\n</svg>\n";

    std::string cmd = "magick svg:- -density 144 \"" + png_path + "\" 2>/dev/null || rsvg-convert -f png -o \"" + png_path + "\"";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (pipe) {
        fwrite(svg.str().c_str(), 1, svg.str().length(), pipe);
        pclose(pipe);
    } else {
        std::cerr << "          -> Warning: Could not execute 'magick' or 'rsvg-convert' to generate PNG.\n";
    }

    // ========================================================================
    // PASS 3: Dynamics Match (V2: LR4 Multiband & Energy-Weighted Detection)
    // ========================================================================
    std::cout << "\n[Pass 3] Analyzing LR4 Multiband Dynamics (Energy-Weighted)...\n";

    int total_analysis_frames = frame_idx;
    
    // 1. LR4 Weight Initialization
    std::vector<double> W_L(N / 2 + 1, 0.0);
    std::vector<double> W_M(N / 2 + 1, 0.0);
    std::vector<double> W_H(N / 2 + 1, 0.0);
    for (int k = 1; k <= N / 2; ++k) {
        double hz = k * bin_hz_width;
        W_L[k] = 1.0 / (1.0 + std::pow(hz / xover_low, 8.0));
        W_H[k] = 1.0 / (1.0 + std::pow(xover_high / hz, 8.0));
        W_M[k] = 1.0 - W_L[k] - W_H[k];
        if (W_M[k] < 0.0) W_M[k] = 0.0;
    }

    // Macro-Band Smoothing Trajectories
    std::vector<double> S_traj_L(total_analysis_frames, 1.0);
    std::vector<double> S_traj_M(total_analysis_frames, 1.0);
    std::vector<double> S_traj_H(total_analysis_frames, 1.0);

    t_a = -(double)N / 2.0; 
    t_a_rounded_prev = std::round(t_a);
    t_s = 0; 
    
    // Task 1: Setup for Energy-Weighted EQ Compensation
    double global_eq_weighted_sum = 0.0;
    double global_src_energy_sum = 0.0;

    for (int f_idx = 0; f_idx < total_analysis_frames; ++f_idx) {
        double alpha = get_alpha(t_s, timemap);
        double R_a = R_s / alpha;
        if (f_idx > 0) t_a += R_a;
        long t_a_rounded = std::round(t_a);

        std::vector<double> M_src(N / 2 + 1, 0.0), M_tgt(N / 2 + 1, 0.0);

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded < src_info.frames) {
            sf_seek(src_snd, std::max(0L, t_a_rounded), SEEK_SET);
            sf_readf_float(src_snd, read_buf.data(), N);
        }
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n) fft_in[n] = read_buf[n * channels + ch] * window[n];
            fftw_execute(plan_fwd);
            for (int k = 0; k <= N / 2; ++k) {
                M_src[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]) / channels;
            }
        }
        for (int k = 0; k <= N / 2; ++k) M_src[k] = std::sqrt(M_src[k]);
        
        // Task 1: EQ Compensation Measurement
        for (int k = 1; k <= N / 2; ++k) {
            double E_k = M_src[k] * M_src[k];
            double m_val = std::max(1e-9, multiplier_array[k]);
            global_eq_weighted_sum += E_k * 20.0 * std::log10(m_val);
            global_src_energy_sum += E_k;
        }

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_s + N <= virtual_tgt_buf.size() / channels) {
            std::copy(virtual_tgt_buf.begin() + t_s * channels, virtual_tgt_buf.begin() + (t_s + N) * channels, read_buf.begin());
        }
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n) fft_in[n] = read_buf[n * channels + ch] * window[n];
            fftw_execute(plan_fwd);
            for (int k = 0; k <= N / 2; ++k) {
                M_tgt[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]) / channels;
            }
        }
        for (int k = 0; k <= N / 2; ++k) M_tgt[k] = std::sqrt(M_tgt[k]);

        // 2. Option B: Energy-Weighted Macro-Scalars
        double den_L = 0.0, den_M = 0.0, den_H = 0.0;
        double num_L = 0.0, num_M = 0.0, num_H = 0.0;

        for (int k = 1; k <= N / 2; ++k) {
            double M_tgt_eq = M_tgt[k] * multiplier_array[k]; 
            double s_raw_k = (M_tgt_eq > M_src[k]) ? (M_src[k] / (M_tgt_eq + 1e-12)) : 1.0;
            
            double mag_sq = M_src[k] * M_src[k];
            
            den_L += mag_sq * W_L[k];
            num_L += s_raw_k * mag_sq * W_L[k];
            
            den_M += mag_sq * W_M[k];
            num_M += s_raw_k * mag_sq * W_M[k];
            
            den_H += mag_sq * W_H[k];
            num_H += s_raw_k * mag_sq * W_H[k];
        }

        double S_raw_L = (den_L > 1e-20) ? (num_L / den_L) : 1.0;
        double S_raw_M = (den_M > 1e-20) ? (num_M / den_M) : 1.0;
        double S_raw_H = (den_H > 1e-20) ? (num_H / den_H) : 1.0;

        // 3. Soft Knee & Range Clamp Application
        double norm_factor = static_cast<double>(N) / 2.0;
        double db_L = 20.0 * std::log10(std::sqrt(den_L) / norm_factor + 1e-9);
        double db_M = 20.0 * std::log10(std::sqrt(den_M) / norm_factor + 1e-9);
        double db_H = 20.0 * std::log10(std::sqrt(den_H) / norm_factor + 1e-9);

        auto evaluate_zone = [](double db, double thresh, double knee, double max_depth, double raw_scalar, double max_atten) {
            double lower_bound = thresh - (knee / 2.0);
            double upper_bound = thresh + (knee / 2.0);
            double active_depth = 0.0;
            
            if (db <= lower_bound) active_depth = 0.0;
            else if (db >= upper_bound) active_depth = max_depth;
            else {
                double factor = (db - lower_bound) / knee;
                active_depth = max_depth * (factor * factor);
            }
            
            double c_max_clamp = std::pow(10.0, max_atten / 20.0);
            double s_depth = std::pow(raw_scalar, active_depth);
            return std::max(c_max_clamp, s_depth);
        };

        S_traj_L[f_idx] = evaluate_zone(db_L, thresh_low, knee_low, depth_low, S_raw_L, atten_low);
        S_traj_M[f_idx] = evaluate_zone(db_M, thresh_mid, knee_mid, depth_mid, S_raw_M, atten_mid);
        S_traj_H[f_idx] = evaluate_zone(db_H, thresh_high, knee_high, depth_high, S_raw_H, atten_high);

        t_s += R_s; 
    }

    // 4. Temporal Smoothing
    std::cout << "          -> Applying Asymmetrical IIR (Fast Backward, Slow Forward)...\n";
    
    double c_back_L = 1.0 - std::exp(-(double)R_s / ((tau_B_low / 1000.0) * src_info.samplerate));
    double c_back_M = 1.0 - std::exp(-(double)R_s / ((tau_B_mid / 1000.0) * src_info.samplerate));
    double c_back_H = 1.0 - std::exp(-(double)R_s / ((tau_B_high / 1000.0) * src_info.samplerate));

    double c_fwd_L = 1.0 - std::exp(-(double)R_s / ((tau_F_low / 1000.0) * src_info.samplerate));
    double c_fwd_M = 1.0 - std::exp(-(double)R_s / ((tau_F_mid / 1000.0) * src_info.samplerate));
    double c_fwd_H = 1.0 - std::exp(-(double)R_s / ((tau_F_high / 1000.0) * src_info.samplerate));

    // Pass A: Pre-Echo Excavator (Backward)
    for (int m = total_analysis_frames - 2; m >= 0; --m) {
        S_traj_L[m] = c_back_L * S_traj_L[m] + (1.0 - c_back_L) * S_traj_L[m+1];
        S_traj_M[m] = c_back_M * S_traj_M[m] + (1.0 - c_back_M) * S_traj_M[m+1];
        S_traj_H[m] = c_back_H * S_traj_H[m] + (1.0 - c_back_H) * S_traj_H[m+1];
    }
    
    // Pass B: Post-Echo Release (Forward)
    for (int m = 1; m < total_analysis_frames; ++m) {
        S_traj_L[m] = c_fwd_L * S_traj_L[m] + (1.0 - c_fwd_L) * S_traj_L[m-1];
        S_traj_M[m] = c_fwd_M * S_traj_M[m] + (1.0 - c_fwd_M) * S_traj_M[m-1];
        S_traj_H[m] = c_fwd_H * S_traj_H[m] + (1.0 - c_fwd_H) * S_traj_H[m-1];
    }

    // Diagnostic Output
    std::cout << "          -> Rendering Dynamics GR Plot...\n";
    std::string dyn_png_path = tgt_audio_file;
    size_t dyn_last_ext = dyn_png_path.find_last_of(".");
    dyn_png_path = (dyn_last_ext != std::string::npos) ? dyn_png_path.substr(0, dyn_last_ext) + "_dynamics.png" : dyn_png_path + "_dynamics.png";

    std::ostringstream dyn_svg;
    dyn_svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1920 1080\" width=\"1920\" height=\"1080\">\n";
    dyn_svg << "  <rect width=\"1920\" height=\"1080\" fill=\"#1e1e1e\" />\n";

    double render_range_db = -2.0; 
    auto get_dyn_y = [render_range_db](double db) { return 100.0 + (db / render_range_db) * 880.0; };
    
    for (double db = 0.0; db >= render_range_db; db -= 0.5) {
        double y = get_dyn_y(db);
        dyn_svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1920\" y2=\"" << y << "\" stroke=\"#ffffff\" stroke-width=\"1\" opacity=\"0.3\" />\n";
        dyn_svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#ffffff\" font-size=\"14\">" << db << " dB</text>\n";
    }

    std::vector<std::vector<double>> trajs = {S_traj_L, S_traj_M, S_traj_H};
    std::string colors[3] = {"#ff5555", "#55ff55", "#5555ff"};
    
    for (int b = 0; b < 3; ++b) {
        dyn_svg << "  <polyline fill=\"none\" stroke=\"" << colors[b] << "\" stroke-width=\"2\" points=\"";
        for (int m = 0; m < total_analysis_frames; m += 10) { 
            double mean_db = 20.0 * std::log10(trajs[b][m]);
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
    if (dyn_pipe) { fwrite(dyn_svg.str().c_str(), 1, dyn_svg.str().length(), dyn_pipe); pclose(dyn_pipe); }

    virtual_tgt_buf.clear(); 
    virtual_tgt_buf.shrink_to_fit();

    // ========================================================================
    // PASS 4: Inline Final Synthesis (Wet Pass)
    // ========================================================================
    std::cout << "\n[Pass 4] Executing Final Synthesis with EQ & LR4 Dynamics...\n";
    
    SF_INFO tgt_info = src_info;
    tgt_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* tgt_snd = sf_open(tgt_audio_file.c_str(), SFM_WRITE, &tgt_info);

    for (int ch = 0; ch < channels; ++ch) {
        std::fill(phi_prev[ch].begin(), phi_prev[ch].end(), 0.0);
        std::fill(theta_prev[ch].begin(), theta_prev[ch].end(), 0.0);
        std::fill(overlap_add[ch].begin(), overlap_add[ch].end(), 0.0);
    }
    t_a = -(double)N / 2.0; t_a_rounded_prev = std::round(t_a);
    t_s = 0; frame_idx = 0; frames_to_skip = N / 2;
    std::vector<float> write_buf(N * channels, 0.0f);
    
    // Task 2: Setup for Dynamics Compensation Measurement
    double global_dyn_atten_sum = 0.0;
    size_t active_dyn_frames = 0;

    while (t_s < target_total_frames) {
        double alpha = get_alpha(t_s, timemap);
        double R_a = R_s / alpha;
        if (frame_idx > 0) t_a += R_a;
        long t_a_rounded = std::round(t_a);
        long R_a_actual = t_a_rounded - t_a_rounded_prev;

        std::fill(read_buf.begin(), read_buf.end(), 0.0f);
        if (t_a_rounded < src_info.frames) {
            sf_seek(src_snd, std::max(0L, t_a_rounded), SEEK_SET);
            sf_readf_float(src_snd, read_buf.data(), N);
        }

        int s_idx = std::min(frame_idx, total_analysis_frames - 1);
        
        // Task 2: Setup Dynamic Compensation Gate
        bool is_active_dyn = (S_traj_L[s_idx] < 0.94 || S_traj_M[s_idx] < 0.94 || S_traj_H[s_idx] < 0.94);
        double frame_dyn_weighted_sum = 0.0;
        double frame_energy_sum = 0.0;

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N; ++n) fft_in[n] = read_buf[n * channels + ch] * window[n];
            fftw_execute(plan_fwd);
            
            std::vector<double> M(N / 2 + 1, 0.0), phi(N / 2 + 1, 0.0), theta(N / 2 + 1, 0.0);
            for (int k = 0; k <= N / 2; ++k) {
                M[k] = std::sqrt(fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);
                phi[k] = std::atan2(fft_out[k][1], fft_out[k][0]);
            }
            
            // Task 2: Dynamics Compensation Measurement
            if (is_active_dyn) {
                for (int k = 1; k <= N / 2; ++k) {
                    double E_k = M[k] * M[k];
                    double dyn_scalar = (S_traj_L[s_idx] * W_L[k]) + 
                                        (S_traj_M[s_idx] * W_M[k]) + 
                                        (S_traj_H[s_idx] * W_H[k]);
                    double s_bin = std::max(1e-9, dyn_scalar);
                    frame_dyn_weighted_sum += E_k * 20.0 * std::log10(s_bin);
                    frame_energy_sum += E_k;
                }
            }

            if (frame_idx == 0) for (int k = 0; k <= N / 2; ++k) theta[k] = phi[k];
            else {
                std::vector<int> peaks;
                for (int k = 1; k < N / 2; ++k) if (M[k] > M[k-1] && M[k] > M[k+1]) peaks.push_back(k);
                if (peaks.empty()) peaks.push_back(N/4); 

                for (int p : peaks) {
                    double omega_p = 2.0 * M_PI * p / N;
                    theta[p] = theta_prev[ch][p] + (omega_p + princarg(phi[p] - phi_prev[ch][p] - omega_p * R_a_actual) / R_a_actual) * R_s;
                }
                size_t peak_idx = 0;
                for (int k = 0; k <= N / 2; ++k) {
                    if (peak_idx < peaks.size() - 1 && std::abs(k - peaks[peak_idx + 1]) < std::abs(k - peaks[peak_idx])) peak_idx++;
                    int p = peaks[peak_idx];
                    if (k != p) theta[k] = theta[p] + phi[k] - phi[p];
                }
            }

            for (int k = 0; k <= N / 2; ++k) {
                phi_prev[ch][k] = phi[k]; theta_prev[ch][k] = theta[k];
                
                M[k] *= multiplier_array[k];             
                
                // --- LR4 Smooth Application ---
                double dyn_scalar = (S_traj_L[s_idx] * W_L[k]) + 
                                    (S_traj_M[s_idx] * W_M[k]) + 
                                    (S_traj_H[s_idx] * W_H[k]);
                M[k] *= dyn_scalar;
                // ------------------------------

                ifft_in[k][0] = M[k] * std::cos(theta[k]);
                ifft_in[k][1] = M[k] * std::sin(theta[k]);
            }
            fftw_execute(plan_inv);

            for (int n = 0; n < N; ++n) overlap_add[ch][n] += (ifft_out[n] / N) * synth_window[n];
        }

        int write_offset = 0, write_len = R_s;
        if (frames_to_skip > 0) {
            if (frames_to_skip >= write_len) { frames_to_skip -= write_len; write_len = 0; } 
            else { write_offset = frames_to_skip; write_len -= frames_to_skip; frames_to_skip = 0; }
        }
        if (write_len > 0) {
            for (int n = 0; n < write_len; ++n) {
                for (int ch = 0; ch < channels; ++ch) write_buf[n * channels + ch] = static_cast<float>(overlap_add[ch][write_offset + n]);
            }
            sf_writef_float(tgt_snd, write_buf.data(), write_len);
        }

        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < N - R_s; ++n) overlap_add[ch][n] = overlap_add[ch][n + R_s];
            for (int n = N - R_s; n < N; ++n) overlap_add[ch][n] = 0.0;
        }
        
        // Task 2: Log active frame attenuation
        if (is_active_dyn && frame_energy_sum > 1e-6) {
            global_dyn_atten_sum += (frame_dyn_weighted_sum / frame_energy_sum);
            active_dyn_frames++;
        }
        
        t_s += R_s; t_a_rounded_prev = t_a_rounded; frame_idx++;
    }

    int remaining = N - R_s;
    if (remaining > 0) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int n = 0; n < remaining; ++n) write_buf[n * channels + ch] = static_cast<float>(overlap_add[ch][n]);
        }
        sf_writef_float(tgt_snd, write_buf.data(), remaining);
    }

    std::cout << "[Success] Final Master Written.\n";

    // --- Task 3: Unification (Pre-Pass 5) ---
    double eq_attenuation = (global_src_energy_sum > 1e-20) ? (global_eq_weighted_sum / global_src_energy_sum) : 0.0;
    double eq_makeup_db = -eq_attenuation;

    double dyn_attenuation = (active_dyn_frames > 0) ? (global_dyn_atten_sum / active_dyn_frames) : 0.0;
    double dyn_makeup_db = -dyn_attenuation;

    double total_makeup_db = eq_makeup_db + dyn_makeup_db;
    double makeup_scalar = std::pow(10.0, total_makeup_db / 20.0);

    std::cout << "\n[Loudness Match (Energy-Weighted)]\n";
    std::cout << "  -> EQ Compensation       : " << (eq_makeup_db > 0 ? "+" : "") << std::fixed << std::setprecision(2) << eq_makeup_db << " dB\n";
    std::cout << "  -> Dynamics Compensation : " << (dyn_makeup_db > 0 ? "+" : "") << std::fixed << std::setprecision(2) << dyn_makeup_db << " dB\n";
    std::cout << "  -> Total Makeup          : " << (total_makeup_db > 0 ? "+" : "") << std::fixed << std::setprecision(2) << total_makeup_db << " dB\n\n";

    // ========================================================================
    // PASS 5: Bake Makeup Gain
    // ========================================================================
    std::cout << "[Pass 5] Baking Makeup Gain (" << std::fixed << std::setprecision(2) << total_makeup_db << " dB) into final WAV...\n";
    
    sf_close(tgt_snd); // Close Pass 4 file so we can read it

    std::string temp_audio_file = tgt_audio_file + ".tmp";
    std::rename(tgt_audio_file.c_str(), temp_audio_file.c_str());

    SF_INFO pass5_info = src_info;
    pass5_info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* in_snd = sf_open(temp_audio_file.c_str(), SFM_READ, &pass5_info);
    SNDFILE* out_snd = sf_open(tgt_audio_file.c_str(), SFM_WRITE, &pass5_info);

    std::vector<float> gain_buf(4096 * channels);
    sf_count_t read_frames;
    while ((read_frames = sf_readf_float(in_snd, gain_buf.data(), 4096)) > 0) {
        for (int i = 0; i < read_frames * channels; ++i) gain_buf[i] *= makeup_scalar;
        sf_writef_float(out_snd, gain_buf.data(), read_frames);
    }

    sf_close(in_snd);
    sf_close(out_snd);
    std::remove(temp_audio_file.c_str()); // Clean up temp file

    std::cout << "[Success] Final Master Loudness Matched & Written.\n";
    // --------------------------------------------

    fftw_destroy_plan(plan_fwd); fftw_destroy_plan(plan_inv);
    fftw_free(fft_in); fftw_free(fft_out); fftw_free(ifft_in); fftw_free(ifft_out);
    sf_close(src_snd); 

    return 0;
}
