#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Returns true if the file extension is a known image format
bool is_known_image_extension(const std::string& ext);

// Returns true if the file extension is a known RAW format (cr2, nef, dng, ...)
bool is_raw_extension(const std::string& ext);

// Returns true if the file extension is a known video format
bool is_known_video_extension(const std::string& ext);

// Returns true if the file extension is a known image or video format
bool is_known_media_extension(const std::string& ext);

// True if `media_path` is a JPEG (jpg/jpeg, case-insensitive) AND a same-basename
// RAW file exists on disk in the same directory (e.g. IMG_1.JPG with IMG_1.CR2
// beside it). Used by scan to demote the JPEG to an attachment of the RAW.
// Case-insensitive stem match; any same-stem RAW counts. Returns false for a
// non-JPEG path, a lone JPEG, or the RAW itself.
bool has_sibling_raw(const std::filesystem::path& media_path);

// Same-basename RAW file path beside `media_path`, or "" if `media_path` is
// not a JPEG or no such RAW file exists on disk. has_sibling_raw is a thin
// wrapper over this.
std::filesystem::path sibling_raw_path(const std::filesystem::path& media_path);

// Same-basename JPEG file path beside `media_path`, or "" if `media_path` is
// not a RAW or no such JPEG file exists on disk. The inverse of
// sibling_raw_path, used when carrying a RAW's attached JPEG on mv/cp.
std::filesystem::path sibling_jpeg_path(const std::filesystem::path& media_path);

// Collects file paths from a directory, optionally recursive.
// Only includes files with known media extensions (images + videos).
// Hidden and temp entries (names starting with '.' or '~') are skipped
// unless include_hidden is true; hidden directories are not descended into.
// The scan root is always honored even if it is itself hidden.
std::vector<fs::path> scan_directory(const std::string& dir, bool recursive,
                                     bool include_hidden = false);
