#include "hasher.h"

#include <opencv2/core.hpp>
#include <opencv2/core/utility.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/img_hash.hpp>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <array>
#include <cstring>

// ---- SHA-256 (minimal implementation, no external dep) ----

namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

std::string sha256_bytes(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    // Pre-processing: pad message
    size_t orig_len = len;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded_len, 0);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bit_len = static_cast<uint64_t>(orig_len) * 8;
    for (int i = 0; i < 8; i++) {
        msg[padded_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    }

    // Process each 512-bit block
    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(msg[offset + i*4]) << 24) |
                    (uint32_t(msg[offset + i*4+1]) << 16) |
                    (uint32_t(msg[offset + i*4+2]) << 8) |
                    uint32_t(msg[offset + i*4+3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }

        uint32_t a=h0, b=h1, c=h2, d=h3, e=h4, f=h5, g=h6, h=h7;
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        h0+=a; h1+=b; h2+=c; h3+=d; h4+=e; h5+=f; h6+=g; h7+=h;
    }

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << h0 << std::setw(8) << h1
        << std::setw(8) << h2 << std::setw(8) << h3
        << std::setw(8) << h4 << std::setw(8) << h5
        << std::setw(8) << h6 << std::setw(8) << h7;
    return oss.str();
}

} // anonymous namespace

std::string compute_sha256(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open file for hashing: " + path);
    }

    // Read entire file
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    return sha256_bytes(buffer.data(), buffer.size());
}

cv::Mat decode_image(const std::string& path) {
    // Suppress OpenCV's internal warnings (e.g., "Invalid SOS parameters")
    // and re-emit them with the filename for context
    std::string cv_warning;
    auto prev_handler = cv::redirectError(
        [](int status, const char* func_name, const char* err_msg,
           const char* file_name, int line, void* userdata) -> int {
            auto* warn = static_cast<std::string*>(userdata);
            *warn = err_msg ? err_msg : "unknown error";
            return 0; // don't throw
        }, &cv_warning);

    // Suppress libjpeg's direct stderr output (e.g., "Premature end of JPEG file")
    // by temporarily redirecting stderr to /dev/null
    int saved_stderr = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);

    // Restore stderr
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }

    // Restore OpenCV error handler
    cv::redirectError(nullptr);

    if (!cv_warning.empty()) {
        std::cerr << "\n[warning] " << path << " -- " << cv_warning << std::endl;
    }

    return img;
}

PhashResult compute_phash(const cv::Mat& img) {
    if (img.empty()) {
        throw std::runtime_error("Cannot compute phash: empty image");
    }

    auto hasher = cv::img_hash::PHash::create();
    cv::Mat hash_mat;
    hasher->compute(img, hash_mat);

    uint64_t hash_val = 0;
    for (int i = 0; i < 8; i++) {
        hash_val |= static_cast<uint64_t>(hash_mat.at<uint8_t>(0, i)) << (i * 8);
    }

    return PhashResult{hash_val, img.cols, img.rows};
}

PhashResult compute_phash(const std::string& path) {
    cv::Mat img = decode_image(path);
    if (img.empty()) {
        throw std::runtime_error("Cannot decode image: " + path);
    }
    return compute_phash(img);
}
