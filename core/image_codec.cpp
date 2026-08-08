#include "image_codec.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

#include "image_codec_internal.h"

namespace {

std::string normalize_ext(std::string ext) {
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

}  // namespace

bool is_codec_extension(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {"heic", "heif", "avif", "jxl"};
    return exts.count(normalize_ext(ext)) > 0;
}

#ifndef __APPLE__
namespace {

// Only the non-Apple dispatch needs this: ImageIO reads every format this
// module handles, so the macOS branch never chooses a backend by extension.
// Compiling it unconditionally trips -Wunused-function, which CI makes fatal.
bool is_jxl(const std::string& path) {
    return normalize_ext(std::filesystem::path(path).extension().string()) == "jxl";
}

}  // namespace
#endif

cv::Mat codec_decode(const std::string& path) {
    // JXL never goes to libheif, which does not handle the format at all.
    // ImageIO does read it (public.jpeg-xl), so only the non-Apple path needs
    // to branch.
#ifdef __APPLE__
    // ImageIO uses the hardware HEVC decoder. Measured on an M3 for one
    // 3840x2160 HEIC: 41.6 ms wall and 0.047 s CPU, against libheif's 188.4 ms
    // and 0.64 s. The gap widens under parallel_for_each, where libheif's
    // software decode is already competing for cores.
    return codec_decode_imageio(path);
#else
    return is_jxl(path) ? codec_decode_libjxl(path) : codec_decode_libheif(path);
#endif
}
