#pragma once

#include <opencv2/core.hpp>
#include <string>

// Decode a RAW file to a BGR cv::Mat for hashing/thumbnail/faces, resolving:
//   1. same-basename sidecar JPEG on disk (IMG_1.JPG next to IMG_1.CR2);
//   2. the RAW's embedded JPEG preview (LibRaw unpack_thumb);
//   3. LibRaw full decode.
// Returns an empty Mat if all tiers fail. Thread-safe (uses libraw_r, a fresh
// LibRaw instance per call — no shared state). Never throws.
cv::Mat raw_decode(const std::string& raw_path);
