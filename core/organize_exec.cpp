#include "organize_exec.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include "live_photo.h"
#include "paths.h"
#include "scanner.h"
#include "sidecar.h"

namespace fs = std::filesystem;

void execute_organize_action(Database& db, const std::string& source, const std::string& dest,
                             bool is_move, int64_t op_id) {
    const bool logging = op_id > 0;

    // No-op: destination resolves to the source itself (e.g. re-organizing a file
    // already at its target path). Touch nothing and log nothing — moving/copying
    // a file onto itself and recording a self-move entry would be meaningless and,
    // for copy, would corrupt the file via a backup-then-copy of itself.
    if (source == dest) return;

    if (is_move) {
        move_file(source, dest);
        // Commit the path update and the log entry together so disk and DB (incl.
        // the log) never disagree. If it fails after the file has moved, move it
        // back so the row still matches the source on disk.
        try {
            db.begin_transaction();
            try {
                db.update_path_in_txn(source, dest);
                if (logging) db.log_add_entry(op_id, {"move", source, dest, ""});
                db.commit_transaction();
            } catch (...) {
                db.rollback_transaction();
                throw;
            }
        } catch (...) {
            try {
                move_file(dest, source);
            } catch (const std::exception& back) {
                // Both the DB update AND the move-back failed: the file is at the
                // destination but the DB still lists the source. Surface both so
                // the caller can report it; a rescan reconciles.
                throw std::runtime_error(std::string("DB update failed and could not move ") +
                                         dest + " back to " + source + ": " + back.what() +
                                         " (file is at the destination but the database still "
                                         "lists the source; rescan to reconcile)");
            }
            throw;  // move-back succeeded — rethrow the original DB failure
        }
    } else {
        // Copy to disk atomically. cmd_organize protects every existing on-disk
        // path, so in normal use dest does not exist here. As defense-in-depth for
        // any caller that does target an existing file, back it up first (a
        // same-dir rename — no byte copy) so a DB failure can restore it instead
        // of leaving an orphan or destroying the prior file (plan 020, option B).
        bool had_existing = fs::exists(dest);
        std::string backup;
        if (had_existing) {
            backup = unique_backup_path(dest);
            fs::rename(dest, backup);
        }
        try {
            atomic_copy_file(source, dest);
            db.begin_transaction();
            try {
                db.insert_copy(source, dest);
                if (logging) db.log_add_entry(op_id, {"copy", source, dest, ""});
                db.commit_transaction();
            } catch (...) {
                db.rollback_transaction();
                throw;
            }
        } catch (...) {
            // Undo the disk change: remove the copy we made, then restore the
            // prior file if we backed one up. A failed restore is NOT swallowed —
            // it means the original now sits only at the backup path, which the
            // user must know about, so surface it as the thrown error.
            std::error_code ec;
            fs::remove(dest, ec);
            if (had_existing) {
                std::error_code rec;
                fs::rename(backup, dest, rec);
                if (rec) {
                    throw std::runtime_error("copy failed AND could not restore " + dest +
                                             " from backup " + backup + ": " + rec.message() +
                                             " (the original file is at the backup path; move it "
                                             "back manually)");
                }
            }
            throw;  // restore (if any) succeeded — rethrow the original failure
        }
        if (had_existing) {
            std::error_code ec;
            fs::remove(backup, ec);  // success — drop the backup
            if (ec) {
                // Non-fatal: the copy succeeded and is committed. Surface the
                // leftover so the user can clean it up rather than silently
                // accumulating hidden .phig-bak files.
                std::fprintf(stderr, "Warning: left backup file %s in place (%s)\n", backup.c_str(),
                             ec.message().c_str());
            }
        }
    }
}

void carry_sidecars(Database& db, const std::string& parent_source, const std::string& parent_dest,
                    bool is_move, int64_t op_id) {
    const bool logging = op_id > 0;
    fs::path src_media(parent_source);
    fs::path dst_media(parent_dest);
    std::string src_stem = src_media.stem().string();      // IMG_1
    std::string src_name = src_media.filename().string();  // IMG_1.CR2
    std::string dst_stem = dst_media.stem().string();      // IMG_1-1
    fs::path dst_dir = dst_media.parent_path();

    // Parent id for linking (parent row already repointed to dst by the caller).
    int64_t parent_id = logging ? db.get_image_id(parent_dest) : 0;

    for (const auto& side : find_sidecars(src_media)) {
        std::string side_name = side.filename().string();  // IMG_1.xmp or IMG_1.CR2.xmp
        // Rewrite the leading parent stem to the destination stem; keep the rest
        // (".xmp" for stem style, ".CR2.xmp" for append style).
        std::string tail;
        if (side_name.rfind(src_name + ".", 0) == 0) {
            tail = side_name.substr(src_name.size());  // ".xmp" after "IMG_1.CR2"
            // append style: dest keeps full media name -> dst_stem + parent ext + tail
            // dst media filename is dst_stem + parent-ext; reconstruct from dst_media.
            std::string dst_name = dst_media.filename().string();  // IMG_1-1.CR2
            tail.insert(0, dst_name);                              // IMG_1-1.CR2.xmp
        } else {
            // stem style: IMG_1.xmp -> IMG_1-1.xmp
            tail = dst_stem + side_name.substr(src_stem.size());
        }
        fs::path side_dst = dst_dir / tail;

        if (src_media.parent_path() == dst_dir && side == side_dst) continue;  // no-op

        if (is_move) {
            move_file(side.string(), side_dst.string());
        } else {
            atomic_copy_file(side.string(), side_dst.string());
        }
        if (logging) {
            std::string rj =
                parent_id > 0 ? ("{\"parent_image_id\":" + std::to_string(parent_id) + "}") : "";
            db.log_add_entry(op_id, {is_move ? "move_sidecar" : "copy_sidecar", side.string(),
                                     side_dst.string(), rj});
        }
    }

    // A Live Photo's video is an attachment of its still (plan 038): scan does
    // not index it, so it is not in find_sidecars and would otherwise be left
    // behind when the still moves. Found by Content Identifier rather than by
    // name — the two halves need not share a stem after an export or rename —
    // and renamed to the destination stem so the pair stays discoverable.
    // Nothing to carry when the parent did not actually go anywhere. The guard
    // is on the operation, not the paths: unlike a sidecar or a RAW's JPEG, a
    // Live Photo's video need not share the parent's stem, so a path comparison
    // would see lp_src != lp_dst and "carry" the file onto itself — renaming
    // the user's video, or copying it to a second untracked name, during an
    // operation that moved nothing.
    if (src_media.parent_path() != dst_dir) {
        // Metadata from the destination copy — the parent has already moved by
        // the time this runs — but the video is still in the source directory.
        fs::path lp_src = live_photo_video_in(dst_media, src_media.parent_path());

        // Only carry a video that has no row of its own. Pairing says "this
        // video belongs to that still"; it does not say scan agreed to treat it
        // as an attachment. The two rules are not inverses — scan refuses a
        // video claimed by two stills, while this function sees one match and
        // would carry it — and on the migration path every pre-038 MOV still
        // has a row. Carrying an indexed file moves it out from under its own
        // row, which then points nowhere and is logged as a sidecar move.
        if (!lp_src.empty() && db.get_image_id(lp_src.string()) < 0) {
            fs::path lp_dst = dst_dir / (dst_stem + lp_src.extension().string());

            // No-clobber, explicitly. move_file already refuses to overwrite,
            // but atomic_copy_file ends in a rename that replaces silently — so
            // on the cp path an existing file at this destination would be
            // destroyed with no error and no restore snapshot. The parent's own
            // destination is protected by resolve_conflict; this one is not
            // seen by the conflict planner at all, and the name it lands on
            // (IMG_NNNN.MOV) is one the user never chose.
            std::error_code exists_ec;
            if (fs::exists(lp_dst, exists_ec)) {
                throw std::runtime_error("Live Photo video destination already exists: " +
                                         lp_dst.string());
            }

            if (is_move) {
                move_file(lp_src.string(), lp_dst.string());
            } else {
                atomic_copy_file(lp_src.string(), lp_dst.string());
            }
            if (logging) {
                std::string rj = parent_id > 0
                                     ? ("{\"parent_image_id\":" + std::to_string(parent_id) + "}")
                                     : "";
                db.log_add_entry(op_id, {is_move ? "move_sidecar" : "copy_sidecar", lp_src.string(),
                                         lp_dst.string(), rj});
            }
        }
    }

    // Attachments pass: a RAW's same-basename JPEG is demoted to an attachment
    // by scan (not indexed as its own photo), so it isn't in find_sidecars and
    // must be carried separately. Stem-style rename only (the JPEG's stem
    // always matches the parent stem), preserving the JPEG's own extension/case.
    if (is_raw_extension(src_media.extension().string())) {
        fs::path jpeg_src = sibling_jpeg_path(src_media);
        if (!jpeg_src.empty()) {
            fs::path jpeg_dst = dst_dir / (dst_stem + jpeg_src.extension().string());

            if (!(src_media.parent_path() == dst_dir && jpeg_src == jpeg_dst)) {  // skip no-op
                if (is_move) {
                    move_file(jpeg_src.string(), jpeg_dst.string());
                } else {
                    atomic_copy_file(jpeg_src.string(), jpeg_dst.string());
                }
                if (logging) {
                    std::string rj =
                        parent_id > 0 ? ("{\"parent_image_id\":" + std::to_string(parent_id) + "}")
                                      : "";
                    db.log_add_entry(op_id, {is_move ? "move_sidecar" : "copy_sidecar",
                                             jpeg_src.string(), jpeg_dst.string(), rj});
                }
            }
        }
    }
}
