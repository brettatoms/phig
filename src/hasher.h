#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <opencv2/core.hpp>

// Compute SHA256 of a file's contents
std::string compute_sha256(const std::string& path);

// Decode image and compute perceptual hash
struct PhashResult {
    uint64_t hash;
    int width;
    int height;
};

// Decode from file path
PhashResult compute_phash(const std::string& path);

// Compute from already-decoded image
PhashResult compute_phash(const cv::Mat& img);

// Decode an image file, returns empty Mat on failure
cv::Mat decode_image(const std::string& path);
