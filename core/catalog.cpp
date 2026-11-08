#include "catalog.h"

#include <filesystem>
#include <system_error>

#include "thumbs.h"

namespace fs = std::filesystem;

namespace {

// Renames src to dst. Across filesystems, copies into a temporary sibling of
// dst, renames that into place, then removes src, so a concurrent run sees
// src or a complete dst and never a partial one. False leaves src in place.
bool move_into_place(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::rename(src, dst, ec);
    if (!ec) return true;
    if (ec != std::errc::cross_device_link) return false;

    const fs::path tmp = temp_sibling(dst);
    std::error_code cleanup;
    fs::copy(src, tmp, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        fs::remove_all(tmp, cleanup);
        return false;
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove_all(tmp, cleanup);
        return false;
    }
    // A failure here leaves a second copy at src, never a lost one.
    fs::remove_all(src, cleanup);
    return true;
}

bool same_path(const std::string& a, const std::string& b) {
    return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
}

}  // namespace

LegacyMigration migrate_legacy_catalog_files(const CatalogPaths& catalog) {
    LegacyMigration result;
    std::error_code ec;
    try {
        if (!same_path(catalog.dir, default_catalog_dir())) return result;
        if (!fs::exists(catalog.db, ec)) return result;

        const std::pair<fs::path, fs::path> moves[] = {
            {fs::path(cache_dir()) / "thumbs", catalog.thumbs},
            {fs::path(state_dir()) / "phig.log", catalog.log},
            {fs::path(state_dir()) / "phig.1.log", fs::path(catalog.dir) / "phig.1.log"},
        };
        for (const auto& [src, dst] : moves) {
            if (!fs::exists(src, ec) || same_path(src.string(), dst.string())) continue;
            if (fs::exists(dst, ec)) {
                result.kept_both.push_back(src.string());
            } else if (move_into_place(src, dst)) {
                result.moved.push_back(src.string());
            } else {
                result.failed.push_back(src.string());
            }
        }
        if (fs::exists(catalog.thumbs, ec)) ensure_cachedir_tag(catalog.thumbs);
    } catch (const std::exception&) {
        // No HOME and no XDG variables: there is no legacy location to read.
    }
    return result;
}
