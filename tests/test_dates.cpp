#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "dates.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

class GetBestDateTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override { dir = make_temp_dir("dates"); }
    void TearDown() override { fs::remove_all(dir); }

    void write(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << contents;
    }
};

// A file with no EXIF date but a dated .xmp sidecar dates from the sidecar
// (folder placement), not mtime.
TEST_F(GetBestDateTest, SidecarUsedWhenNoExif) {
    write(dir / "IMG_1.xmp",
          R"(<x:xmpmeta><rdf:Description xmp:CreateDate="11/25/2010 8:34:57 AM">)"
          R"(</rdf:Description></x:xmpmeta>)");

    ImageInfo img;
    img.path = (dir / "IMG_1.JPG").string();
    img.metadata_json = "{}";
    img.modified_at = "2026-07-11T00:00:00Z";

    auto d = get_best_date(img);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.year, "2010");
    EXPECT_EQ(d.month, "11");
}

// EXIF wins over a differently-dated sidecar.
TEST_F(GetBestDateTest, ExifWinsOverSidecar) {
    write(dir / "IMG_1.xmp",
          R"(<x:xmpmeta><rdf:Description xmp:CreateDate="11/25/2010 8:34:57 AM">)"
          R"(</rdf:Description></x:xmpmeta>)");

    ImageInfo img;
    img.path = (dir / "IMG_1.JPG").string();
    img.metadata_json = R"({"DateTimeOriginal":"2015:01:02 10:00:00"})";
    img.modified_at = "2026-07-11T00:00:00Z";

    auto d = get_best_date(img);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.year, "2015");
}

// No EXIF, no sidecar -> mtime (unchanged behavior).
TEST_F(GetBestDateTest, FallsBackToMtimeWhenNothingElse) {
    ImageInfo img;
    img.path = (dir / "IMG_1.JPG").string();
    img.metadata_json = "{}";
    img.modified_at = "2020:05:06 00:00:00";

    auto d = get_best_date(img);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.year, "2020");
}

class GetBestDateStrTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override { dir = make_temp_dir("dates-str"); }
    void TearDown() override { fs::remove_all(dir); }

    void write(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << contents;
    }
};

// Display string also uses the sidecar (search output agrees with organize).
TEST_F(GetBestDateStrTest, SidecarUsedWhenNoExif) {
    write(dir / "IMG_1.xmp",
          R"(<x:xmpmeta><rdf:Description xmp:CreateDate="11/25/2010 8:34:57 AM">)"
          R"(</rdf:Description></x:xmpmeta>)");

    ImageInfo img;
    img.path = (dir / "IMG_1.JPG").string();
    img.metadata_json = "{}";
    img.modified_at = "2026-07-11T00:00:00Z";

    EXPECT_EQ(get_best_date_str(img), "2010:11:25");
}
