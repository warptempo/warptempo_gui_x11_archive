#include "transientmarkers.h"

#include "time_format.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <regex>
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

bool starts_with(const std::string& s, const char* pfx) {
    const size_t n = std::strlen(pfx);
    return s.size() >= n && s.compare(0, n, pfx) == 0;
}

void strip_bom(std::string& s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

// Parse a "MM:SS.mmm" timestamp token. Returns true and writes to `out` on
// success; on failure fills `err_msg`.
bool parse_timestamp_token(const std::string& tok, double& out,
                           std::string& err_msg) {
    static const std::regex re("^([0-5][0-9]):([0-5][0-9])\\.[0-9]{3}$");
    if (!std::regex_match(tok, re)) {
        err_msg = "expected MM:SS.mmm timestamp: " + tok;
        return false;
    }
    out = parse_timestamp(tok);
    return true;
}

// Parse "[b=|e=][#]MM:SS.mmm" into a GuiTransientMarker. Returns true on
// success; on failure, fills `err_msg` with a one-line diagnostic. Files
// written by pre-X.8.3 builds (carrying an i/d status code or a
// displaced_frame token) are rejected with "unexpected status code" so the
// upgrade requirement surfaces to the user instead of silently misparsing.
bool parse_line(const std::string& raw, GuiTransientMarker& out, std::string& err_msg) {
    std::string t = trim_ws(raw);
    if (t.empty()) {
        err_msg = "empty line";
        return false;
    }

    // Order: b=/e= prefix first, then leading `#` for disabled.
    if (starts_with(t, "b=")) { out.is_begin_time = true; t.erase(0, 2); }
    else if (starts_with(t, "e=")) { out.is_end_time = true; t.erase(0, 2); }

    if (!t.empty() && t[0] == '#') {
        out.disabled = true;
        t.erase(0, 1);
    }

    std::vector<std::string> toks;
    {
        std::istringstream iss(t);
        std::string tk;
        while (iss >> tk) toks.push_back(std::move(tk));
    }
    if (toks.empty()) {
        err_msg = "missing timestamp";
        return false;
    }
    if (toks.size() > 1) {
        err_msg = "unexpected status code";
        return false;
    }

    if (!parse_timestamp_token(toks[0], out.time_seconds, err_msg)) {
        return false;
    }
    return true;
}

} // namespace

bool GuiTransientMarkers::load(const std::string& path) {
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

    bool parse_ok = true;
    double last_time = -1.0;

    for (size_t idx = 0; idx < raw_lines.size(); ++idx) {
        const int line_number = static_cast<int>(idx + 1);
        const std::string& raw = raw_lines[idx];
        const std::string t = trim_ws(raw);

        if (t.empty()) {
            if (!raw.empty()) had_nonstandard_content_ = true;
            continue;
        }

        // Comment-line disambiguation: a `#` followed by digits is a
        // disabled marker (handled by parse_line); a `#` followed by
        // anything else is a comment that the canonical save() drops.
        if (t[0] == '#' && (t.size() < 2 ||
                            !std::isdigit(static_cast<unsigned char>(t[1])))) {
            had_nonstandard_content_ = true;
            continue;
        }

        GuiTransientMarker m;
        std::string err;
        if (!parse_line(t, m, err)) {
            errors_.push_back({line_number, err});
            parse_ok = false;
            continue;
        }
        // Strictly-ascending order is keyed on time_seconds: the
        // visible position of the marker.
        const double eff = m.time_seconds;
        if (last_time >= 0.0 && eff <= last_time) {
            errors_.push_back({line_number,
                "time_seconds not strictly increasing: " +
                format_timestamp(eff)});
            parse_ok = false;
            continue;
        }
        last_time = eff;
        markers_.push_back(std::move(m));
    }

    if (!parse_ok) {
        markers_.clear();
        return false;
    }
    // Time-0 invariant: a non-empty transient list always carries an
    // entry at time_seconds 0.0. Phase reset at render start is always
    // correct, so silently materialize the head if the on-disk file
    // omitted it. An empty file stays empty until the user authors.
    if (!markers_.empty() && markers_.front().time_seconds > 0.0) {
        GuiTransientMarker zero;
        zero.time_seconds = 0.0;
        markers_.insert(markers_.begin(), zero);
    }
    return true;
}

bool GuiTransientMarkers::save(const std::string& path) const {
    return save(path, markers_);
}

bool GuiTransientMarkers::save(const std::string& path,
                         const std::vector<GuiTransientMarker>& markers_) {
    // Mid-edit nudge gestures may transit through equal-time collisions.
    // Drop duplicates silently here (keep the first occurrence) and emit
    // a one-line stderr notice so the user sees that the on-disk content
    // diverges from the in-memory list. Dedup is keyed on time_seconds
    // (exact double match, matching warp-marker save behavior).
    std::vector<GuiTransientMarker> deduped;
    deduped.reserve(markers_.size());
    double last_time = std::numeric_limits<double>::lowest();
    int dropped = 0;
    for (const auto& m : markers_) {
        const double eff = m.time_seconds;
        if (eff == last_time) {
            ++dropped;
            continue;
        }
        deduped.push_back(m);
        last_time = eff;
    }
    if (dropped > 0) {
        std::fprintf(stderr,
            "warptempo_gui: dropped %d duplicate transient(s) on save\n",
            dropped);
    }

    std::ostringstream out;
    for (const auto& m : deduped) {
        if (m.is_begin_time)    out << "b=";
        else if (m.is_end_time) out << "e=";
        if (m.disabled)         out << '#';
        out << format_timestamp(m.time_seconds) << '\n';
    }
    const std::string data = out.str();

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

bool GuiTransientMarkers::delete_file(const std::string& path) const {
    if (path.empty()) return false;
    if (::unlink(path.c_str()) == 0) return true;
    if (errno == ENOENT) return true;
    return false;
}

int GuiTransientMarkers::insert_marker(GuiTransientMarker m) {
    const double time = m.time_seconds;
    auto it = std::lower_bound(
        markers_.begin(), markers_.end(), time,
        [](const GuiTransientMarker& a, double t) { return a.time_seconds < t; });
    const int idx = static_cast<int>(it - markers_.begin());
    markers_.insert(it, std::move(m));
    return idx;
}

void GuiTransientMarkers::remove_marker(int index) {
    if (index < 0 || index >= static_cast<int>(markers_.size())) return;
    markers_.erase(markers_.begin() + index);
}
