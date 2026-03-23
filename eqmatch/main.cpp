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

/* ============================================================================
   EQMatch: Phase Vocoder Artifact Suppressor & Spectral Matcher
   ============================================================================
   This utility corrects spectral coloration and low-frequency artifacts natively 
   introduced by phase vocoder-based time-stretching algorithms. Time-stretching 
   often artificially inflates sub-bass energy, creating an unnatural rumble or 
   ballooning effect that did not exist in the original audio. 

   Architecture & Math:
   1. Phase-Agnostic Profiling: Calculates the Power Spectral Density (PSD) of 
      both the original source and the stretched target across matched temporal 
      blocks. It averages the L/R energy to generate a linked stereo delta curve 
      that is completely immune to stereo phase-cancellation.
      (Delta = Source dB - Target dB. A negative delta means the Target has 
      artificial excess energy that must be cut).
   
   2. Gaussian Smoothing: The raw delta is smoothed using a 1/3-octave Gaussian 
      window to extract the macro-acoustic profile and discard microscopic FFT jitter.
   
   3. Active Cliff Lookback (Spatial Trigger): To handle sub-bass ballooning, the 
      algorithm sweeps backward down the frequency spectrum from Nyquist to DC. 
      It continuously tracks the trajectory of natural room decay. If a continuous 
      downward slope crosses a user-defined threshold (default 50 Hz) and subsequently 
      attempts to rise, the algorithm assumes the rise is an artificial phase vocoder 
      artifact. It reaches back to the steepest inflection point of that specific 
      cliff and projects a mathematical tangent line straight down to 0 Hz, surgically 
      slicing off the artifact without relying on arbitrary minimum-depth thresholds 
      or zero-decibel ceilings.
   
   4. FIR Convolution: The resulting curve is converted into a linear phase FIR 
      impulse response and applied to the target audio using overlap-add convolution.
   ============================================================================ */

// --- Configuration Constants ---
const double ATTACK_TOLERANCE_LU = 3.0;      
const double RELEASE_TOLERANCE_LU = 8.0;     
const double MIN_GAP_SEC = 0.5;              
const double MIN_PHRASE_SEC = 5.0;           
const size_t TOP_X_CHUNKS = 10;               
const int FFT_SIZE = 8192; 

// --- Filter Topology Settings ---
const double SMOOTHING_OCTAVES = 1.0 / 3.0; 
const int CURVE_RESOLUTION = 500; 

// --- Data Structures ---
struct AcousticBlock {
    size_t id;
    size_t start_frame;
    size_t end_frame;
    double duration_sec;
    double lufs;
    double rms;
};

struct ParentChunk {
    size_t src_start, src_end;
    double tgt_start, tgt_end;
    double frame_mapping_ratio; 
};

struct Point {
    double x, y;
};

// --- Helpers ---
std::string format_time(size_t frames, int samplerate) {
    if (samplerate <= 0) return "00:00.000";
    double total_sec = static_cast<double>(frames) / samplerate;
    int mm = static_cast<int>(total_sec / 60.0);
    int ss = static_cast<int>(total_sec) % 60;
    int mmm = static_cast<int>((total_sec - std::floor(total_sec)) * 1000.0 + 0.5);
    if (mmm >= 1000) { mmm -= 1000; ss += 1; if (ss >= 60) { ss -= 60; mm += 1; } }
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << mm << ":" << std::setw(2) << ss << "." << std::setw(3) << mmm;
    return oss.str();
}

void calculate_metrics(SNDFILE* infile, size_t start_frame, size_t end_frame, int channels, int samplerate, double& out_lufs, double& out_rms) {
    ebur128_state* st = ebur128_init(channels, samplerate, EBUR128_MODE_I);
    if (!st) { out_lufs = -70.0; out_rms = -70.0; return; }

    const size_t BUFFER_FRAMES = 4096;
    std::vector<float> buffer(BUFFER_FRAMES * channels);
    double sum_sq = 0.0;
    size_t total_frames_read = 0;

    sf_seek(infile, start_frame, SEEK_SET);
    size_t frames_remaining = end_frame - start_frame;
    
    while (frames_remaining > 0) {
        size_t frames_to_read = std::min(frames_remaining, BUFFER_FRAMES);
        sf_count_t read_count = sf_readf_float(infile, buffer.data(), frames_to_read);
        if (read_count <= 0) break;
        
        ebur128_add_frames_float(st, buffer.data(), read_count);
        for(size_t i = 0; i < static_cast<size_t>(read_count * channels); ++i) {
            sum_sq += static_cast<double>(buffer[i]) * static_cast<double>(buffer[i]);
        }
        total_frames_read += read_count;
        frames_remaining -= read_count;
    }

    if (ebur128_loudness_global(st, &out_lufs) != EBUR128_SUCCESS) out_lufs = -70.0; 
    ebur128_destroy(&st);

    if (total_frames_read > 0) {
        double mean_sq = sum_sq / (total_frames_read * channels);
        out_rms = mean_sq > 0 ? 10.0 * std::log10(mean_sq) : -70.0;
    } else { out_rms = -70.0; }
}

double map_source_to_target(size_t src_frame, const std::vector<ParentChunk>& parents) {
    if (parents.empty()) return static_cast<double>(src_frame);
    if (src_frame <= parents.front().src_start) return parents.front().tgt_start;

    for (const auto& p : parents) {
        if (src_frame >= p.src_start && src_frame < p.src_end) {
            double offset = static_cast<double>(src_frame - p.src_start);
            return p.tgt_start + (offset * p.frame_mapping_ratio);
        }
    }

    const auto& last_p = parents.back();
    if (src_frame >= last_p.src_end) {
        double offset = static_cast<double>(src_frame - last_p.src_start);
        return last_p.tgt_start + (offset * last_p.frame_mapping_ratio);
    }
    return 0.0;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <timemap_file> <source_wav> <target_wav> [low_threshold_hz=50.0]\n";
        std::cerr << "  Architecture: Linked Stereo (No Crossover)\n";
        std::cerr << "  SVG Graph: Yellow = Linked EQ Curve | Red = Active Cliff Threshold\n";
        return 1;
    }

    std::string map_file = argv[1];
    std::string src_wav_file = argv[2];
    std::string tgt_wav_file = argv[3];
    double low_threshold_hz = (argc >= 5) ? std::stod(argv[4]) : 50.0;

    SF_INFO src_sfinfo, tgt_sfinfo;
    src_sfinfo.format = 0; tgt_sfinfo.format = 0;
    
    SNDFILE* src_infile = sf_open(src_wav_file.c_str(), SFM_READ, &src_sfinfo);
    if (!src_infile) { std::cerr << "Error: Could not open Source WAV.\n"; return 1; }
    
    SNDFILE* tgt_infile = sf_open(tgt_wav_file.c_str(), SFM_READ, &tgt_sfinfo);
    if (!tgt_infile) { std::cerr << "Error: Could not open Target WAV.\n"; return 1; }
    
    std::cout << "Opened Source: " << src_sfinfo.samplerate << "Hz, " << src_sfinfo.channels << " channels.\n";
    std::cout << "Opened Target: " << tgt_sfinfo.samplerate << "Hz, " << tgt_sfinfo.channels << " channels.\n";

    if (src_sfinfo.channels != 2 || tgt_sfinfo.channels != 2) {
        std::cerr << "Error: This architecture strictly requires Stereo (2-channel) files.\n";
        return 1;
    }

    double nyquist = src_sfinfo.samplerate / 2.0;

    std::cout << "=> Architecture: Pure Linked Stereo\n";
    std::cout << "=> Low Threshold: " << std::fixed << std::setprecision(1) << low_threshold_hz << " Hz\n";

    std::ifstream file(map_file);
    if (!file.is_open()) { std::cerr << "Error: Could not open timemap.\n"; return 1; }

    std::vector<std::pair<size_t, double>> map_points;
    size_t src; double tgt;
    while (file >> src >> tgt) map_points.push_back({src, tgt});
    file.close();

    std::vector<ParentChunk> parents;
    for (size_t i = 0; i < map_points.size() - 1; ++i) {
        ParentChunk p = {};
        p.src_start = map_points[i].first;
        p.src_end = map_points[i+1].first;
        p.tgt_start = map_points[i].second;
        p.tgt_end = map_points[i+1].second;
        double src_dur = static_cast<double>(p.src_end - p.src_start);
        double tgt_dur = p.tgt_end - p.tgt_start;
        p.frame_mapping_ratio = (src_dur > 0) ? (tgt_dur / src_dur) : 1.0;
        parents.push_back(p);
    }

    // ========================================================================
    // PHASE 1: Acoustic Profiling
    // ========================================================================
    std::cout << "\n[Phase 1] Tracking Continuous Acoustic Envelope...\n";
    double src_global_lufs = 0.0, src_global_rms = 0.0;
    calculate_metrics(src_infile, 0, src_sfinfo.frames, src_sfinfo.channels, src_sfinfo.samplerate, src_global_lufs, src_global_rms);
    
    double attack_thresh = src_global_lufs - ATTACK_TOLERANCE_LU;
    double release_thresh = src_global_lufs - RELEASE_TOLERANCE_LU;

    ebur128_state* st = ebur128_init(src_sfinfo.channels, src_sfinfo.samplerate, EBUR128_MODE_M);
    std::vector<AcousticBlock> raw_blocks;
    bool is_loud = false;
    size_t current_start = 0;

    const size_t CHUNK_SIZE = 4096; 
    std::vector<float> read_buffer(CHUNK_SIZE * src_sfinfo.channels);
    sf_seek(src_infile, 0, SEEK_SET);
    size_t current_frame = 0;

    while (current_frame < static_cast<size_t>(src_sfinfo.frames)) {
        sf_count_t read_count = sf_readf_float(src_infile, read_buffer.data(), CHUNK_SIZE);
        if (read_count <= 0) break;
        ebur128_add_frames_float(st, read_buffer.data(), read_count);
        current_frame += read_count;
        double momentary_lufs;
        if (ebur128_loudness_momentary(st, &momentary_lufs) == EBUR128_SUCCESS) {
            if (!is_loud && momentary_lufs >= attack_thresh) {
                is_loud = true; current_start = current_frame;
            } else if (is_loud && momentary_lufs < release_thresh) {
                is_loud = false; raw_blocks.push_back({0, current_start, current_frame, 0.0, 0.0, 0.0});
            }
        }
    }
    if (is_loud) raw_blocks.push_back({0, current_start, current_frame, 0.0, 0.0, 0.0});
    ebur128_destroy(&st);

    std::vector<AcousticBlock> merged_blocks;
    size_t min_gap_frames = MIN_GAP_SEC * src_sfinfo.samplerate;
    for (const auto& b : raw_blocks) {
        if (merged_blocks.empty()) merged_blocks.push_back(b);
        else {
            if ((b.start_frame - merged_blocks.back().end_frame) < min_gap_frames) merged_blocks.back().end_frame = b.end_frame; 
            else merged_blocks.push_back(b);
        }
    }

    std::vector<AcousticBlock> final_blocks;
    size_t min_phrase_frames = MIN_PHRASE_SEC * src_sfinfo.samplerate;
    for (auto& b : merged_blocks) {
        if ((b.end_frame - b.start_frame) >= min_phrase_frames) {
            b.duration_sec = static_cast<double>(b.end_frame - b.start_frame) / src_sfinfo.samplerate;
            calculate_metrics(src_infile, b.start_frame, b.end_frame, src_sfinfo.channels, src_sfinfo.samplerate, b.lufs, b.rms);
            final_blocks.push_back(b);
        }
    }

    std::sort(final_blocks.begin(), final_blocks.end(), [](const AcousticBlock& a, const AcousticBlock& b) {
        return a.duration_sec > b.duration_sec;
    });
    size_t keep_count = std::min<size_t>(TOP_X_CHUNKS, final_blocks.size());
    final_blocks.resize(keep_count);

    // ========================================================================
    // PHASE 2: Dynamic Target Headroom Detection
    // ========================================================================
    std::cout << "\n[Phase 2] Running Dynamic Headroom Diagnostics...\n";
    double tgt_global_lufs = 0.0, tgt_global_rms = 0.0;
    calculate_metrics(tgt_infile, 0, tgt_sfinfo.frames, tgt_sfinfo.channels, tgt_sfinfo.samplerate, tgt_global_lufs, tgt_global_rms);
    
    double lufs_difference = src_global_lufs - tgt_global_lufs;
    double target_gain_multiplier = std::pow(10.0, lufs_difference / 20.0);
    
    std::cout << "=> Applying exact x" << std::fixed << std::setprecision(3) 
              << target_gain_multiplier << " (" << (lufs_difference >= 0 ? "+" : "") 
              << lufs_difference << " dB) makeup gain to target.\n";

    // ========================================================================
    // PHASE 3: 2-Channel FFTW3 Analysis Loop
    // ========================================================================
    std::cout << "\n[Phase 3] Running 2-Channel FFTW3 Engine (" << FFT_SIZE << " bins)...\n";
    
    double* fft_in = fftw_alloc_real(FFT_SIZE);
    fftw_complex* fft_out = fftw_alloc_complex(FFT_SIZE / 2 + 1);
    fftw_plan plan_r2c = fftw_plan_dft_r2c_1d(FFT_SIZE, fft_in, fft_out, FFTW_MEASURE);

    std::vector<double> src_psd_l(FFT_SIZE / 2 + 1, 0.0);
    std::vector<double> src_psd_r(FFT_SIZE / 2 + 1, 0.0);
    std::vector<double> tgt_psd_l(FFT_SIZE / 2 + 1, 0.0);
    std::vector<double> tgt_psd_r(FFT_SIZE / 2 + 1, 0.0);

    size_t total_windows = 0;
    std::vector<double> hanning(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i) hanning[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (FFT_SIZE - 1)));

    std::vector<float> src_read_buf(FFT_SIZE * 2);
    std::vector<float> tgt_read_buf(FFT_SIZE * 2);
    size_t hop_size = FFT_SIZE / 2;

    for (const auto& b : final_blocks) {
        size_t s_frame = b.start_frame;
        while (s_frame + FFT_SIZE <= b.end_frame) {
            double t_start = map_source_to_target(s_frame, parents);
            
            sf_seek(src_infile, s_frame, SEEK_SET);
            sf_readf_float(src_infile, src_read_buf.data(), FFT_SIZE);
            
            sf_seek(tgt_infile, static_cast<sf_count_t>(t_start), SEEK_SET);
            sf_readf_float(tgt_infile, tgt_read_buf.data(), FFT_SIZE);

            // PROCESS SOURCE FILE (L & R)
            for (int i = 0; i < FFT_SIZE; ++i) fft_in[i] = src_read_buf[i * 2] * hanning[i]; // L
            fftw_execute(plan_r2c);
            for (int k = 1; k <= FFT_SIZE / 2; ++k) src_psd_l[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);

            for (int i = 0; i < FFT_SIZE; ++i) fft_in[i] = src_read_buf[i * 2 + 1] * hanning[i]; // R
            fftw_execute(plan_r2c);
            for (int k = 1; k <= FFT_SIZE / 2; ++k) src_psd_r[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);

            // PROCESS TARGET FILE (L & R)
            for (int i = 0; i < FFT_SIZE; ++i) fft_in[i] = tgt_read_buf[i * 2] * target_gain_multiplier * hanning[i]; // Tgt L
            fftw_execute(plan_r2c);
            for (int k = 1; k <= FFT_SIZE / 2; ++k) tgt_psd_l[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);

            for (int i = 0; i < FFT_SIZE; ++i) fft_in[i] = tgt_read_buf[i * 2 + 1] * target_gain_multiplier * hanning[i]; // Tgt R
            fftw_execute(plan_r2c);
            for (int k = 1; k <= FFT_SIZE / 2; ++k) tgt_psd_r[k] += (fft_out[k][0]*fft_out[k][0] + fft_out[k][1]*fft_out[k][1]);

            total_windows++;
            s_frame += hop_size;
        }
    }

    // Average Power for True Phase-Agnostic Linked Curve.
    // We strictly sum the Power Spectral Densities (squared energy), not the raw audio samples.
    // Summing raw samples to Mono would cause out-of-phase stereo information to cancel out, 
    // creating artificial notches in the spectrum that do not actually exist in the room acoustics.
    std::vector<double> src_psd_linked(FFT_SIZE / 2 + 1, 0.0);
    std::vector<double> tgt_psd_linked(FFT_SIZE / 2 + 1, 0.0);
    for (int k = 1; k <= FFT_SIZE / 2; ++k) {
        src_psd_linked[k] = (src_psd_l[k] + src_psd_r[k]) * 0.5;
        tgt_psd_linked[k] = (tgt_psd_l[k] + tgt_psd_r[k]) * 0.5;
    }

    // ========================================================================
    // PHASE 4: 1/3 Octave Smoothing & Active Cliff Artifact Suppressor
    // ========================================================================
    std::cout << "[Phase 4] Calculating Smoothed Trend Line and Active Cliff Tangent...\n";
    
    std::vector<double> raw_delta_db(FFT_SIZE / 2 + 1, 0.0);
    std::vector<Point> smoothed_curve;
    
    double bin_hz_width = static_cast<double>(src_sfinfo.samplerate) / FFT_SIZE;
    double sigma_octaves = SMOOTHING_OCTAVES / 2.355;
    double log_min = std::log10(10.0); 
    double log_max = std::log10(nyquist); 

    // 1. Calculate Raw Delta (Source minus Target)
    for (int k = 1; k <= FFT_SIZE / 2; ++k) {
        if (src_psd_linked[k] > 0 && tgt_psd_linked[k] > 0) {
            double src_db = 10.0 * std::log10((src_psd_linked[k] / total_windows));
            double tgt_db = 10.0 * std::log10((tgt_psd_linked[k] / total_windows));
            raw_delta_db[k] = src_db - tgt_db; 
        }
    }
    raw_delta_db[FFT_SIZE / 2] = 0.0; 

    // 2. Apply 1/3 Octave Gaussian Smoothing
    for (int i = 0; i < CURVE_RESOLUTION; ++i) {
        double current_target_hz = std::pow(10.0, log_min + i * (log_max - log_min) / (CURVE_RESOLUTION - 1));
        double weight_sum = 0.0, delta_sum = 0.0;

        for (int k = 1; k <= FFT_SIZE / 2; ++k) {
            double bin_hz = k * bin_hz_width;
            if (bin_hz < 10.0) continue; 

            double octave_distance = std::log2(bin_hz / current_target_hz);
            double weight = std::exp(-0.5 * std::pow(octave_distance / sigma_octaves, 2.0));
            
            if (weight < 0.001) continue; 
            weight_sum += weight;
            delta_sum += weight * raw_delta_db[k];
        }
        double final_db = (weight_sum > 0.0) ? (delta_sum / weight_sum) : 0.0;
        smoothed_curve.push_back({current_target_hz, final_db});
    }

    // --- Tangent Extension Artifact Suppressor (Active Cliff Lookback) ---
    // Evaluates the spectrum mathematically from High Frequency (Nyquist) down to DC.
    // As it walks backward, it tracks any continuous downward slope. If that specific 
    // fall crosses the low_threshold_hz boundary, the system arms. The absolute second 
    // the curve bottoms out and attempts to rise, it rewinds to the steepest coordinate 
    // of that specific fall (regardless of whether that steep point was above or below 
    // the threshold) and projects the tangent line downward.
    int num_pts = smoothed_curve.size();
    std::vector<double> drop_slope(num_pts - 1, 0.0);
    
    // A. Calculate continuous High-to-Low trajectory (dB/octave)
    for (int i = 0; i < num_pts - 1; ++i) {
        double y_low = smoothed_curve[i].y;
        double y_high = smoothed_curve[i+1].y;
        double x_low = smoothed_curve[i].x;
        double x_high = smoothed_curve[i+1].x;
        drop_slope[i] = (y_low - y_high) / std::log2(x_high / x_low);
    }

    // B. High-to-Low Sweep State Machine (Pure Spatial Trigger)
    bool in_fall = false;
    bool crossed_threshold = false;
    double current_steepest_slope = 0.0;
    int current_inflection_idx = -1;

    for (int i = num_pts - 2; i >= 0; --i) {
        if (drop_slope[i] < 0.0) {
            if (!in_fall) {
                in_fall = true;
                crossed_threshold = false;
                current_steepest_slope = 0.0;
                current_inflection_idx = -1;
            }
            
            // Track the absolute steepest point of THIS continuous fall
            if (drop_slope[i] < current_steepest_slope) {
                current_steepest_slope = drop_slope[i];
                current_inflection_idx = i;
            }

            // If this fall crosses the spatial sub-bass boundary, arm the system
            if (smoothed_curve[i].x <= low_threshold_hz) {
                crossed_threshold = true;
            }
        } 
        else if (drop_slope[i] > 0.0) {
            // The curve has bottomed out and is attempting to balloon upwards.
            if (in_fall && crossed_threshold) {
                
                // NO ARBITRARY MATH CONSTRAINTS. The Spatial Threshold IS the authority.
                // If it falls below 50Hz, and then tries to rise, lock the tangent!
                if (current_inflection_idx != -1) {
                    double x_trigger = smoothed_curve[current_inflection_idx].x;
                    double y_trigger = smoothed_curve[current_inflection_idx].y;
                    double S_locked = current_steepest_slope;

                    // Execute tangent extension strictly below the inflection point
                    for (int j = current_inflection_idx - 1; j >= 0; --j) {
                        double x_below = std::log2(x_trigger / smoothed_curve[j].x);
                        smoothed_curve[j].y = y_trigger + (S_locked * x_below);
                    }
                    break; // Sub-bass is permanently locked. Stop evaluating.
                }
            }
            
            // If it never crossed the 50Hz threshold, reset and wait for the next cliff.
            in_fall = false;
            crossed_threshold = false;
        }
    }

    // 3. Execution Engine: Pure Native Audio + Nyquist Taper 
    for (auto& pt : smoothed_curve) {
        if (pt.x > nyquist - 20.0) {
            double taper_ratio = (nyquist - pt.x) / 20.0;
            pt.y *= std::max(0.0, std::min(1.0, taper_ratio));
        }
    }

    // ========================================================================
    // PHASE 5: Single Linear Phase FIR Generation
    // ========================================================================
    std::cout << "\n======================================================\n";
    std::cout << "[Phase 5] Generating Linked Stereo Linear Phase FIR...\n";
    std::cout << "======================================================\n";

    fftw_plan ir_plan = fftw_plan_dft_c2r_1d(FFT_SIZE, fft_out, fft_in, FFTW_ESTIMATE); 
    std::vector<float> final_ir(FFT_SIZE, 0.0f);

    auto get_interpolated_db = [&](double hz) {
        if (hz <= smoothed_curve.front().x) return smoothed_curve.front().y;
        if (hz >= smoothed_curve.back().x) return smoothed_curve.back().y;

        auto it = std::lower_bound(smoothed_curve.begin(), smoothed_curve.end(), hz, 
            [](const Point& p, double f) { return p.x < f; });
        if (it == smoothed_curve.begin()) return it->y;
        if (it == smoothed_curve.end()) return smoothed_curve.back().y;
        auto prev = it - 1;
        double t = (hz - prev->x) / (it->x - prev->x);
        return prev->y + t * (it->y - prev->y);
    };

    for (int k = 0; k <= FFT_SIZE / 2; ++k) {
        double bin_hz = k * bin_hz_width;
        double target_db = 0.0;

        if (k == 0) target_db = -100.0; 
        else if (k == FFT_SIZE / 2) target_db = 0.0; 
        else target_db = get_interpolated_db(bin_hz);

        fft_out[k][0] = std::pow(10.0, target_db / 20.0); 
        fft_out[k][1] = 0.0; 
    }

    fftw_execute(ir_plan);

    // Calculate final makeup gain: Match Source LUFS, but subtract 6.0 dB for safety headroom
    double final_makeup_gain_db = lufs_difference - 6.0;
    double final_gain_mult = std::pow(10.0, final_makeup_gain_db / 20.0);

    for (int i = 0; i < FFT_SIZE; ++i) {
        double raw_sample = fft_in[i] / FFT_SIZE;
        int shifted_idx = (i + FFT_SIZE / 2) % FFT_SIZE;
        double window = 0.5 * (1.0 - std::cos(2.0 * M_PI * shifted_idx / (FFT_SIZE - 1)));
        // Bake the -6dB safety offset natively into the convolution FIR
        final_ir[shifted_idx] = static_cast<float>(raw_sample * window * final_gain_mult);
    }

    std::string tgt_dir = tgt_wav_file;
    std::string tgt_filename = tgt_wav_file;
    size_t last_slash = tgt_wav_file.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        tgt_dir = tgt_wav_file.substr(0, last_slash) + "/";
        tgt_filename = tgt_wav_file.substr(last_slash + 1);
    } else tgt_dir = "";
    
    std::cout << "          -> FIR array generated in memory.\n";

    // ========================================================================
    // PHASE 6: High-Resolution SVG Rendering
    // ========================================================================
    std::string svg_path = tgt_dir + "eqmatch_curve.svg";
    std::cout << "[Phase 6] Writing High-Resolution EQ Curve to: " << svg_path << "\n";

    std::ofstream svg(svg_path);
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1600 850\" font-family=\"sans-serif\">\n";
    svg << "  <rect width=\"1600\" height=\"850\" fill=\"#1e1e1e\" />\n";
    
    auto get_x = [nyquist](double hz) { return 1600.0 * (std::log10(hz) - std::log10(10.0)) / (std::log10(nyquist) - std::log10(10.0)); };
    auto get_y = [](double db) { return 400.0 - (std::max(-6.0, std::min(6.0, db)) * 66.66); }; 

    for (int db = -6; db <= 6; ++db) {
        double y = get_y(db);
        if (db == 0) {
            svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1600\" y2=\"" << y << "\" stroke=\"#ffffff\" stroke-width=\"2\" opacity=\"0.6\" />\n";
            svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#ffffff\" font-size=\"14\" font-weight=\"bold\">0 dB</text>\n";
        } else {
            svg << "  <line x1=\"0\" y1=\"" << y << "\" x2=\"1600\" y2=\"" << y << "\" stroke=\"#888888\" stroke-width=\"1\" stroke-dasharray=\"4\" opacity=\"0.3\" />\n";
            std::string sign = (db > 0) ? "+" : "";
            svg << "  <text x=\"10\" y=\"" << (y - 5) << "\" fill=\"#888888\" font-size=\"12\">" << sign << db << " dB</text>\n";
        }
    }

    double freqs[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    for (double f : freqs) {
        if (f > nyquist) continue;
        double x = get_x(f);
        svg << "  <line x1=\"" << x << "\" y1=\"0\" x2=\"" << x << "\" y2=\"800\" stroke=\"#888888\" stroke-width=\"1\" opacity=\"0.15\" />\n";
        std::string label = (f >= 1000) ? std::to_string((int)(f/1000)) + "k" : std::to_string((int)f);
        svg << "  <text x=\"" << x << "\" y=\"830\" fill=\"#888888\" font-size=\"14\" text-anchor=\"middle\">" << label << "</text>\n";
    }
    
    // --- Active Cliff Visual Indicator ---
    double x_thresh = get_x(low_threshold_hz);
    svg << "  <line x1=\"" << x_thresh << "\" y1=\"0\" x2=\"" << x_thresh << "\" y2=\"800\" stroke=\"#ffffff\" stroke-width=\"1.5\" stroke-dasharray=\"5,5\" opacity=\"0.7\" />\n";
    svg << "  <text x=\"" << (x_thresh + 5) << "\" y=\"90\" fill=\"#ffffff\" font-size=\"12\" opacity=\"0.9\">Low Threshold (" << std::fixed << std::setprecision(1) << low_threshold_hz << " Hz)</text>\n";

    // --- High-Resolution Raw Delta Background ---
    auto get_y_raw = [](double db) { return 400.0 - (std::max(-12.0, std::min(12.0, db)) * 66.66); };
    svg << "  <polyline fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1\" stroke-linejoin=\"round\" opacity=\"0.15\" points=\"";
    for (int k = 1; k <= FFT_SIZE / 2; ++k) {
        double bin_hz = k * bin_hz_width;
        if (bin_hz < 10.0 || bin_hz > nyquist) continue;
        svg << std::fixed << std::setprecision(2) << get_x(bin_hz) << "," << get_y_raw(raw_delta_db[k]) << " ";
    }
    svg << "\" />\n";

    // Linked Curve
    svg << "  <polyline fill=\"none\" stroke=\"#00ffff\" stroke-width=\"4\" stroke-linejoin=\"round\" opacity=\"0.9\" points=\"";
    for (const auto& pt : smoothed_curve) svg << std::fixed << std::setprecision(2) << get_x(pt.x) << "," << get_y(pt.y) << " ";
    svg << "\" />\n";
    
    svg << "  <text x=\"1580\" y=\"30\" fill=\"#ffffff\" font-size=\"16\" font-family=\"sans-serif\" text-anchor=\"end\" opacity=\"0.8\">" 
        << "Target Makeup: " << (final_makeup_gain_db >= 0 ? "+" : "") << std::fixed << std::setprecision(2) << final_makeup_gain_db << " dB (-6dB Offset)</text>\n";
    svg << "</svg>\n";    
    svg.close();

    fftw_destroy_plan(plan_r2c); fftw_destroy_plan(ir_plan);
    fftw_free(fft_in); fftw_free(fft_out);
    sf_close(src_infile);

    // ========================================================================
    // PHASE 7: Native Stereo Convolution Engine
    // ========================================================================
    std::cout << "\n======================================================\n";
    std::cout << "[Phase 7] Executing Convolution Loop...\n";
    std::cout << "======================================================\n";

    const int CONV_BLOCK_SIZE = 8192;
    const int CONV_IR_SIZE = FFT_SIZE; // 8192
    const int OVERLAP_SIZE = CONV_IR_SIZE - 1; // 8191
    const int CONV_FFT_SIZE = 16384; 

    double* conv_in = fftw_alloc_real(CONV_FFT_SIZE);
    fftw_complex* conv_out = fftw_alloc_complex(CONV_FFT_SIZE / 2 + 1);
    fftw_plan plan_fwd = fftw_plan_dft_r2c_1d(CONV_FFT_SIZE, conv_in, conv_out, FFTW_ESTIMATE);
    double* ifft_in_real = fftw_alloc_real(CONV_FFT_SIZE);
    fftw_plan plan_inv = fftw_plan_dft_c2r_1d(CONV_FFT_SIZE, conv_out, ifft_in_real, FFTW_ESTIMATE);

    // Pre-Transform Single IR
    fftw_complex* conv_ir_cmplx = fftw_alloc_complex(CONV_FFT_SIZE / 2 + 1);
    fftw_plan plan_ir_fwd = fftw_plan_dft_r2c_1d(CONV_FFT_SIZE, ifft_in_real, conv_out, FFTW_ESTIMATE); 
    
    std::fill_n(ifft_in_real, CONV_FFT_SIZE, 0.0);
    for (int i = 0; i < CONV_IR_SIZE; ++i) ifft_in_real[i] = final_ir[i];
    fftw_execute(plan_ir_fwd);
    for (int k = 0; k <= CONV_FFT_SIZE / 2; ++k) {
        conv_ir_cmplx[k][0] = conv_out[k][0];
        conv_ir_cmplx[k][1] = conv_out[k][1];
    }
    fftw_destroy_plan(plan_ir_fwd);

    std::string target_in_file = tgt_wav_file; 
    std::string final_out_file = tgt_dir + "eqmatch_" + tgt_filename;

    SF_INFO in_info;
    SNDFILE* in_wav = sf_open(target_in_file.c_str(), SFM_READ, &in_info);
    if (!in_wav) {
        std::cerr << "Error: Could not open target file " << target_in_file << " for convolution.\n";
        return 1;
    }

    SF_INFO out_info = in_info; 
    // Force 32-bit Float output to guarantee over-zero headroom preservation
    out_info.format = (in_info.format & SF_FORMAT_TYPEMASK) | SF_FORMAT_FLOAT;
    SNDFILE* out_wav = sf_open(final_out_file.c_str(), SFM_WRITE, &out_info);

    std::vector<std::vector<float>> overlap_buffers(2, std::vector<float>(OVERLAP_SIZE, 0.0f));
    std::vector<std::vector<float>> channel_write_buffers(2, std::vector<float>(CONV_BLOCK_SIZE, 0.0f));
    
    std::vector<float> conv_read_buffer(2 * CONV_BLOCK_SIZE, 0.0f);
    std::vector<float> interleaved_write_buffer(2 * CONV_BLOCK_SIZE, 0.0f);
    
    int frames_to_skip = 4096; 
    const float NOISE_FLOOR_GATE = 1e-10f; 
    
    sf_count_t read_count;
    std::cout << "          -> Processing direct stereo. Latency compensation: " << frames_to_skip << " frames...\n";

    std::vector<std::vector<double>> proc_buf(2, std::vector<double>(CONV_BLOCK_SIZE, 0.0));

    while ((read_count = sf_readf_float(in_wav, conv_read_buffer.data(), CONV_BLOCK_SIZE)) > 0) {
        
        for (int i = 0; i < read_count; ++i) {
            proc_buf[0][i] = conv_read_buffer[i * 2];     // L
            proc_buf[1][i] = conv_read_buffer[i * 2 + 1]; // R
        }

        // Convolve Left and Right against the single IR
        for (int ch = 0; ch < 2; ++ch) {
            std::fill_n(conv_in, CONV_FFT_SIZE, 0.0);
            for (int i = 0; i < read_count; ++i) conv_in[i] = proc_buf[ch][i];

            fftw_execute(plan_fwd);

            for (int k = 0; k <= CONV_FFT_SIZE / 2; ++k) {
                double real_audio = conv_out[k][0], imag_audio = conv_out[k][1];
                double real_ir = conv_ir_cmplx[k][0], imag_ir = conv_ir_cmplx[k][1];
                
                conv_out[k][0] = (real_audio * real_ir) - (imag_audio * imag_ir);
                conv_out[k][1] = (real_audio * imag_ir) + (imag_audio * real_ir);
            }

            fftw_execute(plan_inv);

            for (int i = 0; i < CONV_BLOCK_SIZE; ++i) {
                double sample = (ifft_in_real[i] / CONV_FFT_SIZE) + (i < OVERLAP_SIZE ? overlap_buffers[ch][i] : 0.0);
                channel_write_buffers[ch][i] = static_cast<float>(sample);
            }

            for (int i = 0; i < OVERLAP_SIZE; ++i) {
                overlap_buffers[ch][i] = static_cast<float>(ifft_in_real[CONV_BLOCK_SIZE + i] / CONV_FFT_SIZE);
            }
        }

        // Interleave output directly
        for (int i = 0; i < read_count; ++i) {
            interleaved_write_buffer[i * 2] = channel_write_buffers[0][i];
            interleaved_write_buffer[i * 2 + 1] = channel_write_buffers[1][i];
        }

        int write_offset_frames = 0;
        int write_len_frames = read_count; 

        if (frames_to_skip > 0) {
            if (frames_to_skip >= write_len_frames) {
                frames_to_skip -= write_len_frames;
                write_len_frames = 0; 
            } else {
                write_offset_frames = frames_to_skip;
                write_len_frames -= frames_to_skip;
                frames_to_skip = 0;
            }
        }

        if (write_len_frames > 0) {
            sf_writef_float(out_wav, &interleaved_write_buffer[write_offset_frames * 2], write_len_frames);
        }
    }

    int valid_tail_frames = OVERLAP_SIZE;
    while (valid_tail_frames > 0) {
        float max_val = std::max(std::abs(overlap_buffers[0][valid_tail_frames - 1]), std::abs(overlap_buffers[1][valid_tail_frames - 1]));
        if (max_val >= NOISE_FLOOR_GATE) break;
        valid_tail_frames--;
    }

    if (valid_tail_frames > 0) {
        std::vector<float> tail_out(valid_tail_frames * 2);
        for (int i = 0; i < valid_tail_frames; ++i) {
            tail_out[i * 2] = overlap_buffers[0][i];
            tail_out[i * 2 + 1] = overlap_buffers[1][i];
        }
        sf_writef_float(out_wav, tail_out.data(), valid_tail_frames);
        std::cout << "          -> -200dB Gate engaged. Drained " << valid_tail_frames << " tail frames.\n";
    }

    fftw_destroy_plan(plan_fwd); fftw_destroy_plan(plan_inv); 
    fftw_free(conv_in); fftw_free(conv_out); fftw_free(ifft_in_real);
    fftw_free(conv_ir_cmplx);
    sf_close(in_wav); sf_close(out_wav);

    std::cout << "\n[Success] Final Master written to: " << final_out_file << "\n";
    std::cout << "          Output incorporates " << (final_makeup_gain_db >= 0 ? "+" : "") 
              << std::fixed << std::setprecision(6) << final_makeup_gain_db << " dB makeup gain.\n\n";
    
    return 0;
}
