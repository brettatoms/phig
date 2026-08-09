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
//
// allow_overwrite: the caller explicitly authorized replacing an occupied
// destination. Only then may `dest` already exist on a move — move_file refuses
// to overwrite otherwise, which is the guard that keeps an unauthorized run from
// destroying bytes. When set, the occupant is renamed aside first and restored if
// anything below fails, matching what copy already does unconditionally.
//
// Throws std::exception on any failure (the disk state is left consistent).
void execute_organize_action(Database& db, const std::string& source, const std::string& dest,
                             bool is_move, int64_t op_id, bool allow_overwrite = false);

// Move/copy every sidecar of `parent_source` to sit beside `parent_dest`,
// deriving each sidecar's destination name from parent_dest's final name
// (stem style: IMG_1234.xmp -> <dest-stem>.xmp; append style:
// IMG_1234.CR2.xmp -> <dest-stem>.CR2.xmp). Each real move/copy is logged as
// kind "move_sidecar"/"copy_sidecar" with restore_json {"parent_image_id":N}
// (N from get_image_id(parent_dest); omitted if the parent row is absent).
// op_id <= 0 skips logging. Never touches the images table.
//
// When the parent is a RAW, also carries its same-basename attached JPEG
// (e.g. IMG_1234.CR2 + IMG_1234.JPG) to sit beside parent_dest, renamed
// stem-style to match (IMG_1234.JPG -> <dest-stem>.JPG, preserving the
// JPEG's own extension/case), logged the same way as a sidecar.
//
// Returns how many files it actually moved or copied, so a caller can count the
// work a parent dragged along without listing the directory a second time. A
// no-op carry (source and destination already the same path) is not counted.
size_t carry_sidecars(Database& db, const std::string& parent_source,
                      const std::string& parent_dest, bool is_move, int64_t op_id);
