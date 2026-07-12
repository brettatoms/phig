#include <gtest/gtest.h>

#include <filesystem>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "raw.h"

namespace fs = std::filesystem;

class RawDecodeTest : public ::testing::Test {
   protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "image-cleanup-test-raw";
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
};

// Tier 1: a real sidecar JPEG beside the RAW is used directly (no LibRaw
// decode of the CR2 itself — the CR2 here is a truncated fixture that LibRaw
// cannot decode, proving tier 1 never touches it).
TEST_F(RawDecodeTest, UsesSidecarJpegWhenPresent) {
    create_image("IMG_1.JPG", 64, 48, cv::Scalar(0, 128, 255));

    fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
    fs::path raw_path = fs::path(test_dir) / "IMG_1.CR2";
    fs::copy_file(fixture, raw_path);

    cv::Mat result = raw_decode(raw_path.string());

    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result.cols, 64);
    EXPECT_EQ(result.rows, 48);
}

// No sidecar, undecodable RAW -> empty Mat, no crash (graceful failure path).
// The truncated fixture rejects LibRaw's own unpack/decode (verified with
// dcraw_emu/identify), so this only exercises tiers 2+3 failing gracefully;
// the LibRaw-success path (valid CR2 -> preview or full decode) has no
// committable fixture and is covered by real use and the Task 8 e2e instead.
TEST_F(RawDecodeTest, TruncatedRawWithNoSidecarReturnsEmpty) {
    fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
    fs::path raw_path = fs::path(test_dir) / "truncated.cr2";
    fs::copy_file(fixture, raw_path);

    cv::Mat result;
    EXPECT_NO_THROW(result = raw_decode(raw_path.string()));
    EXPECT_TRUE(result.empty());
}

TEST_F(RawDecodeTest, NonExistentFileReturnsEmpty) {
    cv::Mat result;
    EXPECT_NO_THROW(result = raw_decode((fs::path(test_dir) / "missing.cr2").string()));
    EXPECT_TRUE(result.empty());
}
