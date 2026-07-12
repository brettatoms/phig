#include "remove_exec.h"

#include <filesystem>
#include <system_error>

#include "paths.h"
#include "sidecar.h"

namespace fs = std::filesystem;

std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id) {
    fs::create_directories(trash_dir());
    std::string dst = trash_path_for(fs::path(path).filename().string());

    std::error_code ec;
    fs::rename(path, dst, ec);
    if (ec) {
        // Cross-mount (EXDEV) or other rename failure: copy then remove.
        fs::copy_file(path, dst, fs::copy_options::overwrite_existing);
        fs::remove(path);  // throws on failure — leaves the copy in trash
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
