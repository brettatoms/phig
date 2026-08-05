#include "relink.h"

#include <filesystem>
#include <system_error>

#include "scanner.h"

namespace fs = std::filesystem;

int relink_moved_rows(Database& db, const std::vector<std::string>& removed_paths, int64_t op_id) {
    int relinked = 0;
    for (const auto& old_path : removed_paths) {
        auto sha = db.get_sha256(old_path);
        if (!sha || sha->empty()) continue;

        // Candidates: other rows with identical content whose file is real.
        std::string target;
        int on_disk = 0;
        for (const auto& candidate : db.get_paths_for_sha256(*sha, old_path)) {
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                on_disk++;
                target = candidate;
                if (on_disk > 1) break;  // ambiguous — no need to keep looking
            }
        }
        if (on_disk != 1) continue;

        int64_t from_id = db.get_image_id(old_path);
        int64_t to_id = db.get_image_id(target);
        if (from_id < 0 || to_id < 0) continue;

        // Transfer + log entry commit together: the audit trail must never
        // claim a re-link that didn't stick (plan 020 invariant).
        db.begin_transaction();
        try {
            db.transfer_user_metadata(from_id, to_id);
            if (op_id > 0) db.log_add_entry(op_id, {"relink", old_path, target, ""});
            db.commit_transaction();
        } catch (...) {
            db.rollback_transaction();
            throw;
        }
        relinked++;
    }
    return relinked;
}

std::vector<std::string> demote_jpeg_rows(Database& db, const std::string& scan_prefix,
                                          int64_t op_id) {
    std::vector<std::string> demoted;
    for (const auto& path : db.get_paths_with_prefix(scan_prefix)) {
        std::error_code ec;
        if (!fs::exists(path, ec) || ec)
            continue;  // gone from disk -> removed_paths handles it, not demotion

        fs::path raw_path = sibling_raw_path(path);
        if (raw_path.empty()) continue;  // not a JPEG, or no sibling RAW on disk

        int64_t raw_id = db.get_image_id(raw_path.string());
        if (raw_id < 0) continue;  // RAW not indexed yet — demote on a later scan

        int64_t jpeg_id = db.get_image_id(path);
        if (jpeg_id < 0) continue;

        // Transfer + log entry commit together: the audit trail must never
        // claim a re-link that didn't stick (plan 020 invariant).
        db.begin_transaction();
        try {
            db.transfer_user_metadata(jpeg_id, raw_id);
            if (op_id > 0) db.log_add_entry(op_id, {"relink", path, raw_path.string(), ""});
            db.commit_transaction();
        } catch (...) {
            db.rollback_transaction();
            throw;
        }
        demoted.push_back(path);
    }
    return demoted;
}
