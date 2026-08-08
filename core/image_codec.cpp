#include "image_codec.h"

#include <algorithm>
#include <unordered_set>

namespace {

std::string normalize_ext(std::string ext) {
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

}  // namespace

bool is_codec_extension(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {"heic", "heif", "avif"};
    return exts.count(normalize_ext(ext)) > 0;
}
