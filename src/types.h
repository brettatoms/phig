#pragma once

#include <cstdint>
#include <string>

struct ImageInfo {
    std::string path;
    std::string filename;
    std::string extension;
    int64_t file_size = 0;
    std::string created_at;
    std::string modified_at;
    std::string sha256;
    uint64_t phash = 0;
    int width = 0;
    int height = 0;
    std::string metadata_json;
    std::string media_type = "image";  // "image" or "video"
    int64_t duration_ms = 0;           // video duration, 0 for images
};
