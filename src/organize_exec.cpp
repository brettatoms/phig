#include "organize_exec.h"

#include <filesystem>
#include <stdexcept>
#include <system_error>

#include "paths.h"

namespace fs = std::filesystem;

void execute_organize_action(Database& db, const std::string& source, const std::string& dest,
                             bool is_move, int64_t op_id) {
    const bool logging = op_id > 0;

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
        // Copy to disk atomically. On the overwrite path we may land on a
        // pre-existing file; back it up first (a same-dir rename — no byte copy)
        // so a DB failure can restore it instead of leaving an orphan or
        // destroying the prior file (plan 020, option B).
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
            // Undo the disk change: restore the prior file if we backed one up,
            // otherwise remove the copy we just made.
            std::error_code ec;
            fs::remove(dest, ec);
            if (had_existing) fs::rename(backup, dest, ec);
            throw;
        }
        if (had_existing) {
            std::error_code ec;
            fs::remove(backup, ec);  // success — drop the backup
        }
    }
}
