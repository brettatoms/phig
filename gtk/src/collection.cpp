#include "collection.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <unordered_map>
#include <utility>

#include "catalog.h"
#include "config.h"
#include "database.h"
#include "dates.h"
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

// phig's built-in favorite tag: favorites are images that carry it.
constexpr const char* kFavoriteTag = "favorite";

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

uint32_t to_count(int64_t n) { return n > 0 ? static_cast<uint32_t>(n) : 0u; }

// Search text as a glob over the full path: as typed when it has a wildcard,
// otherwise matching the text anywhere in the path.
std::string search_glob(const std::string& text) {
    if (text.find_first_of("*?") != std::string::npos) return text;
    return "*" + text + "*";
}

Database::PageCriteria criteria_for(const Query& query) {
    const Filter& f = query.filter;
    Database::PageCriteria c;
    // search_page and ids_matching require an image to carry as many distinct
    // tags as the list names, so the list must not repeat one.
    std::vector<std::string> tags = f.tags;
    if (f.favorite) tags.push_back(kFavoriteTag);
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    c.filter.tags = std::move(tags);
    c.filter.gallery = f.gallery;
    c.filter.after = f.after;
    c.filter.before = f.before;
    c.filter.camera = f.camera;
    c.filter.make = f.make;
    c.filter.media = f.media;
    c.filter.extension = f.extension;
    if (!query.search.empty()) c.filter.matches.push_back(search_glob(query.search));
    switch (query.sort) {
        case Sort::DateNewest:
            c.sort = Database::SortKey::Date;
            c.descending = true;
            break;
        case Sort::DateOldest:
            c.sort = Database::SortKey::Date;
            c.descending = false;
            break;
        case Sort::Filename:
            c.sort = Database::SortKey::Filename;
            c.descending = false;
            break;
        case Sort::Size:
            c.sort = Database::SortKey::Size;
            c.descending = true;
            break;
    }
    c.offset = 0;
    c.limit = -1;  // every match: a negative limit turns off ids_matching's paging
    return c;
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

std::expected<std::vector<int64_t>, std::string> ids(Collection& coll,
                                                     const Query& query) noexcept {
    try {
        return coll.db->ids_matching(criteria_for(query));
    } catch (...) {
        return std::unexpected(caught("Could not query the catalog."));
    }
}

std::expected<std::vector<Row>, std::string> rows(Collection& coll,
                                                  std::span<const int64_t> ids) noexcept {
    try {
        std::unordered_map<int64_t, ImageInfo> found;
        for (ImageInfo& info :
             coll.db->get_images_by_ids(std::vector<int64_t>(ids.begin(), ids.end())))
            found.emplace(info.id, std::move(info));
        std::vector<Row> out;
        out.reserve(found.size());
        for (int64_t id : ids) {
            auto it = found.find(id);
            if (it == found.end()) continue;
            ImageInfo& info = it->second;
            out.push_back(Row{info.id, std::move(info.path), std::move(info.sha256),
                              std::move(info.filename)});
            found.erase(it);
        }
        return out;
    } catch (...) {
        return std::unexpected(caught("Could not read a page of the catalog."));
    }
}

std::expected<Counts, std::string> counts(Collection& coll) noexcept {
    try {
        Counts out;
        out.all = to_count(coll.db->count_matching(Database::SearchCriteria{}));
        for (auto& [name, n] : coll.db->get_tag_counts()) {
            if (name == kFavoriteTag)
                out.favorites = to_count(n);
            else if (n > 0)
                out.tags.push_back(NamedCount{std::move(name), to_count(n)});
        }
        for (auto& [name, n] : coll.db->get_gallery_counts())
            out.galleries.push_back(NamedCount{std::move(name), to_count(n)});
        return out;
    } catch (...) {
        return std::unexpected(caught("Could not count the catalog's tags and galleries."));
    }
}

std::expected<Details, std::string> details(Collection& coll, int64_t id) noexcept {
    try {
        std::vector<ImageInfo> found = coll.db->get_images_by_ids({id});
        if (found.empty()) return std::unexpected("No photo has id " + std::to_string(id) + ".");
        ImageInfo& info = found.front();
        Details d;
        d.id = info.id;
        d.filename = info.filename;
        d.sha256 = info.sha256;
        d.media_type = info.media_type;
        d.date = info.effective_date;
        d.make = json_get(info.metadata_json, "Make");
        d.model = json_get(info.metadata_json, "Model");
        d.exposure_time = json_get(info.metadata_json, "ExposureTime");
        d.f_number = json_get(info.metadata_json, "FNumber");
        d.iso = json_get(info.metadata_json, "ISOSpeedRatings");
        d.focal_length = json_get(info.metadata_json, "FocalLength");
        d.width = info.width;
        d.height = info.height;
        d.file_size = info.file_size;
        d.blur_score = info.blur_score;
        for (std::string& tag : coll.db->get_tags_for_image(info.path))
            if (tag != kFavoriteTag) d.tags.push_back(std::move(tag));
        d.gallery = info.gallery;
        d.favorite = info.favorite;
        d.path = std::move(info.path);
        return d;
    } catch (...) {
        return std::unexpected(caught("Could not read the photo's details."));
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
