#pragma once

#include <string>
#include <vector>

// Simple glob matching (supports * and ?)
bool glob_match(const std::string& pattern, const std::string& str);

// Check if a path passes match/exclude rules.
// Matches against the full path (not just filename).
// - If matches is empty, everything matches.
// - If matches is non-empty, path must match at least one.
// - If path matches any exclude, it's excluded (exclude wins).
bool passes_selection(const std::string& path, const std::vector<std::string>& matches,
                      const std::vector<std::string>& excludes);
