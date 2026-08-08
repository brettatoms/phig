#pragma once

#include <opencv2/core.hpp>
#include <string>

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
