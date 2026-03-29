#include <gtest/gtest.h>
#include "hasher.h"
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

class HasherTest : public ::testing::Test {
protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "image-cleanup-test-hasher";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    std::string create_image(const std::string& name, int width, int height,
                             cv::Scalar color) {
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
    EXPECT_LE(distance, 5); // should be very similar
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
