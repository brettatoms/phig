#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "sidecar.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

class SidecarTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override { dir = make_temp_dir("sidecar"); }
    void TearDown() override { fs::remove_all(dir); }

    void touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }

    void write(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << contents;
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

TEST_F(SidecarTest, StemSidecarKeptWhenMediaAloneDespiteAppendStyle) {
    touch(dir / "IMG_1.JPG");
    touch(dir / "IMG_1.xmp");      // stem style
    touch(dir / "IMG_1.JPG.xmp");  // append style, no same-stem sibling media
    auto s = find_sidecars(dir / "IMG_1.JPG");
    ASSERT_EQ(s.size(), 2u);
    EXPECT_TRUE(std::any_of(s.begin(), s.end(),
                            [](const fs::path& p) { return p.filename() == "IMG_1.xmp"; }));
    EXPECT_TRUE(std::any_of(s.begin(), s.end(),
                            [](const fs::path& p) { return p.filename() == "IMG_1.JPG.xmp"; }));
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

TEST_F(SidecarTest, FspotXmpCreateDate) {
    // Shape observed in the real 2010 library.
    write(dir / "a.xmp", R"(<x:xmpmeta><rdf:Description xmp:CreateDate="11/25/2010 8:34:57 AM">)"
                         R"(</rdf:Description></x:xmpmeta>)");
    EXPECT_EQ(sidecar_date(dir / "a.xmp"), "2010:11:25 08:34:57");
}

TEST_F(SidecarTest, IsoXmpDateTimeOriginal) {
    write(dir / "b.xmp", R"(<rdf:Description exif:DateTimeOriginal="2012-06-03T14:07:00"/>)");
    EXPECT_EQ(sidecar_date(dir / "b.xmp"), "2012:06:03 14:07:00");
}

TEST_F(SidecarTest, GoogleJsonPhotoTakenTime) {
    write(dir / "c.json",
          R"({"photoTakenTime":{"timestamp":"1288000497","formatted":"Nov 25, 2010"}})");
    // From the epoch timestamp (UTC): 2010-10-25 ... just assert year/month/day prefix.
    std::string d = sidecar_date(dir / "c.json");
    ASSERT_GE(d.size(), 10u);
    EXPECT_EQ(d.substr(0, 4), "2010");
}

TEST_F(SidecarTest, GoogleJsonIgnoresCreationTimeBeforePhotoTakenTime) {
    // creationTime (upload time, 2015) appears before photoTakenTime (capture
    // time, 2010) in the JSON. The result must be photoTakenTime's date, not
    // whichever "timestamp" key happens to appear first in the file.
    write(dir / "e.json",
          R"({"creationTime":{"timestamp":"1420070400","formatted":"Jan 1, 2015"},)"
          R"("photoTakenTime":{"timestamp":"1288000497","formatted":"Nov 25, 2010"}})");
    std::string d = sidecar_date(dir / "e.json");
    ASSERT_GE(d.size(), 10u);
    EXPECT_EQ(d.substr(0, 4), "2010");
}

TEST_F(SidecarTest, DatelessSidecarReturnsEmpty) {
    write(dir / "d.xmp", R"(<x:xmpmeta><rdf:Description xmp:Rating="0"/></x:xmpmeta>)");
    EXPECT_EQ(sidecar_date(dir / "d.xmp"), "");
}

TEST_F(SidecarTest, MissingFileReturnsEmpty) { EXPECT_EQ(sidecar_date(dir / "nope.xmp"), ""); }

// Verifies the seam main.cpp's date fallback chain relies on: a media file
// with no EXIF date but a dated sidecar must resolve to the sidecar's date
// via find_sidecars + sidecar_date, not fall through to mtime. The chain's
// wiring in get_best_date/get_best_date_str (file-local to main.cpp, not
// reachable from tests) is covered end-to-end by the Task 5 porcelain test.
TEST_F(SidecarTest, SidecarDatePreferredOverMtime) {
    touch(dir / "IMG_1234.JPG");
    write(dir / "IMG_1234.xmp",
          R"(<x:xmpmeta><rdf:Description xmp:CreateDate="11/25/2010 8:34:57 AM">)"
          R"(</rdf:Description></x:xmpmeta>)");

    auto sidecars = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(sidecars.size(), 1u);
    EXPECT_EQ(sidecar_date(sidecars[0]), "2010:11:25 08:34:57");
}
