#include <gtest/gtest.h>
#include "thumbs.h"
#include "hasher.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

class ThumbsTest : public ::testing::Test {
protected:
    std::string orig_cache;
    std::string temp_dir;

    void SetUp() override {
        // Save original XDG_CACHE_HOME and redirect to temp
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg) orig_cache = xdg;

        temp_dir = fs::temp_directory_path() / "phig-test-thumbs";
        fs::create_directories(temp_dir);
        setenv("XDG_CACHE_HOME", temp_dir.c_str(), 1);
    }

    void TearDown() override {
        // Restore
        if (orig_cache.empty()) {
            unsetenv("XDG_CACHE_HOME");
        } else {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        }
        fs::remove_all(temp_dir);
    }
};

TEST_F(ThumbsTest, GetThumbPath) {
    std::string sha = "ab3f7c9e1234567890abcdef1234567890abcdef1234567890abcdef12345678";
    std::string path = get_thumb_path(sha);
    EXPECT_TRUE(path.find("/phig/thumbs/ab/") != std::string::npos);
    EXPECT_TRUE(path.ends_with(sha + ".jpg"));
}

TEST_F(ThumbsTest, GetThumbPathShortHash) {
    EXPECT_EQ(get_thumb_path(""), "");
    EXPECT_EQ(get_thumb_path("a"), "");
}

TEST_F(ThumbsTest, ThumbExistsReturnsFalse) {
    EXPECT_FALSE(thumb_exists("deadbeef12345678901234567890123456789012345678901234567890123456"));
}

TEST_F(ThumbsTest, GenerateFromMat) {
    // Create a 1000x600 test image (landscape)
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(100, 150, 200));
    std::string sha = "aabbccdd12345678901234567890123456789012345678901234567890123456";

    EXPECT_TRUE(generate_thumb(img, sha));
    EXPECT_TRUE(thumb_exists(sha));

    // Verify the thumbnail was written and is a valid JPEG
    std::string path = get_thumb_path(sha);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_GT(fs::file_size(path), 0);

    // Read it back and check dimensions (512px on longest edge)
    cv::Mat thumb = cv::imread(path);
    EXPECT_FALSE(thumb.empty());
    EXPECT_EQ(thumb.cols, 512);  // longest edge
    EXPECT_EQ(thumb.rows, 307);  // 600 * (512/1000) = 307.2 -> 307
}

TEST_F(ThumbsTest, GenerateFromMatPortrait) {
    // 600x1000 portrait image
    cv::Mat img(1000, 600, CV_8UC3, cv::Scalar(100, 150, 200));
    std::string sha = "bbccddee12345678901234567890123456789012345678901234567890123456";

    EXPECT_TRUE(generate_thumb(img, sha));

    cv::Mat thumb = cv::imread(get_thumb_path(sha));
    EXPECT_FALSE(thumb.empty());
    EXPECT_EQ(thumb.rows, 512);  // longest edge
    EXPECT_EQ(thumb.cols, 307);  // 600 * (512/1000) = 307
}

TEST_F(ThumbsTest, NoUpscaleSmallImage) {
    // 200x100 image — should not be upscaled
    cv::Mat img(100, 200, CV_8UC3, cv::Scalar(50, 100, 150));
    std::string sha = "ccddeeFF12345678901234567890123456789012345678901234567890123456";

    EXPECT_TRUE(generate_thumb(img, sha));

    cv::Mat thumb = cv::imread(get_thumb_path(sha));
    EXPECT_FALSE(thumb.empty());
    EXPECT_EQ(thumb.cols, 200);
    EXPECT_EQ(thumb.rows, 100);
}

TEST_F(ThumbsTest, GenerateEmptyMatFails) {
    cv::Mat empty;
    EXPECT_FALSE(generate_thumb(empty, "abcd1234567890123456789012345678901234567890123456789012345678"));
}

TEST_F(ThumbsTest, GenerateEmptyShaFails) {
    cv::Mat img(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    EXPECT_FALSE(generate_thumb(img, ""));
}

TEST_F(ThumbsTest, CacheDirectoryXDG) {
    std::string dir = get_thumb_cache_dir();
    EXPECT_EQ(dir, temp_dir + "/phig/thumbs");
}
