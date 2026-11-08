#include "collection.h"

#include <cctype>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "database.h"
#include "paths.h"
#include "thumbs.h"
#include "thumbs_engine.h"
#include "types.h"

struct PhigCollection {
    std::unique_ptr<Database> db;
};

namespace {

// GTK reaches this file through C frames, which an exception must not unwind
// through, so every entry point catches and reports here.
void warn_caught(const char* what) noexcept {
    try {
        throw;
    } catch (const std::exception& e) {
        g_warning("phig collection: %s failed: %s", what, e.what());
    } catch (...) {
        g_warning("phig collection: %s failed", what);
    }
}

// A thumbnail path is built from the hash, so it must be exactly 64 hex digits.
bool is_hex64(const char* s) {
    size_t n = 0;
    for (; s[n] != '\0'; ++n) {
        if (n >= 64) return false;
        if (!std::isxdigit(static_cast<unsigned char>(s[n]))) return false;
    }
    return n == 64;
}

}  // namespace

extern "C" {

PhigCollection* phig_collection_open(const char* db_path, char** error) {
    *error = nullptr;
    try {
        auto coll = std::make_unique<PhigCollection>();
        coll->db = std::make_unique<Database>(db_path, /*read_only=*/true);
        // A read-only connection cannot migrate, and every query below assumes
        // this build's schema.
        int have = coll->db->schema_version();
        int want = Database::expected_schema_version();
        if (have != want) {
            *error = g_strdup_printf(
                "The collection is at schema version %d and this build reads version %d.", have,
                want);
            return nullptr;
        }
        return coll.release();
    } catch (const std::exception& e) {
        *error = g_strdup(e.what());
    } catch (...) {
        *error = g_strdup("Could not open the phig database.");
    }
    return nullptr;
}

void phig_collection_close(PhigCollection* coll) { delete coll; }

char* phig_collection_default_path(void) {
    try {
        return g_strdup(default_db_path().c_str());
    } catch (...) {
        warn_caught("default path");
    }
    return nullptr;
}

gboolean phig_collection_count(PhigCollection* coll, guint* out) {
    try {
        int64_t n = coll->db->count_matching(Database::SearchCriteria{});
        *out = n > 0 ? static_cast<guint>(n) : 0;
        return TRUE;
    } catch (...) {
        warn_caught("count");
    }
    return FALSE;
}

PhigRowPage* phig_collection_page(PhigCollection* coll, guint offset, guint limit) {
    try {
        Database::PageCriteria c;
        c.sort = Database::SortKey::Date;
        c.descending = true;
        c.offset = offset;
        c.limit = static_cast<int32_t>(limit);
        std::vector<ImageInfo> rows = coll->db->search_page(c);

        PhigRowPage* page = phig_row_page_new(static_cast<guint>(rows.size()));
        for (size_t i = 0; i < rows.size(); i++) {
            page->rows[i].id = rows[i].id;
            page->rows[i].path = g_strdup(rows[i].path.c_str());
            page->rows[i].sha256 = g_strdup(rows[i].sha256.c_str());
            page->rows[i].filename = g_strdup(rows[i].filename.c_str());
        }
        return page;
    } catch (...) {
        warn_caught("page");
    }
    return nullptr;
}

PhigThumbLookup phig_collection_thumb_lookup(PhigCollection* coll, const char* sha256, int size,
                                             char** path) {
    (void)coll;  // the thumbnail cache is located by core/paths, not by the database
    *path = nullptr;
    try {
        if (sha256 == nullptr || size <= 0 || !is_hex64(sha256)) return PHIG_THUMB_ERROR;
        std::string p = get_thumb_path(sha256, size);
        if (p.empty()) return PHIG_THUMB_ERROR;
        *path = g_strdup(p.c_str());
        return thumb_exists(sha256, size) ? PHIG_THUMB_PRESENT : PHIG_THUMB_ABSENT;
    } catch (...) {
        warn_caught("thumbnail lookup");
    }
    g_free(*path);
    *path = nullptr;
    return PHIG_THUMB_ERROR;
}

gboolean phig_collection_thumb_generate(PhigCollection* coll, int64_t image_id, int size) {
    if (size <= 0) return FALSE;
    try {
        return ensure_thumb(*coll->db, image_id, size) == EnsureThumb::Ok;
    } catch (...) {
        warn_caught("thumbnail generation");
    }
    return FALSE;
}

}  // extern "C"
