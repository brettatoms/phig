#include "catalog.h"

#include <cstdlib>
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

std::string catalog_dir_of(const std::string& db_path) {
    return fs::path(db_path).parent_path().string();
}

}  // namespace

void require_catalog(const std::string& db_path) {
    std::error_code ec;
    if (fs::exists(db_path, ec)) return;
    throw CatalogError(CatalogError::Kind::Missing,
                       "no phig catalog at " + catalog_dir_of(db_path) +
                           ": it has no phig.db. 'phig scan <directory>' creates one.");
}

std::unique_ptr<Database> open_catalog_db(const std::string& db_path, CatalogAccess access) {
    if (access != CatalogAccess::Create) require_catalog(db_path);

    auto db = std::make_unique<Database>(db_path, access == CatalogAccess::Read);
    const int have = db->schema_version();
    const int want = Database::expected_schema_version();
    const std::string dir = catalog_dir_of(db_path);
    if (have > want) {
        throw CatalogError(CatalogError::Kind::Newer,
                           "the catalog at " + dir + " is at schema version " +
                               std::to_string(have) + ", newer than this build's " +
                               std::to_string(want) + "; use a newer phig");
    }
    if (access == CatalogAccess::Read) {
        if (have != want) {
            throw CatalogError(CatalogError::Kind::Outdated,
                               "the catalog at " + dir + " is at schema version " +
                                   std::to_string(have) + " and this build reads version " +
                                   std::to_string(want) +
                                   "; a command that writes to it, such as 'phig scan', "
                                   "upgrades it");
        }
        return db;
    }
    db->init_schema();
    return db;
}

std::unique_ptr<WriterLock> acquire_catalog_lock(const CatalogPaths& catalog, bool no_wait,
                                                 bool may_create) {
    if (!may_create) require_catalog(catalog.db);
    fs::create_directories(fs::path(catalog.lock).parent_path());
    return std::make_unique<WriterLock>(catalog.lock, no_wait);
}

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

namespace {

std::string absolute_dir(const std::string& path) {
    return fs::absolute(expand_tilde(path)).lexically_normal().string();
}

}  // namespace

ResolvedCatalog resolve_catalog_dir(const std::optional<std::string>& flag, const Config& cfg) {
    if (flag) {
        if (flag->empty()) throw std::runtime_error("--catalog names no directory");
        return {absolute_dir(*flag), CatalogSource::Flag};
    }
    const char* env = std::getenv("PHIG_CATALOG");
    if (env != nullptr && env[0] != '\0') return {absolute_dir(env), CatalogSource::Environment};
    if (cfg.catalog) {
        if (cfg.catalog->empty()) {
            throw std::runtime_error("[paths] catalog in the config file names no directory");
        }
        return {absolute_dir(*cfg.catalog), CatalogSource::Config};
    }
    return {default_catalog_dir(), CatalogSource::Default};
}

const char* catalog_source_name(CatalogSource source) {
    switch (source) {
        case CatalogSource::Flag:
            return "--catalog";
        case CatalogSource::Environment:
            return "PHIG_CATALOG";
        case CatalogSource::Config:
            return "config";
        case CatalogSource::Default:
            break;
    }
    return "default";
}
