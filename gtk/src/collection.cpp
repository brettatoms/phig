#include "collection.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <utility>

#include "catalog.h"
#include "config.h"
#include "database.h"
#include "paths.h"
#include "thumbs.h"
#include "thumbs_engine.h"
#include "types.h"

namespace phig::gtk {

struct Collection {
    std::unique_ptr<Database> db;
};

void CollectionClose::operator()(Collection* coll) const noexcept { delete coll; }

namespace {

// The current exception's message, or fallback when it carries none. Call only
// inside a catch block.
std::string caught(const char* fallback) noexcept {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return fallback;
    }
}

// A thumbnail path is built from the hash, so it must be exactly 64 hex digits.
bool is_hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

}  // namespace

std::expected<CollectionPtr, OpenError> open(const std::string& catalog_dir) noexcept {
    try {
        const CatalogPaths paths = catalog_paths(catalog_dir);
        CollectionPtr coll(new Collection{open_catalog_db(paths.db, CatalogAccess::Write)});
        set_catalog(paths);
        migrate_legacy_catalog_files(paths);
        return coll;
    } catch (const CatalogError& e) {
        return std::unexpected(OpenError{e.kind() == CatalogError::Kind::Missing, e.what()});
    } catch (...) {
        return std::unexpected(OpenError{false, caught("Could not open the phig catalog.")});
    }
}

std::expected<std::string, std::string> default_catalog() noexcept {
    try {
        const Config cfg = load_config(config_path());
        return resolve_catalog_dir(std::nullopt, cfg).dir;
    } catch (...) {
        return std::unexpected(caught("Could not resolve the default catalog."));
    }
}

std::expected<uint32_t, std::string> count(Collection& coll) noexcept {
    try {
        const int64_t n = coll.db->count_matching(Database::SearchCriteria{});
        return n > 0 ? static_cast<uint32_t>(n) : 0u;
    } catch (...) {
        return std::unexpected(caught("Could not count the catalog's rows."));
    }
}

std::expected<std::vector<Row>, std::string> page(Collection& coll, uint32_t offset,
                                                  uint32_t limit) noexcept {
    try {
        Database::PageCriteria c;
        c.sort = Database::SortKey::Date;
        c.descending = true;
        c.offset = offset;
        c.limit = static_cast<int32_t>(limit);
        std::vector<Row> rows;
        for (ImageInfo& info : coll.db->search_page(c))
            rows.push_back(Row{info.id, std::move(info.path), std::move(info.sha256),
                               std::move(info.filename)});
        return rows;
    } catch (...) {
        return std::unexpected(caught("Could not read a page of the catalog."));
    }
}

ThumbLocation thumb_lookup(Collection& coll, std::string_view sha256, int size) noexcept {
    (void)coll;  // the thumbnail cache is located by core/paths, not by the database
    try {
        if (size <= 0 || !is_hex64(sha256)) return {};
        const std::string sha(sha256);
        std::string path = get_thumb_path(sha, size);
        if (path.empty()) return {};
        const ThumbLookup state =
            thumb_exists(sha, size) ? ThumbLookup::Present : ThumbLookup::Absent;
        return {state, std::move(path)};
    } catch (...) {
        g_warning("phig collection: thumbnail lookup failed: %s", caught("unknown error").c_str());
    }
    return {};
}

ThumbGenerate thumb_generate(Collection& coll, int64_t image_id, int size) noexcept {
    if (size <= 0) return ThumbGenerate::Failed;
    try {
        switch (ensure_thumb(*coll.db, image_id, size)) {
            case EnsureThumb::Ok:
                return ThumbGenerate::Ok;
            case EnsureThumb::NoSuchRow:
                return ThumbGenerate::NoSuchRow;
            case EnsureThumb::SourceMissing:
                return ThumbGenerate::SourceMissing;
            case EnsureThumb::Failed:
                return ThumbGenerate::Failed;
        }
    } catch (...) {
        g_warning("phig collection: thumbnail generation failed: %s",
                  caught("unknown error").c_str());
    }
    return ThumbGenerate::Failed;
}

}  // namespace phig::gtk
