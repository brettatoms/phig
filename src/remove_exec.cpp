#include "remove_exec.h"

#include <filesystem>
#include <iostream>
#include <system_error>

#include "log.h"
#include "paths.h"
#include "selection.h"
#include "sidecar.h"

namespace fs = std::filesystem;

std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id) {
    fs::create_directories(trash_dir());

    // move_file() never clobbers: it throws errc::file_exists if the chosen
    // trash name is already taken (a random-hex collision, or a concurrent phig
    // run that grabbed the same name). Retry with a fresh random name in that
    // case; any other error propagates. move_file also fsyncs the cross-device
    // copy before unlinking the source, so a power loss can't leave the only
    // copy truncated.
    std::string filename = fs::path(path).filename().string();
    std::string dst;
    for (;;) {
        dst = trash_path_for(filename);
        try {
            move_file(path, dst);
            break;
        } catch (const fs::filesystem_error& e) {
            if (e.code() == std::errc::file_exists) continue;  // regenerate + retry
            throw;
        }
    }

    if (op_id > 0) db.log_add_entry(op_id, {"trash", path, dst, ""});
    return dst;
}

void execute_hard_delete_action(Database& db, const std::string& path, int64_t op_id) {
    std::error_code ec;
    fs::remove(path, ec);  // false+no-ec if already gone; that's fine
    if (ec) throw fs::filesystem_error("hard delete failed", path, ec);
    if (op_id > 0) db.log_add_entry(op_id, {"delete_file", path, "", ""});
}

std::vector<std::string> remove_media_with_sidecars(Database& db, const std::string& media_path,
                                                    bool hard, int64_t op_id) {
    std::vector<std::string> acted;
    auto act = [&](const std::string& p) {
        if (hard)
            execute_hard_delete_action(db, p, op_id);
        else
            execute_trash_action(db, p, op_id);
        acted.push_back(p);
    };

    act(media_path);
    for (const auto& side : find_sidecars(fs::path(media_path))) {
        act(side.string());
    }
    return acted;
}

int cmd_rm(const RemoveOptions& opts, ConfirmFn confirm) {
    const bool needs_match = opts.mode == RemoveMode::Trash || opts.mode == RemoveMode::Hard ||
                             opts.mode == RemoveMode::DbOnly;
    if (needs_match && opts.matches.empty()) {
        std::cerr << "Error: rm requires at least one --match pattern (except with --orphans)\n";
        return 1;
    }

    Database db(opts.db_path);
    db.init_schema();

    auto all_images = db.get_all_images();
    const size_t total = all_images.size();

    // Selection: match/exclude over the DB.
    std::vector<std::string> matched;
    for (const auto& img : all_images) {
        if (passes_selection(img.path, opts.matches, opts.excludes)) matched.push_back(img.path);
    }

    // --orphans narrows to rows whose files are missing (mount-safe).
    if (opts.mode == RemoveMode::Orphans) {
        matched = paths_safe_to_delete(matched);
    }

    if (matched.empty()) {
        std::cout << "No matching entries found.\n";
        return 0;
    }

    const char* mode_name = opts.mode == RemoveMode::Trash    ? "trash"
                            : opts.mode == RemoveMode::Hard   ? "hard-delete"
                            : opts.mode == RemoveMode::DbOnly ? "db-only"
                                                              : "orphans";
    std::cout << "Found " << matched.size() << " matching entries (mode: " << mode_name << ")\n";

    if (opts.dry_run) {
        std::cout << "\n[DRY RUN]\n";
        for (const auto& p : matched) std::cout << "  " << p << "\n";
        return 0;
    }

    // Guard: --hard always needs confirmation; other modes only when broad.
    const bool broad = total > 0 && matched.size() * 2 >= total && matched.size() >= 20;
    const bool needs_confirm = opts.mode == RemoveMode::Hard || broad;
    if (needs_confirm && !opts.assume_yes) {
        std::string warning;
        if (opts.mode == RemoveMode::Hard)
            warning =
                "\nThis will PERMANENTLY delete " + std::to_string(matched.size()) + " file(s).\n";
        else
            warning = "\nThis will remove " + std::to_string(matched.size()) + " of " +
                      std::to_string(total) + " database entries (" +
                      std::to_string(matched.size() * 100 / total) + "%).\n";
        if (!confirm || !confirm(warning)) return 1;
    }

    const bool does_disk = opts.mode == RemoveMode::Trash || opts.mode == RemoveMode::Hard;
    const bool hard = opts.mode == RemoveMode::Hard;

    int64_t op_id = 0;
    const bool logging = opts.log_enabled;
    if (logging) op_id = db.log_begin_operation("rm", opts.argv_str, iso_utc_now());

    std::vector<std::string> rows_to_delete;  // media + sidecars whose rows to drop
    size_t failed = 0;
    try {
        if (does_disk) {
            // Per-file disk action first; collect the paths whose rows to drop.
            for (const auto& media : matched) {
                try {
                    remove_media_with_sidecars(db, media, hard, op_id);
                    // Only the media path has a DB row; sidecars are not indexed.
                    rows_to_delete.push_back(media);
                } catch (const std::exception& e) {
                    log_error(std::string("rm: ") + media + ": " + e.what());
                    failed++;
                    // remove_media_with_sidecars acts on the media file first,
                    // then its sidecars; if a sidecar action throws, the media
                    // file is already gone from disk even though the whole
                    // call threw. Don't leave its row alive pointing at a file
                    // that no longer exists.
                    if (!fs::exists(media)) rows_to_delete.push_back(media);
                }
            }
        } else {
            // db-only / orphans: no disk action; every matched path is a row.
            rows_to_delete = matched;
        }
        // Drop rows (writes purge_row entries) in the same operation.
        db.delete_paths_logged(rows_to_delete, op_id);
    } catch (...) {
        if (logging) {
            try {
                db.log_finish_operation(op_id, "interrupted");
            } catch (...) {
            }
        }
        throw;
    }

    if (logging) {
        db.log_finish_operation(op_id, failed ? "interrupted" : "completed");
        db.log_prune(opts.log_max_entries);
    }

    std::cout << "Removed " << rows_to_delete.size() << " entries";
    if (failed) std::cout << " (" << failed << " file(s) failed and were skipped)";
    std::cout << ".\nTotal DB: " << db.count() << " images\n";
    return failed ? 1 : 0;
}
