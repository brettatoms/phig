#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "hasher.h"
#include "paths.h"
#include "temp_dir.h"
#include "thumbs.h"

namespace fs = std::filesystem;

class ThumbsTest : public ::testing::Test {
   protected:
    std::string orig_data;
    std::string temp_dir;

    void SetUp() override {
        // Save original XDG_DATA_HOME and redirect to temp
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg) orig_data = xdg;

        temp_dir = make_temp_dir("thumbs");
        setenv("XDG_DATA_HOME", temp_dir.c_str(), 1);
    }

    void TearDown() override {
        // Restore
        if (orig_data.empty()) {
            unsetenv("XDG_DATA_HOME");
        } else {
            setenv("XDG_DATA_HOME", orig_data.c_str(), 1);
        }
        fs::remove_all(temp_dir);
    }
};

TEST_F(ThumbsTest, GetThumbPath) {
    std::string sha = "ab3f7c9e1234567890abcdef1234567890abcdef1234567890abcdef12345678";
    std::string path = get_thumb_path(sha, PHIG_DEFAULT_THUMB_SIZE);
    EXPECT_TRUE(path.find("/phig/thumbs/512/ab/") != std::string::npos) << path;
    EXPECT_TRUE(path.ends_with(sha + ".jpg"));
}

TEST_F(ThumbsTest, GetThumbPathShortHash) {
    EXPECT_EQ(get_thumb_path("", PHIG_DEFAULT_THUMB_SIZE), "");
    EXPECT_EQ(get_thumb_path("a", PHIG_DEFAULT_THUMB_SIZE), "");
}

TEST_F(ThumbsTest, ThumbExistsReturnsFalse) {
    EXPECT_FALSE(thumb_exists("deadbeef12345678901234567890123456789012345678901234567890123456",
                              PHIG_DEFAULT_THUMB_SIZE));
}

TEST_F(ThumbsTest, GenerateFromMat) {
    // Create a 1000x600 test image (landscape)
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(100, 150, 200));
    std::string sha = "aabbccdd12345678901234567890123456789012345678901234567890123456";

    EXPECT_TRUE(generate_thumb(img, sha, PHIG_DEFAULT_THUMB_SIZE));
    EXPECT_TRUE(thumb_exists(sha, PHIG_DEFAULT_THUMB_SIZE));

    // Verify the thumbnail was written and is a valid JPEG
    std::string path = get_thumb_path(sha, PHIG_DEFAULT_THUMB_SIZE);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_GT(fs::file_size(path), 0);

    // Read it back and check dimensions (longest edge == the requested size)
    cv::Mat thumb = cv::imread(path);
    EXPECT_FALSE(thumb.empty());
    EXPECT_EQ(thumb.cols, PHIG_DEFAULT_THUMB_SIZE);  // longest edge
    EXPECT_EQ(thumb.rows, 600 * PHIG_DEFAULT_THUMB_SIZE / 1000);
}

TEST_F(ThumbsTest, GenerateFromMatPortrait) {
    // 600x1000 portrait image
    cv::Mat img(1000, 600, CV_8UC3, cv::Scalar(100, 150, 200));
    std::string sha = "bbccddee12345678901234567890123456789012345678901234567890123456";

    EXPECT_TRUE(generate_thumb(img, sha, PHIG_DEFAULT_THUMB_SIZE));

    cv::Mat thumb = cv::imread(get_thumb_path(sha, PHIG_DEFAULT_THUMB_SIZE));
    EXPECT_FALSE(thumb.empty());
    EXPECT_EQ(thumb.rows, PHIG_DEFAULT_THUMB_SIZE);  // longest edge
    EXPECT_EQ(thumb.cols, 600 * PHIG_DEFAULT_THUMB_SIZE / 1000);
}

TEST_F(ThumbsTest, NoUpscaleSmallImage) {
    // 200x100 image — should not be upscaled
    cv::Mat img(100, 200, CV_8UC3, cv::Scalar(50, 100, 150));
    std::string sha = "ccddeeFF12345678901234567890123456789012345678901234567890123456";

    EXPECT_TRUE(generate_thumb(img, sha, PHIG_DEFAULT_THUMB_SIZE));

    cv::Mat thumb = cv::imread(get_thumb_path(sha, PHIG_DEFAULT_THUMB_SIZE));
    EXPECT_FALSE(thumb.empty());
    EXPECT_EQ(thumb.cols, 200);
    EXPECT_EQ(thumb.rows, 100);
}

TEST_F(ThumbsTest, GenerateEmptyMatFails) {
    cv::Mat empty;
    EXPECT_FALSE(generate_thumb(empty,
                                "abcd1234567890123456789012345678901234567890123456789012345678",
                                PHIG_DEFAULT_THUMB_SIZE));
}

TEST_F(ThumbsTest, GenerateEmptyShaFails) {
    cv::Mat img(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
    EXPECT_FALSE(generate_thumb(img, "", PHIG_DEFAULT_THUMB_SIZE));
}

TEST_F(ThumbsTest, CacheDirectoryXDG) { EXPECT_EQ(thumbs_dir(), temp_dir + "/phig/thumbs"); }

// is_thumbnail_filename gates `thumbs clean` deletion: only true thumbnail names
// may ever be removed, so a misconfigured cache dir holding user data is safe.
TEST_F(ThumbsTest, ThumbnailNameAcceptsValidSha256Jpg) {
    std::string valid = std::string(64, 'a') + ".jpg";  // 64 hex + .jpg
    EXPECT_TRUE(is_thumbnail_filename(valid));
    EXPECT_TRUE(is_thumbnail_filename(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.jpg"));
}

TEST_F(ThumbsTest, ThumbnailNameRejectsForeignFiles) {
    EXPECT_FALSE(is_thumbnail_filename("README.txt"));
    EXPECT_FALSE(is_thumbnail_filename("vacation.jpg"));  // not hex, wrong length
    EXPECT_FALSE(is_thumbnail_filename("important-document.pdf"));
    EXPECT_FALSE(is_thumbnail_filename(std::string(64, 'a')));           // no extension
    EXPECT_FALSE(is_thumbnail_filename(std::string(64, 'a') + ".png"));  // wrong extension
    EXPECT_FALSE(is_thumbnail_filename(std::string(63, 'a') + ".jpg"));  // too short
    EXPECT_FALSE(is_thumbnail_filename(std::string(65, 'a') + ".jpg"));  // too long
    EXPECT_FALSE(is_thumbnail_filename(std::string(64, 'A') + ".jpg"));  // uppercase hex rejected
    EXPECT_FALSE(is_thumbnail_filename(std::string(64, 'g') + ".jpg"));  // 'g' not hex
    EXPECT_FALSE(is_thumbnail_filename(""));
}

// ---- size partitioning (plan 040) ----

TEST_F(ThumbsTest, PathIncludesSizeAndDiffersBetweenSizes) {
    std::string sha(64, 'a');
    std::string p512 = get_thumb_path(sha, 512);
    std::string p768 = get_thumb_path(sha, 768);
    EXPECT_NE(p512, p768);
    EXPECT_NE(p512.find("/512/"), std::string::npos) << p512;
    EXPECT_NE(p768.find("/768/"), std::string::npos) << p768;
    // The sha fanout survives under the size dir.
    EXPECT_NE(p512.find("/512/aa/"), std::string::npos) << p512;
}

TEST_F(ThumbsTest, RejectsNonPositiveSize) {
    std::string sha(64, 'b');
    EXPECT_EQ(get_thumb_path(sha, 0), "");
    EXPECT_EQ(get_thumb_path(sha, -1), "");
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(1, 2, 3));
    EXPECT_FALSE(generate_thumb(img, sha, 0));
}

TEST_F(ThumbsTest, TwoSizesCoexistNeitherClobbersTheOther) {
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(100, 150, 200));
    std::string sha(64, 'c');

    ASSERT_TRUE(generate_thumb(img, sha, 512));
    ASSERT_TRUE(generate_thumb(img, sha, 256));

    EXPECT_TRUE(thumb_exists(sha, 512));
    EXPECT_TRUE(thumb_exists(sha, 256));

    cv::Mat big = cv::imread(get_thumb_path(sha, 512));
    cv::Mat small = cv::imread(get_thumb_path(sha, 256));
    ASSERT_FALSE(big.empty());
    ASSERT_FALSE(small.empty());
    EXPECT_EQ(big.cols, 512);
    EXPECT_EQ(small.cols, 256);
}

TEST_F(ThumbsTest, ExistsIsPerSize) {
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(10, 20, 30));
    std::string sha(64, 'd');
    ASSERT_TRUE(generate_thumb(img, sha, 768));
    EXPECT_TRUE(thumb_exists(sha, 768));
    // A thumbnail at another size must not satisfy this size.
    EXPECT_FALSE(thumb_exists(sha, 512));
}

TEST_F(ThumbsTest, MigrationMovesFlatCacheIntoSizeDirWithoutReencoding) {
    // Lay out a thumbnail the way phig wrote them before sizes were in the path.
    std::string sha(64, 'e');
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(7, 8, 9));
    ASSERT_TRUE(generate_thumb(img, sha, PHIG_DEFAULT_THUMB_SIZE));
    fs::path modern = get_thumb_path(sha, PHIG_DEFAULT_THUMB_SIZE);
    fs::path legacy = fs::path(thumbs_dir()) / sha.substr(0, 2) / (sha + ".jpg");
    fs::create_directories(legacy.parent_path());
    fs::rename(modern, legacy);
    ASSERT_TRUE(fs::exists(legacy));
    ASSERT_FALSE(fs::exists(modern));
    auto before = fs::file_size(legacy);

    EXPECT_EQ(migrate_flat_thumb_cache(), 1);

    EXPECT_TRUE(fs::exists(modern));
    EXPECT_FALSE(fs::exists(legacy));
    // Same bytes: a rename, never a re-encode.
    EXPECT_EQ(fs::file_size(modern), before);
    // The emptied legacy fanout dir is cleaned up.
    EXPECT_FALSE(fs::exists(legacy.parent_path()));
}

TEST_F(ThumbsTest, MigrationIsIdempotentAndIgnoresSizeDirs) {
    std::string sha(64, 'f');
    cv::Mat img(600, 1000, CV_8UC3, cv::Scalar(4, 5, 6));
    ASSERT_TRUE(generate_thumb(img, sha, PHIG_DEFAULT_THUMB_SIZE));

    // Nothing at the legacy layout: a no-op that must not disturb the size dirs.
    EXPECT_EQ(migrate_flat_thumb_cache(), 0);
    EXPECT_TRUE(thumb_exists(sha, PHIG_DEFAULT_THUMB_SIZE));
    EXPECT_EQ(migrate_flat_thumb_cache(), 0);
    EXPECT_TRUE(thumb_exists(sha, PHIG_DEFAULT_THUMB_SIZE));
}

TEST_F(ThumbsTest, WritingAThumbnailTagsTheCacheDirectory) {
    cv::Mat img(64, 64, CV_8UC3, cv::Scalar(10, 20, 30));
    const std::string sha(64, 'a');
    ASSERT_TRUE(generate_thumb(img, sha, 128));
    const fs::path tag = fs::path(thumbs_dir()) / PHIG_CACHEDIR_TAG;
    ASSERT_TRUE(fs::exists(tag));
    std::ifstream in(tag);
    std::string first;
    std::getline(in, first);
    EXPECT_EQ(first, "Signature: 8a477f597d28d172789f06886806bc55");
}
