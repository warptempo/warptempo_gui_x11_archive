#include "gui_markers.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string trim_ws(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

bool is_valid_time_format(const std::string& s) {
    static const std::regex re("^([0-5][0-9]):([0-5][0-9])\\.[0-9]{3}$");
    return std::regex_match(s, re);
}

bool is_valid_label_format(const std::string& s) {
    static const std::regex re("^[a-z]\\.[a-z0-9]{2}$");
    return std::regex_match(s, re);
}

// Parse "MM:SS.mmm" to seconds. Caller should validate format first.
double parse_timestamp(const std::string& s) {
    const int min    = std::stoi(s.substr(0, 2));
    const double sec = std::stod(s.substr(3));
    return min * 60.0 + sec;
}

// Evaluate a sum-of-signed-decimals like "+0.003", "-0.126",
// "1.23+0.05-0.03". Whitespace is stripped. Non-numeric trailing tokens end
// evaluation silently — the parser validates format upstream, so this is
// best-effort for well-formed input only.
double eval_math_string(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    }
    double total = 0.0;
    char op = '+';
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 0;
        while (i + len < s.size() &&
               (std::isdigit(static_cast<unsigned char>(s[i + len])) ||
                s[i + len] == '.')) {
            ++len;
        }
        if (len > 0) {
            const double v = std::stod(s.substr(i, len));
            if (op == '+') total += v;
            else if (op == '-') total -= v;
            i += len;
        } else if (s[i] == '+' || s[i] == '-') {
            op = s[i];
            ++i;
        } else {
            break;
        }
    }
    return total;
}

bool starts_with(const std::string& s, const char* pfx) {
    const size_t n = std::strlen(pfx);
    return s.size() >= n && s.compare(0, n, pfx) == 0;
}

std::vector<std::string> split_pipe(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '|')) out.push_back(seg);
    // getline on a string ending with '|' won't emit the empty trailing
    // column; for the warpmarkers format every non-empty column matters,
    // and a trailing empty col has no meaning, so we leave this as-is.
    return out;
}

// Strip b=/begin_time=/e=/end_time= prefixes in place and flag which were
// present. The full-word forms take priority (checked first).
void strip_trim_prefix(std::string& line, bool& is_begin, bool& is_end) {
    if (starts_with(line, "begin_time=")) {
        is_begin = true;
        line.erase(0, 11);
    } else if (starts_with(line, "b=")) {
        is_begin = true;
        line.erase(0, 2);
    }
    if (starts_with(line, "end_time=")) {
        is_end = true;
        line.erase(0, 9);
    } else if (starts_with(line, "e=")) {
        is_end = true;
        line.erase(0, 2);
    }
}

bool is_indented_raw(const std::string& raw) {
    return !raw.empty() && (raw[0] == ' ' || raw[0] == '\t');
}

void strip_bom(std::string& s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

std::string format_timestamp(double seconds) {
    if (seconds < 0) seconds = 0;
    long total_ms = static_cast<long>(std::llround(seconds * 1000.0));
    const long m  = total_ms / 60000;
    total_ms     -= m * 60000;
    const long s  = total_ms / 1000;
    const long ms = total_ms - s * 1000;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld.%03ld", m, s, ms);
    return buf;
}

} // namespace

bool GuiMarkers::load(const std::string& path) {
    markers_.clear();
    errors_.clear();
    had_nonstandard_content_ = false;

    std::ifstream f(path);
    if (!f.is_open()) {
        errors_.push_back({0, "cannot open file: " + path});
        return false;
    }

    std::vector<std::string> raw_lines;
    {
        std::string line;
        while (std::getline(f, line)) raw_lines.push_back(std::move(line));
    }
    if (!raw_lines.empty()) strip_bom(raw_lines.front());

    // ----- Pass 1: gather the set of defined labels -----------------------
    //
    // A label reference is only valid if its label appears as a def somewhere
    // in the file. Disabled-def lines (`#a.xx`) still count as defs; the
    // cascade is resolved at render time from the defining marker.

    std::set<std::string> defined;

    for (const std::string& raw : raw_lines) {
        if (is_indented_raw(raw)) continue;
        std::string t = trim_ws(raw);
        if (t.empty()) continue;
        if (t[0] == '#') continue;

        bool ib = false, ie = false;
        strip_trim_prefix(t, ib, ie);

        const size_t sp = t.find(' ');
        if (sp != std::string::npos) t = t.substr(0, sp);

        if (t.find('|') == std::string::npos) continue;

        const auto cols = split_pipe(t);
        if (cols.size() > 2 && !cols[2].empty()) {
            std::string lbl = cols[2];
            if (!lbl.empty() && lbl[0] == '#') lbl.erase(0, 1);
            if (is_valid_label_format(lbl)) defined.insert(lbl);
        }
    }

    // ----- Pass 2: build markers ------------------------------------------

    double      previous_tempo_base  = 1.0;
    std::string previous_tempo_scale;
    bool have_prev_numeric = false;
    bool parse_ok          = true;
    bool first_marker_seen = false;
    double last_time       = -1.0;

    for (size_t idx = 0; idx < raw_lines.size(); ++idx) {
        const int line_number = static_cast<int>(idx + 1);
        const std::string& raw = raw_lines[idx];
        if (is_indented_raw(raw)) {
            had_nonstandard_content_ = true;
            continue;
        }
        std::string t = trim_ws(raw);
        if (t.empty()) {
            if (!raw.empty()) had_nonstandard_content_ = true;
            continue;
        }
        if (t[0] == '#') {
            had_nonstandard_content_ = true;
            continue;
        }

        bool is_begin = false, is_end = false;
        strip_trim_prefix(t, is_begin, is_end);

        const size_t sp = t.find(' ');
        if (sp != std::string::npos) {
            had_nonstandard_content_ = true;
            t = t.substr(0, sp);
        }

        if (t.find('|') == std::string::npos) continue;

        const auto cols = split_pipe(t);
        if (cols.size() < 2) {
            errors_.push_back({line_number,
                "need at least time|tempo columns"});
            parse_ok = false;
            continue;
        }

        const std::string& time_raw  = cols[0];
        const std::string& tempo_raw = cols[1];
        const std::string  label_raw = (cols.size() > 2) ? cols[2]
                                                         : std::string();

        // Validate time format.
        if (time_raw.size() < 9 ||
            !is_valid_time_format(time_raw.substr(0, 9))) {
            errors_.push_back({line_number,
                "invalid time format: " + time_raw});
            parse_ok = false;
            continue;
        }

        const std::string time_initial = time_raw.substr(0, 9);
        double final_time = parse_timestamp(time_initial);
        if (time_raw.size() > 9) {
            final_time += eval_math_string(time_raw.substr(9));
        }

        // First emitted marker must be exactly 00:00.000.
        if (!first_marker_seen) {
            if (time_initial != "00:00.000") {
                errors_.push_back({line_number,
                    "first marker must be 00:00.000 (got " + time_initial +
                    ")"});
                parse_ok = false;
                first_marker_seen = true;
                continue;
            }
            first_marker_seen = true;
        }

        // Monotonic time check.
        if (last_time >= 0.0 && final_time <= last_time) {
            errors_.push_back({line_number,
                "time not strictly increasing: " + time_initial});
            parse_ok = false;
            continue;
        }

        GuiMarker m;
        m.time_seconds  = final_time;
        m.is_begin_time = is_begin;
        m.is_end_time   = is_end;

        const bool tempo_quoted  = (tempo_raw == "\"\"\"\"");
        const bool tempo_numeric = !tempo_raw.empty() &&
            (std::isdigit(static_cast<unsigned char>(tempo_raw[0])) ||
             tempo_raw[0] == '.');

        if (tempo_quoted) {
            if (!have_prev_numeric) {
                errors_.push_back({line_number,
                    "ditto tempo \"\"\"\" has no preceding numeric tempo"});
                parse_ok = false;
                continue;
            }
            had_nonstandard_content_ = true;
            // Inherit marker: the tempo is resolved at render time by
            // walking backward. The cached numeric is preserved here so
            // toggling to "owned" in the editor restores a sensible value.
            m.tempo_inherits = true;
            m.tempo_base     = previous_tempo_base;
            m.tempo_scale    = previous_tempo_scale;
        } else if (tempo_numeric) {
            const size_t star = tempo_raw.find('*');
            const std::string base_part = (star == std::string::npos)
                ? tempo_raw : tempo_raw.substr(0, star);
            m.tempo_inherits = false;
            m.tempo_base     = eval_math_string(base_part);
            m.tempo_scale    = (star == std::string::npos)
                ? std::string() : tempo_raw.substr(star + 1);
            previous_tempo_base    = m.tempo_base;
            previous_tempo_scale   = m.tempo_scale;
            have_prev_numeric      = true;
        } else {
            if (!is_valid_label_format(tempo_raw)) {
                errors_.push_back({line_number,
                    "invalid tempo or label reference: " + tempo_raw});
                parse_ok = false;
                continue;
            }
            if (defined.count(tempo_raw) == 0) {
                errors_.push_back({line_number,
                    "reference to undefined label: " + tempo_raw});
                parse_ok = false;
                continue;
            }
            m.label_ref      = tempo_raw;
            m.tempo_inherits = false; // irrelevant for refs but keep tidy
            m.tempo_base     = 0.0;
            m.tempo_scale.clear();
        }

        // Column-3 label definition. Leading `#` means the definition is
        // disabled; the cascade to references is resolved at render time.
        if (!label_raw.empty()) {
            std::string def = label_raw;
            bool def_disabled = false;
            if (def[0] == '#') {
                def_disabled = true;
                def.erase(0, 1);
            }
            if (!is_valid_label_format(def)) {
                errors_.push_back({line_number,
                    "invalid label definition: " + label_raw});
                parse_ok = false;
                continue;
            }
            if (!m.label_ref.empty()) {
                errors_.push_back({line_number,
                    "marker cannot be both a label reference and a label "
                    "definition"});
                parse_ok = false;
                continue;
            }
            m.label_def = def;
            m.disabled  = def_disabled;
        }

        last_time = m.time_seconds;
        markers_.push_back(std::move(m));
    }

    if (!first_marker_seen) {
        errors_.push_back({0, "file contains no markers"});
        parse_ok = false;
    }

    if (!parse_ok) {
        markers_.clear();
        return false;
    }
    return true;
}

bool GuiMarkers::save(const std::string& path) const {
    std::ostringstream out;
    for (const auto& m : markers_) {
        if (m.is_begin_time) out << "b=";
        if (m.is_end_time)   out << "e=";
        out << format_timestamp(m.time_seconds) << '|';

        // Defensive guards against invalid in-memory combinations.
        const bool both_def_and_ref =
            !m.label_def.empty() && !m.label_ref.empty();
        if (both_def_and_ref) {
            std::fprintf(stderr,
                "warptempo_gui: marker at %.3fs has both label_ref and "
                "label_def; emitting as reference\n",
                m.time_seconds);
        }

        // Tempo column:
        //   label reference     → label text
        //   tempo_inherits=true → """"
        //   otherwise           → "%.2f" with optional *scale
        if (!m.label_ref.empty()) {
            out << m.label_ref;
        } else if (m.tempo_inherits) {
            out << "\"\"\"\"";
        } else {
            char tbuf[32];
            std::snprintf(tbuf, sizeof(tbuf), "%.2f", m.tempo_base);
            out << tbuf;
            if (!m.tempo_scale.empty()) out << '*' << m.tempo_scale;
        }

        // Label column (only for definitions, not references).
        if (!m.label_def.empty() && m.label_ref.empty()) {
            out << '|';
            if (m.disabled) out << '#';
            out << m.label_def;
        }

        out << '\n';
    }
    const std::string data = out.str();

    // Preserve existing mode or fall back to 0644.
    mode_t mode = 0644;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        mode = st.st_mode & 07777;
    }

    const std::string tmp_path = path + ".tmp";

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;

    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = ::write(fd, data.data() + written,
                                  data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp_path.c_str());
            return false;
        }
        written += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tmp_path.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    // open() applies umask; reassert intended mode explicitly.
    ::chmod(tmp_path.c_str(), mode);

    if (::rename(tmp_path.c_str(), path.c_str()) != 0) {
        ::unlink(tmp_path.c_str());
        return false;
    }
    return true;
}

int GuiMarkers::insert_marker(GuiMarker m) {
    auto it = std::lower_bound(
        markers_.begin(), markers_.end(), m.time_seconds,
        [](const GuiMarker& a, double t) { return a.time_seconds < t; });
    const int idx = static_cast<int>(it - markers_.begin());
    markers_.insert(it, std::move(m));
    return idx;
}

void GuiMarkers::remove_marker(int index) {
    if (index < 0 || index >= static_cast<int>(markers_.size())) return;
    markers_.erase(markers_.begin() + index);
}
