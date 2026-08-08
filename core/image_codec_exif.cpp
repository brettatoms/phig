#include <libheif/heif.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>

#include "image_codec.h"
#include "image_codec_internal.h"

namespace {

// Same ownership problem as image_codec_libheif.cpp, and the same reason for
// `auto Release`: libheif's release functions differ in constness across its
// own API and across versions.
template <typename T, auto Release>
class HeifPtr {
   public:
    HeifPtr() = default;
    ~HeifPtr() {
        if (p_) Release(p_);
    }
    HeifPtr(const HeifPtr&) = delete;
    HeifPtr& operator=(const HeifPtr&) = delete;

    T** out() { return &p_; }
    T* get() const { return p_; }
    void reset(T* p) { p_ = p; }

   private:
    T* p_ = nullptr;
};

bool is_jxl_path(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == "jxl";
}

bool starts_with_tiff_header(const std::vector<unsigned char>& b, size_t at) {
    if (at + 4 > b.size()) return false;
    const bool little = b[at] == 'I' && b[at + 1] == 'I' && b[at + 2] == 42 && b[at + 3] == 0;
    const bool big = b[at] == 'M' && b[at + 1] == 'M' && b[at + 2] == 0 && b[at + 3] == 42;
    return little || big;
}

}  // namespace

std::vector<unsigned char> codec_exif_blob(const std::string& path) try {
    // libheif cannot open a JXL container, so its Exif box is read by libjxl.
    if (is_jxl_path(path)) return codec_jxl_exif_blob(path);

    HeifPtr<heif_context, heif_context_free> ctx;
    ctx.reset(heif_context_alloc());
    if (!ctx.get()) return {};

    if (heif_context_read_from_file(ctx.get(), path.c_str(), nullptr).code != heif_error_Ok) {
        return {};
    }

    HeifPtr<heif_image_handle, heif_image_handle_release> handle;
    if (heif_context_get_primary_image_handle(ctx.get(), handle.out()).code != heif_error_Ok) {
        return {};
    }

    heif_item_id id = 0;
    if (heif_image_handle_get_list_of_metadata_block_IDs(handle.get(), "Exif", &id, 1) != 1) {
        return {};
    }

    const size_t size = heif_image_handle_get_metadata_size(handle.get(), id);
    if (size == 0) return {};

    std::vector<unsigned char> raw(size);
    if (heif_image_handle_get_metadata(handle.get(), id, raw.data()).code != heif_error_Ok) {
        return {};
    }

    // Both HEIF and JXL prefix the payload the same way, so the trimming rule
    // lives in one place.
    return codec_trim_to_tiff_header(raw);
} catch (...) {
    return {};
}

std::vector<unsigned char> codec_trim_to_tiff_header(const std::vector<unsigned char>& raw) {
    if (raw.size() < 4) return {};

    // 4-byte big-endian offset from the end of the prefix to the TIFF header,
    // per ISO/IEC 23008-12; JXL's Exif box uses the same convention.
    const size_t offset = (static_cast<size_t>(raw[0]) << 24) |
                          (static_cast<size_t>(raw[1]) << 16) | (static_cast<size_t>(raw[2]) << 8) |
                          static_cast<size_t>(raw[3]);

    size_t start = 4 + offset;
    if (start > raw.size()) start = raw.size();

    // Not every writer emits the prefix: some store a bare TIFF header. Trust
    // the byte-order marker over the declared offset, falling back to the start
    // of the block, and give up if neither position has one rather than
    // handing libexif bytes it will silently fail to parse.
    if (!starts_with_tiff_header(raw, start)) {
        if (!starts_with_tiff_header(raw, 0)) return {};
        start = 0;
    }

    return std::vector<unsigned char>(raw.begin() + static_cast<std::ptrdiff_t>(start), raw.end());
}
