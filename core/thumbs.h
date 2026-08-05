#pragma once

#include <filesystem>
#include <opencv2/core.hpp>
#include <string>

// Derive the thumbnail cache path from a SHA256 hash
// e.g. ~/.cache/phig/thumbs/ab/ab3f7c9e...d4e2.jpg
std::string get_thumb_path(const std::string& sha256);

// Check if a thumbnail exists in the cache
bool thumb_exists(const std::string& sha256);

// Generate a thumbnail from an already-decoded image
// Returns true on success
bool generate_thumb(const cv::Mat& img, const std::string& sha256);

// Generate a thumbnail by reading an image from disk
// Returns true on success
bool generate_thumb(const std::string& image_path, const std::string& sha256);

// True if `filename` matches phig's thumbnail naming contract: a 64-char
// lowercase hex SHA256 stem with a ".jpg" extension (e.g. "ab3f...d4e2.jpg").
// Used to confine `thumbs clean` deletion so it can never remove a non-thumbnail
// file if the cache directory is misconfigured to hold user data.
bool is_thumbnail_filename(const std::string& filename);
