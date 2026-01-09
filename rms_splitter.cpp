/*
 * rms_splitter.cpp
 * Compile: g++ -o rms_splitter rms_splitter.cpp -lsndfile -O3
 */

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <sndfile.h>
#include <iomanip>
#include <cstdio>

struct Section {
    long long start_sample = 0;
    long long end_sample = 0;
    long long end_warp = 0;
    double loudness_db = -144.0;
    bool is_loud = false;
};

// Helper: Linear to dB
double linear_to_db(double linear) {
    if (linear <= 0.0) return -144.0;
    return 20.0 * log10(linear);
}

// Helper: Samples to MM:SS.mmm using built-in Banker's Rounding
std::string format_time(long long samples, int samplerate) {
    double total_seconds = (double)samples / samplerate;
    int minutes = (int)(total_seconds / 60);
    double seconds = total_seconds - (minutes * 60);
    
    // Banker's Rounding
    seconds = std::rint(seconds * 1000.0) / 1000.0;
    
    if (seconds >= 60.0) {
        seconds -= 60.0;
        minutes += 1;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%02d:%06.3f", minutes, seconds);
    return std::string(buffer);
}

// Helper: Count valid data lines in the file
size_t count_valid_lines(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    size_t count = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line.find("[") != std::string::npos) continue;
        count++;
    }
    return count;
}

int main(int argc, char* argv[]) {
    // --- ARGUMENT CHECK ---
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <wav> <timemap> [ramp_ms] [thresh_db]" << std::endl;
        return 1;
    }

    std::string wav_path = argv[1];
    std::string map_path = argv[2];
    double ramp_ms = (argc >= 4) ? std::stod(argv[3]) : 30.0;
    double threshold_db = (argc >= 5) ? std::stod(argv[4]) : -26.0;

    // --- 1. LOAD AUDIO INFO ---
    SF_INFO sfinfo;
    SNDFILE* infile = sf_open(wav_path.c_str(), SFM_READ, &sfinfo);
    if (!infile) {
        std::cerr << "Error: Could not open input WAV." << std::endl;
        return 1;
    }
    
    long long total_frames = sfinfo.frames;
    int channels = sfinfo.channels;
    int samplerate = sfinfo.samplerate;
    
    std::cout << "Loaded: " << samplerate << "Hz, " << channels << "ch, " << total_frames << " frames." << std::endl;

    // --- 2. PARSE TIMEMAP (Pre-Count Strategy) ---
    // First, count how many points we have so we know when to stop.
    size_t total_points = count_valid_lines(map_path);
    std::cout << "Found " << total_points << " points in timemap." << std::endl;

    std::vector<Section> sections;
    std::ifstream map_file(map_path);
    std::string line;
    bool first_point = true;
    size_t points_processed = 0;
    
    while (std::getline(map_file, line)) {
        if (line.empty()) continue;
        if (line.find("[") != std::string::npos) continue; 
        
        std::stringstream ss(line);
        double warp_time, orig_time;
        if (ss >> warp_time >> orig_time) {
            points_processed++;
            
            long long current_warp = std::llrint(warp_time);
            long long current_sample = std::llrint(orig_time);
            
            // 1. Close the previous section (if any)
            if (!first_point) {
                 if (!sections.empty()) {
                     sections.back().end_sample = current_sample;
                     sections.back().end_warp = current_warp;
                 }
            } else {
                // Handle Header (0 to First Point)
                if (current_sample > 0) {
                    Section header;
                    header.start_sample = 0;
                    header.end_sample = current_sample;
                    header.end_warp = current_warp;
                    sections.push_back(header);
                }
                first_point = false;
            }

            // 2. Start New Section?
            // Only start a new section if this is NOT the last point.
            // If it IS the last point, we just used it to close the previous section, 
            // effectively stopping the map exactly where the timemap ends.
            if (points_processed < total_points) {
                Section s;
                s.start_sample = current_sample;
                s.end_sample = total_frames; // Placeholder, will be updated next iter
                sections.push_back(s);
            }
        }
    }
    map_file.close();

    std::cout << "Parsed " << sections.size() << " sections." << std::endl;

    // --- 3. ANALYZE LOUDNESS (RMS) ---
    std::cout << "Scanning loudness (RMS)..." << std::endl;
    
    const int buffer_size = 4096;
    std::vector<float> read_buffer(buffer_size * channels);
    
    for (auto& sec : sections) {
        long long current = sec.start_sample;
        long long end = sec.end_sample;
        
        if (end > total_frames) end = total_frames;
        if (current >= end) continue;

        double sum_squares = 0.0;
        long long total_samples = 0;

        sf_seek(infile, current, SEEK_SET);
        
        while (current < end) {
            long long frames_to_read = std::min((long long)buffer_size, end - current);
            sf_count_t read_count = sf_readf_float(infile, read_buffer.data(), frames_to_read);
            
            if (read_count == 0) break;

            for (int i = 0; i < read_count * channels; ++i) {
                float val = read_buffer[i];
                sum_squares += val * val;
            }
            total_samples += read_count * channels;
            current += read_count;
        }

        double rms = 0.0;
        if (total_samples > 0) {
            rms = std::sqrt(sum_squares / total_samples);
        }

        sec.loudness_db = linear_to_db(rms);
        sec.is_loud = (sec.loudness_db > threshold_db);
    }

    // --- LOGIC: CARRYOVER STATE ---
    // The final valid section (which now ends exactly at the last timemap point)
    // should inherit the state of the one before it to preserve context.
    if (sections.size() >= 2) {
        bool penultimate_state = sections[sections.size() - 2].is_loud;
        sections.back().is_loud = penultimate_state;
        std::cout << "Last section override: Inherited " 
                  << (penultimate_state ? "LOUD" : "QUIET") 
                  << " from previous section." << std::endl;
    }

    // --- 4. WRITE MAP FILE ---
    std::string out_map_path = map_path + "-loudness";
    std::ofstream out_map(out_map_path);
    out_map << std::fixed << std::setprecision(4);
    
    for (const auto& sec : sections) {
        out_map << sec.end_warp << " " 
                << sec.end_sample << " " 
                << format_time(sec.start_sample, samplerate) << " "
                << sec.loudness_db << " " 
                << (sec.is_loud ? "LOUD" : "QUIET") << "\n";
    }
    out_map.close();
    std::cout << "Map saved: " << out_map_path << std::endl;


    // --- 5. SPLIT AUDIO ---
    std::cout << "Splitting (Ramp: " << ramp_ms << "ms)..." << std::endl;

    std::string input_path = argv[1];
    std::string dir = "";
    std::string filename = input_path;
    
    size_t last_slash = input_path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        dir = input_path.substr(0, last_slash + 1); 
        filename = input_path.substr(last_slash + 1);
    }
    
    std::string name_loud = dir + "channel=loud;" + filename;
    std::string name_quiet = dir + "channel=quiet;" + filename;

    std::cout << "Writing to: " << name_loud << std::endl;

    SF_INFO out_info = sfinfo; 
    
    SNDFILE* out_loud = sf_open(name_loud.c_str(), SFM_WRITE, &out_info);
    SNDFILE* out_quiet = sf_open(name_quiet.c_str(), SFM_WRITE, &out_info);

    if (!out_loud || !out_quiet) {
        std::cerr << "Error creating output files." << std::endl;
        sf_close(infile);
        return 1;
    }

    long long ramp_frames = static_cast<long long>((ramp_ms / 1000.0) * samplerate);
    
    sf_seek(infile, 0, SEEK_SET);
    
    long long current_frame = 0;
    size_t section_idx = 0;
    std::vector<float> loud_buffer(buffer_size * channels);
    std::vector<float> quiet_buffer(buffer_size * channels);

    while (current_frame < total_frames) {
        long long frames_to_read = std::min((long long)buffer_size, total_frames - current_frame);
        sf_count_t read_count = sf_readf_float(infile, read_buffer.data(), frames_to_read);
        if (read_count == 0) break;

        for (int i = 0; i < read_count; ++i) {
            long long global_pos = current_frame + i;

            // Only advance if we haven't reached the last defined section
            if (section_idx < sections.size() - 1) {
                if (global_pos >= sections[section_idx].end_sample) section_idx++;
            }
            
            // Note: If global_pos is beyond the end of the last section (the rounding gap),
            // section_idx stays at sections.size() - 1. 
            // The audio will naturally continue using the state of the final section.

            // Lookahead Ramping
            float target_gain;
            long long frames_until_next = -1;
            
            if (section_idx < sections.size() - 1) {
                 frames_until_next = sections[section_idx].end_sample - global_pos;
            }

            if (frames_until_next != -1 && frames_until_next <= ramp_frames) {
                float next_state = sections[section_idx + 1].is_loud ? 1.0f : 0.0f;
                float curr_state = sections[section_idx].is_loud ? 1.0f : 0.0f;
                float progress = 1.0f - (static_cast<float>(frames_until_next) / ramp_frames);
                target_gain = curr_state + (next_state - curr_state) * progress;
            } else {
                target_gain = sections[section_idx].is_loud ? 1.0f : 0.0f;
            }

            for (int c = 0; c < channels; ++c) {
                float sample = read_buffer[i * channels + c];
                loud_buffer[i * channels + c] = sample * target_gain;
                quiet_buffer[i * channels + c] = sample * (1.0f - target_gain);
            }
        }

        sf_writef_float(out_loud, loud_buffer.data(), read_count);
        sf_writef_float(out_quiet, quiet_buffer.data(), read_count);
        
        current_frame += read_count;
    }

    sf_close(infile);
    sf_close(out_loud);
    sf_close(out_quiet);

    std::cout << "Done." << std::endl;
    return 0;
}
