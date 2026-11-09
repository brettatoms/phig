#pragma once

#include <cstddef>
#include <cstdint>
#include <opencv2/core.hpp>
#include <string>
#include <utility>

#include "decode_result.h"

// Compute SHA256 of a file's contents
std::string compute_sha256(const std::string& path);

// Incremental SHA-256. compute_sha256 streams files through this rather than
// reading them whole, so peak memory does not scale with file size.
//
// Exposed here (rather than kept private to hasher.cpp) so tests can drive it
// with chunk sizes that are not multiples of 64. compute_sha256 always reads
// 64-aligned 1 MiB blocks, which never exercises the partial-block carry in
// sha256_update; only a direct test can reach that path.
struct Sha256Ctx {
    uint32_t h[8];
    uint64_t total_len;  // total bytes fed in, for the length field
    uint8_t block[64];   // partial block carried between updates
    size_t block_len;    // bytes currently buffered in `block`
};

void sha256_init(Sha256Ctx& ctx);

// Feed arbitrary-length input. Any bytes that do not complete a 64-byte block
// are carried to the next call, so the digest does not depend on how the input
// is split across calls.
void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len);

// Append padding and the length field, then return the lowercase hex digest.
std::string sha256_final(Sha256Ctx& ctx);

// Decode image and compute perceptual hash
struct PhashResult {
    uint64_t hash;
    int width;
    int height;
};

// Decode from file path
PhashResult compute_phash(const std::string& path);

// Compute from already-decoded image
PhashResult compute_phash(const cv::Mat& img);

// Compute a sharpness score (variance of the Laplacian) for an already-decoded
// image. Higher = sharper; blurry images score low. Resolution-normalized by
// downscaling to a fixed longest edge first so scores are comparable across
// image sizes. Returns 0 for an empty image.
double compute_blur_score(const cv::Mat& img);

// Decode an image file, returns empty Mat on failure
cv::Mat decode_image(const std::string& path);

// Decode an image file, capturing any OpenCV-internal error message for this
// specific decode. The warning is attributed correctly even when many threads
// decode concurrently. Empty warning means no OpenCV-internal error.
DecodeResult decode_image_with_warning(const std::string& path);

// A representative video frame plus the source's reported dimensions
// (which can differ from the decoded frame after scaling/rotation).
struct VideoKeyframe {
    cv::Mat frame;  // empty if extraction failed
    int width = 0;
    int height = 0;
};

// Extract a representative keyframe from a video: seeks to ~10% of duration
// to skip black intros, falls back to the first non-black frame, then to
// whatever frame is available. Returns an empty frame if the video cannot be
// opened or decoded. Used for both phash and thumbnails.
VideoKeyframe extract_video_keyframe(const std::string& path);

// Compute phash from a video file by extracting a representative keyframe.
PhashResult compute_video_phash(const std::string& path);
