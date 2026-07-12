#include "selection.h"

#include <cstdlib>

bool glob_match(const std::string& pattern, const std::string& str) {
    size_t pi = 0, si = 0;
    size_t star_p = std::string::npos, star_s = 0;

    while (si < str.size()) {
        if (pi + 1 < pattern.size() && pattern[pi] == '\\') {
            // Escaped char: match the following char literally (so a
            // tab-completed "\ " matches a space, and "\*"/"\?" match
            // literal wildcards).
            if (pattern[pi + 1] != str[si]) {
                if (star_p != std::string::npos) {
                    pi = star_p + 1;
                    si = ++star_s;
                    continue;
                }
                return false;
            }
            pi += 2;
            si++;
        } else if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == str[si])) {
            pi++;
            si++;
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

bool passes_selection(const std::string& filename, const std::vector<std::string>& matches,
                      const std::vector<std::string>& excludes) {
    // Exclude wins — check exclusions first
    for (const auto& e : excludes) {
        if (glob_match(e, filename)) return false;
    }
    // If no match patterns, everything passes
    if (matches.empty()) return true;
    // Must match at least one
    for (const auto& m : matches) {
        if (glob_match(m, filename)) return true;
    }
    return false;
}
