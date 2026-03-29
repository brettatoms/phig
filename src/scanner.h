#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Returns true if the file extension is a known image format
bool is_known_image_extension(const std::string& ext);

// Collects file paths from a directory, optionally recursive.
// Only includes files with known image extensions.
std::vector<fs::path> scan_directory(const std::string& dir, bool recursive);
