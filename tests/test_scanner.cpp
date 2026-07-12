#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "scanner.h"

namespace fs = std::filesystem;

class ScannerTest : public ::testing::Test {
   protected:
    std::string test_dir;

    void SetUp() override {
        test_dir = fs::temp_directory_path() / "image-cleanup-test-scanner";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override { fs::remove_all(test_dir); }

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

TEST_F(ScannerTest, SkipsHiddenAndTempFiles) {
    create_file("photo.jpg");
    create_file(".hidden.jpg");
    create_file("~temp.jpg");
    create_file(".pending-123-video.mp4");  // sync temp file
    auto files = scan_directory(test_dir, false);
    EXPECT_EQ(files.size(), 1);
}

TEST_F(ScannerTest, IncludeHiddenReincludesFiles) {
    create_file("photo.jpg");
    create_file(".hidden.jpg");
    create_file("~temp.jpg");
    auto files = scan_directory(test_dir, false, /*include_hidden=*/true);
    EXPECT_EQ(files.size(), 3);
}

TEST_F(ScannerTest, RecursiveSkipsHiddenDirectories) {
    create_file("photo.jpg");
    create_file(".thumbnails/cached.jpg");
    create_file("~tmp/scratch.png");
    auto files = scan_directory(test_dir, true);
    EXPECT_EQ(files.size(), 1);

    // With --hidden, files under hidden dirs are included.
    auto all = scan_directory(test_dir, true, /*include_hidden=*/true);
    EXPECT_EQ(all.size(), 3);
}

TEST_F(ScannerTest, HiddenScanRootIsHonored) {
    // A hidden directory named explicitly as the scan root must be scanned,
    // even without --hidden.
    create_file(".album/photo.jpg");
    create_file(".album/other.png");
    auto root = (fs::path(test_dir) / ".album").string();
    auto files = scan_directory(root, true);
    EXPECT_EQ(files.size(), 2);
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

TEST(ScannerExtensionTest, IsRawExtension) {
    EXPECT_TRUE(is_raw_extension("cr2"));
    EXPECT_TRUE(is_raw_extension("CR2"));
    EXPECT_TRUE(is_raw_extension(".nef"));
    EXPECT_FALSE(is_raw_extension("jpg"));
}

TEST_F(ScannerTest, HasSiblingRawTrueWhenRawBeside) {
    create_file("IMG_1.JPG");
    create_file("IMG_1.CR2");
    EXPECT_TRUE(has_sibling_raw(fs::path(test_dir) / "IMG_1.JPG"));
}

TEST_F(ScannerTest, HasSiblingRawFalseWhenLoneJpeg) {
    create_file("IMG_2.JPG");
    EXPECT_FALSE(has_sibling_raw(fs::path(test_dir) / "IMG_2.JPG"));
}

TEST_F(ScannerTest, HasSiblingRawFalseForRawItself) {
    create_file("IMG_1.JPG");
    create_file("IMG_1.CR2");
    EXPECT_FALSE(has_sibling_raw(fs::path(test_dir) / "IMG_1.CR2"));
}

TEST_F(ScannerTest, HasSiblingRawCaseInsensitiveExtension) {
    create_file("IMG_1.jpg");
    create_file("IMG_1.NEF");
    EXPECT_TRUE(has_sibling_raw(fs::path(test_dir) / "IMG_1.jpg"));
}

TEST_F(ScannerTest, HasSiblingRawCaseInsensitiveStem) {
    create_file("img_1.JPG");
    create_file("IMG_1.cr2");
    EXPECT_TRUE(has_sibling_raw(fs::path(test_dir) / "img_1.JPG"));
}

TEST_F(ScannerTest, HasSiblingRawFalseForNonJpegWithSiblingRaw) {
    create_file("IMG_1.png");
    create_file("IMG_1.CR2");
    EXPECT_FALSE(has_sibling_raw(fs::path(test_dir) / "IMG_1.png"));
}
