#include "relink.h"

#include <filesystem>
#include <system_error>

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
