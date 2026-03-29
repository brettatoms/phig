#pragma once

#include <string>
#include <vector>

// Simple glob matching (supports * and ?)
bool glob_match(const std::string& pattern, const std::string& str);

// Check if a filename passes match/filter rules.
// - If matches is empty, everything matches.
// - If matches is non-empty, filename must match at least one.
// - If filename matches any filter, it's excluded (filter wins).
bool passes_filters(const std::string& filename,
                    const std::vector<std::string>& matches,
                    const std::vector<std::string>& filters);
