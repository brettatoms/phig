#pragma once

#include <string>
#include <vector>

#include "database.h"

// Scan re-link (plan 023): for each removed path whose stored sha256 matches
// EXACTLY ONE other DB row whose file exists on disk, transfer user-authored
// metadata (tags; gallery if the target has none; faces if the target has
// none) to that row, logging a 'relink' entry when op_id > 0. Zero or multiple
// on-disk candidates: do nothing (ambiguous). Rows are NOT deleted here — the
// caller deletes them afterwards. Returns the number of rows re-linked.
int relink_moved_rows(Database& db, const std::vector<std::string>& removed_paths, int64_t op_id);
