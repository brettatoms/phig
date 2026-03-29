#pragma once

#include <string>

// Extract all EXIF data from a file and return as a JSON string.
// Returns "{}" if no EXIF data found.
std::string extract_exif_json(const std::string& path);
