#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <vector>

#include "hasher.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

class HasherTest : public ::testing::Test {
   protected:
    std::string test_dir;

    void SetUp() override { test_dir = make_temp_dir("hasher").string(); }

    void TearDown() override { fs::remove_all(test_dir); }

    std::string create_image(const std::string& name, int width, int height, cv::Scalar color) {
        auto path = (fs::path(test_dir) / name).string();
        cv::Mat img(height, width, CV_8UC3, color);
        cv::imwrite(path, img);
        return path;
    }

    std::string create_text_file(const std::string& name, const std::string& content) {
        auto path = (fs::path(test_dir) / name).string();
        std::ofstream(path) << content;
        return path;
    }
};

// ---- SHA256 tests ----

TEST_F(HasherTest, SHA256EmptyFile) {
    auto path = create_text_file("empty.txt", "");
    auto hash = compute_sha256(path);
    // SHA256 of empty input
    EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_F(HasherTest, SHA256KnownValue) {
    auto path = create_text_file("hello.txt", "hello");
    auto hash = compute_sha256(path);
    // SHA256 of "hello"
    EXPECT_EQ(hash, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

TEST_F(HasherTest, SHA256ConsistentAcrossCalls) {
    auto path = create_text_file("test.txt", "some content here");
    auto hash1 = compute_sha256(path);
    auto hash2 = compute_sha256(path);
    EXPECT_EQ(hash1, hash2);
}

TEST_F(HasherTest, SHA256DifferentForDifferentContent) {
    auto path1 = create_text_file("a.txt", "content a");
    auto path2 = create_text_file("b.txt", "content b");
    EXPECT_NE(compute_sha256(path1), compute_sha256(path2));
}

TEST_F(HasherTest, SHA256NonExistentFile) {
    EXPECT_THROW(compute_sha256("/nonexistent/file.txt"), std::runtime_error);
}

TEST_F(HasherTest, SHA256IdenticalFiles) {
    auto path1 = create_text_file("a.txt", "same content");
    auto path2 = create_text_file("b.txt", "same content");
    EXPECT_EQ(compute_sha256(path1), compute_sha256(path2));
}

TEST_F(HasherTest, Sha256ThrowsOnUnreadableInput) {
    // A directory opens as a stream on Linux/macOS but reads fail — the old
    // code silently hashed a zero-filled buffer of the directory's st_size.
    auto dir = fs::path(test_dir) / "unreadable";
    fs::create_directories(dir);
    EXPECT_THROW(compute_sha256(dir.string()), std::runtime_error);
}

// ---- SHA256 padding-boundary characterization ----
//
// The digest in images.sha256 is the key for duplicate detection, thumbnail
// cache lookup and relink-on-rescan, so it must never change for any input.
// These vectors are the published FIPS 180-2 values, cross-checked against
// sha256sum; they pin the digest independently of phig's own implementation.

// Write `content` to a file with no trailing newline or text translation.
static std::string write_binary(const std::string& dir, const std::string& name,
                                const std::string& content) {
    auto path = (fs::path(dir) / name).string();
    std::ofstream out(path, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return path;
}

TEST_F(HasherTest, Sha256MatchesKnownVectorAbc) {
    auto path = write_binary(test_dir, "abc.bin", "abc");
    EXPECT_EQ(compute_sha256(path),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// 56 bytes: length mod 64 == 56, so the 8-byte length field does not fit and
// padding must spill into a second block. The classic off-by-one case.
TEST_F(HasherTest, Sha256MatchesKnownVectorTwoBlockPadding) {
    auto path = write_binary(test_dir, "two_block.bin",
                             "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    EXPECT_EQ(compute_sha256(path),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// Every length from 0 to 200 bytes sweeps every padding boundary that matters:
// 55/56 (length field just fits / just does not), 63/64/65 (block edge) and
// 119/120 (the same boundaries one block up). Expected digests are the ones
// the implementation produced before it was made incremental, each verified
// against sha256sum.
TEST_F(HasherTest, Sha256AllLengthsUpTo200MatchReference) {
    // Deterministic pseudo-random bytes so the sweep is not all-ASCII.
    std::string data;
    data.reserve(200);
    uint32_t state = 12345;
    for (int i = 0; i < 200; i++) {
        state = state * 1103515245u + 12345u;
        data += static_cast<char>((state >> 16) & 0xFF);
    }

    // Reference digests for prefixes of `data`, indexed by length, generated
    // with sha256sum. See tests/data/sha256_length_sweep.txt.
    auto ref_path = fs::path(__FILE__).parent_path() / "data" / "sha256_length_sweep.txt";
    std::ifstream ref(ref_path);
    ASSERT_TRUE(ref.is_open()) << "missing reference file " << ref_path;

    std::vector<std::string> expected;
    std::string line;
    while (std::getline(ref, line)) {
        if (!line.empty()) expected.push_back(line);
    }
    ASSERT_EQ(expected.size(), 201u) << "expected digests for lengths 0..200";

    for (size_t len = 0; len <= 200; len++) {
        auto path = write_binary(test_dir, "sweep.bin", data.substr(0, len));
        EXPECT_EQ(compute_sha256(path), expected[len]) << "length " << len;
    }
}

// A file several blocks long whose size is not a multiple of any plausible
// block size, so a carry-over bug in the streaming reader shows up.
TEST_F(HasherTest, Sha256ManyBlocksMatchesReference) {
    std::string data;
    data.reserve(200000);
    for (int i = 0; i < 200000; ++i) data += static_cast<char>('a' + (i % 26));
    auto path = write_binary(test_dir, "many_blocks.bin", data);
    EXPECT_EQ(compute_sha256(path),
              "215fd793b3307b85788c29cd609b538beebaf5fb352bdf7c549fb6951ce0314d");
}

// A file larger than the streaming read block so the loop runs more than once,
// with a final partial read.
TEST_F(HasherTest, Sha256LargerThanReadBlockMatchesReference) {
    const size_t kSize = (1u << 20) + 12345;  // 1 MiB + a partial tail
    std::string data;
    data.reserve(kSize);
    uint32_t state = 987654321u;
    for (size_t i = 0; i < kSize; i++) {
        state = state * 1103515245u + 12345u;
        data += static_cast<char>((state >> 16) & 0xFF);
    }
    auto path = write_binary(test_dir, "big.bin", data);

    auto ref_path = fs::path(__FILE__).parent_path() / "data" / "sha256_large_file.txt";
    std::ifstream ref(ref_path);
    ASSERT_TRUE(ref.is_open()) << "missing reference file " << ref_path;
    std::string expected;
    std::getline(ref, expected);

    EXPECT_EQ(compute_sha256(path), expected);
}

// ---- Incremental SHA256: chunked feeding ----
//
// compute_sha256 reads 64-aligned 1 MiB blocks, so every sha256_update call it
// makes starts with an empty carry buffer. That leaves the partial-block
// top-up in sha256_update unreachable from the file path: mutating
// `take = 64 - block_len`, `if (block_len < 64) return` or `while (len >= 64)`
// does not fail any file-based test. These tests drive the incremental API
// directly with chunk sizes that are not multiples of 64, which is the only
// way to reach that carry logic.

namespace {

// Deterministic pseudo-random bytes; matches the generator used to produce the
// reference digests below.
std::string make_bytes(size_t n, uint32_t seed) {
    std::string out;
    out.reserve(n);
    uint32_t state = seed;
    for (size_t i = 0; i < n; i++) {
        state = state * 1103515245u + 12345u;
        out += static_cast<char>((state >> 16) & 0xFF);
    }
    return out;
}

std::string hash_in_chunks(const std::string& data, size_t chunk) {
    Sha256Ctx ctx;
    sha256_init(ctx);
    size_t off = 0;
    while (off < data.size()) {
        size_t n = std::min(chunk, data.size() - off);
        sha256_update(ctx, reinterpret_cast<const uint8_t*>(data.data()) + off, n);
        off += n;
    }
    return sha256_final(ctx);
}

}  // namespace

// 300 bytes, 300 % 64 == 44, so padding fits in one block.
TEST_F(HasherTest, Sha256ChunkedFeedingMatchesReference) {
    const std::string data = make_bytes(300, 24680);
    // Generated with sha256sum, not with phig's own implementation.
    const std::string expected = "9c0dd331d36db1239fc11f0808d768569384ee0f8a6ada419be64cb0f77cf97e";

    EXPECT_EQ(hash_in_chunks(data, data.size()), expected) << "single-shot";
    for (size_t chunk : {1u, 3u, 7u, 17u, 63u, 64u, 65u, 127u, 128u}) {
        EXPECT_EQ(hash_in_chunks(data, chunk), expected) << "chunk size " << chunk;
    }
}

// 312 bytes, 312 % 64 == 56, so the length field does not fit in the final
// block and padding spills into a second one — the two-block path reached
// through chunked updates rather than a single call.
TEST_F(HasherTest, Sha256ChunkedFeedingTwoBlockPaddingMatchesReference) {
    const std::string data = make_bytes(312, 13579);
    ASSERT_EQ(data.size() % 64, 56u) << "input must exercise two-block padding";
    // Generated with sha256sum, not with phig's own implementation.
    const std::string expected = "b9d6b99a4faabe0a562818ef30aac815dd4fc3f27f5d6c719726d11727a72e58";

    EXPECT_EQ(hash_in_chunks(data, data.size()), expected) << "single-shot";
    for (size_t chunk : {1u, 3u, 7u, 17u, 63u, 64u, 65u, 127u, 128u}) {
        EXPECT_EQ(hash_in_chunks(data, chunk), expected) << "chunk size " << chunk;
    }
}

// 320 bytes, an exact multiple of 64, so the input ends on a block boundary
// with nothing left in the carry buffer. Covers the case where sha256_final is
// reached with block_len == 0 and the whole message was consumed by the bulk
// loop, fed at chunk sizes that do and do not divide the input evenly.
TEST_F(HasherTest, Sha256ChunkedFeedingBlockMultipleMatchesReference) {
    const std::string data = make_bytes(320, 86420);
    ASSERT_EQ(data.size() % 64, 0u) << "input must end exactly on a block boundary";
    // Generated with sha256sum, not with phig's own implementation.
    const std::string expected = "cbecc59ce3470cab239422e5138af3570ccd439ab73a76aac4223781ae2e36f8";

    EXPECT_EQ(hash_in_chunks(data, data.size()), expected) << "single-shot";
    for (size_t chunk : {1u, 3u, 7u, 17u, 63u, 64u, 65u, 127u, 128u, 192u}) {
        EXPECT_EQ(hash_in_chunks(data, chunk), expected) << "chunk size " << chunk;
    }
}

// Every chunk size must agree with the file path for the same bytes, tying the
// directly-driven incremental API back to what compute_sha256 produces.
TEST_F(HasherTest, Sha256ChunkedFeedingAgreesWithFilePath) {
    const std::string data = make_bytes(5000, 97531);
    auto path = write_binary(test_dir, "chunked.bin", data);
    const std::string via_file = compute_sha256(path);

    for (size_t chunk : {1u, 3u, 7u, 17u, 63u, 64u, 65u, 127u, 128u, 4096u}) {
        EXPECT_EQ(hash_in_chunks(data, chunk), via_file) << "chunk size " << chunk;
    }
}

// ---- PHash tests ----

TEST_F(HasherTest, PhashBasicImage) {
    auto path = create_image("test.jpg", 640, 480, cv::Scalar(255, 0, 0));
    auto result = compute_phash(path);
    EXPECT_EQ(result.width, 640);
    EXPECT_EQ(result.height, 480);
}

TEST_F(HasherTest, PhashConsistent) {
    auto path = create_image("test.jpg", 640, 480, cv::Scalar(100, 150, 200));
    auto r1 = compute_phash(path);
    auto r2 = compute_phash(path);
    EXPECT_EQ(r1.hash, r2.hash);
}

TEST_F(HasherTest, PhashIdenticalImages) {
    auto path1 = create_image("a.jpg", 640, 480, cv::Scalar(100, 150, 200));
    auto path2 = create_image("b.jpg", 640, 480, cv::Scalar(100, 150, 200));
    EXPECT_EQ(compute_phash(path1).hash, compute_phash(path2).hash);
}

TEST_F(HasherTest, PhashDifferentImages) {
    auto path1 = create_image("a.jpg", 640, 480, cv::Scalar(255, 0, 0));
    auto path2 = create_image("b.jpg", 640, 480, cv::Scalar(0, 0, 255));
    // Solid color images might still hash similarly, but let's just check it runs
    compute_phash(path1);
    compute_phash(path2);
}

TEST_F(HasherTest, PhashDifferentSizes) {
    auto path1 = create_image("small.jpg", 100, 100, cv::Scalar(128, 128, 128));
    auto path2 = create_image("large.jpg", 1920, 1080, cv::Scalar(128, 128, 128));
    // Same content at different sizes should produce same/similar phash
    auto h1 = compute_phash(path1).hash;
    auto h2 = compute_phash(path2).hash;
    int distance = std::popcount(h1 ^ h2);
    EXPECT_LE(distance, 5);  // should be very similar
}

TEST_F(HasherTest, PhashNonImage) {
    auto path = create_text_file("not_an_image.jpg", "this is not an image");
    EXPECT_THROW(compute_phash(path), std::runtime_error);
}

TEST_F(HasherTest, PhashNonExistentFile) {
    EXPECT_THROW(compute_phash("/nonexistent/image.jpg"), std::runtime_error);
}

TEST_F(HasherTest, PhashPNG) {
    auto path = create_image("test.png", 320, 240, cv::Scalar(50, 100, 150));
    auto result = compute_phash(path);
    EXPECT_EQ(result.width, 320);
    EXPECT_EQ(result.height, 240);
}

// ---- Concurrent warning attribution (imread-issue fix) ----

// A truncated old-style-OJPEG Canon CR2. RAW files are now routed through
// raw_decode (Task 3), not cv::imread, so a truncated CR2 with no sidecar
// fails gracefully at every raw_decode tier and reports a RAW-specific
// warning rather than OpenCV's "Internal imread issue".
static const char* kBadCr2 = "tests/data/truncated_ojpeg.cr2";

TEST_F(HasherTest, DecodeWarningReportsRawFailureForBadCr2) {
    auto r = decode_image_with_warning(kBadCr2);
    EXPECT_TRUE(r.img.empty());
    EXPECT_NE(r.warning.find("Cannot decode RAW file"), std::string::npos) << r.warning;
}

// A CR2 with a same-basename sidecar JPEG resolves via raw_decode's tier 1
// (sidecar JPEG) even though the CR2 itself is the truncated, undecodable
// fixture -- proving decode_image_with_warning routes RAW extensions through
// raw_decode rather than cv::imread.
TEST_F(HasherTest, DecodeWarningUsesSidecarJpegForCr2) {
    create_image("IMG_1.JPG", 64, 48, cv::Scalar(0, 128, 255));

    fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
    fs::path raw_path = fs::path(test_dir) / "IMG_1.CR2";
    fs::copy_file(fixture, raw_path);

    auto r = decode_image_with_warning(raw_path.string());

    EXPECT_FALSE(r.img.empty());
    EXPECT_TRUE(r.warning.empty()) << r.warning;
}

TEST_F(HasherTest, DecodeWarningEmptyForGoodImage) {
    auto path = create_image("good.jpg", 320, 240, cv::Scalar(10, 20, 30));
    auto r = decode_image_with_warning(path);
    EXPECT_FALSE(r.img.empty());
    EXPECT_TRUE(r.warning.empty()) << r.warning;
}

// The bug: cv::redirectError installs a *process-global* handler, so when many
// threads decode at once, a warning raised by one thread's bad decode can be
// captured against another thread's good decode. Each call's warning must
// belong to that call's file, no matter how many threads run concurrently.
TEST_F(HasherTest, DecodeWarningNotMisattributedAcrossThreads) {
    auto good = create_image("good_concurrent.jpg", 320, 240, cv::Scalar(40, 80, 120));

    const int kThreads = 12;
    const int kIters = 10;
    std::atomic<int> good_false_warning{0};   // good file wrongly got a warning
    std::atomic<int> bad_missing_warning{0};  // bad file wrongly got none

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; t++) {
        // Half the threads hammer the bad CR2, half the good JPEG.
        const bool decode_bad = (t % 2 == 0);
        const std::string path = decode_bad ? std::string(kBadCr2) : good;
        threads.emplace_back([&, path, decode_bad] {
            for (int i = 0; i < kIters; i++) {
                auto r = decode_image_with_warning(path);
                if (decode_bad) {
                    if (r.warning.find("Cannot decode RAW file") == std::string::npos)
                        bad_missing_warning.fetch_add(1);
                } else {
                    if (!r.warning.empty()) good_false_warning.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(good_false_warning.load(), 0);
    EXPECT_EQ(bad_missing_warning.load(), 0);
}

// ---- Blur / sharpness (plan 012) ----

// A high-contrast checkerboard — many sharp edges.
static cv::Mat make_checkerboard(int size, int cell) {
    cv::Mat img(size, size, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int y = 0; y < size; y += cell) {
        for (int x = 0; x < size; x += cell) {
            if (((x / cell) + (y / cell)) % 2 == 0) {
                cv::rectangle(img, cv::Point(x, y), cv::Point(x + cell, y + cell),
                              cv::Scalar(255, 255, 255), cv::FILLED);
            }
        }
    }
    return img;
}

TEST_F(HasherTest, BlurScoreUniformImageIsLow) {
    cv::Mat uniform(256, 256, CV_8UC3, cv::Scalar(128, 128, 128));
    EXPECT_LT(compute_blur_score(uniform), 1.0);  // no edges -> ~0 variance
}

TEST_F(HasherTest, BlurScoreSharpExceedsBlurred) {
    cv::Mat sharp = make_checkerboard(256, 16);
    cv::Mat blurred;
    cv::GaussianBlur(sharp, blurred, cv::Size(15, 15), 0);
    EXPECT_GT(compute_blur_score(sharp), compute_blur_score(blurred));
}

TEST_F(HasherTest, BlurScoreDeterministic) {
    cv::Mat img = make_checkerboard(256, 16);
    EXPECT_DOUBLE_EQ(compute_blur_score(img), compute_blur_score(img));
}

TEST_F(HasherTest, BlurScoreResolutionNormalized) {
    // The same pattern at two resolutions (both at/above the 512px normalization
    // threshold) should score similarly, since both downscale to 512 first.
    cv::Mat base = make_checkerboard(512, 32);
    cv::Mat large = make_checkerboard(1024, 64);  // same pattern, 2x scale
    double b = compute_blur_score(base);
    double l = compute_blur_score(large);
    EXPECT_GT(b, 0.0);
    EXPECT_NEAR(b, l, b * 0.5);  // comparable, not orders of magnitude apart
}
