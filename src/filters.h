#pragma once

#include <string>
#include <vector>

// Simple glob matching (supports * and ?)
bool glob_match(const std::string& pattern, const std::string& str);

// Expand leading ~ to $HOME in a pattern
std::string expand_tilde(const std::string& pattern);

// Check if a path passes match/filter rules.
// Matches against the full path (not just filename).
// - If matches is empty, everything matches.
// - If matches is non-empty, path must match at least one.
// - If path matches any filter, it's excluded (filter wins).
bool passes_filters(const std::string& path,
                    const std::vector<std::string>& matches,
                    const std::vector<std::string>& filters);
