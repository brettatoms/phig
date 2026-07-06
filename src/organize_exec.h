#pragma once

#include <cstdint>
#include <string>

#include "database.h"

// Execute one planned cp/mv action for a single file (plan 020).
//
// Move (is_move=true): rename the file to `dest`, then commit the DB path update
// and — when op_id > 0 — a 'move' log entry in one transaction. If the DB step
// fails after the file has moved, move it back so the row still matches disk.
//
// Copy (is_move=false): copy the file to `dest` atomically; if `dest` already
// exists it is backed up first (to a unique sibling) so a DB failure can restore
// it. Then commit insert_copy and — when op_id > 0 — a 'copy' log entry in one
// transaction. On failure the copy is removed and any backup restored.
//
// op_id: pass the open operation id to log an entry, or 0 to skip logging.
// Throws std::exception on any failure (the disk state is left consistent).
void execute_organize_action(Database& db, const std::string& source, const std::string& dest,
                             bool is_move, int64_t op_id);
