#include "hasher.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <sstream>
#include <stdexcept>

#include "log.h"
#include "raw.h"
#include "scanner.h"

// ---- SHA-256 (minimal implementation, no external dep) ----

namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// Incremental SHA-256 so files can be hashed in fixed-size blocks rather than
// read whole into memory. sha256_compress is the single implementation of the
// block step; init/update/final are the only way to reach it, so there is no
// second code path that could drift from it.
struct Sha256Ctx {
    uint32_t h[8];
    uint64_t total_len;  // total bytes fed in, for the length field
    uint8_t block[64];   // partial block carried between updates
    size_t block_len;    // bytes currently buffered in `block`
};

// Compress one 512-bit block into the running state.
void sha256_compress(uint32_t h[8], const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t(block[static_cast<size_t>(i * 4)]) << 24) |
               (uint32_t(block[static_cast<size_t>(i * 4) + 1]) << 16) |
               (uint32_t(block[static_cast<size_t>(i * 4) + 2]) << 8) |
               uint32_t(block[static_cast<size_t>(i * 4) + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void sha256_init(Sha256Ctx& ctx) {
    ctx.h[0] = 0x6a09e667;
    ctx.h[1] = 0xbb67ae85;
    ctx.h[2] = 0x3c6ef372;
    ctx.h[3] = 0xa54ff53a;
    ctx.h[4] = 0x510e527f;
    ctx.h[5] = 0x9b05688c;
    ctx.h[6] = 0x1f83d9ab;
    ctx.h[7] = 0x5be0cd19;
    ctx.total_len = 0;
    ctx.block_len = 0;
}

// Feed arbitrary-length input. Buffers whatever does not complete a 64-byte
// block and carries it to the next call, so the digest does not depend on how
// the input is chunked.
void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
    ctx.total_len += len;

    // Top up a partial block left by a previous update.
    if (ctx.block_len > 0) {
        size_t take = 64 - ctx.block_len;
        if (take > len) take = len;
        std::memcpy(ctx.block + ctx.block_len, data, take);
        ctx.block_len += take;
        data += take;
        len -= take;
        if (ctx.block_len < 64) return;  // still short of a full block
        sha256_compress(ctx.h, ctx.block);
        ctx.block_len = 0;
    }

    while (len >= 64) {
        sha256_compress(ctx.h, data);
        data += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(ctx.block, data, len);
        ctx.block_len = len;
    }
}

// Append the 0x80 marker, zero-pad, write the big-endian 64-bit *bit* length
// and emit the hex digest. When the buffered tail leaves fewer than 8 bytes of
// room the length field spills into a second padding block.
std::string sha256_final(Sha256Ctx& ctx) {
    uint64_t bit_len = ctx.total_len * 8;

    uint8_t pad[128] = {0};
    size_t pad_len = (ctx.block_len < 56) ? 64 : 128;
    std::memcpy(pad, ctx.block, ctx.block_len);
    pad[ctx.block_len] = 0x80;
    for (int i = 0; i < 8; i++) {
        pad[pad_len - 1 - static_cast<size_t>(i)] = static_cast<uint8_t>(bit_len >> (i * 8));
    }

    for (size_t offset = 0; offset < pad_len; offset += 64) {
        sha256_compress(ctx.h, pad + offset);
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint32_t word : ctx.h) {
        oss << std::setw(8) << word;
    }
    return oss.str();
}

// RAII guard that redirects the process-global stderr to /dev/null for its
// lifetime. Because STDERR_FILENO is shared across threads, concurrent scan
// workers must not overlap save/restore or stderr can be left pointing at
// /dev/null; a mutex serializes the whole suppressed region.
class StderrSuppressor {
   public:
    StderrSuppressor() : lock_(mutex()) {
        saved_ = dup(STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }
    ~StderrSuppressor() {
        if (saved_ >= 0) {
            dup2(saved_, STDERR_FILENO);
            close(saved_);
        }
    }
    StderrSuppressor(const StderrSuppressor&) = delete;
    StderrSuppressor& operator=(const StderrSuppressor&) = delete;

   private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    std::lock_guard<std::mutex> lock_;
    int saved_ = -1;
};

}  // anonymous namespace

std::string compute_sha256(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file for hashing: " + path);
    }

    file.seekg(0, std::ios::end);
    std::streampos end = file.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot determine file size for hashing: " + path);
    }
    size_t size = static_cast<size_t>(end);
    file.seekg(0, std::ios::beg);

    // Stream in fixed blocks: buffering the whole file costs ~2x its size in
    // RSS per worker thread, and scan runs one worker per core.
    constexpr size_t kBlock = 1 << 20;  // 1 MiB
    std::vector<uint8_t> buf(kBlock);
    size_t total_read = 0;

    Sha256Ctx ctx;
    sha256_init(ctx);

    while (file) {
        file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(kBlock));
        std::streamsize got = file.gcount();
        if (got <= 0) break;
        total_read += static_cast<size_t>(got);
        sha256_update(ctx, buf.data(), static_cast<size_t>(got));
    }

    // A short read (I/O error, file shrank mid-scan) must not produce a
    // silently wrong hash — duplicates output drives user deletions.
    if (total_read != size) {
        throw std::runtime_error("Short read while hashing (file changed or I/O error): " + path);
    }

    return sha256_final(ctx);
}

namespace {

// OpenCV's error handler is process-global, so it cannot carry a per-call
// pointer safely across concurrent decoders. Instead we install one handler
// once and have it record into thread-local storage. OpenCV invokes the
// handler on the same thread that called imread(), so each decode reads back
// its own error message with no cross-thread misattribution.
thread_local std::string t_cv_warning;

void install_cv_error_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        cv::redirectError(
            [](int, const char*, const char* err_msg, const char*, int, void*) -> int {
                t_cv_warning = err_msg ? err_msg : "unknown error";
                return 0;  // don't throw
            },
            nullptr);
    });
}

}  // namespace

DecodeResult decode_image_with_warning(const std::string& path) {
    DecodeResult result;

    if (is_raw_extension(std::filesystem::path(path).extension().string())) {
        result.img = raw_decode(path);
        if (result.img.empty()) result.warning = "Cannot decode RAW file: " + path;
        return result;
    }

    install_cv_error_handler();

    // Suppress OpenCV's internal warnings (e.g., "Invalid SOS parameters") and
    // capture them for context via the thread-local buffer set by the handler.
    t_cv_warning.clear();

    // Suppress libjpeg's direct stderr output (e.g., "Premature end of JPEG
    // file") by redirecting stderr to /dev/null for the duration of the read.
    {
        StderrSuppressor suppress_stderr;
        result.img = cv::imread(path, cv::IMREAD_COLOR);
    }

    result.warning = t_cv_warning;
    return result;
}

cv::Mat decode_image(const std::string& path) {
    DecodeResult result = decode_image_with_warning(path);
    if (!result.warning.empty()) {
        // Build the whole message and write it in one operation so concurrent
        // scan workers' warnings don't interleave at newline boundaries.
        std::cerr << '\n';  // break out of the scan progress line, as before
        log_warn(path + " -- " + result.warning);
    }
    return result.img;
}

PhashResult compute_phash(const cv::Mat& img) {
    if (img.empty()) {
        throw std::runtime_error("Cannot compute phash: empty image");
    }

    // Perceptual hash (pHash) using only core OpenCV (no opencv_contrib):
    // grayscale -> 32x32 -> DCT -> top-left 8x8 low-frequency block ->
    // threshold each coefficient against the block mean -> 64-bit hash.
    cv::Mat gray;
    if (img.channels() == 1) {
        gray = img;
    } else {
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Mat small;
    cv::resize(gray, small, cv::Size(32, 32), 0, 0, cv::INTER_AREA);

    cv::Mat floats;
    small.convertTo(floats, CV_32F);

    cv::Mat freq;
    cv::dct(floats, freq);

    cv::Mat block = freq(cv::Rect(0, 0, 8, 8)).clone();
    double mean = cv::mean(block)[0];

    uint64_t hash_val = 0;
    for (int i = 0; i < 64; i++) {
        if (block.at<float>(i / 8, i % 8) > mean) {
            hash_val |= (uint64_t(1) << i);
        }
    }

    return PhashResult{hash_val, img.cols, img.rows};
}

double compute_blur_score(const cv::Mat& img) {
    if (img.empty()) {
        return 0.0;
    }

    cv::Mat gray;
    if (img.channels() == 1) {
        gray = img;
    } else {
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    }

    // Downscale (only shrink) to a fixed longest edge so the sharpness score is
    // comparable across image resolutions.
    const int max_edge = 512;
    int longest = gray.cols > gray.rows ? gray.cols : gray.rows;
    if (longest > max_edge) {
        double scale = static_cast<double>(max_edge) / longest;
        cv::resize(gray, gray, cv::Size(), scale, scale, cv::INTER_AREA);
    }

    // Sharpness = variance of the Laplacian (second derivative).
    cv::Mat laplacian;
    cv::Laplacian(gray, laplacian, CV_64F);
    cv::Scalar mean;
    cv::Scalar stddev;
    cv::meanStdDev(laplacian, mean, stddev);
    return stddev[0] * stddev[0];
}

PhashResult compute_phash(const std::string& path) {
    cv::Mat img = decode_image(path);
    if (img.empty()) {
        throw std::runtime_error("Cannot decode image: " + path);
    }
    return compute_phash(img);
}

VideoKeyframe extract_video_keyframe(const std::string& path) {
    VideoKeyframe kf;

    cv::VideoCapture cap(path);
    if (!cap.isOpened()) {
        return kf;
    }

    // Suppress noisy OpenCV/GStreamer stderr warnings during decode.
    StderrSuppressor suppress_stderr;

    double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    kf.width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    kf.height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

    // Try seeking to ~10% of duration to skip black intros
    if (total_frames > 10) {
        cap.set(cv::CAP_PROP_POS_FRAMES, total_frames * 0.1);
        cap.read(kf.frame);
    }

    // Fallback: scan first 30 frames for a non-black frame. Only reached when
    // the 10% frame is missing or near-black, so most videos never pay for it.
    if (kf.frame.empty() || cv::mean(kf.frame)[0] < 5.0) {
        cap.set(cv::CAP_PROP_POS_FRAMES, 0);
        for (int i = 0; i < 30; i++) {
            if (!cap.read(kf.frame)) break;
            if (cv::mean(kf.frame)[0] >= 5.0) break;
        }
    }

    // Last resort: just use whatever frame we have
    if (kf.frame.empty()) {
        cap.set(cv::CAP_PROP_POS_FRAMES, 0);
        cap.read(kf.frame);
    }

    return kf;
}

PhashResult compute_video_phash(const std::string& path) {
    VideoKeyframe kf = extract_video_keyframe(path);

    if (kf.frame.empty()) {
        // width/height are only set once the capture opened, so a zero here
        // means the video could not be opened at all (e.g. an empty/corrupt
        // file) versus opened-but-undecodable.
        if (kf.width == 0 && kf.height == 0) {
            throw std::runtime_error("Cannot open video: " + path);
        }
        throw std::runtime_error("Cannot extract frame from video: " + path);
    }

    auto result = compute_phash(kf.frame);
    // Use video dimensions rather than frame dimensions (which may differ after decode)
    if (kf.width > 0 && kf.height > 0) {
        result.width = kf.width;
        result.height = kf.height;
    }
    return result;
}
