#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "database.h"

// Per-file removal executor for `phig rm` (plan 027). Mirrors organize_exec:
// the disk action plus its activity-log entry live here; the command function
// owns the operation lifecycle (log_begin/finish) and DB-row deletion.

// Move `path` into trash_dir() (created on first use). Same-filesystem moves
// use fs::rename; cross-mount (EXDEV) falls back to copy-then-remove. Collision
// names are suffixed via trash_path_for(). Logs {"trash", path, <dst>, ""} when
// op_id > 0. Returns the trash destination. Throws on failure (unlogged).
std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id);

// Permanently remove `path` (fs::remove). A file already gone is not an error.
// Logs {"delete_file", path, "", ""} when op_id > 0. Throws on a real error.
void execute_hard_delete_action(Database& db, const std::string& path, int64_t op_id);

// Apply the disk action (hard ? hard-delete : trash) to `media_path` and each
// of its find_sidecars(), each logged as its own entry. Returns every path
// acted on (media first, then sidecars) for the caller to delete rows for.
// Throws on the first disk error.
std::vector<std::string> remove_media_with_sidecars(Database& db, const std::string& media_path,
                                                    bool hard, int64_t op_id);
