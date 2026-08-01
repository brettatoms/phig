#include <string>
#include <vector>

#include "internal.h"

namespace {
const char* kFavoriteTag = "favorite";

bool bad_ids(const int64_t* ids, size_t n) { return n > 0 && ids == nullptr; }
bool bad_tag(const char* t) { return t == nullptr || t[0] == '\0'; }
}  // namespace

void phig_acquire_write_lock(phig_ctx* ctx, std::unique_ptr<WriterLock>& out) {
    out = std::make_unique<WriterLock>(ctx->db_path + ".lock", /*no_wait=*/true);
}

extern "C" {

phig_status phig_tag_add(phig_ctx* ctx, const int64_t* ids, size_t n, const char* tag) {
    if (bad_ids(ids, n) || bad_tag(tag)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    std::unique_ptr<WriterLock> lock;
    phig_acquire_write_lock(ctx, lock);
    if (n == 0) return PHIG_OK;
    ctx->db->set_tag_by_ids(std::vector<int64_t>(ids, ids + n), tag, true);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_tag_remove(phig_ctx* ctx, const int64_t* ids, size_t n, const char* tag) {
    if (bad_ids(ids, n) || bad_tag(tag)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    std::unique_ptr<WriterLock> lock;
    phig_acquire_write_lock(ctx, lock);
    if (n == 0) return PHIG_OK;
    ctx->db->set_tag_by_ids(std::vector<int64_t>(ids, ids + n), tag, false);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_gallery_set(phig_ctx* ctx, const int64_t* ids, size_t n, const char* gallery) {
    if (bad_ids(ids, n) || gallery == nullptr || gallery[0] == '\0') return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    std::unique_ptr<WriterLock> lock;
    phig_acquire_write_lock(ctx, lock);
    if (n == 0) return PHIG_OK;
    ctx->db->set_gallery_by_ids(std::vector<int64_t>(ids, ids + n), gallery);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_gallery_clear(phig_ctx* ctx, const int64_t* ids, size_t n) {
    if (bad_ids(ids, n)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    std::unique_ptr<WriterLock> lock;
    phig_acquire_write_lock(ctx, lock);
    if (n == 0) return PHIG_OK;
    ctx->db->set_gallery_by_ids(std::vector<int64_t>(ids, ids + n), "");
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_favorite_set(phig_ctx* ctx, const int64_t* ids, size_t n, bool on) {
    if (bad_ids(ids, n)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    std::unique_ptr<WriterLock> lock;
    phig_acquire_write_lock(ctx, lock);
    if (n == 0) return PHIG_OK;
    ctx->db->set_tag_by_ids(std::vector<int64_t>(ids, ids + n), kFavoriteTag, on);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

}  // extern "C"
