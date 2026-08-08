#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

// The decode backends behind `codec_decode`. Declared separately from
// image_codec.h so tests can reach past the dispatch and compare them against
// each other: on macOS both are compiled, which is what makes the Linux decode
// path testable on a Mac and what the cross-decoder agreement test needs.
//
// Both keep `codec_decode`'s contract: empty Mat on failure, thread-safe,
// never throws.

// libheif. Built on every platform, and the production decoder everywhere
// except macOS. Applies the container's rotation and mirror transforms itself,
// which already matches what cv::imread does for JPEG.
cv::Mat codec_decode_libheif(const std::string& path);

#ifdef __APPLE__
// Apple ImageIO. Uses the hardware HEVC decoder. Unlike libheif it does not
// apply orientation, so the implementation applies the reported value itself.
cv::Mat codec_decode_imageio(const std::string& path);
#endif

// libjxl. JPEG XL has its own backend because libheif handles only the
// HEIF-family containers. Built on every platform for the same reason as
// codec_decode_libheif.
cv::Mat codec_decode_libjxl(const std::string& path);

// The Exif box from a JXL container, trimmed to the TIFF header.
std::vector<unsigned char> codec_jxl_exif_blob(const std::string& path);

// Drops the 4-byte big-endian offset-to-TIFF-header prefix that both HEIF
// (ISO/IEC 23008-12) and JXL put in front of an Exif payload, returning the
// block from its "II*\0" or "MM\0*" marker onward. Empty if no marker is
// found. Shared because both containers use the same convention, and because
// not every writer emits the prefix -- the marker is trusted over the declared
// offset, with a fallback to the start of the block.
std::vector<unsigned char> codec_trim_to_tiff_header(const std::vector<unsigned char>& raw);
