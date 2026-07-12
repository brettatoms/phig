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

// Move/copy every sidecar of `parent_source` to sit beside `parent_dest`,
// deriving each sidecar's destination name from parent_dest's final name
// (stem style: IMG_1234.xmp -> <dest-stem>.xmp; append style:
// IMG_1234.CR2.xmp -> <dest-stem>.CR2.xmp). Each real move/copy is logged as
// kind "move_sidecar"/"copy_sidecar" with restore_json {"parent_image_id":N}
// (N from get_image_id(parent_dest); omitted if the parent row is absent).
// op_id <= 0 skips logging. Never touches the images table.
void carry_sidecars(Database& db, const std::string& parent_source, const std::string& parent_dest,
                    bool is_move, int64_t op_id);
