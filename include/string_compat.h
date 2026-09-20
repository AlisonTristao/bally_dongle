#pragma once

// Small free functions mirroring the subset of Arduino String's API that
// lib/ShellOutput (and, as later phases port more of the shell layer, other
// libs) actually calls -- ESP-IDF migration phase 2, see
// PLANO_ESPIDF_DONGLE.md secao 4. Kept semantically identical to their
// Arduino::String counterparts (same -1-for-not-found convention on
// indexOf, same [from,to) convention on substring range math at call
// sites) so a String->std::string port is a mechanical, line-by-line
// substitution rather than a re-derivation of index arithmetic.

#include <cctype>
#include <cstring>
#include <string>

namespace strcompat {

inline void trim(std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    s = s.substr(start, end - start);
}

inline bool startsWith(const std::string& s, const char* prefix) {
    const size_t len = std::strlen(prefix);
    return s.size() >= len && s.compare(0, len, prefix) == 0;
}

inline bool endsWith(const std::string& s, const char* suffix) {
    const size_t len = std::strlen(suffix);
    return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

// -1 when not found, same as Arduino String::indexOf.
inline int indexOf(const std::string& s, char ch, int fromIndex = 0) {
    if (fromIndex < 0) fromIndex = 0;
    if (static_cast<size_t>(fromIndex) > s.size()) return -1;
    const size_t pos = s.find(ch, static_cast<size_t>(fromIndex));
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
}

// Substring overload -- same -1-for-not-found convention.
inline int indexOf(const std::string& s, const char* needle, int fromIndex = 0) {
    if (fromIndex < 0) fromIndex = 0;
    if (static_cast<size_t>(fromIndex) > s.size()) return -1;
    const size_t pos = s.find(needle, static_cast<size_t>(fromIndex));
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
}

// Same [from,to) convention as Arduino String::substring(left, right).
inline std::string substring(const std::string& s, size_t from, size_t to) {
    if (to <= from || from >= s.size()) return std::string();
    return s.substr(from, to - from);
}

inline void toLowerCase(std::string& s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
}

inline bool equalsIgnoreCase(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

// Replaces every occurrence of `from` in `s` with `to`, same as Arduino
// String::replace(const char*, const char*) / replace(char, char) (a
// single-character replace is just a 1-length `from`/`to` here).
inline void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

}  // namespace strcompat
