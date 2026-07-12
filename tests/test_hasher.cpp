#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <vector>

#include "hasher.h"

namespace fs = std::filesystem;

class HasherTest : public ::testing::Test {
   protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "image-cleanup-test-hasher";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

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
    auto dir = fs::temp_directory_path() / "phig-hash-unreadable-test";
    fs::create_directories(dir);
    EXPECT_THROW(compute_sha256(dir.string()), std::runtime_error);
    fs::remove_all(dir);
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
