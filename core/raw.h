#pragma once

#include <opencv2/core.hpp>
#include <string>

#include "decode_result.h"

// Decode a RAW file to a BGR cv::Mat for hashing/thumbnail/faces, resolving:
//   1. same-basename sidecar JPEG on disk (IMG_1.JPG next to IMG_1.CR2);
//   2. the RAW's embedded JPEG preview (LibRaw unpack_thumb);
//   3. LibRaw full decode.
// Returns an empty Mat if all tiers fail. Thread-safe (uses libraw_r, a fresh
// LibRaw instance per call — no shared state). Never throws.
// The result reports which tier produced the image and its frame. hash_source
// also fills frame.source_sha256 for a sidecar JPEG or an embedded preview, at
// the cost of a sha256 over those bytes; ask only when the frame will be
// recorded. A tier that fails leaves its message in warning only if no later
// tier succeeds.
DecodeResult raw_decode_result(const std::string& raw_path, bool hash_source = false);

// raw_decode_result(raw_path).img
cv::Mat raw_decode(const std::string& raw_path);
