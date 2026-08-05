#include "thumbs.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

#include "hasher.h"
#include "internal.h"
#include "types.h"

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

extern "C" phig_status phig_thumb_lookup(phig_ctx* ctx, const char* sha256, char* out_path,
                                         size_t out_len, bool* exists) {
    if (out_path == nullptr || exists == nullptr || out_len == 0) {
        return PHIG_ERR_INVALID_ARG;
    }
    if (!is_hex64(sha256)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    std::string path = get_thumb_path(sha256);
    if (path.size() + 1 > out_len) return PHIG_ERR_INVALID_ARG;
    std::memcpy(out_path, path.c_str(), path.size() + 1);
    *exists = thumb_exists(sha256);
    return PHIG_OK;
    PHIG_CATCH(ctx)
}

extern "C" phig_status phig_thumb_generate(phig_ctx* ctx, int64_t image_id) {
    PHIG_TRY(ctx)
    ImageInfo img;
    if (!ctx->db->get_image_for_thumb(image_id, img)) return PHIG_ERR_DB;
    if (img.sha256.empty()) return PHIG_ERR_DB;

    if (thumb_exists(img.sha256)) return PHIG_OK;

    std::error_code ec;
    if (!std::filesystem::exists(img.path, ec)) return PHIG_ERR_NOT_FOUND;

    bool ok = false;
    if (img.media_type == "video") {
        auto kf = extract_video_keyframe(img.path);
        if (!kf.frame.empty()) ok = generate_thumb(kf.frame, img.sha256);
    } else {
        ok = generate_thumb(img.path, img.sha256);
    }
    return ok ? PHIG_OK : PHIG_ERR_INTERNAL;
    PHIG_CATCH(ctx)
}
