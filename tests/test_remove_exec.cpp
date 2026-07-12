#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "database.h"
#include "paths.h"
#include "remove_exec.h"
#include "types.h"

namespace fs = std::filesystem;

class RemoveExecTest : public ::testing::Test {
   protected:
    fs::path root;   // XDG_DATA_HOME
    fs::path photos; // where media lives
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        root = fs::temp_directory_path() / "phig-remove-exec-test";
        fs::remove_all(root);
        fs::create_directories(root);
        setenv("XDG_DATA_HOME", root.c_str(), 1);
        photos = root / "photos";
        fs::create_directories(photos);
        db_path = (root / "t.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        unsetenv("XDG_DATA_HOME");
        fs::remove_all(root);
    }

    void write_file(const fs::path& p, const std::string& c) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << c;
    }

    // Insert a minimal images row for `path`.
    void insert_row(const std::string& path) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.file_size = 1;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = "s";
        info.phash = 0;
        info.width = 1;
        info.height = 1;
        info.metadata_json = "{}";
        db->insert(info);
    }

    // Count operation_entries rows of a given kind for a src path.
    int count_entries(const std::string& kind, const std::string& src) {
        std::string sql = "SELECT COUNT(*) FROM operation_entries WHERE kind='" + kind +
                          "' AND src='" + src + "'";
        int n = 0;
        sqlite3_exec(
            db->db_ptr(), sql.c_str(),
            [](void* out, int, char** v, char**) {
                *static_cast<int*>(out) = std::atoi(v[0]);
                return 0;
            },
            &n, nullptr);
        return n;
    }
};

TEST_F(RemoveExecTest, TrashMovesFileAndLogs) {
    std::string src = (photos / "IMG_1.jpg").string();
    write_file(src, "data");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    std::string dst = execute_trash_action(*db, src, op);

    EXPECT_FALSE(fs::exists(src));
    EXPECT_TRUE(fs::exists(dst));
    EXPECT_EQ(fs::path(dst).parent_path(), fs::path(trash_dir()));
    EXPECT_EQ(count_entries("trash", src), 1);
}

TEST_F(RemoveExecTest, TrashSuffixesOnCollisionAndLogsSuffixedPath) {
    std::string a = (photos / "IMG_1.jpg").string();
    std::string b = (photos / "sub" / "IMG_1.jpg").string();  // same filename
    write_file(a, "a");
    write_file(b, "b");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    std::string da = execute_trash_action(*db, a, op);
    std::string db2 = execute_trash_action(*db, b, op);

    EXPECT_EQ(fs::path(da).filename().string(), "IMG_1.jpg");
    EXPECT_EQ(fs::path(db2).filename().string(), "IMG_1.jpg.1");
    EXPECT_TRUE(fs::exists(da));
    EXPECT_TRUE(fs::exists(db2));

    // The log records the actual (suffixed) trash path in dst, so a future undo
    // restores from the right place. Verify dst for the second file.
    std::string dst_logged;
    std::string sql = "SELECT dst FROM operation_entries WHERE kind='trash' AND src='" + b + "'";
    sqlite3_exec(
        db->db_ptr(), sql.c_str(),
        [](void* out, int, char** v, char**) {
            *static_cast<std::string*>(out) = v[0] ? v[0] : "";
            return 0;
        },
        &dst_logged, nullptr);
    EXPECT_EQ(dst_logged, db2);
}

TEST_F(RemoveExecTest, HardDeleteRemovesAndLogs) {
    std::string src = (photos / "IMG_2.jpg").string();
    write_file(src, "data");
    int64_t op = db->log_begin_operation("rm", "phig rm --hard", "2026-07-12T00:00:00Z");

    execute_hard_delete_action(*db, src, op);

    EXPECT_FALSE(fs::exists(src));
    EXPECT_EQ(count_entries("delete_file", src), 1);
    EXPECT_FALSE(fs::exists(trash_dir() + "/IMG_2.jpg"));  // not trashed
}

TEST_F(RemoveExecTest, HardDeleteMissingFileStillLogs) {
    std::string src = (photos / "gone.jpg").string();  // never created
    int64_t op = db->log_begin_operation("rm", "phig rm --hard", "2026-07-12T00:00:00Z");

    EXPECT_NO_THROW(execute_hard_delete_action(*db, src, op));
    EXPECT_EQ(count_entries("delete_file", src), 1);
}

TEST_F(RemoveExecTest, RemoveMediaCarriesSidecarsNotSiblings) {
    std::string raw = (photos / "IMG_9.CR2").string();
    std::string jpg = (photos / "IMG_9.JPG").string();   // same-stem sibling
    std::string xmp = (photos / "IMG_9.CR2.xmp").string();    // sidecar (append style)
    write_file(raw, "raw");
    write_file(jpg, "jpg");
    write_file(xmp, "xmp");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    auto acted = remove_media_with_sidecars(*db, raw, /*hard=*/false, op);

    // RAW and its sidecar are trashed; the JPEG sibling is left alone.
    EXPECT_FALSE(fs::exists(raw));
    EXPECT_FALSE(fs::exists(xmp));
    EXPECT_TRUE(fs::exists(jpg));
    // acted contains raw + xmp (order: media first).
    ASSERT_EQ(acted.size(), 2u);
    EXPECT_EQ(acted[0], raw);
    EXPECT_EQ(count_entries("trash", raw), 1);
    EXPECT_EQ(count_entries("trash", xmp), 1);
}

TEST_F(RemoveExecTest, RmTrashRemovesFileAndRow) {
    std::string p = (photos / "a.jpg").string();
    write_file(p, "x");
    insert_row(p);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*a.jpg"};
    opts.mode = RemoveMode::Trash;
    opts.assume_yes = true;
    opts.argv_str = "phig rm --match *a.jpg";

    int rc = cmd_rm(opts);
    EXPECT_EQ(rc, 0);
    EXPECT_FALSE(fs::exists(p));                    // file trashed
    EXPECT_EQ(db->get_all_images().size(), 0u);     // row gone
}

TEST_F(RemoveExecTest, RmDbOnlyKeepsFile) {
    std::string p = (photos / "b.jpg").string();
    write_file(p, "x");
    insert_row(p);

    RemoveOptions opts;
    opts.db_path = db_path; opts.matches = {"*b.jpg"};
    opts.mode = RemoveMode::DbOnly; opts.assume_yes = true;
    opts.argv_str = "phig rm --db-only --match *b.jpg";

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(fs::exists(p));                      // file kept
    EXPECT_EQ(db->get_all_images().size(), 0u);      // row gone
}

TEST_F(RemoveExecTest, RmRequiresMatchExceptOrphans) {
    RemoveOptions opts;
    opts.db_path = db_path; opts.mode = RemoveMode::Trash; opts.assume_yes = true;
    EXPECT_EQ(cmd_rm(opts), 1);                      // no --match, non-orphans → error

    opts.mode = RemoveMode::Orphans;
    EXPECT_EQ(cmd_rm(opts), 0);                      // orphans with no rows → "none found", ok
}

TEST_F(RemoveExecTest, RmDryRunTouchesNothing) {
    std::string p = (photos / "c.jpg").string();
    write_file(p, "x");
    insert_row(p);

    RemoveOptions opts;
    opts.db_path = db_path; opts.matches = {"*c.jpg"};
    opts.mode = RemoveMode::Trash; opts.dry_run = true; opts.assume_yes = true;

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(fs::exists(p));                      // untouched
    EXPECT_EQ(db->get_all_images().size(), 1u);      // row kept
}

TEST_F(RemoveExecTest, RmOrphansDropsMissingKeepsPresent) {
    std::string present = (photos / "here.jpg").string();
    std::string missing = (photos / "gone.jpg").string();
    write_file(present, "x");
    write_file(missing, "x");
    insert_row(present);
    insert_row(missing);
    fs::remove(missing);  // now orphaned; parent dir still exists (mount-safe keeps present)

    RemoveOptions opts;
    opts.db_path = db_path; opts.mode = RemoveMode::Orphans; opts.assume_yes = true;
    opts.argv_str = "phig rm --orphans";

    EXPECT_EQ(cmd_rm(opts), 0);
    auto rows = db->get_all_images();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, present);                // present kept, missing dropped
}
