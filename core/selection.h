#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "database.h"

// Simple glob matching (supports * and ?)
bool glob_match(const std::string& pattern, const std::string& str);

// Check if a path passes match/exclude rules.
// Matches against the full path (not just filename).
// - If matches is empty, everything matches.
// - If matches is non-empty, path must match at least one.
// - If path matches any exclude, it's excluded (exclude wins).
bool passes_selection(const std::string& path, const std::vector<std::string>& matches,
                      const std::vector<std::string>& excludes);

// Resolve a caller's selection to the image ids it names, applying every filter
// in SQL: the globs through phig_glob, the prefix as a range over the path
// index.
//
// Returns nullopt when the caller named nothing at all — no glob, no exclude,
// no prefix. That is not the same as naming a selection that matches nothing,
// which returns an empty list: the caller said what it wanted and nothing
// qualified. An unnamed subject resolves to the whole collection, and a
// metadata write over the whole collection strips tags or empties galleries
// with no restore snapshot and no activity-log entry behind it (R78).
//
// The refusal lives here rather than in a caller so that every surface which
// resolves a selection inherits it — `cli/` renders it as an error, and a
// future phigd that accepts a glob over HTTP gets the same answer instead of
// reimplementing the check. It cannot live on the write itself: set_tag_by_ids
// receives an id list and cannot tell a caller that asked for everything from
// one that forgot to filter.
std::optional<std::vector<int64_t>> resolve_named_selection(
    Database& db, const std::string& path_prefix, const std::vector<std::string>& matches,
    const std::vector<std::string>& excludes);
