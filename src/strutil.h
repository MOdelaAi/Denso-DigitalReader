// Small string helpers shared by the pure parsing code. Header-only (inline)
// so there is no separate translation unit. Mirrors the bits of Rust's
// `str`/`String` API the ported parsers relied on (`trim`, `lines`, etc.).
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace denso::strutil {

/// Trim ASCII whitespace from both ends (Rust `str::trim`).
inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// ASCII case-insensitive equality (Rust `eq_ignore_ascii_case`).
inline bool iequals(const std::string& a, const std::string& b) {
    return to_lower(a) == to_lower(b);
}

inline std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

/// Split into lines like Rust `str::lines`: break on '\n', drop a trailing
/// '\r', and yield no final empty element for a trailing newline.
inline std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            if (start < s.size()) {
                std::string line = s.substr(start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                out.push_back(std::move(line));
            }
            break;
        }
        std::string line = s.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        start = nl + 1;
    }
    return out;
}

} // namespace denso::strutil
