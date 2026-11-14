#include "organize_exec.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include "dates.h"
#include "hasher.h"
#include "live_photo.h"
#include "paths.h"
#include "remove_exec.h"
#include "scanner.h"
#include "sidecar.h"

namespace fs = std::filesystem;

void execute_organize_action(Database& db, const std::string& source, const std::string& dest,
                             bool is_move, int64_t op_id, bool allow_overwrite) {
    const bool logging = op_id > 0;

    // No-op: destination resolves to the source itself (e.g. re-organizing a file
    // already at its target path). Touch nothing and log nothing — moving/copying
    // a file onto itself and recording a self-move entry would be meaningless and,
    // for copy, would corrupt the file via a backup-then-copy of itself.
    if (source == dest) return;

    if (is_move) {
        // move_file never overwrites, so an occupied destination is only
        // reachable when the caller explicitly authorized replacing it. Get the
        // occupant out of the way with a same-directory rename — no byte copy —
        // so every failure path below can put it back rather than leaving the
        // caller with neither file.
        std::string backup;
        if (allow_overwrite && fs::exists(dest)) {
            backup = unique_backup_path(dest);
            fs::rename(dest, backup);
        }
        auto restore_backup = [&]() {
            if (backup.empty()) return;
            std::error_code ec;
            fs::rename(backup, dest, ec);
        };

        try {
            move_file(source, dest);
        } catch (...) {
            restore_backup();
            throw;
        }

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
                restore_backup();
                throw std::runtime_error(std::string("DB update failed and could not move ") +
                                         dest + " back to " + source + ": " + back.what() +
                                         " (file is at the destination but the database still "
                                         "lists the source; rescan to reconcile)");
            }
            restore_backup();
            throw;  // move-back succeeded — rethrow the original DB failure
        }

        // Committed. The replaced file is now genuinely gone, as authorized.
        if (!backup.empty()) {
            std::error_code ec;
            fs::remove(backup, ec);
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

namespace {

// A carried attachment's destination is derived, not chosen by the user, and
// the conflict planner never sees it — so unlike the parent's destination there
// is no skip/overwrite/rename policy to consult and nothing renames it out of
// the way. Refusing is the only safe default.
//
// move_file already declines to overwrite and throws, so mv was never at risk.
// atomic_copy_file ends in a rename that replaces silently, so on the cp path
// an occupied destination was destroyed with no error raised and no restore
// snapshot written to the activity log.
void refuse_occupied_carry_destination(const fs::path& dest) {
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        throw std::runtime_error("carry destination already exists: " + dest.string());
    }
}

}  // namespace

std::vector<AttachmentMove> attachment_moves(Database& db, const std::string& parent_source,
                                             const std::string& parent_dest) {
    std::vector<AttachmentMove> moves;
    fs::path src_media(parent_source);
    fs::path dst_media(parent_dest);
    std::string src_stem = src_media.stem().string();      // IMG_1
    std::string src_name = src_media.filename().string();  // IMG_1.CR2
    std::string dst_stem = dst_media.stem().string();      // IMG_1-1
    fs::path dst_dir = dst_media.parent_path();

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
        moves.push_back({side, side_dst});
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
            moves.push_back({lp_src, dst_dir / (dst_stem + lp_src.extension().string())});
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
                moves.push_back({jpeg_src, jpeg_dst});
            }
        }
    }
    return moves;
}

namespace {

// Moves or copies one attachment and logs it, reporting a failure rather than
// throwing so the rest are still carried.
void carry_one(Database& db, const AttachmentMove& m, bool is_move, int64_t op_id,
               int64_t parent_id, CarryResult& result) {
    try {
        refuse_occupied_carry_destination(m.dest);
        if (is_move) {
            move_file(m.source.string(), m.dest.string());
        } else {
            atomic_copy_file(m.source.string(), m.dest.string());
        }
        result.carried++;
        if (op_id > 0) {
            std::string rj =
                parent_id > 0 ? ("{\"parent_image_id\":" + std::to_string(parent_id) + "}") : "";
            db.log_add_entry(op_id, {is_move ? "move_sidecar" : "copy_sidecar", m.source.string(),
                                     m.dest.string(), rj});
        }
    } catch (const std::exception& e) {
        result.failures.push_back(m.source.string() + ": " + e.what());
    }
}

}  // namespace

CarryResult carry_sidecars(Database& db, const std::string& parent_source,
                           const std::string& parent_dest, bool is_move, int64_t op_id) {
    CarryResult result;
    // Parent id for linking (parent row already repointed to dst by the caller).
    const int64_t parent_id = op_id > 0 ? db.get_image_id(parent_dest) : 0;
    for (const auto& m : attachment_moves(db, parent_source, parent_dest)) {
        carry_one(db, m, is_move, op_id, parent_id, result);
    }
    return result;
}

bool same_bytes(const std::string& a, const std::string& b) {
    std::error_code ec_a, ec_b;
    const auto size_a = fs::file_size(a, ec_a);
    const auto size_b = fs::file_size(b, ec_b);
    if (ec_a || ec_b || size_a != size_b) return false;
    try {
        return compute_sha256(a) == compute_sha256(b);
    } catch (const std::exception&) {
        return false;
    }
}

bool attachments_mergeable(const std::vector<AttachmentMove>& moves) {
    for (const auto& m : moves) {
        std::error_code ec;
        const bool there = fs::exists(m.dest, ec);
        if (ec) return false;
        if (there && !same_bytes(m.source.string(), m.dest.string())) return false;
    }
    return true;
}

std::string metadata_merge_blocker(Database& db, int64_t from_id, int64_t to_id) {
    const auto rows = db.get_images_by_ids({from_id, to_id});
    const ImageInfo* from = nullptr;
    const ImageInfo* to = nullptr;
    for (const auto& r : rows) {
        if (r.id == from_id) from = &r;
        if (r.id == to_id) to = &r;
    }
    if (!from || !to) return "";
    if (!from->gallery.empty() && !to->gallery.empty() && from->gallery != to->gallery) {
        return "the two photos are in different galleries";
    }
    if (!db.get_labelled_faces_for_image(from_id).empty() && db.has_faces(to->path)) {
        return "the source has named faces and the destination already has faces";
    }
    return "";
}

namespace {

bool dated_by_mtime(ImageInfo info, fs::file_time_type mtime) {
    info.modified_at = format_time(mtime);
    DateSource source;
    get_best_date(info, source);
    return source == DateSource::Mtime;
}

}  // namespace

bool plausible_mtime(fs::file_time_type t) {
    const std::string s = format_time(t);
    return s >= "1990-01-01" && s <= format_time(fs::file_time_type::clock::now());
}

CarryResult settle_attachments(Database& db, const std::vector<AttachmentMove>& moves, bool is_move,
                               int64_t op_id, int64_t parent_id) {
    CarryResult result;
    for (const auto& m : moves) {
        std::error_code ec;
        const bool there = fs::exists(m.dest, ec);
        if (ec) {
            result.failures.push_back(m.source.string() + ": cannot check " + m.dest.string() +
                                      ": " + ec.message());
            continue;
        }
        if (!there) {
            carry_one(db, m, is_move, op_id, parent_id, result);
            continue;
        }
        if (!same_bytes(m.source.string(), m.dest.string())) {
            result.failures.push_back(m.source.string() + ": " + m.dest.string() +
                                      " already exists with different contents");
            continue;
        }
        if (!is_move) continue;  // the same bytes are already there
        try {
            execute_trash_action(db, m.source.string(), op_id);
        } catch (const std::exception& e) {
            result.failures.push_back(m.source.string() + ": " + e.what());
        }
    }
    return result;
}

fs::file_time_type mtime_to_keep(const ImageInfo& keep, fs::file_time_type keep_mtime,
                                 const ImageInfo& other, fs::file_time_type other_mtime) {
    if (other_mtime >= keep_mtime || !plausible_mtime(other_mtime)) return keep_mtime;
    if (!dated_by_mtime(keep, keep_mtime) || !dated_by_mtime(other, other_mtime)) return keep_mtime;
    return other_mtime;
}

IdenticalResult execute_identical_action(Database& db, const std::string& source,
                                         const std::string& dest, bool is_move, int64_t op_id) {
    IdenticalResult out;
    const bool logging = op_id > 0;

    if (!same_bytes(source, dest)) return out;
    const std::vector<AttachmentMove> moves = attachment_moves(db, source, dest);
    if (!attachments_mergeable(moves)) return out;
    const int64_t src_id = db.get_image_id(source);
    const int64_t dst_id = db.get_image_id(dest);
    if (is_move && src_id >= 0 && dst_id >= 0 &&
        !metadata_merge_blocker(db, src_id, dst_id).empty())
        return out;

    if (!is_move) {
        if (dst_id < 0) {
            db.begin_transaction();
            try {
                db.insert_copy(source, dest);
                if (logging) db.log_add_entry(op_id, {"adopt", source, dest, ""});
                db.commit_transaction();
            } catch (...) {
                db.rollback_transaction();
                throw;
            }
        }
    } else {
        // Both rows describe the same bytes, so an untracked destination is
        // judged by the source row's metadata at the destination's path.
        ImageInfo src_info, dst_info;
        for (const auto& r : db.get_images_by_ids({src_id, dst_id})) {
            if (r.id == src_id) src_info = r;
            if (r.id == dst_id) dst_info = r;
        }
        src_info.path = source;
        if (dst_id < 0) dst_info.metadata_json = src_info.metadata_json;
        dst_info.path = dest;
        const fs::file_time_type src_mtime = fs::last_write_time(source);
        const fs::file_time_type dst_mtime = fs::last_write_time(dest);
        const fs::file_time_type final_mtime =
            mtime_to_keep(dst_info, dst_mtime, src_info, src_mtime);
        const bool retime = final_mtime != dst_mtime;
        auto restore_mtime = [&] {
            if (!retime) return;
            std::error_code ec;
            fs::last_write_time(dest, dst_mtime, ec);
        };

        if (retime) fs::last_write_time(dest, final_mtime);
        // Out of the way next, unlogged: the trash entry commits with the row
        // changes below, so the log never records a trash that was undone.
        std::string trashed;
        try {
            trashed = execute_trash_action(db, source, /*op_id=*/0);
        } catch (...) {
            restore_mtime();
            throw;
        }
        try {
            db.begin_transaction();
            try {
                int64_t survivor = dst_id;
                if (dst_id >= 0) {
                    db.transfer_user_metadata(src_id, dst_id);
                    if (logging) db.log_add_entry(op_id, {"relink", source, dest, ""});
                    db.delete_paths_logged_in_txn({source}, op_id);
                } else {
                    db.update_path_in_txn(source, dest);
                    survivor = src_id;
                    if (logging) db.log_add_entry(op_id, {"adopt", source, dest, ""});
                }
                if (survivor >= 0) db.set_modified_at_in_txn(survivor, format_time(final_mtime));
                if (retime && logging) {
                    db.log_add_entry(op_id,
                                     {"set_mtime", dest, dest,
                                      "{\"previous_mtime\":\"" + format_time(dst_mtime) + "\"}"});
                }
                if (logging) db.log_add_entry(op_id, {"trash", source, trashed, ""});
                db.commit_transaction();
            } catch (...) {
                db.rollback_transaction();
                throw;
            }
        } catch (...) {
            restore_mtime();
            try {
                move_file(trashed, source);
            } catch (const std::exception& back) {
                throw std::runtime_error("database update failed and could not restore " + source +
                                         " from the trash at " + trashed + ": " + back.what());
            }
            throw;
        }
    }
    out.resolved = true;

    // Attachments follow their parent only once it is resolved (R44).
    out.carry = settle_attachments(db, moves, is_move, op_id, logging ? db.get_image_id(dest) : 0);
    return out;
}
