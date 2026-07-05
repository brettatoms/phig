#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Returns true if the file extension is a known image format
bool is_known_image_extension(const std::string& ext);

// Returns true if the file extension is a known video format
bool is_known_video_extension(const std::string& ext);

// Returns true if the file extension is a known image or video format
bool is_known_media_extension(const std::string& ext);

// Collects file paths from a directory, optionally recursive.
// Only includes files with known media extensions (images + videos).
// Hidden and temp entries (names starting with '.' or '~') are skipped
// unless include_hidden is true; hidden directories are not descended into.
// The scan root is always honored even if it is itself hidden.
std::vector<fs::path> scan_directory(const std::string& dir, bool recursive,
                                     bool include_hidden = false);
