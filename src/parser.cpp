#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cmath>
#include <map>
#include <set>
#include <regex>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <sys/stat.h>

using namespace std;

struct WarpMarker {
    string original_line;
    string time_str;
    string tempo_str;
    string label_def;
    string label_prefix;
    
    double time_seconds;
    double source_frame; // Calculated input frame
    double target_frame; // Calculated output frame
    
    // Parsed properties
    bool is_log_sync = false;
    bool is_label_ref = false;
    bool is_numeric = false;
};

struct Settings {
    string title;
    double scale = 1.0;
};

// --- Helper Functions ---

string trim_str(const string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool file_exists(const string& name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0);
}

double parse_timestamp(const string& time_str) {
    int min;
    double sec;
    char colon;
    stringstream ss(time_str);
    ss >> min >> colon >> sec;
    if (ss.fail() || colon != ':') return -1.0;
    return (min * 60.0) + sec;
}

string format_timestamp(double seconds) {
    int min = (int)(seconds / 60);
    double sec = seconds - (min * 60);
    stringstream ss;
    ss << setfill('0') << setw(2) << min << ":" 
       << fixed << setfill('0') << setw(6) << setprecision(3) << sec;
    return ss.str();
}

double eval_math_string(string s) {
    s.erase(remove_if(s.begin(), s.end(), ::isspace), s.end());
    double total = 0;
    double current = 0;
    char op = '+';
    size_t i = 0;
    while (i < s.length()) {
        size_t len = 0;
        while (i + len < s.length() && (isdigit(s[i+len]) || s[i+len] == '.')) len++;
        if (len > 0) {
            current = stod(s.substr(i, len));
            if (op == '+') total += current;
            else if (op == '-') total -= current;
            i += len;
        } else {
            if (s[i] == '+' || s[i] == '-') { op = s[i]; i++; }
            else break; 
        }
    }
    return total;
}

double parse_tempo_math(const string& tempo_raw, string& out_formatted) {
    string base_part;
    string scale_part;
    size_t star_pos = tempo_raw.find('*');
    
    if (star_pos != string::npos) {
        base_part = tempo_raw.substr(0, star_pos);
        scale_part = tempo_raw.substr(star_pos + 1);
    } else {
        base_part = tempo_raw;
    }
    
    double base_val = eval_math_string(base_part);
    stringstream ss_base;
    ss_base << fixed << setprecision(2) << base_val;
    string base_formatted = ss_base.str();
    
    double final_val = base_val;
    if (!scale_part.empty()) {
        double scale_val = stod(scale_part);
        final_val *= scale_val;
        out_formatted = base_formatted + "*" + scale_part;
    } else {
        out_formatted = base_formatted;
    }
    return final_val;
}

Settings load_settings(const string& filename) {
    Settings s;
    ifstream file(filename);
    string line;
    while (getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = trim_str(line.substr(0, eq));
        string val = trim_str(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        if (key == "title") s.title = val;
        else if (key == "scale") s.scale = stod(val);
    }
    return s;
}

map<string, pair<long, long>> load_log_file(const string& filename) {
    map<string, pair<long, long>> log_map;
    ifstream file(filename);
    if (!file.is_open()) return log_map;
    string line;
    long line_num = 0;
    while (getline(file, line)) {
        if (line.empty()) continue;
        size_t pipe = line.find('|');
        if (pipe != string::npos) {
            string timestamp = line.substr(0, pipe);
            if (line.length() >= 53) {
                string frame_str = line.substr(44, 9);
                try {
                    long frame = stol(frame_str);
                    log_map[timestamp] = {frame, line_num};
                } catch (...) {}
            }
        }
        line_num++;
    }
    return log_map;
}

long get_log_next_frame(const string& log_filename, long current_line_idx) {
    ifstream file(log_filename);
    string line;
    long idx = 0;
    while (getline(file, line)) {
        if (idx == current_line_idx + 1) {
             if (line.length() >= 53) {
                string frame_str = line.substr(44, 9);
                try { return stol(frame_str); } catch (...) { return 0; }
            }
        }
        idx++;
    }
    return 0;
}

bool is_valid_time_format(const string& s) {
    // Matches MM:SS.mmm (00-59 minutes, 00-59 seconds)
    static const regex re("^([0-5][0-9]):([0-5][0-9])\\.[0-9]{3}$");
    return regex_match(s, re);
}

bool is_valid_label_format(const string& s) {
    // Matches a.01, b.2b, etc.
    static const regex re("^[a-z]\\.[a-z0-9]{2}$");
    return regex_match(s, re);
}

string get_label_error_msg(const string& line) {
    return "Error: Invalid label - use a.01, b.2b, etc. First character must be a lowercase letter, followed by a period, then any two lowercase letters or digits. Use a hash symbol before the declaration of the label (ie, when the label is declared after a tempo value) to disable all warp markers with that label (" + line + ").";
}

// --- Main ---

int main(int argc, char* argv[]) {
    // Expected Args: 
    // 1: warpmarkers, 2: settings, 3: audio, 4: sr, 5: frames, 6: md5, 7: log, 8: total_time_ms
    
    if (argc < 7) {
        cerr << "Usage: warptempo_parser <warpmarkers> <settings> <audio_input> <sample_rate> <total_frames> <md5> [log] [total_time_ms]" << endl;
        return 1;
    }

    string wm_file = argv[1];
    string set_file = argv[2];
    string audio_input = argv[3];
    long sample_rate = stol(argv[4]);
    long total_frames = stol(argv[5]);
    string md5 = argv[6];
    string log_file_path = (argc > 7 && string(argv[7]) != "0") ? argv[7] : "";
    string total_time_ms_arg = (argc > 8) ? argv[8] : "";

    if (!file_exists(set_file)) { cerr << "Error: Settings file not found: " << set_file << endl; return 1; }
    Settings settings = load_settings(set_file);
    if (settings.title.empty()) { cerr << "Error: Title required in settings file: " << set_file << endl; return 1; }

    string timemap_file = "." + md5 + "-timemap";

    // [NEW] Tempomap for MIDI Adapter (Time + Multiplier)
    string tempomap_file = "." + md5 + "-timemap-midi";
    ofstream of_tempo(tempomap_file);
    of_tempo << fixed << setprecision(16);
    
    string log_output_file = settings.title + ".log";

    string begin_time_str = "";
    string end_time_str = "";
    string previous_tempo = "1.00";
    vector<string> disabled_labels;
    vector<WarpMarker> markers;
    bool has_zero_time = false;

    ifstream wmf(wm_file);
    if (!wmf.is_open()) { cerr << "Error: Cannot open warpmarkers file: " << wm_file << endl; return 1; }
    
    set<string> defined_labels; // Track labels to distinguish Def vs Ref in Col 2

    // =========================================================
    // [NEW] PRE-SCAN PASS: Find all forward-declared labels
    // =========================================================
    string pre_scan_line;
    while (getline(wmf, pre_scan_line)) {
        if (!pre_scan_line.empty() && (pre_scan_line[0] == ' ' || pre_scan_line[0] == '\t'))
            continue;
        string t_line = trim_str(pre_scan_line);
        if (t_line.empty()) continue;
        
        // Strip text after space (same as main logic)
        size_t first_space = t_line.find(' ');
        if (first_space != string::npos) t_line = t_line.substr(0, first_space);

        // Check for columns
        if (t_line.find('|') == string::npos) continue;

        stringstream ss(t_line);
        string segment;
        vector<string> cols;
        while(getline(ss, segment, '|')) cols.push_back(segment);

        // Check Column 3 for Label Definition
        if (cols.size() > 2 && !cols[2].empty()) {
            string lbl = cols[2];
            // Handle disabled labels (#a.01) by stripping # for the definition check
            if (lbl[0] == '#') lbl = lbl.substr(1);
            
            if (is_valid_label_format(lbl)) {
                defined_labels.insert(lbl);
            }
        }
    }

    // Reset file stream for the main parsing pass
    wmf.clear();
    wmf.seekg(0);
    // =========================================================

    string line;
    while (getline(wmf, line)) {
        // Indented lines are treated as comments (ignored silently).
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) continue;

        string t_line = trim_str(line);
        if (t_line.empty()) continue;

        t_line = trim_str(t_line);
        size_t first_space = t_line.find(' ');
        if (first_space != string::npos) t_line = t_line.substr(0, first_space);

        bool is_begin_line = false;
        bool is_end_line = false;

        // Check for b= / begin_time=
        if (t_line.rfind("begin_time=", 0) == 0) { 
            begin_time_str = t_line.substr(11); 
            is_begin_line = true; 
            t_line = t_line.substr(11); // [FIX] Strip prefix so line can be parsed
        } else if (t_line.rfind("b=", 0) == 0) { 
            begin_time_str = t_line.substr(2); 
            is_begin_line = true; 
            t_line = t_line.substr(2); // [FIX] Strip prefix
        }

        // Check for e= / end_time=
        if (t_line.rfind("end_time=", 0) == 0) { 
            end_time_str = t_line.substr(9); 
            is_end_line = true; 
            t_line = t_line.substr(9); // [FIX] Strip prefix
        } else if (t_line.rfind("e=", 0) == 0) { 
            end_time_str = t_line.substr(2); 
            is_end_line = true; 
            t_line = t_line.substr(2); // [FIX] Strip prefix
        }
        
        // --- BASIC LINE VALIDATION (Bash Equivalent) ---
        size_t pipe_pos = t_line.find('|');
        if (pipe_pos == string::npos) continue;
        // Must be at least 9 chars long before pipe, specific chars at indices
        if (pipe_pos < 9 || t_line[2] != ':' || t_line[5] != '.') continue;

        // --- PARSE COLUMNS ---
        stringstream ss(t_line);
        string segment;
        vector<string> cols;
        while(getline(ss, segment, '|')) cols.push_back(segment);
        if (cols.size() < 2) continue;

        string time_raw = cols[0];
        string tempo_raw = cols[1];
        string label_raw = (cols.size() > 2) ? cols[2] : "";

        // --- 1. HANDLE DISABLED LABELS (#) ---
        // Logic: If label starts with # AND matches format -> Disable & Continue.
        // If it starts with # but fails regex -> Fall through to trigger "Invalid label" error.
        if (!label_raw.empty() && label_raw[0] == '#') {
            string stripped = label_raw.substr(1);
            if (is_valid_label_format(stripped)) {
                disabled_labels.push_back(stripped);
                defined_labels.insert(stripped); // Mark defined so references don't crash parser
                continue; // Skip creating marker
            }
        }

        // --- 2. VALIDATE TIME FORMAT (Bash: Line 24) ---
        string time_initial = time_raw.substr(0, 9);
        if (!is_valid_time_format(time_initial)) {
            cerr << "Error: Invalid time format - use MM:SS.mmm (" << line << ")." << endl;
            return 1;
        }

        if (time_initial == "00:00.000") has_zero_time = true;

        // --- 3. VALIDATE LABEL DEFINITION (Bash: Line 38) ---
        if (!label_raw.empty()) {
            if (!is_valid_label_format(label_raw)) {
                cerr << get_label_error_msg(line) << endl;
                return 1;
            }
        }

        // --- 4. COL 2 LOGIC (Reference vs Tempo) ---
        bool is_tempo_log = (tempo_raw.find(':') != string::npos);
        bool is_tempo_quoted = (tempo_raw == "\"\"\"\"");
        bool is_tempo_numeric = (isdigit(tempo_raw[0]) || tempo_raw[0] == '.');

        if (!is_tempo_log && !is_tempo_quoted && !is_tempo_numeric) {
            // Case: Label Reference
            // VALIDATE REFERENCE FORMAT (Bash: Line 36)
            if (!is_valid_label_format(tempo_raw)) {
                 cerr << get_label_error_msg(line) << endl;
                 return 1;
            }
            // Check existence
            if (defined_labels.find(tempo_raw) == defined_labels.end()) {
                 cerr << "Error: Undefined label reference '" << tempo_raw << "' in line: " << line << endl;
                 return 1;
            }
            if (label_raw.empty()) label_raw = tempo_raw;
        }

        if (!label_raw.empty()) defined_labels.insert(label_raw);
        
        // --- 5. REDUNDANCY CHECK (Skip if tempo == prev & no label) ---
        
        string effective_tempo = tempo_raw;
        if (is_tempo_quoted) effective_tempo = previous_tempo;

        if ((is_tempo_numeric || is_tempo_quoted) && effective_tempo == previous_tempo && time_initial != "00:00.000") {
             // FIX: Only skip if the CURRENT line also has NO label. 
             // If this line defines a label (e.g. a.01), we must keep it regardless of tempo redundancy.
             if (label_raw.empty()) {
                 if (!markers.empty()) {
                     const WarpMarker& prev = markers.back();
                     
                     // Condition: 
                     // 1. Previous marker was numeric 
                     // 2. Previous marker had NO label definition
                     // 3. Current line is NOT defining a start/end trim point
                     if (prev.is_numeric && prev.label_def.empty() && !is_begin_line && !is_end_line) {
                         continue; // SKIP REDUNDANT MARKER
                     }
                 }
             }
        }
        
        // --- PARSE TIMESTAMP (Offsets) ---
        double final_time_sec;
        if (parse_timestamp(time_initial) < 0) { 
             cerr << "Error: Invalid time format - use MM:SS.mmm (" << line << ")." << endl; 
             return 1; 
        }
        
        if (time_raw.length() > 9) {
            double base_sec = parse_timestamp(time_initial);
            double offset = eval_math_string(time_raw.substr(9));
            final_time_sec = base_sec + offset;
        } else {
            final_time_sec = parse_timestamp(time_initial);
        }

        WarpMarker wm;
        wm.original_line = line;
        wm.time_str = time_raw; 
        wm.time_seconds = final_time_sec;
        wm.label_def = label_raw;
        
        if (!label_raw.empty()) {
            size_t dot_pos = label_raw.find('.');
            if (dot_pos != string::npos) wm.label_prefix = label_raw.substr(0, dot_pos);
            else wm.label_prefix = label_raw;
        }

        if (is_tempo_log) {
            if (log_file_path.empty()) { 
                cerr << "Error: Log time found in second column but no log file provided (" << line << ")." << endl; 
                return 1; 
            }
            if (time_initial == "00:00.000") { 
                cerr << "Error: Log time cannot be used in second column in first time (" << line << ")." << endl; 
                return 1; 
            }
            wm.is_log_sync = true;
            wm.tempo_str = tempo_raw; 
        } else if (is_tempo_quoted) {
            wm.tempo_str = previous_tempo;
            wm.is_numeric = true; 
        } else if (is_tempo_numeric) {
            wm.tempo_str = tempo_raw;
            wm.is_numeric = true;
            previous_tempo = tempo_raw;
        } else {
            wm.tempo_str = tempo_raw;
            wm.is_label_ref = true;
        }
        
        markers.push_back(wm);
    }
    wmf.close();

    if (!has_zero_time) { cerr << "Error: First time must be 00:00.000 in file: " << wm_file << endl; return 1; }

    if (!disabled_labels.empty()) {
        vector<WarpMarker> filtered;
        for (const auto& m : markers) {
            bool disabled = false;
            for (const auto& dl : disabled_labels) {
                if (m.tempo_str == dl) { disabled = true; break; }
            }
            if (!disabled) filtered.push_back(m);
        }
        markers = filtered;
    }

    // --- 2. Handle Trim ---
    if (!begin_time_str.empty() || !end_time_str.empty()) {
        // CLEANUP: Remove everything after the first pipe '|'
        if (begin_time_str.find('|') != string::npos) {
            begin_time_str = begin_time_str.substr(0, begin_time_str.find('|'));
        }
        if (end_time_str.find('|') != string::npos) {
            end_time_str = end_time_str.substr(0, end_time_str.find('|'));
        }

        // Also strip the 'b=' or 'e=' prefixes if your parser included them
        if (begin_time_str.rfind("b=", 0) == 0) begin_time_str.erase(0, 2);
        if (end_time_str.rfind("e=", 0) == 0) end_time_str.erase(0, 2);

        if (begin_time_str == "00:00.000") { cerr << "Error: Begin time cannot be 00:00.000. Found: " << begin_time_str << endl; return 1; }
        
        string trim_out = "." + md5 + "-trimmed.wav";
        string cmd = "sox \"" + audio_input + "\" -b 32 -e float " + trim_out + " trim " + 
                     (begin_time_str.empty() ? "0" : begin_time_str);
        
        if (!end_time_str.empty()) cmd += " =" + end_time_str;
        
        int ret = system(cmd.c_str());
        if (ret != 0) { cerr << "Error: Sox trim failed. Command: " << cmd << endl; return 1; }
        audio_input = trim_out;
    }

    // --- 3. Pass 1: Label Deltas ---
    map<string, double> label_deltas; 
    map<string, string> label_tempos; 

    double prev_src_frame = 0.0; // Changed to double
    double prev_tgt_frame = 0.0;
    
    // [NEW] Track the last used multiplier
    double last_valid_multiplier = 1.0;

    for (size_t i = 0; i < markers.size(); ++i) {
        double src_frame; // Changed to double
        if (i + 1 < markers.size()) {
            // Keep full precision, do not round yet
            src_frame = markers[i+1].time_seconds * sample_rate;
        } else {
            src_frame = (double)total_frames;
        }

        if (src_frame > (double)total_frames) { cerr << "Error: Invalid time > file length. Time: " << markers[i].time_str << " (Frame: " << fixed << setprecision(2) << src_frame << " > Total: " << total_frames << ")" << endl; return 1; }
        // Use epsilon or small threshold for "distance <= 0" check if needed, but < 1.0 is safe for frames
        if (src_frame - prev_src_frame < 1.0) { cerr << "Error: Invalid time - distance <= 0. Time: " << markers[i].time_str << " (Current: " << src_frame << " <= Prev: " << prev_src_frame << ")" << endl; return 1; }

        markers[i].source_frame = src_frame;
        double current_tgt = prev_tgt_frame;
        
        if (markers[i].is_numeric || (!markers[i].is_log_sync && !markers[i].is_label_ref)) {
             if (!markers[i].is_label_ref && !markers[i].is_log_sync) {
                string dummy;
                double tempo_val = parse_tempo_math(markers[i].tempo_str, dummy);
                
                // --- BASH EQUIVALENT ERROR CHECKS ---
                if (tempo_val > 9.99) {
                    cerr << "Error: Tempo greater than 9.99 (" << markers[i].original_line << ")." << endl;
                    return 1;
                }
                if (tempo_val <= 0.0) {
                    cerr << "Error: Tempo less than or equal to 0 (" << markers[i].original_line << ")." << endl;
                    return 1;
                }
                // ------------------------------------

                double delta_src = src_frame - prev_src_frame; 
                double delta_tgt = delta_src / (tempo_val * settings.scale);
                current_tgt += delta_tgt;

                if (!markers[i].label_def.empty()) {
                    string lbl = markers[i].label_def;
                    if (label_deltas.count(lbl)) { cerr << "Error: Label already exists: " << lbl << " in line: " << markers[i].original_line << endl; return 1; }
                    label_deltas[lbl] = delta_tgt;
                    label_tempos[lbl] = markers[i].tempo_str;
                }
             }
        }
        
        if (markers[i].is_numeric) prev_tgt_frame = current_tgt;
        prev_src_frame = src_frame;
    }

    // --- 4. Pass 2: Generate Timemap & Logs ---
    ofstream of_tm(timemap_file);
    
    // [NEW] Explicitly write the 00:00.000 point to the maps
    // Since we validated has_zero_time earlier, markers[0] is guaranteed to be 0.0
    of_tm << "0 0" << endl;
    
    ofstream of_log(log_output_file);
    map<string, pair<long, long>> log_ref_map;
    if (!log_file_path.empty()) log_ref_map = load_log_file(log_file_path);

    prev_src_frame = 0.0;
    prev_tgt_frame = 0.0;
    
    // Formatting helper
    // FIX: Changed p_src from 'long' to 'double' to prevent implicit truncation
    auto print_log_line = [&](string left_part, double p_src, double p_tgt) {
        int padding = 34 - (int)left_part.length();
        if (padding < 0) padding = 0;
        
        of_log << left_part << string(padding, ' ')
               << setfill('0') << setw(9) << llrint(p_src) << " " // FIX: Use llrint() here
               << setfill('0') << setw(9) << llrint(p_tgt) << " "
               << format_timestamp(p_tgt/sample_rate) << endl;
    };

    for (size_t i = 0; i < markers.size(); ++i) {
        double src_frame = markers[i].source_frame; // Now double
        double target_frame = 0;
        string left_part;
        
        string display_time = format_timestamp(markers[i].time_seconds);

        if (markers[i].is_log_sync) {
            string log_time = markers[i].tempo_str;
            if (log_ref_map.find(log_time) == log_ref_map.end()) { cerr << "Error: Time not found in log: " << log_time << " in line: " << markers[i].original_line << endl; return 1; }
            
            long curr_ref_frame = log_ref_map[log_time].first;
            long line_idx = log_ref_map[log_time].second;
            long next_ref_frame = get_log_next_frame(log_file_path, line_idx);
            
            double delta = (double)(next_ref_frame - curr_ref_frame);
            target_frame = prev_tgt_frame + delta;
            
            double input_delta = (double)(src_frame - prev_src_frame);
            double exact_tempo = input_delta / (delta * settings.scale);
            double base_tempo = rint(exact_tempo * 100.0) / 100.0;
            double multiplier = (base_tempo != 0) ? (exact_tempo / base_tempo) : 0;
            
            stringstream ss_approx;
            ss_approx << " ~=" << fixed << setprecision(2) << base_tempo << "*" << setprecision(4) << multiplier;
            
            left_part = display_time + "|" + log_time + ss_approx.str();
            print_log_line(left_part, prev_src_frame, prev_tgt_frame);

        } else if (markers[i].is_numeric) {
            string fmt_tempo;
            double tempo_val = parse_tempo_math(markers[i].tempo_str, fmt_tempo);
            double delta_src = (double)(src_frame - prev_src_frame);
            target_frame = prev_tgt_frame + (delta_src / (tempo_val * settings.scale));
            
            stringstream ss;
            ss << fmt_tempo;
            if (!markers[i].label_def.empty()) ss << "|" << markers[i].label_def;
            
            left_part = display_time + "|" + ss.str();
            print_log_line(left_part, prev_src_frame, prev_tgt_frame);

        } else if (markers[i].is_label_ref) {
            string lbl = markers[i].tempo_str;
            if (label_deltas.find(lbl) == label_deltas.end()) { cerr << "Error: Label does not exist: " << lbl << " referenced in line: " << markers[i].original_line << endl; return 1; }
            
            double lbl_delta = label_deltas[lbl];
            string lbl_tempo_str = label_tempos[lbl];
            
            target_frame = prev_tgt_frame + lbl_delta;
            
            size_t star_idx = lbl_tempo_str.find('*');
            string base_part_raw = (star_idx == string::npos) ? lbl_tempo_str : lbl_tempo_str.substr(0, star_idx);
            double base_val = eval_math_string(base_part_raw);
            
            stringstream ss_base;
            ss_base << fixed << setprecision(2) << base_val;
            string base_display = ss_base.str();

            double unadj = prev_tgt_frame + ((src_frame - prev_src_frame)/(base_val * settings.scale));
            double multiplier = (unadj - prev_tgt_frame) / (target_frame - prev_tgt_frame);
            
            stringstream ss_approx;
            ss_approx << lbl << " ~=";
            
            if (star_idx != string::npos) {
                string scale_orig_str = lbl_tempo_str.substr(star_idx+1);
                double s_val = stod(scale_orig_str);
                double final_scale = s_val * multiplier;
                ss_approx << base_display << "*" << fixed << setprecision(4) << final_scale;
            } else {
                ss_approx << base_display << "*" << fixed << setprecision(4) << multiplier;
            }
            
            // Validate final multiplier
            double final_multiplier;
            if (star_idx != string::npos) {
                string scale_orig_str = lbl_tempo_str.substr(star_idx+1);
                double s_val = stold(scale_orig_str);
                final_multiplier = s_val * multiplier;
            } else {
                final_multiplier = multiplier;
            }
            
            if (final_multiplier > 9.9999) {
                cerr << "Error: Label tempo final multiplier greater than 9.9999 (" 
                     << markers[i].original_line << ")." << endl;
                return 1;
            }

            left_part = display_time + "|" + ss_approx.str();
            print_log_line(left_part, prev_src_frame, prev_tgt_frame);
        }

        // [NEW] Calculate and write effective multiplier for MIDI
        double seg_src_dur = src_frame - prev_src_frame;
        double seg_tgt_dur = target_frame - prev_tgt_frame;
        
        if (seg_tgt_dur > 0.000001) {
            double effective_multiplier = seg_src_dur / seg_tgt_dur;
            
            // [NEW] Update tracker
            last_valid_multiplier = effective_multiplier;
            
            // [FIXED] Use TARGET time (Output Time), not Source Time
            // The MIDI events must align with the warped audio timeline.
            double seg_start_time = prev_tgt_frame / (double)sample_rate;
            of_tempo << seg_start_time << " " << effective_multiplier << endl;
        }

        // 1. Standard Timemap
        of_tm << llrint(src_frame) << " " << llrint(target_frame) << endl;
        
        prev_src_frame = src_frame;
        prev_tgt_frame = target_frame;
    }
    
    // [FIX] Write the final end time to the MIDI tempomap
    // This provides the "stop" point so the adapter can calculate the duration of the last segment.
    double final_tgt_sec = prev_tgt_frame / (double)sample_rate;
    of_tempo << final_tgt_sec << " " << last_valid_multiplier << endl;

    string end_time_display;
    if (!end_time_str.empty()) {
        end_time_display = end_time_str;
    } else if (!total_time_ms_arg.empty()) {
        end_time_display = total_time_ms_arg;
    } else {
        double total_sec = (double)total_frames / sample_rate;
        end_time_display = format_timestamp(total_sec);
    }
    
    print_log_line(end_time_display, prev_src_frame, prev_tgt_frame);

    of_tm.close();
    of_tempo.close(); // [NEW]
    of_log.close();

    // --- 5. Post-Process Trim on Timemap ---
    if (!begin_time_str.empty()) {
        long begin_frame = (long)llrint(parse_timestamp(begin_time_str) * sample_rate);
        long end_frame = (!end_time_str.empty()) ? 
                         (long)llrint(parse_timestamp(end_time_str) * sample_rate) : total_frames;
        
        // [Existing Standard Timemap Trim Logic...]
        ifstream tm_in(timemap_file);
        string tm_trimmed_file = timemap_file + "-trimmed";
        ofstream tm_out(tm_trimmed_file);
        
        long begin_target_frame = -1;
        long end_target_frame = -1; // [FIX 1: Declare this]
        
        long s_f, t_f;
        while (tm_in >> s_f >> t_f) {
            if (s_f >= begin_frame && s_f <= end_frame) {
                if (begin_target_frame == -1) begin_target_frame = t_f;
                
                end_target_frame = t_f; // [FIX 2: Update this every iteration]
                
                tm_out << (s_f - begin_frame) << " " << (t_f - begin_target_frame) << endl;
            }
        }
        tm_in.close();
        tm_out.close();
        
        remove(timemap_file.c_str());
        rename(tm_trimmed_file.c_str(), timemap_file.c_str());
        
        // [FIXED] Tempomap Trim (Using Calculated Target Times)
        if (begin_target_frame != -1) {
            double begin_target_sec = begin_target_frame / (double)sample_rate;
            double end_target_sec = (end_target_frame != -1) ? (end_target_frame / (double)sample_rate) : (total_frames / (double)sample_rate);

            ifstream tp_in(tempomap_file);
            string tp_trimmed_file = tempomap_file + "-trimmed";
            ofstream tp_out(tp_trimmed_file);
            tp_out << fixed << setprecision(16);
            
            double t_sec, m_val;
            double active_multiplier = 1.0; // Default state
            bool start_point_written = false;
            
            while (tp_in >> t_sec >> m_val) {
                // 1. Track the tempo active JUST BEFORE our cut point
                if (t_sec < begin_target_sec) {
                    active_multiplier = m_val;
                } 
                // 2. Process points inside the trim range
                else if (t_sec <= end_target_sec) {
                    
                    // If the first point in range is NOT at the very start (gap),
                    // we must backfill 0.0 with the last known active multiplier.
                    if (!start_point_written) {
                        if (t_sec > begin_target_sec) {
                            tp_out << "0.0 " << active_multiplier << endl;
                        }
                        start_point_written = true;
                    }
                    
                    // Shift time by start offset
                    tp_out << (t_sec - begin_target_sec) << " " << m_val << endl;
                    
                    // Update active state (in case we have multiple points)
                    active_multiplier = m_val;
                }
            }
            
            // 3. Handle Empty Range / Constant Tempo Segment
            // If no points fell within the range (e.g. trimming the middle of a long segment),
            // we must explicitly write the active multiplier at 0.0.
            if (!start_point_written) {
                 tp_out << "0.0 " << active_multiplier << endl;
            }
            
            tp_in.close();
            tp_out.close();
            
            remove(tempomap_file.c_str());
            rename(tp_trimmed_file.c_str(), tempomap_file.c_str());
        }

        cout << "AUDIO_INPUT_UPDATED=." << md5 << "-trimmed.wav" << endl;
    }

    return 0;
}
