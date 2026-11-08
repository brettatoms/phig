#include "thumbs.h"

#include <cctype>
#include <cstring>
#include <string>

#include "internal.h"

namespace {
bool is_hex64(const char* s) {
    if (s == nullptr) return false;
    size_t n = 0;
    for (; s[n] != '\0'; ++n) {
        if (n >= 64) return false;
        if (!std::isxdigit(static_cast<unsigned char>(s[n]))) return false;
    }
    return n == 64;
}
}  // namespace

extern "C" phig_status phig_thumb_lookup(phig_ctx* ctx, const char* sha256, int size,
                                         char* out_path, size_t out_len, bool* exists) {
    if (out_path == nullptr || exists == nullptr || out_len == 0 || size <= 0) {
        return PHIG_ERR_INVALID_ARG;
    }
    if (!is_hex64(sha256)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    std::string path = get_thumb_path(sha256, size);
    if (path.size() + 1 > out_len) return PHIG_ERR_INVALID_ARG;
    std::memcpy(out_path, path.c_str(), path.size() + 1);
    *exists = thumb_exists(sha256, size);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

extern "C" phig_status phig_thumb_generate(phig_ctx* ctx, int64_t image_id, int size) {
    if (size <= 0) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    switch (ensure_thumb(*ctx->db, image_id, size)) {
        case EnsureThumb::Ok:
            return PHIG_OK;
        case EnsureThumb::NoSuchRow:
            return PHIG_ERR_DB;
        case EnsureThumb::SourceMissing:
            return PHIG_ERR_NOT_FOUND;
        case EnsureThumb::Failed:
            return PHIG_ERR_INTERNAL;
    }
    return PHIG_ERR_INTERNAL;
    PHIG_CATCH(ctx)
}
