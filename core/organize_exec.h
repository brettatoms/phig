#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

// What carry_sidecars did: files moved or copied, and one message per attachment it did not.
struct CarryResult {
    size_t carried = 0;                 // files actually moved or copied
    std::vector<std::string> failures;  // "<attachment path>: <reason>"
};

// One attachment of a photo — a metadata sidecar, a Live Photo's video, or a
// RAW's same-basename JPEG — and where it belongs beside the photo's destination.
struct AttachmentMove {
    std::filesystem::path source, dest;
};

// Every attachment of `parent_source`, paired with its destination beside
// `parent_dest` (naming as carry_sidecars describes below). An attachment
// whose destination is its own path is left out. The Live Photo video is found
// from the still at `parent_dest`, so that file must exist, and is listed only
// when the still changes directory and the video has no row of its own (R47).
std::vector<AttachmentMove> attachment_moves(Database& db, const std::string& parent_source,
                                             const std::string& parent_dest);

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
// Each attachment is carried on its own: one that cannot be carried (its
// destination is occupied, the move or copy fails, or its log entry cannot be
// written) is reported in `failures` rather than thrown, and the rest are still
// carried. A file already moved when its log write fails counts in `carried`
// and also appears in `failures`. A no-op carry (source and destination already
// the same path) is not counted.
CarryResult carry_sidecars(Database& db, const std::string& parent_source,
                           const std::string& parent_dest, bool is_move, int64_t op_id);

// ---- A destination that already holds the source's bytes (plan 076) ----

// Whether the two files hold the same bytes, read from disk: sizes first, then
// SHA-256. False when either cannot be read.
bool same_bytes(const std::string& a, const std::string& b);

// Whether every attachment in `moves` whose destination exists holds the same
// bytes there. One that differs means the two photos are not identical as a set.
bool attachments_mergeable(const std::vector<AttachmentMove>& moves);

// Why carrying `from_id`'s user metadata onto `to_id` would lose some of it, or
// "" when it would not. R37's transfer fills a gallery only when the target has
// none and moves faces only when it has none, so two different galleries, or
// named faces on the source when the target already has faces, cannot merge.
std::string metadata_merge_blocker(Database& db, int64_t from_id, int64_t to_id);

// Whether a file time can be a photo's real date: on or after 1990-01-01 and not
// in the future. Earlier times are the Unix epoch, the FAT default of
// 1980-01-01, or a clock that was never set.
bool plausible_mtime(std::filesystem::file_time_type t);

// Settles the attachments of a photo whose bytes already sit at another path:
// one missing beside that path is carried there (moved, or copied when
// `is_move` is false), one already there with the same bytes is trashed on a
// move and left alone on a copy, and one already there with different bytes is
// left where it is and reported. Each action is logged against `parent_id` as
// carry_sidecars logs it; failures are reported, never thrown.
CarryResult settle_attachments(Database& db, const std::vector<AttachmentMove>& moves, bool is_move,
                               int64_t op_id, int64_t parent_id);

// The older-mtime rule. Given the file being kept and a byte-identical copy
// about to be removed, the mtime the kept file should end up with:
// `other_mtime` when both are dated by their mtime (no EXIF and no sidecar
// date, so the time is what files them) and `other_mtime` is older and
// plausible (plausible_mtime). Otherwise `keep_mtime`. A copy takes the time it
// was made, so the older of two is usually the original's. Only `path` and
// `metadata_json` of the two rows are read.
std::filesystem::file_time_type mtime_to_keep(const ImageInfo& keep,
                                              std::filesystem::file_time_type keep_mtime,
                                              const ImageInfo& other,
                                              std::filesystem::file_time_type other_mtime);

struct IdenticalResult {
    // False when the files, their attachments or their metadata no longer match
    // what was planned. Nothing was changed; the caller skips it.
    bool resolved = false;
    CarryResult carry;  // attachments carried after the parent was resolved
};

// Resolve an item whose destination already holds the source's bytes. Every
// condition is re-checked from disk first, because a move trashes the source.
//
// Copy: no file is written. An untracked destination gets a row as insert_copy
// gives one, carrying the source's user metadata, logged as "adopt" rather than
// "copy", since undoing a copy deletes the file it wrote and phig wrote none
// here. A tracked destination receives the source's user metadata through
// copy_user_metadata, logged as "copy_metadata"; the source keeps its own.
//
// Move: the destination takes the source's mtime when mtime_to_keep says so,
// and the source goes to the trash. In one transaction, a tracked destination
// receives the source row's user metadata (R37) and the source row is deleted
// with a restore snapshot (R04); an untracked destination takes over the source
// row, logged as "adopt"; the surviving row records the destination's final
// mtime; and a changed mtime is logged as "set_mtime" with the time it
// replaced. If anything fails, the source is moved back from the trash, the
// destination's mtime restored, and the error rethrown.
//
// Afterwards each attachment is resolved the same way: one already present with
// the same bytes is left alone on copy and trashed on move, and a missing one
// is carried as carry_sidecars would.
IdenticalResult execute_identical_action(Database& db, const std::string& source,
                                         const std::string& dest, bool is_move, int64_t op_id);
