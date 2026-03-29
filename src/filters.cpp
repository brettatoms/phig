#include "filters.h"
#include <cstdlib>

bool glob_match(const std::string& pattern, const std::string& str) {
    size_t pi = 0, si = 0;
    size_t star_p = std::string::npos, star_s = 0;

    while (si < str.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == str[si])) {
            pi++; si++;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            star_p = pi++;
            star_s = si;
        } else if (star_p != std::string::npos) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

std::string expand_tilde(const std::string& pattern) {
    if (!pattern.empty() && pattern[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + pattern.substr(1);
        }
    }
    return pattern;
}

bool passes_filters(const std::string& filename,
                    const std::vector<std::string>& matches,
                    const std::vector<std::string>& filters) {
    // Filter wins — check exclusions first
    for (const auto& f : filters) {
        if (glob_match(f, filename)) return false;
    }
    // If no match patterns, everything passes
    if (matches.empty()) return true;
    // Must match at least one
    for (const auto& m : matches) {
        if (glob_match(m, filename)) return true;
    }
    return false;
}
