#include <gtest/gtest.h>
#include "exif.h"
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

class ExifTest : public ::testing::Test {
protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "image-cleanup-test-exif";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    std::string create_image(const std::string& name) {
        auto path = (fs::path(test_dir) / name).string();
        cv::Mat img(100, 100, CV_8UC3, cv::Scalar(128, 128, 128));
        cv::imwrite(path, img);
        return path;
    }

    std::string create_text_file(const std::string& name, const std::string& content) {
        auto path = (fs::path(test_dir) / name).string();
        std::ofstream(path) << content;
        return path;
    }
};

TEST_F(ExifTest, NoExifReturnsEmptyJson) {
    // OpenCV-generated images typically have no EXIF
    auto path = create_image("no_exif.jpg");
    auto json = extract_exif_json(path);
    EXPECT_EQ(json, "{}");
}

TEST_F(ExifTest, NonExistentFile) {
    auto json = extract_exif_json("/nonexistent/file.jpg");
    EXPECT_EQ(json, "{}");
}

TEST_F(ExifTest, NonImageFile) {
    auto path = create_text_file("text.txt", "not an image");
    auto json = extract_exif_json(path);
    EXPECT_EQ(json, "{}");
}

TEST_F(ExifTest, EmptyFile) {
    auto path = create_text_file("empty.jpg", "");
    auto json = extract_exif_json(path);
    EXPECT_EQ(json, "{}");
}

TEST_F(ExifTest, PNGNoExif) {
    auto path = (fs::path(test_dir) / "test.png").string();
    cv::Mat img(100, 100, CV_8UC3, cv::Scalar(128, 128, 128));
    cv::imwrite(path, img);
    auto json = extract_exif_json(path);
    EXPECT_EQ(json, "{}");
}

TEST_F(ExifTest, ResultIsValidJson) {
    // Even with no EXIF, should return valid JSON
    auto path = create_image("test.jpg");
    auto json = extract_exif_json(path);
    // Must start with { and end with }
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}
