#include <gtest/gtest.h>
#include "scanner.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class ScannerTest : public ::testing::Test {
protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "image-cleanup-test-scanner";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    void create_file(const std::string& relative_path) {
        auto path = fs::path(test_dir) / relative_path;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "dummy";
    }
};

TEST_F(ScannerTest, EmptyDirectory) {
    auto files = scan_directory(test_dir, false);
    EXPECT_TRUE(files.empty());
}

TEST_F(ScannerTest, NonExistentDirectory) {
    EXPECT_THROW(scan_directory("/nonexistent/path", false), std::runtime_error);
}

TEST_F(ScannerTest, NotADirectory) {
    create_file("file.jpg");
    auto file_path = fs::path(test_dir) / "file.jpg";
    EXPECT_THROW(scan_directory(file_path.string(), false), std::runtime_error);
}

TEST_F(ScannerTest, FindsImageFiles) {
    create_file("photo.jpg");
    create_file("image.png");
    create_file("raw.cr2");
    auto files = scan_directory(test_dir, false);
    EXPECT_EQ(files.size(), 3);
}

TEST_F(ScannerTest, SkipsUnknownExtensions) {
    create_file("photo.jpg");
    create_file("document.pdf");
    create_file("data.db");
    create_file(".DS_Store");
    create_file("notes.txt");
    auto files = scan_directory(test_dir, false);
    EXPECT_EQ(files.size(), 1);
}

TEST_F(ScannerTest, SkipsMacResourceForks) {
    create_file("photo.jpg");
    create_file("._photo.jpg");
    create_file("._another.png");
    auto files = scan_directory(test_dir, false);
    EXPECT_EQ(files.size(), 1);
}

TEST_F(ScannerTest, NonRecursiveSkipsSubdirs) {
    create_file("photo.jpg");
    create_file("subdir/nested.jpg");
    auto files = scan_directory(test_dir, false);
    EXPECT_EQ(files.size(), 1);
}

TEST_F(ScannerTest, RecursiveIncludesSubdirs) {
    create_file("photo.jpg");
    create_file("subdir/nested.jpg");
    create_file("subdir/deep/more.png");
    auto files = scan_directory(test_dir, true);
    EXPECT_EQ(files.size(), 3);
}

TEST_F(ScannerTest, KnownImageExtensions) {
    // Common
    EXPECT_TRUE(is_known_image_extension("jpg"));
    EXPECT_TRUE(is_known_image_extension("jpeg"));
    EXPECT_TRUE(is_known_image_extension("png"));
    EXPECT_TRUE(is_known_image_extension("gif"));
    EXPECT_TRUE(is_known_image_extension("bmp"));
    EXPECT_TRUE(is_known_image_extension("tiff"));
    EXPECT_TRUE(is_known_image_extension("tif"));
    EXPECT_TRUE(is_known_image_extension("webp"));

    // RAW
    EXPECT_TRUE(is_known_image_extension("cr2"));
    EXPECT_TRUE(is_known_image_extension("cr3"));
    EXPECT_TRUE(is_known_image_extension("nef"));
    EXPECT_TRUE(is_known_image_extension("arw"));
    EXPECT_TRUE(is_known_image_extension("dng"));
    EXPECT_TRUE(is_known_image_extension("raf"));

    // Other
    EXPECT_TRUE(is_known_image_extension("heic"));
    EXPECT_TRUE(is_known_image_extension("heif"));
    EXPECT_TRUE(is_known_image_extension("avif"));
    EXPECT_TRUE(is_known_image_extension("psd"));

    // Case insensitive
    EXPECT_TRUE(is_known_image_extension("JPG"));
    EXPECT_TRUE(is_known_image_extension("Png"));
    EXPECT_TRUE(is_known_image_extension(".jpg"));

    // Not images
    EXPECT_FALSE(is_known_image_extension("pdf"));
    EXPECT_FALSE(is_known_image_extension("txt"));
    EXPECT_FALSE(is_known_image_extension("db"));
    EXPECT_FALSE(is_known_image_extension(""));
}
