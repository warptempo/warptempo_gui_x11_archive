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
#include <map>
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

// New-format tempo: exactly one integer digit, dot, two decimal digits.
bool is_valid_tempo_format(const std::string& s) {
    static const std::regex re("^[0-9]\\.[0-9]{2}$");
    return std::regex_match(s, re);
}

// New-format scale: exactly one integer digit, dot, four decimal digits.
bool is_valid_scale_format(const std::string& s) {
    static const std::regex re("^[0-9]\\.[0-9]{4}$");
    return std::regex_match(s, re);
}

// Parse "MM:SS.mmm" to seconds. Caller validates format first.
double parse_timestamp(const std::string& s) {
    const int min    = std::stoi(s.substr(0, 2));
    const double sec = std::stod(s.substr(3));
    return min * 60.0 + sec;
}

// Legacy-only: evaluate a sum-of-signed-decimals like "1.23+0.05-0.03".
// Whitespace is stripped. Used on legacy load only; new format rejects
// arithmetic in the tempo column.
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
    return out;
}

// Strip b=/begin_time=/e=/end_time= prefixes in place. The full-word forms
// take priority (checked first).
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

// Normalize a scale string to canonical N.NNNN form. Used by save() to
// re-emit legacy-loaded scales (which may have had any precision) in the
// new format. If the string can't be parsed, returns it unchanged so the
// data isn't lost — but the next reload will reject it.
std::string normalize_scale_string(const std::string& s) {
    if (s.empty()) return s;
    try {
        const double v = std::stod(s);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", v);
        return buf;
    } catch (...) {
        return s;
    }
}

// Parse a new-format payload (the part after the pipe) into a partly-
// populated GuiMarker — sets tempo/label fields only. Cross-marker checks
// (label_ref existence, label_def uniqueness) are the caller's job.
//
// On success, returns true and the GuiMarker carries the parsed payload.
// On failure, returns false and `error_out` is set.
//
// `disabled_in` is plumbed through so the caller can attach a metadata
// flag (`#`) that came from outside the payload. Time and trim flags
// are not handled here.
bool parse_new_payload(const std::string& payload,
                       GuiMarker& m,
                       std::string& error_out) {
    if (payload.empty()) {
        error_out = "empty payload";
        return false;
    }
    if (payload.find('(') != std::string::npos ||
        payload.find(')') != std::string::npos) {
        error_out = "parens are not valid in the new format: " + payload;
        return false;
    }
    if (payload.find(' ') != std::string::npos ||
        payload.find('\t') != std::string::npos) {
        error_out = "whitespace is not valid in the new format: " + payload;
        return false;
    }

    // Split on `:` — at most one colon expected.
    const size_t colon = payload.find(':');
    if (colon != std::string::npos &&
        payload.find(':', colon + 1) != std::string::npos) {
        error_out = "too many colons in payload: " + payload;
        return false;
    }

    if (colon == std::string::npos) {
        // Single part: tempo, pass, or label_ref.
        if (payload == "pass") {
            m.tempo_inherits = true;
            m.tempo_base     = 1.0;
            m.tempo_scale    = "1.0000";
            return true;
        }
        if (is_valid_label_format(payload)) {
            m.label_ref      = payload;
            m.tempo_inherits = false;
            m.tempo_base     = 0.0;
            m.tempo_scale.clear();
            return true;
        }
        // Tempo (numeric, with optional *scale).
        const size_t star = payload.find('*');
        const std::string tempo_part = (star == std::string::npos)
            ? payload : payload.substr(0, star);
        const std::string scale_part = (star == std::string::npos)
            ? std::string() : payload.substr(star + 1);
        if (!is_valid_tempo_format(tempo_part)) {
            error_out = "tempo must be N.NN format: " + tempo_part;
            return false;
        }
        if (star != std::string::npos && !is_valid_scale_format(scale_part)) {
            error_out = "scale must be N.NNNN format: " + scale_part;
            return false;
        }
        m.tempo_inherits = false;
        m.tempo_base     = std::stod(tempo_part);
        m.tempo_scale    = scale_part;
        return true;
    }

    // Two parts: (TEMPO[*SCALE] | pass) : label_def. The three GuiMarker
    // state axes (tempo source, label relationship, disabled) are
    // independent; `pass:LABEL` is the inheriting + label_def combination.
    const std::string tempo_with_scale = payload.substr(0, colon);
    const std::string label_def        = payload.substr(colon + 1);

    if (tempo_with_scale.empty()) {
        error_out = "missing tempo before colon";
        return false;
    }
    if (!is_valid_label_format(label_def)) {
        error_out = "invalid label definition: " + label_def;
        return false;
    }
    if (tempo_with_scale == "pass") {
        m.tempo_inherits = true;
        m.tempo_base     = 1.0;
        m.tempo_scale    = "1.0000";
        m.label_def      = label_def;
        return true;
    }
    const size_t star = tempo_with_scale.find('*');
    const std::string tempo_part = (star == std::string::npos)
        ? tempo_with_scale : tempo_with_scale.substr(0, star);
    const std::string scale_part = (star == std::string::npos)
        ? std::string() : tempo_with_scale.substr(star + 1);
    if (!is_valid_tempo_format(tempo_part)) {
        error_out = "tempo must be N.NN format: " + tempo_part;
        return false;
    }
    if (star != std::string::npos && !is_valid_scale_format(scale_part)) {
        error_out = "scale must be N.NNNN format: " + scale_part;
        return false;
    }
    m.tempo_inherits = false;
    m.tempo_base     = std::stod(tempo_part);
    m.tempo_scale    = scale_part;
    m.label_def      = label_def;
    return true;
}

} // namespace

// --- public single-line parser ---------------------------------------------
//
// Used by the GUI text editor to validate a single canonical line during
// commit. Cross-marker checks (label_ref existence, label_def uniqueness)
// are the caller's responsibility — pass `known_label_defs` with the names
// of every other marker's label_def so the editor can reject refs to
// undefined labels. Time monotonicity isn't checked (single line).
//
// Defined in the header as a free function for editor reuse.
namespace gui_markers_internal {

bool parse_single_canonical_line(
    const std::string& raw_line,
    GuiMarker& out,
    std::string* error_out) {

    auto fail = [&](const char* msg) {
        if (error_out) *error_out = msg;
        return false;
    };
    auto fail_s = [&](const std::string& msg) {
        if (error_out) *error_out = msg;
        return false;
    };

    std::string t = raw_line;
    if (t.empty()) return fail("empty line");

    // No whitespace anywhere on the line.
    for (char c : t) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            return fail("no whitespace allowed in canonical line");
        }
    }

    out = GuiMarker{};

    // [b=|e=]?  [#]?  MM:SS.SSS  |  PAYLOAD
    bool ib = false, ie = false;
    strip_trim_prefix(t, ib, ie);
    if (ib && ie) return fail("cannot have both b= and e=");
    out.is_begin_time = ib;
    out.is_end_time   = ie;

    if (!t.empty() && t[0] == '#') {
        out.disabled = true;
        t.erase(0, 1);
    }

    if (t.size() < 9 || !is_valid_time_format(t.substr(0, 9))) {
        return fail_s("invalid time format: " + t.substr(0, std::min<size_t>(9, t.size())));
    }
    out.time_seconds = parse_timestamp(t.substr(0, 9));
    t.erase(0, 9);

    if (t.empty() || t[0] != '|') {
        return fail("expected '|' after timestamp");
    }
    t.erase(0, 1);

    return parse_new_payload(t, out, *error_out);
}

} // namespace gui_markers_internal

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

    // ----- File-level legacy detection ------------------------------------
    //
    // A file is legacy if any line contains the `""""` ditto sentinel.
    // Mixed-format files are not handled — the first save migrates the
    // entire file in one shot.
    bool is_legacy_file = false;
    for (const auto& raw : raw_lines) {
        if (raw.find("\"\"\"\"") != std::string::npos) {
            is_legacy_file = true;
            break;
        }
    }

    // ----- Pass 1: gather defined labels ---------------------------------
    //
    // For both formats. A label reference is only valid if its label
    // appears as a def somewhere in the file. Disabled defs still count.

    std::set<std::string>            defined;
    std::map<std::string, int>       first_def_line;

    for (size_t idx = 0; idx < raw_lines.size(); ++idx) {
        const int line_number = static_cast<int>(idx + 1);
        const std::string& raw = raw_lines[idx];
        if (is_indented_raw(raw)) continue;
        std::string t = trim_ws(raw);
        if (t.empty()) continue;

        bool ib = false, ie = false;
        strip_trim_prefix(t, ib, ie);

        if (!t.empty() && t[0] == '#') {
            if (t.size() >= 10 && is_valid_time_format(t.substr(1, 9))) {
                t.erase(0, 1);
            } else {
                continue;
            }
        }
        if (t.empty()) continue;

        if (is_legacy_file) {
            // Legacy: column 3 holds the label def. Truncate at first space
            // so trailing freeform text doesn't end up inside the column.
            const size_t sp = t.find(' ');
            const std::string body =
                (sp == std::string::npos) ? t : t.substr(0, sp);
            const auto cols = split_pipe(body);
            if (cols.size() > 2 && !cols[2].empty()) {
                std::string lbl = cols[2];
                if (!lbl.empty() && lbl[0] == '#') lbl.erase(0, 1);
                if (is_valid_label_format(lbl)) {
                    if (defined.insert(lbl).second) {
                        first_def_line[lbl] = line_number;
                    }
                }
            }
        } else {
            // New format: payload after `|`, optionally containing `:label`.
            const size_t pipe = t.find('|');
            if (pipe == std::string::npos) continue;
            const std::string payload = t.substr(pipe + 1);
            if (payload.find('(') != std::string::npos ||
                payload.find(')') != std::string::npos) {
                continue;
            }
            const size_t colon = payload.find(':');
            if (colon == std::string::npos) continue;
            const std::string lbl = payload.substr(colon + 1);
            if (is_valid_label_format(lbl)) {
                if (defined.count(lbl) == 0) {
                    defined.insert(lbl);
                    first_def_line[lbl] = line_number;
                }
            }
        }
    }

    // ----- Pass 2: build markers -----------------------------------------

    // `have_prev_numeric` gates the legacy `""""` ditto sentinel: ditto can
    // only appear after a numeric tempo. The actual tempo carried forward
    // is no longer recorded — pass markers in the in-memory model carry
    // inert defaults and resolve live via walk-backward instead.
    bool have_prev_numeric = false;
    bool parse_ok          = true;
    bool first_marker_seen = false;
    double last_time       = -1.0;

    // Track which line first defined each label (for duplicate errors).
    std::set<std::string>      seen_def_in_pass2;
    std::map<std::string, int> seen_def_line;

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

        bool is_begin = false, is_end = false;
        strip_trim_prefix(t, is_begin, is_end);

        bool line_disabled = false;
        if (!t.empty() && t[0] == '#') {
            if (t.size() >= 10 && is_valid_time_format(t.substr(1, 9))) {
                line_disabled = true;
                t.erase(0, 1);
            } else {
                had_nonstandard_content_ = true;
                continue;
            }
        }
        if (t.empty()) {
            had_nonstandard_content_ = true;
            continue;
        }

        // ---------- Legacy parse path (load-only, file-level routed) ----
        if (is_legacy_file) {
            const size_t sp = t.find(' ');
            if (sp != std::string::npos) {
                had_nonstandard_content_ = true;
                t = t.substr(0, sp);
            }
            const auto cols = split_pipe(t);
            if (cols.size() < 2) {
                errors_.push_back({line_number,
                    "need at least time|tempo columns"});
                parse_ok = false;
                continue;
            }
            const std::string& time_raw = cols[0];

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

            const std::string& tempo_raw = cols[1];
            const std::string  label_raw = (cols.size() > 2) ? cols[2]
                                                             : std::string();

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
                m.tempo_inherits = true;
                m.tempo_base     = 1.0;
                m.tempo_scale    = "1.0000";
            } else if (tempo_numeric) {
                const size_t star = tempo_raw.find('*');
                const std::string base_part = (star == std::string::npos)
                    ? tempo_raw : tempo_raw.substr(0, star);
                m.tempo_inherits = false;
                m.tempo_base     = eval_math_string(base_part);
                m.tempo_scale    = (star == std::string::npos)
                    ? std::string() : tempo_raw.substr(star + 1);
                have_prev_numeric = true;
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
                m.tempo_inherits = false;
                m.tempo_base     = 0.0;
                m.tempo_scale.clear();
            }

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
            (void)line_disabled;

            last_time = m.time_seconds;
            markers_.push_back(std::move(m));
            continue;
        }

        // ---------- New-format parse path -------------------------------

        // Reject leftover parens-form input early.
        if (t.find('(') != std::string::npos ||
            t.find(')') != std::string::npos) {
            errors_.push_back({line_number,
                "parens are not valid in the new format"});
            parse_ok = false;
            continue;
        }

        // Disallow whitespace adjacent to anything inside the line — the
        // canonical form is whitespace-free entirely. trim_ws already
        // trimmed leading/trailing; an internal space here is malformed.
        if (t.find(' ') != std::string::npos ||
            t.find('\t') != std::string::npos) {
            errors_.push_back({line_number,
                "whitespace is not valid in the new format"});
            parse_ok = false;
            continue;
        }

        const size_t pipe = t.find('|');
        if (pipe == std::string::npos) {
            errors_.push_back({line_number,
                "missing '|' between time and payload"});
            parse_ok = false;
            continue;
        }
        if (t.find('|', pipe + 1) != std::string::npos) {
            errors_.push_back({line_number,
                "too many pipes in line"});
            parse_ok = false;
            continue;
        }

        const std::string time_raw = t.substr(0, pipe);
        const std::string payload  = t.substr(pipe + 1);

        if (time_raw.size() != 9 || !is_valid_time_format(time_raw)) {
            errors_.push_back({line_number,
                "invalid time format: " + time_raw});
            parse_ok = false;
            continue;
        }
        const double final_time = parse_timestamp(time_raw);

        if (!first_marker_seen) {
            if (time_raw != "00:00.000") {
                errors_.push_back({line_number,
                    "first marker must be 00:00.000 (got " + time_raw +
                    ")"});
                parse_ok = false;
                first_marker_seen = true;
                continue;
            }
            first_marker_seen = true;
        }
        if (last_time >= 0.0 && final_time <= last_time) {
            errors_.push_back({line_number,
                "time not strictly increasing: " + time_raw});
            parse_ok = false;
            continue;
        }

        GuiMarker m;
        m.time_seconds  = final_time;
        m.is_begin_time = is_begin;
        m.is_end_time   = is_end;
        if (line_disabled) m.disabled = true;

        std::string err;
        if (!parse_new_payload(payload, m, err)) {
            errors_.push_back({line_number, err});
            parse_ok = false;
            continue;
        }

        // Cross-marker validation.
        if (!m.label_ref.empty() && defined.count(m.label_ref) == 0) {
            errors_.push_back({line_number,
                "reference to undefined label: " + m.label_ref});
            parse_ok = false;
            continue;
        }
        if (!m.label_def.empty()) {
            if (seen_def_in_pass2.count(m.label_def)) {
                errors_.push_back({line_number,
                    "duplicate label definition: " + m.label_def +
                    " (first defined at line " +
                    std::to_string(seen_def_line[m.label_def]) + ")"});
                parse_ok = false;
                continue;
            }
            seen_def_in_pass2.insert(m.label_def);
            seen_def_line[m.label_def] = line_number;
        }

        // pass markers carry inert defaults (set by parse_new_payload). No
        // cache: their effective tempo is resolved live via walk-backward
        // through the marker list at every read site.

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
    return save(path, markers_);
}

bool GuiMarkers::save(const std::string& path,
                      const std::vector<GuiMarker>& markers_) {
    std::ostringstream out;
    for (const auto& m : markers_) {
        // Canonical new format, no whitespace anywhere on the line:
        //   [b=|e=]?[#]?MM:SS.SSS|PAYLOAD
        // PAYLOAD per Part 1A of V.A1.
        if (m.is_begin_time)    out << "b=";
        else if (m.is_end_time) out << "e=";
        if (m.disabled) out << '#';
        out << format_timestamp(m.time_seconds) << '|';

        // Defensive guard against invalid in-memory combinations.
        const bool both_def_and_ref =
            !m.label_def.empty() && !m.label_ref.empty();
        if (both_def_and_ref) {
            std::fprintf(stderr,
                "warptempo_gui: marker at %.3fs has both label_ref and "
                "label_def; emitting as reference\n",
                m.time_seconds);
        }

        // Payload:
        //   label_ref               → "a.42"
        //   inherit, no def         → "pass"
        //   inherit, with def       → "pass:a.42"
        //   owning, no scale        → "1.23"
        //   owning, with scale      → "1.23*1.2345"
        //   def, no scale           → "1.23:a.03"
        //   def, with scale         → "1.23*1.2345:a.03"
        if (!m.label_ref.empty()) {
            out << m.label_ref;
        } else {
            if (m.tempo_inherits) {
                out << "pass";
            } else {
                char tbuf[32];
                std::snprintf(tbuf, sizeof(tbuf), "%.2f", m.tempo_base);
                out << tbuf;
                if (!m.tempo_scale.empty()) {
                    out << '*' << normalize_scale_string(m.tempo_scale);
                }
            }
            if (!m.label_def.empty()) {
                out << ':' << m.label_def;
            }
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

bool effective_disabled(const std::vector<GuiMarker>& markers, int idx) {
    if (idx < 0 || idx >= static_cast<int>(markers.size())) return false;
    const auto& m = markers[idx];
    if (m.disabled) return true;
    if (!m.label_ref.empty()) {
        // Walk all markers to find the definition. O(N^2) across the list
        // but N is small (hundreds max).
        for (const auto& other : markers) {
            if (!other.label_def.empty() &&
                other.label_def == m.label_ref) {
                return other.disabled;
            }
        }
    }
    return false;
}
