/*
 * ableton_adapter.cpp
 * Replaces the Bash adapter for creating Ableton Live projects.
 * Handles audio conversion, precise duration calculation, and .als XML patching.
 *
 * Compile: g++ -o ableton_adapter ableton_adapter.cpp -O3
 * Usage:   ./ableton_adapter <audio_input> <timemap_precise> <md5> <script_dir>
 */

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>

using namespace std;

// --- Helpers ---

// Execute shell command and get output
string exec(const char* cmd) {
    char buffer[128];
    string result = "";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) throw runtime_error("popen() failed!");
    while (fgets(buffer, sizeof buffer, pipe) != NULL) {
        result += buffer;
    }
    pclose(pipe);
    // Trim trailing whitespace/newline
    size_t last = result.find_last_not_of(" \n\r\t");
    if (last != string::npos) result = result.substr(0, last + 1);
    else result = ""; // Empty or all whitespace
    return result;
}

// Check file existence
bool file_exists(const string& name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0);
}

// Get file size in bytes
long long get_file_size(const string& filename) {
    struct stat stat_buf;
    int rc = stat(filename.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

// Replace all occurrences of a string
void replace_all(string& str, const string& from, const string& to) {
    if(from.empty()) return;
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

// --- Main ---

int main(int argc, char* argv[]) {
    if (argc < 5) {
        cerr << "Usage: ./ableton_adapter <audio_input> <timemap_precise> <md5> <script_dir>" << endl;
        return 1;
    }

    string audio_input = argv[1];
    string timemap_file = argv[2];
    string md5 = argv[3];
    string script_dir = argv[4];

    // Derived paths
    string project_dir = ".ableton Project";
    string imported_dir = project_dir + "/Samples/Imported";
    
    // 1. Setup Project Directory
    cout << "[Adapter] Setting up project structure..." << endl;
    string cmd_rm = "rm -rf \"" + project_dir + "\"";
    system(cmd_rm.c_str());
    
    // Ensure quotes around paths to handle spaces
    string cmd_cp = "cp -r \"" + script_dir + "/adapters/ableton/.ableton Project\" .";
    if (system(cmd_cp.c_str()) != 0) {
        cerr << "Error: Could not copy template from " << script_dir << endl;
        return 1;
    }

    // 2. Handle Audio Processing
    // Check for trimmed file first (from parser logic)
    string final_audio_name = "";
    string source_wav = "." + md5 + "-trimmed.wav";
    string audio_source_path = "";
    
    if (file_exists(source_wav)) {
        audio_source_path = source_wav;
        // Use original filename stem for the destination in Ableton
        size_t last_slash = audio_input.find_last_of("/\\");
        string filename = (last_slash == string::npos) ? audio_input : audio_input.substr(last_slash + 1);
        size_t last_dot = filename.find_last_of(".");
        final_audio_name = filename.substr(0, last_dot) + ".wav";
    } else {
        // Fallback to input file logic
        string base_input = audio_input;
        size_t last_dot = base_input.find_last_of(".");
        string stem = base_input.substr(0, last_dot);
        string wav_candidate = stem + ".wav";
        
        string check_file = audio_input;
        if (file_exists(wav_candidate)) check_file = wav_candidate;

        // Run ffprobe to check format
        string fmt_cmd = "ffprobe -v error -select_streams a:0 -show_entries stream=sample_fmt -of default=noprint_wrappers=1:nokey=1 \"" + check_file + "\"";
        string fmt = exec(fmt_cmd.c_str());

        if (fmt != "flt") {
            cout << "[Adapter] Converting to 32-bit float WAV..." << endl;
            string conv_cmd = "ffmpeg -y -i \"" + audio_input + "\" -c:a pcm_f32le \"" + wav_candidate + "\" >/dev/null 2>&1";
            system(conv_cmd.c_str());
            audio_source_path = wav_candidate;
        } else {
            audio_source_path = check_file;
        }
        
        size_t last_slash = audio_source_path.find_last_of("/\\");
        final_audio_name = (last_slash == string::npos) ? audio_source_path : audio_source_path.substr(last_slash + 1);
    }

    // Copy audio to project
    string dest_path = imported_dir + "/" + final_audio_name;
    string copy_cmd = "cp \"" + audio_source_path + "\" \"" + dest_path + "\"";
    system(copy_cmd.c_str());

    // 3. Get Audio Stats (Sample Rate & Duration in Samples)
    // Probe Sample Rate
    string probe_sr_cmd = "ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate -of default=noprint_wrappers=1:nokey=1 \"" + dest_path + "\"";
    string sample_rate_str = exec(probe_sr_cmd.c_str());
    double sample_rate = stod(sample_rate_str);

    // Probe Duration in Samples (duration_ts) for DefaultDuration
    string probe_dur_cmd = "ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts -of default=noprint_wrappers=1:nokey=1 \"" + dest_path + "\"";
    string duration_ts = exec(probe_dur_cmd.c_str());
    
    // Probe File Size
    long long file_size = get_file_size(dest_path);

    if (sample_rate <= 0) { cerr << "Error: Invalid sample rate." << endl; return 1; }

    // 4. Calculate Duration from Timemap (For Timeline/Loops)
    ifstream tm(timemap_file);
    if (!tm.is_open()) { cerr << "Error: Cannot open timemap " << timemap_file << endl; return 1; }
    
    double last_src = 0, last_tgt = 0;
    string line;
    while(getline(tm, line)) {
         if (line.empty()) continue;
         stringstream ss(line);
         ss >> last_src >> last_tgt;
    }
    tm.close();

    // This duration (in seconds) is used for positioning markers on the grid
    double final_duration = last_tgt / sample_rate;
    
    // 5. Generate Warp Markers XML
    cout << "[Adapter] Generating Warp Markers..." << endl;
    
    string als_file = project_dir + "/.ableton.als";
    string xml_file = als_file + ".xml";
    
    // Unzip ALS
    string rename_gz = "mv \"" + als_file + "\" \"" + als_file + ".gz\"";
    system(rename_gz.c_str());
    string unzip_cmd = "gunzip \"" + als_file + ".gz\"";
    system(unzip_cmd.c_str());
    
    if (file_exists(project_dir + "/.ableton")) {
        rename((project_dir + "/.ableton").c_str(), xml_file.c_str());
    } else if (!file_exists(xml_file)) {
        // Some systems might just drop the .gz
        rename(als_file.c_str(), xml_file.c_str());
    }

    // Read XML
    ifstream xml_in(xml_file);
    stringstream buffer;
    buffer << xml_in.rdbuf();
    string xml_content = buffer.str();
    xml_in.close();

    // Find Max ID
    int max_id = 0;
    size_t id_pos = 0;
    while ((id_pos = xml_content.find("Id=\"", id_pos)) != string::npos) {
        id_pos += 4;
        size_t end_quote = xml_content.find("\"", id_pos);
        string id_str = xml_content.substr(id_pos, end_quote - id_pos);
        try {
            int curr_id = stoi(id_str);
            if (curr_id > max_id) max_id = curr_id;
        } catch(...) {}
    }
    int marker_id = max_id + 1;

    // Build Markers
    stringstream markers_ss;
    ifstream tm_read(timemap_file);
    double src_f, tgt_f;
    
    markers_ss << fixed << setprecision(16); 

    while(getline(tm_read, line)) {
        if (line.empty()) continue;
        stringstream ss(line);
        ss >> src_f >> tgt_f;
        
        double sec_time = src_f / sample_rate;
        double beat_time = tgt_f / sample_rate;
        
        markers_ss << "<WarpMarker Id=\"" << marker_id++ << "\" SecTime=\"" << sec_time << "\" BeatTime=\"" << beat_time << "\" />";
    }

    // 6. Patch the XML
    cout << "[Adapter] Patching Ableton Project..." << endl;

    string template_filename = "";
    string template_basename = "";
    string final_basename = final_audio_name.substr(0, final_audio_name.find_last_of("."));

    // Strategy: Find the first <SampleRef> block (Main Clip) and overwrite its file references directly.
    size_t sample_ref_pos = xml_content.find("<SampleRef>");
    
    if (sample_ref_pos != string::npos) {
        // Within SampleRef, find <FileRef>
        size_t file_ref_pos = xml_content.find("<FileRef>", sample_ref_pos);
        size_t end_fileref = xml_content.find("</FileRef>", file_ref_pos);
        
        if (file_ref_pos != string::npos && end_fileref != string::npos) {
            
            // A. Handle <RelativePath Value="..."> (Common in generic templates)
            size_t rel_path_pos = xml_content.find("<RelativePath Value=\"", file_ref_pos);
            if (rel_path_pos != string::npos && rel_path_pos < end_fileref) {
                size_t val_start = rel_path_pos + 21; // Length of <RelativePath Value="
                size_t val_end = xml_content.find("\"", val_start);
                
                // Capture OLD name
                string old_rel_path = xml_content.substr(val_start, val_end - val_start);
                size_t last_slash = old_rel_path.find_last_of("/\\");
                if (last_slash != string::npos) {
                    template_filename = old_rel_path.substr(last_slash + 1);
                } else {
                    template_filename = old_rel_path;
                }

                // Replace with NEW path
                string new_path = "Samples/Imported/" + final_audio_name;
                xml_content.replace(val_start, val_end - val_start, new_path);
                
                // Shift offset for subsequent search
                long shift = (long)new_path.length() - (long)(val_end - val_start);
                end_fileref += shift; 
            }

            // B. Handle <Path Value="..."> (If present, usually absolute)
            size_t path_pos = xml_content.find("<Path Value=\"", file_ref_pos);
            if (path_pos != string::npos && path_pos < end_fileref) {
                size_t val_start = path_pos + 13; 
                size_t val_end = xml_content.find("\"", val_start);
                
                string new_path = "Samples/Imported/" + final_audio_name;
                xml_content.replace(val_start, val_end - val_start, new_path);
                
                long shift = (long)new_path.length() - (long)(val_end - val_start);
                end_fileref += shift; 
            }

            // C. Handle <Name Value="..."> (If present)
            size_t name_pos = xml_content.find("<Name Value=\"", file_ref_pos);
            if (name_pos != string::npos && name_pos < end_fileref) {
                size_t val_start = name_pos + 13;
                size_t val_end = xml_content.find("\"", val_start);
                
                // If we didn't get it from RelativePath, grab it here
                if (template_filename.empty()) {
                    template_filename = xml_content.substr(val_start, val_end - val_start);
                }
                
                xml_content.replace(val_start, val_end - val_start, final_audio_name);
                
                long shift = (long)final_audio_name.length() - (long)(val_end - val_start);
                end_fileref += shift;
            }
        }
    } else {
         cerr << "[Adapter] Warning: Could not find <SampleRef> block in template." << endl;
    }

    // 7. Update Track/Clip Names and Metadata
    
    // Replace Names
    if (!template_filename.empty()) {
        size_t dot_pos = template_filename.find_last_of(".");
        template_basename = (dot_pos != string::npos) ? template_filename.substr(0, dot_pos) : template_filename;
        
        cout << "[Adapter] Identified template file: " << template_filename << endl;
        
        replace_all(xml_content, "Value=\"" + template_basename + "\"", "Value=\"" + final_basename + "\"");
        replace_all(xml_content, "Value=\"" + template_filename + "\"", "Value=\"" + final_audio_name + "\"");
    }

    // File Size (Protect "0")
    size_t scan_pos = 0;
    while ((scan_pos = xml_content.find("OriginalFileSize", scan_pos)) != string::npos) {
        size_t val_pos = xml_content.find("Value=\"", scan_pos);
        if (val_pos != string::npos) {
            size_t val_end = xml_content.find("\"", val_pos + 7);
            string curr_val = xml_content.substr(val_pos + 7, val_end - (val_pos + 7));
            if (curr_val != "0") {
                 xml_content.replace(val_pos + 7, curr_val.length(), to_string(file_size));
            }
        }
        scan_pos += 1;
    }

    // Sample Volume (-6dB)
    scan_pos = 0;
    while ((scan_pos = xml_content.find("SampleVolume", scan_pos)) != string::npos) {
        size_t val_pos = xml_content.find("Value=\"", scan_pos);
        if (val_pos != string::npos) {
            size_t val_end = xml_content.find("\"", val_pos + 7);
            xml_content.replace(val_pos + 7, val_end - (val_pos + 7), "0.5011872053");
        }
        scan_pos += 1;
    }

    // Global Duration & Sample Rate
    auto replace_xml_val = [&](string key, string new_val) {
        size_t pos = 0;
        while ((pos = xml_content.find(key, pos)) != string::npos) {
            size_t v_pos = xml_content.find("Value=\"", pos);
            if (v_pos != string::npos && v_pos < pos + 50) { 
                 size_t v_end = xml_content.find("\"", v_pos + 7);
                 xml_content.replace(v_pos + 7, v_end - (v_pos + 7), new_val);
            }
            pos += 1;
        }
    };
    
    // Calculated Duration in Seconds (For Timeline Markers)
    stringstream dur_ss; dur_ss << fixed << setprecision(16) << final_duration;
    
    // --- FIX: Use Integer Samples for DefaultDuration ---
    replace_xml_val("DefaultDuration", duration_ts); 
    replace_xml_val("DefaultSampleRate", to_string((int)sample_rate));
    
    // --- Use Seconds for Timeline ---
    replace_xml_val("CurrentEnd", dur_ss.str());
    replace_xml_val("LoopEnd", dur_ss.str());
    replace_xml_val("HiddenLoopEnd", dur_ss.str());
    replace_xml_val("OutMarker", dur_ss.str());
    replace_xml_val("LoopLength", dur_ss.str());
    replace_xml_val("CurrentTime", dur_ss.str());
    replace_xml_val("OtherTime", dur_ss.str()); 

    // 8. Inject Warp Markers
    size_t wm_start = xml_content.find("<WarpMarkers>");
    size_t wm_end = xml_content.find("</WarpMarkers>");
    if (wm_start != string::npos && wm_end != string::npos) {
        xml_content.replace(wm_start + 13, wm_end - (wm_start + 13), markers_ss.str());
    } else {
        cerr << "Warning: <WarpMarkers> tag not found in template." << endl;
    }

    // 9. Write and Zip
    ofstream xml_out(xml_file);
    xml_out << xml_content;
    xml_out.close();

    cout << "[Adapter] Compressing project file..." << endl;
    string gzip_cmd = "gzip -c \"" + xml_file + "\" > \"" + als_file + "\"";
    system(gzip_cmd.c_str());
    
    remove(xml_file.c_str());

    cout << "[Adapter] Done. Project created at: " << project_dir << endl;

    return 0;
}
