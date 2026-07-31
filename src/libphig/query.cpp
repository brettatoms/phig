#include <cstring>
#include <string>

#include "internal.h"

namespace {
Database::SortKey to_sort_key(phig_sort_key k) {
    switch (k) {
        case PHIG_SORT_PATH:
            return Database::SortKey::Path;
        case PHIG_SORT_SIZE:
            return Database::SortKey::Size;
        case PHIG_SORT_FILENAME:
            return Database::SortKey::Filename;
        case PHIG_SORT_DATE:
        default:
            return Database::SortKey::Date;
    }
}
std::string str_or_empty(const char* s) { return s ? s : ""; }
}  // namespace

extern "C" {

phig_status phig_query_new(phig_ctx* ctx, phig_query** out) {
    if (ctx == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    try {
        *out = new phig_query();
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

void phig_query_free(phig_query* q) { delete q; }

void phig_query_set_extension(phig_query* q, const char* ext) {
    if (q) q->criteria.filter.extension = str_or_empty(ext);
}

void phig_query_set_date_range(phig_query* q, const char* after, const char* before) {
    if (!q) return;
    q->criteria.filter.after = str_or_empty(after);
    q->criteria.filter.before = str_or_empty(before);
}

void phig_query_set_camera(phig_query* q, const char* model) {
    if (q) q->criteria.filter.camera = str_or_empty(model);
}

void phig_query_set_make(phig_query* q, const char* make) {
    if (q) q->criteria.filter.make = str_or_empty(make);
}

void phig_query_set_size_range(phig_query* q, int64_t min_bytes, int64_t max_bytes) {
    if (!q) return;
    q->criteria.filter.min_size = min_bytes;
    q->criteria.filter.max_size = max_bytes;
}

void phig_query_set_media(phig_query* q, phig_media_type media) {
    if (!q) return;
    switch (media) {
        case PHIG_MEDIA_IMAGE:
            q->criteria.filter.media = "image";
            break;
        case PHIG_MEDIA_VIDEO:
            q->criteria.filter.media = "video";
            break;
        default:
            q->criteria.filter.media = "";
            break;
    }
}

void phig_query_set_duration_range(phig_query* q, int64_t min_ms, int64_t max_ms) {
    if (!q) return;
    q->criteria.filter.min_duration_ms = min_ms;
    q->criteria.filter.max_duration_ms = max_ms;
}

void phig_query_add_tag(phig_query* q, const char* tag) {
    if (q && tag && tag[0] != '\0') q->criteria.filter.tags.push_back(tag);
}

void phig_query_set_gallery(phig_query* q, const char* gallery) {
    if (q) q->criteria.filter.gallery = str_or_empty(gallery);
}

void phig_query_set_sharpness(phig_query* q, double min_score, double max_score) {
    if (!q) return;
    q->criteria.filter.min_sharpness = min_score;
    q->criteria.filter.max_sharpness = max_score;
}

void phig_query_set_sort(phig_query* q, phig_sort_key key, bool descending) {
    if (!q) return;
    q->criteria.sort = to_sort_key(key);
    q->criteria.descending = descending;
}

phig_status phig_query_count(phig_ctx* ctx, const phig_query* q, int64_t* out) {
    if (q == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    *out = ctx->db->count_matching(q->criteria.filter);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_query_page(phig_ctx* ctx, const phig_query* q, int64_t offset, int32_t limit,
                            phig_image_list** out) {
    if (q == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (limit <= 0 || offset < 0) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    Database::PageCriteria c = q->criteria;
    c.offset = offset;
    c.limit = limit;
    *out = phig_make_image_list(ctx->db->search_page(c));
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_image_metadata(phig_ctx* ctx, int64_t id, char** json_out) {
    if (json_out == nullptr) return PHIG_ERR_INVALID_ARG;
    *json_out = nullptr;
    PHIG_TRY(ctx)
    std::string json;
    if (!ctx->db->get_metadata_json(id, json)) return PHIG_ERR_NOT_FOUND;
    char* buf = new char[json.size() + 1];
    std::memcpy(buf, json.c_str(), json.size() + 1);
    *json_out = buf;
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

}  // extern "C"
