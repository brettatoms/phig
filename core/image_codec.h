#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

// Decoding for the modern container formats OpenCV's imgcodecs does not
// handle: HEIC/HEIF and AVIF. Structurally this is `core/raw.h` for a
// different family of formats, and it keeps the same contract.

// True if `ext` (with or without a leading dot, any case) names a format this
// module decodes rather than cv::imread.
bool is_codec_extension(const std::string& ext);

// Decode to an upright BGR cv::Mat for hashing/thumbnails/faces. Orientation
// is applied so the result matches what cv::imread produces for an equivalent
// JPEG. Returns an empty Mat if decoding fails for any reason. Thread-safe.
// Never throws.
cv::Mat codec_decode(const std::string& path);

// The container's raw EXIF block, trimmed so it begins at the TIFF header
// ("II*\0" or "MM\0*"). Empty if the file carries none. Thread-safe. Never
// throws.
//
// This is the bare TIFF block, not a libexif-ready buffer: libexif parses JPEG
// APP1 payloads and needs a leading "Exif\0\0", which core/exif.cpp adds. Note
// that omitting it fails silently rather than loudly.
std::vector<unsigned char> codec_exif_blob(const std::string& path);
