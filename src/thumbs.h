#pragma once

#include <string>
#include <filesystem>
#include <opencv2/core.hpp>

// Get the thumbnail cache directory (XDG_CACHE_HOME/phig/thumbs/)
std::string get_thumb_cache_dir();

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
