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
#include "pffft.h"

// --- Configuration Constants ---
const double ATTACK_TOLERANCE_LU = 3.0;      
const double RELEASE_TOLERANCE_LU = 8.0;     
const double MIN_GAP_SEC = 0.5;              
const double MIN_PHRASE_SEC = 5.0;           
const size_t TOP_X_CHUNKS = 10;               
const double SERATO_DETECTION_THRESH_LU = 1.0; 
const double SERATO_MAKEUP_GAIN_DB = 6.0;
const int FFT_SIZE = 8192; 
const int NUM_BANDS = 10; // Logarithmic macro-bands

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
        std::cerr << "Usage: " << argv[0] << " <timemap_file> <source_wav> <target_wav>\n";
        return 1;
    }

    std::string map_file = argv[1];
    std::string src_wav_file = argv[2];
    std::string tgt_wav_file = argv[3];

    SF_INFO src_sfinfo, tgt_sfinfo;
    src_sfinfo.format = 0; tgt_sfinfo.format = 0;
    
    SNDFILE* src_infile = sf_open(src_wav_file.c_str(), SFM_READ, &src_sfinfo);
    if (!src_infile) { std::cerr << "Error: Could not open Source WAV.\n"; return 1; }
    
    SNDFILE* tgt_infile = sf_open(tgt_wav_file.c_str(), SFM_READ, &tgt_sfinfo);
    if (!tgt_infile) { std::cerr << "Error: Could not open Target WAV.\n"; return 1; }
    
    std::cout << "Opened Source: " << src_sfinfo.samplerate << "Hz, " << src_sfinfo.channels << " channels.\n";
    std::cout << "Opened Target: " << tgt_sfinfo.samplerate << "Hz, " << tgt_sfinfo.channels << " channels.\n";

    std::ifstream file(map_file);
    if (!file.is_open()) { std::cerr << "Error: Could not open timemap.\n"; return 1; }

    std::vector<std::pair<size_t, double>> map_points;
    size_t src;
    double tgt;
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
    for (size_t i = 0; i < final_blocks.size(); ++i) final_blocks[i].id = i + 1;

    // ========================================================================
    // PHASE 2: Serato Headroom Detection
    // ========================================================================
    std::cout << "\n[Phase 2] Running Serato Headroom Diagnostics...\n";
    double tgt_global_lufs = 0.0, tgt_global_rms = 0.0;
    calculate_metrics(tgt_infile, 0, tgt_sfinfo.frames, tgt_sfinfo.channels, tgt_sfinfo.samplerate, tgt_global_lufs, tgt_global_rms);
    
    double target_gain_multiplier = 1.0;
    double lufs_difference = src_global_lufs - tgt_global_lufs;
    
    if (lufs_difference >= SERATO_DETECTION_THRESH_LU) {
        target_gain_multiplier = std::pow(10.0, SERATO_MAKEUP_GAIN_DB / 20.0);
        std::cout << "=> Serato headroom drop detected. Applying x" << std::fixed << std::setprecision(2) << target_gain_multiplier << " makeup gain to target.\n";
    } else {
        std::cout << "=> Tracks are gain-matched. No makeup gain required.\n";
    }

    // ========================================================================
    // PHASE 3: PFFFT Preparation & Chunk Processing Loop
    // ========================================================================
    std::cout << "\n[Phase 3] Running Hanning-Windowed PFFFT Engine (" << FFT_SIZE << " bins)...\n";
    
    PFFFT_Setup* pffft_setup = pffft_new_setup(FFT_SIZE, PFFFT_REAL);
    float* src_fft_input  = (float*)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float* src_fft_output = (float*)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float* tgt_fft_input  = (float*)pffft_aligned_malloc(FFT_SIZE * sizeof(float));
    float* tgt_fft_output = (float*)pffft_aligned_malloc(FFT_SIZE * sizeof(float));

    std::vector<double> src_psd_accumulator(FFT_SIZE / 2, 0.0);
    std::vector<double> tgt_psd_accumulator(FFT_SIZE / 2, 0.0);
    size_t total_windows = 0;

    std::vector<float> hanning(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i) {
        hanning[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
    }

    std::vector<float> src_read_buf(FFT_SIZE * src_sfinfo.channels);
    std::vector<float> tgt_read_buf(FFT_SIZE * tgt_sfinfo.channels);
    size_t hop_size = FFT_SIZE / 2;

    for (const auto& b : final_blocks) {
        size_t s_frame = b.start_frame;
        while (s_frame + FFT_SIZE <= b.end_frame) {
            double t_start = map_source_to_target(s_frame, parents);
            
            sf_seek(src_infile, s_frame, SEEK_SET);
            sf_readf_float(src_infile, src_read_buf.data(), FFT_SIZE);
            for (int i = 0; i < FFT_SIZE; ++i) {
                float mono = (src_read_buf[i*src_sfinfo.channels] + src_read_buf[i*src_sfinfo.channels + 1]) * 0.5f;
                src_fft_input[i] = mono * hanning[i];
            }

            sf_seek(tgt_infile, static_cast<sf_count_t>(t_start), SEEK_SET);
            sf_readf_float(tgt_infile, tgt_read_buf.data(), FFT_SIZE);
            for (int i = 0; i < FFT_SIZE; ++i) {
                float mono = (tgt_read_buf[i*tgt_sfinfo.channels] + tgt_read_buf[i*tgt_sfinfo.channels + 1]) * 0.5f;
                tgt_fft_input[i] = (mono * target_gain_multiplier) * hanning[i];
            }

            pffft_transform_ordered(pffft_setup, src_fft_input, src_fft_output, nullptr, PFFFT_FORWARD);
            pffft_transform_ordered(pffft_setup, tgt_fft_input, tgt_fft_output, nullptr, PFFFT_FORWARD);

            for (int k = 0; k < FFT_SIZE / 2; ++k) {
                float s_re = src_fft_output[2*k], s_im = src_fft_output[2*k+1];
                float t_re = tgt_fft_output[2*k], t_im = tgt_fft_output[2*k+1];
                src_psd_accumulator[k] += (s_re*s_re + s_im*s_im);
                tgt_psd_accumulator[k] += (t_re*t_re + t_im*t_im);
            }
            
            total_windows++;
            s_frame += hop_size;
        }
    }

    // ========================================================================
    // PHASE 4: 10-Band Macro Grouping & Universal Mozart Floor
    // ========================================================================
    std::cout << "[Phase 4] Applying 10-Band Logarithmic Grouping & Universal Mozart Floor...\n";
    
    double macro_freqs[NUM_BANDS];
    std::vector<double> band_deltas(NUM_BANDS, 0.0);
    double bin_width = static_cast<double>(src_sfinfo.samplerate) / FFT_SIZE;

    double log_min = std::log10(20.0);
    double log_max = std::log10(20000.0);
    for (int i = 0; i < NUM_BANDS; ++i) {
        macro_freqs[i] = std::pow(10.0, log_min + i * (log_max - log_min) / (NUM_BANDS - 1));
    }

    for (int i = 0; i < NUM_BANDS; ++i) {
        double f_center = macro_freqs[i];
        double f_low = (i == 0) ? 20.0 : std::pow(10.0, (std::log10(macro_freqs[i-1]) + std::log10(f_center)) / 2.0);
        double f_high = (i == NUM_BANDS - 1) ? 20000.0 : std::pow(10.0, (std::log10(f_center) + std::log10(macro_freqs[i+1])) / 2.0);
        
        int bin_start = std::max(1, static_cast<int>(f_low / bin_width));
        int bin_end = std::min(FFT_SIZE / 2 - 1, static_cast<int>(f_high / bin_width));
        
        double src_sum = 0.0, tgt_sum = 0.0;
        int count = 0;
        for (int k = bin_start; k <= bin_end; ++k) {
            src_sum += src_psd_accumulator[k];
            tgt_sum += tgt_psd_accumulator[k];
            count++;
        }
        
        if (count > 0 && tgt_sum > 0 && src_sum > 0) {
            double src_db = 10.0 * std::log10((src_sum / count) / total_windows);
            double tgt_db = 10.0 * std::log10((tgt_sum / count) / total_windows);
            band_deltas[i] = src_db - tgt_db; 
        }
    }

    // The Universal Mozart Floor: 20Hz noise bin overwritten with a 0.5dB protective cut relative to 43Hz
    band_deltas[0] = band_deltas[1] - 0.5;

    // ========================================================================
    // PHASE 5: SVG Rendering
    // ========================================================================
    std::string output_dir = src_wav_file;
    size_t last_slash = output_dir.find_last_of("/\\");
    if (last_slash != std::string::npos) output_dir = output_dir.substr(0, last_slash) + "/";
    else output_dir = "";

    std::string svg_path = output_dir + "eq_match_curve.svg";
    std::cout << "[Phase 5] Writing 1600x800 Bezier Vector Map to: " << svg_path << "\n";

    std::ofstream svg(svg_path);
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1600 800\" style=\"background-color:#1e1e1e;\">\n";
    svg << "  <line x1=\"0\" y1=\"400\" x2=\"1600\" y2=\"400\" stroke=\"#ffffff\" stroke-width=\"2\" opacity=\"0.5\" />\n";
    svg << "  <line x1=\"0\" y1=\"200\" x2=\"1600\" y2=\"200\" stroke=\"#888888\" stroke-width=\"1\" stroke-dasharray=\"6\" opacity=\"0.3\" />\n";
    svg << "  <line x1=\"0\" y1=\"600\" x2=\"1600\" y2=\"600\" stroke=\"#888888\" stroke-width=\"1\" stroke-dasharray=\"6\" opacity=\"0.3\" />\n";

    std::vector<Point> pts(NUM_BANDS);
    for (int i = 0; i < NUM_BANDS; ++i) {
        double f = macro_freqs[i];
        double db = std::max(-3.0, std::min(3.0, band_deltas[i])); 
        pts[i].x = 1600.0 * (std::log10(f) - std::log10(20.0)) / (std::log10(20000.0) - std::log10(20.0));
        pts[i].y = 400.0 - (db * (400.0 / 3.0));
    }

    svg << "  <path d=\"M " << pts[0].x << "," << pts[0].y << " ";
    for (int i = 0; i < NUM_BANDS - 1; ++i) {
        double dx = pts[i+1].x - pts[i].x;
        double cp1x = pts[i].x + (dx / 3.0);
        double cp1y = pts[i].y;
        double cp2x = pts[i+1].x - (dx / 3.0);
        double cp2y = pts[i+1].y;
        svg << "C " << cp1x << "," << cp1y << " " << cp2x << "," << cp2y << " " << pts[i+1].x << "," << pts[i+1].y << " ";
    }
    svg << "\" fill=\"none\" stroke=\"#00ffff\" stroke-width=\"4\" stroke-linejoin=\"round\" />\n";

    for (int i = 0; i < NUM_BANDS; ++i) {
        svg << "  <circle cx=\"" << pts[i].x << "\" cy=\"" << pts[i].y << "\" r=\"5\" fill=\"#00ffff\" />\n";
        double y_txt = (pts[i].y <= 400.0) ? pts[i].y - 15.0 : pts[i].y + 25.0;
        svg << "  <text x=\"" << pts[i].x << "\" y=\"" << y_txt << "\" fill=\"#ffffff\" font-family=\"sans-serif\" font-size=\"14\" text-anchor=\"middle\">"
            << std::fixed << std::setprecision(0) << macro_freqs[i] << " Hz | ";
        if (band_deltas[i] > 0) svg << "+";
        svg << std::fixed << std::setprecision(2) << band_deltas[i] << " dB</text>\n";
    }
    svg << "</svg>\n";
    svg.close();

    // ========================================================================
    // PHASE 6: Translation Engine (Pro-Q 4 Parameter Terminal Output)
    // ========================================================================
    std::cout << "\n======================================================\n";
    std::cout << " FABFILTER PRO-Q 4 PARAMETER MAP\n";
    std::cout << "======================================================\n";
    std::cout << std::left << std::setw(12) << "[Type]"
              << std::setw(12) << "[Freq]"
              << std::setw(12) << "[Gain]"
              << std::setw(10) << "[Q]"
              << std::setw(12) << "[Slope]" << "\n";
    std::cout << "------------------------------------------------------\n";

    // Store nodes for Phase 7 Lua generation
    struct EqNode { int band; std::string type; double freq; double gain; double q; std::string slope; };
    std::vector<EqNode> parsed_nodes;

    for (int i = 0; i < NUM_BANDS; ++i) {
        std::string type = "Bell";
        double q = 1.0;
        std::string slope = "12 dB/oct"; 
        double target_freq = macro_freqs[i];

        if (i == 0) {
            type = "Bell";
            q = 2.0; 
            slope = "48 dB/oct"; 
        } else if (i == 1) {
            type = "Low Shelf";
            q = 1.0; 
            target_freq = macro_freqs[i] * 2.0; // The Exact Shelf Formula (One-Octave Half-Gain Point)
        } else if (i == NUM_BANDS - 1) {
            type = "High Shelf";
            q = 1.0;
        } else {
            double prev_delta = std::abs(band_deltas[i] - band_deltas[i-1]);
            double next_delta = std::abs(band_deltas[i] - band_deltas[i+1]);
            double sharpness = prev_delta + next_delta;
            q = std::max(0.5, std::min(2.5, 0.5 + (sharpness * 2.0)));
        }

        parsed_nodes.push_back({i + 1, type, target_freq, band_deltas[i], q, slope});

        std::string gain_str = (band_deltas[i] >= 0 ? "+" : "") + std::to_string(band_deltas[i]);
        gain_str = gain_str.substr(0, gain_str.find('.') + 3) + " dB";

        std::cout << std::left << std::setw(12) << type
                  << std::setw(12) << (std::to_string(static_cast<int>(target_freq)) + " Hz")
                  << std::setw(12) << gain_str
                  << std::setw(10) << std::fixed << std::setprecision(2) << q
                  << std::setw(12) << slope << "\n";
    }
    std::cout << "======================================================\n\n";

    // ========================================================================
    // PHASE 7: Native Reaper Lua Automation Script Generation
    // ========================================================================
    std::string lua_path = output_dir + "apply_eq_match.lua";
    std::cout << "[Phase 7] Generating Reaper Automation Script: " << lua_path << "\n";

    std::ofstream lua(lua_path);
    lua << R"(-- Auto-generated Pro-Q 4 Match Script
local track = reaper.GetSelectedTrack(0, 0)
if not track then
    reaper.ShowConsoleMsg("Error: No track selected in Reaper.\n")
    return
end

local fx_idx = -1
for i = 0, reaper.TrackFX_GetCount(track) - 1 do
    local _, name = reaper.TrackFX_GetFXName(track, i)
    if name:match("Pro%-Q 4") then
        fx_idx = i
        break
    end
end

if fx_idx == -1 then
    reaper.ShowConsoleMsg("Error: 'Pro-Q 4' not found in the selected track's FX chain.\n")
    return
end

local param_map = {}
local num_params = reaper.TrackFX_GetNumParams(track, fx_idx)
for i = 0, num_params - 1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx_idx, i)
    param_map[name] = i
end

local function set_param(name, val)
    local idx = param_map[name]
    if idx then
        reaper.TrackFX_SetParamNormalized(track, fx_idx, idx, val)
    end
end

-- Normalization Math
local function norm_freq(f) return math.log(f / 10.0) / math.log(30000.0 / 10.0) end
local function norm_gain(g) return (g + 30.0) / 60.0 end
local function norm_q(q) return math.log(q / 0.025) / math.log(40.0 / 0.025) end

local shapes = { ["Bell"] = 0.0, ["Low Shelf"] = 1.0/9.0, ["High Shelf"] = 3.0/9.0 }

local eq_nodes = {
)";

    for (const auto& n : parsed_nodes) {
        lua << "    { band = " << n.band 
            << ", type = \"" << n.type 
            << "\", freq = " << std::fixed << std::setprecision(2) << n.freq 
            << ", gain = " << n.gain 
            << ", q = " << n.q << " },\n";
    }

    lua << R"(}

reaper.Undo_BeginBlock()

for _, n in ipairs(eq_nodes) do
    local prefix = "Band " .. n.band .. " "
    
    -- Enable Band
    set_param(prefix .. "Used", 1.0)
    
    -- Apply Normalized Values
    set_param(prefix .. "Frequency", norm_freq(n.freq))
    set_param(prefix .. "Gain", norm_gain(n.gain))
    set_param(prefix .. "Q", norm_q(n.q))
    set_param(prefix .. "Shape", shapes[n.type])
end

reaper.Undo_EndBlock("Apply Pro-Q 4 Eq Match", -1)
reaper.ShowConsoleMsg("Successfully applied " .. #eq_nodes .. " nodes to Pro-Q 4.\n")
)";

    lua.close();
    std::cout << "Done! Drag 'apply_eq_match.lua' into your Reaper action list to execute.\n";

    // Cleanup
    pffft_aligned_free(src_fft_input); pffft_aligned_free(src_fft_output);
    pffft_aligned_free(tgt_fft_input); pffft_aligned_free(tgt_fft_output);
    pffft_destroy_setup(pffft_setup);
    sf_close(src_infile); sf_close(tgt_infile);

    return 0;
}
