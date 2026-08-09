#pragma once

#include <string>
#include <vector>

// Extract all EXIF data from a file and return as a JSON string.
// Returns "{}" if no EXIF data found.
std::string extract_exif_json(const std::string& path);

// The raw Apple/vendor MakerNote block, or empty when the file has none.
// Returned unparsed: libexif has no parser for Apple's dialect, so Live Photo
// pairing searches these bytes for a Content Identifier rather than reading a
// tag out of them. See core/live_photo.h.
std::vector<unsigned char> extract_maker_note(const std::string& path);
