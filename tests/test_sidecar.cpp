#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "sidecar.h"

namespace fs = std::filesystem;

class SidecarTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-sidecar-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    void touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }
};

TEST_F(SidecarTest, StemStyleAttachesToImage) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.xmp");
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].filename(), "IMG_1234.xmp");
}

TEST_F(SidecarTest, AppendStyleAttachesToRaw) {
    touch(dir / "IMG_1234.CR2");
    touch(dir / "IMG_1234.CR2.xmp");
    auto s = find_sidecars(dir / "IMG_1234.CR2");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].filename(), "IMG_1234.CR2.xmp");
}

TEST_F(SidecarTest, RawAndJpegKeepDistinctSidecars) {
    touch(dir / "IMG_1234.CR2");
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.CR2.xmp");  // belongs to the CR2
    touch(dir / "IMG_1234.xmp");      // belongs to the JPG (stem style)

    auto raw = find_sidecars(dir / "IMG_1234.CR2");
    ASSERT_EQ(raw.size(), 1u);
    EXPECT_EQ(raw[0].filename(), "IMG_1234.CR2.xmp");

    auto jpg = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(jpg.size(), 1u);
    EXPECT_EQ(jpg[0].filename(), "IMG_1234.xmp");
}

TEST_F(SidecarTest, NonAllowlistSameStemIgnored) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.txt");  // not a sidecar ext
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    EXPECT_TRUE(s.empty());
}

TEST_F(SidecarTest, MultipleSidecarsReturned) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.xmp");
    touch(dir / "IMG_1234.aae");
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    EXPECT_EQ(s.size(), 2u);
}

TEST_F(SidecarTest, NoSidecarIsEmpty) {
    touch(dir / "IMG_1234.JPG");
    EXPECT_TRUE(find_sidecars(dir / "IMG_1234.JPG").empty());
}

TEST_F(SidecarTest, ExtensionMatchIsCaseInsensitive) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.XMP");
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].filename(), "IMG_1234.XMP");
}
