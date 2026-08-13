#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "database.h"
#include "paths.h"
#include "phig_binary.h"
#include "remove_exec.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

class RemoveExecTest : public ::testing::Test {
   protected:
    fs::path root;    // XDG_DATA_HOME
    fs::path photos;  // where media lives
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        root = make_temp_dir("remove-exec");
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

    // Rows in `table` of the test database.
    int count_table(const std::string& table) {
        std::string sql = "SELECT COUNT(*) FROM " + table;
        int n = -1;
        sqlite3_exec(
            db->db_ptr(), sql.c_str(),
            [](void* out, int, char** v, char**) {
                *static_cast<int*>(out) = std::atoi(v[0]);
                return 0;
            },
            &n, nullptr);
        return n;
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

TEST_F(RemoveExecTest, TrashTwoSameNameFilesGetDistinctPathsNoClobberAndLogs) {
    std::string a = (photos / "IMG_1.jpg").string();
    std::string b = (photos / "sub" / "IMG_1.jpg").string();  // same filename
    write_file(a, "aaa");
    write_file(b, "bbb");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    std::string da = execute_trash_action(*db, a, op);
    std::string db2 = execute_trash_action(*db, b, op);

    // Distinct random-hex names: neither clobbers the other, both survive with
    // their original bytes intact.
    EXPECT_NE(da, db2);
    ASSERT_TRUE(fs::exists(da));
    ASSERT_TRUE(fs::exists(db2));
    auto read = [](const std::string& p) {
        std::ifstream in(p);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    EXPECT_EQ(read(da), "aaa");
    EXPECT_EQ(read(db2), "bbb");
    EXPECT_EQ(fs::path(da).parent_path(), fs::path(trash_dir()));
    EXPECT_EQ(fs::path(db2).parent_path(), fs::path(trash_dir()));

    // The log records the actual trash path in dst, so a future undo restores
    // from the right place. Verify dst for the second file.
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

TEST_F(RemoveExecTest, TrashManySameNameFilesAllSurviveDistinctly) {
    // Stress the random-hex naming + no-clobber move: many identically-named
    // files must all land in the trash at distinct paths, none overwritten.
    const int N = 50;
    std::set<std::string> dsts;
    for (int i = 0; i < N; i++) {
        std::string src = (photos / ("dir" + std::to_string(i)) / "SAME.jpg").string();
        write_file(src, "content-" + std::to_string(i));
        std::string dst = execute_trash_action(*db, src, 0);
        EXPECT_TRUE(dsts.insert(dst).second) << "duplicate trash path: " << dst;
        ASSERT_TRUE(fs::exists(dst));
        std::ifstream in(dst);
        std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(got, "content-" + std::to_string(i));
    }
    EXPECT_EQ(dsts.size(), static_cast<size_t>(N));
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
    std::string jpg = (photos / "IMG_9.JPG").string();      // same-stem sibling
    std::string xmp = (photos / "IMG_9.CR2.xmp").string();  // sidecar (append style)
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

// The other half of the same statement. A Live Photo's video is an attachment
// of its still, carried on mv and cp — but removal takes only find_sidecars'
// output, whose allowlist holds no media extension, so the video is left
// behind. The pair here is real: live_photo_still.jpg carries the identifier
// live_photo.mov carries, and both are given the same stem, which is the shape
// a stem-matching sidecar finder would take by mistake.
TEST_F(RemoveExecTest, RemoveMediaLeavesAPairedLivePhotoVideoAlone) {
    const std::string still = (photos / "shot.jpg").string();
    const std::string video = (photos / "shot.mov").string();
    fs::copy_file("tests/data/live_photo_still.jpg", still);
    fs::copy_file("tests/data/live_photo.mov", video);
    const auto video_size = fs::file_size(video);
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    auto acted = remove_media_with_sidecars(*db, still, /*hard=*/false, op);

    EXPECT_FALSE(fs::exists(still));
    ASSERT_TRUE(fs::exists(video)) << "an attachment deleted with its parent is not recoverable";
    EXPECT_EQ(fs::file_size(video), video_size);
    EXPECT_EQ(acted, std::vector<std::string>{still});
    EXPECT_EQ(count_entries("trash", video), 0) << "the video must not be logged as removed";
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
    EXPECT_FALSE(fs::exists(p));                 // file trashed
    EXPECT_EQ(db->get_all_images().size(), 0u);  // row gone
}

TEST_F(RemoveExecTest, RmDbOnlyKeepsFile) {
    std::string p = (photos / "b.jpg").string();
    write_file(p, "x");
    insert_row(p);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*b.jpg"};
    opts.mode = RemoveMode::DbOnly;
    opts.assume_yes = true;
    opts.argv_str = "phig rm --db-only --match *b.jpg";

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(fs::exists(p));                  // file kept
    EXPECT_EQ(db->get_all_images().size(), 0u);  // row gone
}

TEST_F(RemoveExecTest, RmRequiresMatchExceptOrphans) {
    RemoveOptions opts;
    opts.db_path = db_path;
    opts.mode = RemoveMode::Trash;
    opts.assume_yes = true;
    EXPECT_EQ(cmd_rm(opts), 1);  // no --match, non-orphans → error

    opts.mode = RemoveMode::Orphans;
    EXPECT_EQ(cmd_rm(opts), 0);  // orphans with no rows → "none found", ok
}

TEST_F(RemoveExecTest, RmDryRunTouchesNothing) {
    std::string p = (photos / "c.jpg").string();
    write_file(p, "x");
    insert_row(p);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*c.jpg"};
    opts.mode = RemoveMode::Trash;
    opts.dry_run = true;
    opts.assume_yes = true;

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(fs::exists(p));                  // untouched
    EXPECT_EQ(db->get_all_images().size(), 1u);  // row kept
    // The preview returns before log_begin_operation, so nothing about the
    // selection is recorded either.
    EXPECT_EQ(count_table("operations"), 0) << "a preview must not open an operation";
    EXPECT_EQ(count_table("operation_entries"), 0);
}

// The second half of the same rule, and the surprising half: what a dry run
// does not skip is the connection. cmd_rm constructs a Database and calls
// init_schema before it reads dry_run, so a preview against a path with no
// database there creates one at the current schema version. The selection is
// necessarily empty — a database that did not exist has no rows — which is
// exactly the run a caller expects to be inert.
TEST_F(RemoveExecTest, RmDryRunCreatesAndMigratesADatabaseThatWasNotThere) {
    const std::string fresh = (root / "fresh.db").string();
    ASSERT_FALSE(fs::exists(fresh));

    RemoveOptions opts;
    opts.db_path = fresh;
    opts.matches = {"*c.jpg"};
    opts.mode = RemoveMode::Trash;
    opts.dry_run = true;
    opts.assume_yes = true;

    EXPECT_EQ(cmd_rm(opts), 0);

    ASSERT_TRUE(fs::exists(fresh)) << "the preview left no database behind";
    Database opened(fresh);
    EXPECT_EQ(opened.schema_version(), Database::expected_schema_version())
        << "the preview created the file but left it un-migrated";
}

TEST_F(RemoveExecTest, ConfirmCallbackRefusalCancelsTheOperation) {
    // A caller that declines must leave everything untouched.
    std::string p = (photos / "d.jpg").string();
    write_file(p, "x");
    insert_row(p);

    bool asked = false;
    ConfirmFn decline = [&](const std::string& prompt) {
        asked = true;
        EXPECT_EQ(prompt, "\nThis will PERMANENTLY delete 1 file(s).\n");
        return false;
    };

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*d.jpg"};
    opts.mode = RemoveMode::Hard;  // --hard always needs confirmation

    int rc = cmd_rm(opts, decline);
    EXPECT_EQ(rc, 1);
    EXPECT_TRUE(asked);
    EXPECT_TRUE(fs::exists(p));                  // file untouched
    EXPECT_EQ(db->get_all_images().size(), 1u);  // row untouched
}

TEST_F(RemoveExecTest, ConfirmCallbackAcceptanceProceeds) {
    std::string p = (photos / "e.jpg").string();
    write_file(p, "x");
    insert_row(p);

    ConfirmFn accept = [](const std::string&) { return true; };

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*e.jpg"};
    opts.mode = RemoveMode::Hard;

    int rc = cmd_rm(opts, accept);
    EXPECT_EQ(rc, 0);
    EXPECT_FALSE(fs::exists(p));                 // file removed
    EXPECT_EQ(db->get_all_images().size(), 0u);  // row removed
}

TEST_F(RemoveExecTest, ConfirmCallbackNullRefusesWithoutAsking) {
    // A null callback must not silently proceed: it's equivalent to declining.
    std::string p = (photos / "f.jpg").string();
    write_file(p, "x");
    insert_row(p);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*f.jpg"};
    opts.mode = RemoveMode::Hard;

    int rc = cmd_rm(opts);  // default confirm = nullptr
    EXPECT_EQ(rc, 1);
    EXPECT_TRUE(fs::exists(p));
    EXPECT_EQ(db->get_all_images().size(), 1u);
}

TEST_F(RemoveExecTest, ConfirmCallbackBroadRemovalWarningTextIsExact) {
    // A broad (non-hard) removal composes a different warning than --hard;
    // pin its exact text too, since the two branches diverge.
    const int N = 20;  // matched.size() >= 20 and matched*2 >= total triggers "broad"
    for (int i = 0; i < N; i++) {
        std::string p = (photos / ("bulk" + std::to_string(i) + ".jpg")).string();
        write_file(p, "x");
        insert_row(p);
    }

    std::string seen_prompt;
    ConfirmFn decline = [&](const std::string& prompt) {
        seen_prompt = prompt;
        return false;
    };

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*bulk*.jpg"};
    opts.mode = RemoveMode::Trash;

    int rc = cmd_rm(opts, decline);
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(seen_prompt, "\nThis will remove 20 of 20 database entries (100%).\n");
    EXPECT_EQ(db->get_all_images().size(), static_cast<size_t>(N));  // untouched
}

// The isatty-based non-interactive refusal is CLI code (src/main.cpp), which
// isn't linked into phig-tests, so nothing above exercises it. Invoke the
// built `phig` binary as a subprocess to pin that the guard is still there.
TEST_F(RemoveExecTest, CliRefusesNonInteractiveConfirmationEvenWithPipedYes) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }

    // A small scratch collection scanned into its own DB, independent of the
    // fixture's `db` (this test drives the real `phig scan`/`phig rm` CLI).
    fs::path cli_root = make_temp_dir("remove-exec-cli");
    fs::path cli_photos = cli_root / "photos";
    fs::create_directories(cli_photos);
    const int N = 25;
    for (int i = 0; i < N; i++) {
        write_file(cli_photos / ("img" + std::to_string(i) + ".jpg"), "not-really-a-jpeg");
    }
    std::string cli_db = (cli_root / "cli.db").string();
    {
        Database cli(cli_db);
        cli.init_schema();
        for (int i = 0; i < N; i++) {
            ImageInfo info;
            info.path = (cli_photos / ("img" + std::to_string(i) + ".jpg")).string();
            info.filename = "img" + std::to_string(i) + ".jpg";
            info.extension = "jpg";
            info.file_size = 1;
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            info.sha256 = "s" + std::to_string(i);
            info.phash = 0;
            info.width = 1;
            info.height = 1;
            info.metadata_json = "{}";
            cli.insert(info);
        }
    }

    fs::path out_path = cli_root / "out.txt";
    std::string cmd = "echo yes | " + phig_bin.string() + " rm --match '*' --hard --db " + cli_db +
                      " > " + out_path.string() + " 2>&1";
    int rc = std::system(cmd.c_str());
    int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    std::ifstream in(out_path);
    std::string output((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    EXPECT_EQ(exit_code, 1) << "output was:\n" << output;
    EXPECT_NE(output.find("Refusing without confirmation."), std::string::npos) << "output was:\n"
                                                                                << output;

    int surviving = 0;
    for (int i = 0; i < N; i++) {
        if (fs::exists(cli_photos / ("img" + std::to_string(i) + ".jpg"))) surviving++;
    }
    EXPECT_EQ(surviving, N) << "piped 'yes' must not bypass the non-interactive refusal";

    fs::remove_all(cli_root);
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
    opts.db_path = db_path;
    opts.mode = RemoveMode::Orphans;
    opts.assume_yes = true;
    opts.argv_str = "phig rm --orphans";

    EXPECT_EQ(cmd_rm(opts), 0);
    auto rows = db->get_all_images();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, present);  // present kept, missing dropped
}

// ---- --orphans and the mount guard (plan 032) ----

// Captures stdout for the duration of a cmd_rm call: the reported bug is about
// what the command prints, so row counts alone can't verify the fix.
class CaptureStdout {
   public:
    CaptureStdout() : old_(std::cout.rdbuf(buf_.rdbuf())) {}
    ~CaptureStdout() { std::cout.rdbuf(old_); }
    std::string str() const { return buf_.str(); }

   private:
    std::ostringstream buf_;
    std::streambuf* old_;
};

TEST_F(RemoveExecTest, RmOrphansSkipsRowsWhoseParentIsMissingByDefault) {
    // A whole directory is gone: looks like an unmounted drive, so the guard
    // keeps the rows unless explicitly overridden.
    fs::path album = photos / "2026" / "07";
    std::string a = (album / "a.jpg").string();
    std::string b = (album / "b.jpg").string();
    write_file(a, "x");
    write_file(b, "x");
    insert_row(a);
    insert_row(b);
    fs::remove_all(photos / "2026");  // parent directory itself deleted

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.mode = RemoveMode::Orphans;
    opts.assume_yes = true;
    opts.argv_str = "phig rm --orphans";

    std::string output;
    {
        CaptureStdout cap;
        EXPECT_EQ(cmd_rm(opts), 0);
        output = cap.str();
    }

    EXPECT_EQ(db->get_all_images().size(), 2u) << "guard must keep possibly-unmounted rows";

    // The reported bug: the skip was silent and the output said nothing matched.
    EXPECT_EQ(output.find("No matching entries found."), std::string::npos) << "output was:\n"
                                                                            << output;
    EXPECT_NE(output.find("2"), std::string::npos) << "output was:\n" << output;
    EXPECT_NE(output.find("--ignore-mount-warning"), std::string::npos) << "output was:\n"
                                                                        << output;
}

TEST_F(RemoveExecTest, RmOrphansRemovesMissingParentRowsWithIgnoreMountWarning) {
    fs::path album = photos / "2026" / "07";
    std::string a = (album / "a.jpg").string();
    std::string b = (album / "b.jpg").string();
    write_file(a, "x");
    write_file(b, "x");
    insert_row(a);
    insert_row(b);
    fs::remove_all(photos / "2026");

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.mode = RemoveMode::Orphans;
    opts.assume_yes = true;
    opts.ignore_mount_warning = true;
    opts.argv_str = "phig rm --orphans --ignore-mount-warning";

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(db->get_all_images().empty()) << "override must remove the orphaned rows";
}

TEST_F(RemoveExecTest, RmOrphansNeverRemovesPresentFilesEvenWithIgnoreMountWarning) {
    std::string present = (photos / "here.jpg").string();
    write_file(present, "x");
    insert_row(present);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.mode = RemoveMode::Orphans;
    opts.assume_yes = true;
    opts.ignore_mount_warning = true;
    opts.argv_str = "phig rm --orphans --ignore-mount-warning";

    EXPECT_EQ(cmd_rm(opts), 0);
    auto rows = db->get_all_images();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, present);
    EXPECT_TRUE(fs::exists(present));
}

TEST_F(RemoveExecTest, RmOrphansNeverRemovesUnreadablePathsEvenWithIgnoreMountWarning) {
    // An unreadable parent makes existence unknowable — an unknown state, not a
    // mount heuristic, so the override must not reach it.
    if (geteuid() == 0) GTEST_SKIP() << "root bypasses directory permissions";

    fs::path locked = photos / "locked";
    std::string inside = (locked / "img.jpg").string();
    write_file(inside, "x");
    insert_row(inside);
    ASSERT_EQ(chmod(locked.c_str(), 0000), 0);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.mode = RemoveMode::Orphans;
    opts.assume_yes = true;
    opts.ignore_mount_warning = true;
    opts.argv_str = "phig rm --orphans --ignore-mount-warning";

    int rc = cmd_rm(opts);
    size_t remaining = db->get_all_images().size();
    chmod(locked.c_str(), 0755);  // restore so TearDown can clean up

    EXPECT_EQ(rc, 0);
    EXPECT_EQ(remaining, 1u) << "an unreadable path is never overridden";
}
