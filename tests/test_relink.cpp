#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "database.h"
#include "faces.h"
#include "relink.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

class RelinkTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = make_temp_dir("relink");
        db_path = (dir / "t.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove_all(dir);
    }

    // Insert a row; when on_disk, also create the real file it points at.
    std::string add_row(const std::string& rel, const std::string& sha, bool on_disk) {
        fs::path p = dir / rel;
        if (on_disk) {
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "bytes-" << sha;
        }
        ImageInfo info;
        info.path = p.string();
        info.filename = p.filename().string();
        info.extension = "jpg";
        info.file_size = 10;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = sha;
        info.phash = 0;
        info.width = 8;
        info.height = 8;
        info.metadata_json = "{}";
        db->insert(info);
        return p.string();
    }

    static int64_t count_rows(sqlite3* h, const std::string& sql) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return -1;
        int64_t n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
        sqlite3_finalize(s);
        return n;
    }
};

TEST_F(RelinkTest, SingleCandidateTransfersMetadata) {
    std::string old_path = add_row("old/a.jpg", "sameSha", /*on_disk=*/false);
    std::string new_path = add_row("new/a.jpg", "sameSha", /*on_disk=*/true);
    db->set_tag({old_path}, "favorite", true);
    db->set_gallery({old_path}, "summer", false);

    int n = relink_moved_rows(*db, {old_path}, 0);

    EXPECT_EQ(n, 1);
    auto tags = db->get_tags_for_image(new_path);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
    for (const auto& img : db->get_all_images()) {
        if (img.path == new_path) EXPECT_EQ(img.gallery, "summer");
    }
}

TEST_F(RelinkTest, NoCandidateDoesNothing) {
    std::string old_path = add_row("old/a.jpg", "uniqueSha", false);
    db->set_tag({old_path}, "favorite", true);
    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 0);
}

TEST_F(RelinkTest, AmbiguousCandidatesSkipped) {
    std::string old_path = add_row("old/a.jpg", "dupSha", false);
    std::string c1 = add_row("new/a.jpg", "dupSha", true);
    std::string c2 = add_row("new/b.jpg", "dupSha", true);
    db->set_tag({old_path}, "favorite", true);

    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 0);
    EXPECT_TRUE(db->get_tags_for_image(c1).empty());
    EXPECT_TRUE(db->get_tags_for_image(c2).empty());
}

TEST_F(RelinkTest, CandidateMissingOnDiskSkipped) {
    std::string old_path = add_row("old/a.jpg", "sameSha", false);
    add_row("new/a.jpg", "sameSha", /*on_disk=*/false);  // row exists, file doesn't
    db->set_tag({old_path}, "favorite", true);
    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 0);
}

TEST_F(RelinkTest, MissingCandidateFileDoesNotCountTowardAmbiguity) {
    // One on-disk candidate + one stale row: still an unambiguous move.
    std::string old_path = add_row("old/a.jpg", "sameSha", false);
    std::string target = add_row("new/a.jpg", "sameSha", true);
    add_row("stale/a.jpg", "sameSha", false);
    db->set_tag({old_path}, "favorite", true);

    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 1);
    EXPECT_EQ(db->get_tags_for_image(target).size(), 1u);
}

TEST_F(RelinkTest, LogsRelinkEntryUnderOperation) {
    std::string old_path = add_row("old/a.jpg", "sameSha", false);
    std::string new_path = add_row("new/a.jpg", "sameSha", true);
    db->set_tag({old_path}, "favorite", true);

    int64_t op_id = db->log_begin_operation("scan", "phig scan test", "2026-07-11T00:00:00Z");
    EXPECT_EQ(relink_moved_rows(*db, {old_path}, op_id), 1);
    db->log_finish_operation(op_id, "completed");

    EXPECT_EQ(
        count_rows(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE kind = 'relink'"),
        1);
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM operations WHERE entry_count = 1"), 1);
}

TEST_F(RelinkTest, DemotesJpegToSameBasenameRaw) {
    std::string jpeg_path = add_row("a.jpg", "jpegSha", /*on_disk=*/true);
    std::string raw_path = add_row("a.cr2", "rawSha", /*on_disk=*/true);
    db->set_tag({jpeg_path}, "favorite", true);

    int64_t op_id = db->log_begin_operation("scan", "phig scan test", "2026-07-11T00:00:00Z");
    auto demoted = demote_jpeg_rows(*db, dir.string(), op_id);
    db->log_finish_operation(op_id, "completed");

    ASSERT_EQ(demoted.size(), 1u);
    EXPECT_EQ(demoted[0], jpeg_path);
    auto tags = db->get_tags_for_image(raw_path);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
    EXPECT_EQ(
        count_rows(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE kind = 'relink'"),
        1);
}

TEST_F(RelinkTest, LoneJpegNotDemoted) {
    std::string jpeg_path = add_row("a.jpg", "jpegSha", /*on_disk=*/true);
    db->set_tag({jpeg_path}, "favorite", true);

    auto demoted = demote_jpeg_rows(*db, dir.string(), 0);

    EXPECT_TRUE(demoted.empty());
    auto tags = db->get_tags_for_image(jpeg_path);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
}

TEST_F(RelinkTest, DeletedJpegWithIndexedRawNotDemoted) {
    // JPEG row exists (indexed earlier) but its file is now gone from disk.
    std::string jpeg_path = add_row("a.jpg", "jpegSha", /*on_disk=*/false);
    std::string raw_path = add_row("a.cr2", "rawSha", /*on_disk=*/true);
    db->set_tag({jpeg_path}, "favorite", true);

    auto demoted = demote_jpeg_rows(*db, dir.string(), 0);

    EXPECT_TRUE(demoted.empty());
    auto tags = db->get_tags_for_image(jpeg_path);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
}

TEST_F(RelinkTest, JpegWithRawRowMissingNotDemoted) {
    std::string jpeg_path = add_row("a.jpg", "jpegSha", /*on_disk=*/true);
    // RAW file on disk, but no DB row for it.
    fs::path raw_file = dir / "a.cr2";
    std::ofstream(raw_file) << "raw-bytes";
    db->set_tag({jpeg_path}, "favorite", true);

    auto demoted = demote_jpeg_rows(*db, dir.string(), 0);

    EXPECT_TRUE(demoted.empty());
    auto tags = db->get_tags_for_image(jpeg_path);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
}

TEST_F(RelinkTest, RelinkRunsBeforeDemoteOnSharedTarget) {
    // A RAW file moved (old/a.cr2 -> new/a.cr2) in the same scan that also
    // finds a JPEG sitting next to the RAW's new location. relink_moved_rows
    // and demote_jpeg_rows both target the RAW row at new/a.cr2: relink fills
    // its gallery from the moved row, demote fills it from the JPEG. Gallery
    // transfer only fills a NULL target, so whichever call runs first wins —
    // row/demoted counts are identical in both orders, only the surviving
    // gallery value differs.
    std::string raw_old = add_row("old/a.cr2", "rawSha", /*on_disk=*/false);
    std::string raw_new = add_row("new/a.cr2", "rawSha", /*on_disk=*/true);
    std::string jpeg = add_row("new/a.jpg", "jpegSha", /*on_disk=*/true);
    db->set_tag({raw_old}, "favorite", true);
    db->set_gallery({raw_old}, "trip1", false);
    db->set_gallery({jpeg}, "trip2", false);

    int64_t op_id = db->log_begin_operation("scan", "phig scan test", "2026-07-11T00:00:00Z");
    int relinked = relink_moved_rows(*db, {raw_old}, op_id);
    auto demoted = demote_jpeg_rows(*db, dir.string(), op_id);
    db->log_finish_operation(op_id, "completed");

    EXPECT_EQ(relinked, 1);
    ASSERT_EQ(demoted.size(), 1u);
    EXPECT_EQ(demoted[0], jpeg);

    auto tags = db->get_tags_for_image(raw_new);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");

    std::string gallery;
    for (const auto& img : db->get_all_images()) {
        if (img.path == raw_new) gallery = img.gallery;
    }
    EXPECT_EQ(gallery, "trip1") << "relink must fill the gallery before demote sees it, "
                                   "otherwise the JPEG's gallery wins instead";

    EXPECT_EQ(
        count_rows(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE kind = 'relink'"),
        2);
}
