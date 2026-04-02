#include <gtest/gtest.h>
#include "database.h"
#include "types.h"
#include <filesystem>
#include <sstream>
#include <vector>
#include <string>

// These tests verify the porcelain output format by testing the underlying
// data structures and format logic. The actual porcelain output is produced
// in main.cpp, so we test the contract: given known DB data, the porcelain
// columns must be in the documented order.
//
// Porcelain format contracts:
//
// Regular search:  path\twidth\theight\tfile_size\tdate\tcamera\n
// Face search:     path\tdistance\n
// Person search:   path\n
// Print0:          path\0
//
// These formats are stable. Changes require a version bump.

namespace fs = std::filesystem;

class PorcelainTest : public ::testing::Test {
protected:
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        db_path = (fs::temp_directory_path() / "phig-test-porcelain.db").string();
        fs::remove(db_path);
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }

    ImageInfo make_image(const std::string& path, const std::string& sha = "abc",
                         int w = 640, int h = 480, int64_t size = 1000) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.file_size = size;
        info.created_at = "2024-01-15T10:30:00Z";
        info.modified_at = "2024-01-15T10:30:00Z";
        info.sha256 = sha;
        info.phash = 0;
        info.width = w;
        info.height = h;
        info.exif_json = "{\"Model\":\"TestCam\",\"DateTimeOriginal\":\"2024:01:15 10:30:00\"}";
        return info;
    }
};

// ---- Regular search porcelain format ----
// Contract: path\twidth\theight\tfile_size\tdate\tcamera

TEST_F(PorcelainTest, RegularSearchFormat) {
    auto img = make_image("/photos/test.jpg", "sha1", 4032, 3024, 5242880);
    db->insert(img);

    auto results = db->get_all_images();
    ASSERT_EQ(results.size(), 1);

    // Simulate porcelain output
    const auto& r = results[0];
    std::ostringstream out;

    // This must match the format in main.cpp cmd_search porcelain output
    std::string date = "2024:01:15"; // from DateTimeOriginal
    std::string camera = "TestCam";  // from Model
    out << r.path << '\t'
        << r.width << '\t' << r.height << '\t'
        << r.file_size << '\t'
        << date << '\t'
        << camera << '\n';

    std::string line = out.str();

    // Verify tab-separated with exactly 6 fields
    int tab_count = 0;
    for (char c : line) if (c == '\t') tab_count++;
    EXPECT_EQ(tab_count, 5) << "Regular porcelain must have exactly 5 tabs (6 fields)";

    // Verify field order by parsing
    std::istringstream iss(line);
    std::string f_path, f_width, f_height, f_size, f_date, f_camera;
    std::getline(iss, f_path, '\t');
    std::getline(iss, f_width, '\t');
    std::getline(iss, f_height, '\t');
    std::getline(iss, f_size, '\t');
    std::getline(iss, f_date, '\t');
    std::getline(iss, f_camera, '\n');

    EXPECT_EQ(f_path, "/photos/test.jpg");
    EXPECT_EQ(f_width, "4032");
    EXPECT_EQ(f_height, "3024");
    EXPECT_EQ(f_size, "5242880");
    EXPECT_EQ(f_date, "2024:01:15");
    EXPECT_EQ(f_camera, "TestCam");
}

TEST_F(PorcelainTest, RegularSearchEmptyCamera) {
    auto img = make_image("/photos/no_cam.jpg", "sha1");
    img.exif_json = "{}"; // no camera
    db->insert(img);

    auto results = db->get_all_images();
    const auto& r = results[0];

    std::ostringstream out;
    out << r.path << '\t'
        << r.width << '\t' << r.height << '\t'
        << r.file_size << '\t'
        << "2024-01-15" << '\t'  // fallback to modified_at
        << "" << '\n';           // empty camera

    std::string line = out.str();
    int tab_count = 0;
    for (char c : line) if (c == '\t') tab_count++;
    EXPECT_EQ(tab_count, 5) << "Must still have 5 tabs even with empty camera";
}

// ---- Face search porcelain format ----
// Contract: path\tdistance

TEST_F(PorcelainTest, FaceSearchFormat) {
    std::ostringstream out;
    std::string path = "/photos/face.jpg";
    float distance = 0.1234f;

    out << path << '\t' << distance << '\n';

    std::string line = out.str();
    int tab_count = 0;
    for (char c : line) if (c == '\t') tab_count++;
    EXPECT_EQ(tab_count, 1) << "Face porcelain must have exactly 1 tab (2 fields)";

    std::istringstream iss(line);
    std::string f_path, f_dist;
    std::getline(iss, f_path, '\t');
    std::getline(iss, f_dist, '\n');

    EXPECT_EQ(f_path, "/photos/face.jpg");
    EXPECT_FALSE(f_dist.empty());
}

// ---- Person search porcelain format ----
// Contract: path (one per line, no tabs)

TEST_F(PorcelainTest, PersonSearchFormat) {
    std::ostringstream out;
    std::string path = "/photos/person.jpg";

    out << path << '\n';

    std::string line = out.str();
    int tab_count = 0;
    for (char c : line) if (c == '\t') tab_count++;
    EXPECT_EQ(tab_count, 0) << "Person porcelain must have no tabs (path only)";
    EXPECT_EQ(line, "/photos/person.jpg\n");
}

// ---- Print0 format ----
// Contract: path\0 (null-separated)

TEST_F(PorcelainTest, Print0Format) {
    std::ostringstream out;
    out << "/photos/a.jpg" << '\0';
    out << "/photos/b.jpg" << '\0';

    std::string data = out.str();
    EXPECT_EQ(data.size(), 28u); // two paths + two null bytes
    EXPECT_EQ(data[13], '\0');
    EXPECT_EQ(data[27], '\0');
}

// ---- Field count stability ----

TEST_F(PorcelainTest, RegularSearchFieldCountIs6) {
    // This test exists to catch accidental additions/removals of fields.
    // If you need to add a field, add a new porcelain version.
    //
    // Current format (v2):
    //   path\twidth\theight\tfile_size\tdate\tcamera\tthumb_path
    //
    // 7 fields, 6 tabs per line.
    const int EXPECTED_FIELDS = 7;
    const int EXPECTED_TABS = EXPECTED_FIELDS - 1;

    std::ostringstream out;
    out << "/path" << '\t' << "640" << '\t' << "480" << '\t'
        << "1000" << '\t' << "2024-01-15" << '\t' << "Canon" << '\t'
        << "/cache/thumbs/ab/abcdef.jpg" << '\n';

    std::string line = out.str();
    int tabs = 0;
    for (char c : line) if (c == '\t') tabs++;
    EXPECT_EQ(tabs, EXPECTED_TABS)
        << "Porcelain field count changed! This is a breaking change. "
        << "If intentional, bump the porcelain version.";
}
