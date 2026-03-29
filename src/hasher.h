#pragma once

#include <cstdint>
#include <string>
#include <utility>

// Compute SHA256 of a file's contents
std::string compute_sha256(const std::string& path);

// Decode image and compute perceptual hash
// Returns {phash, {width, height}} or throws on failure
struct PhashResult {
    uint64_t hash;
    int width;
    int height;
};

PhashResult compute_phash(const std::string& path);
