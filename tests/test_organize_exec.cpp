#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "database.h"
#include "organize_exec.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

// Exercises the per-file cp/mv executor and, critically, its failure
// compensators. DB writes are forced to fail with `PRAGMA query_only=ON` so the
// failure happens *after* the disk op — the exact window the compensators guard.
class OrganizeExecTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = make_temp_dir("organize-exec");
        db_path = (dir / "t.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove_all(dir);
    }

    // Write a file with known contents.
    void write_file(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << contents;
    }

    std::string read_file(const fs::path& p) {
        std::ifstream in(p);
        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return s;
    }

    // Insert a minimal images row for `path` so update_path_in_txn has a row to
    // repoint on the move path.
    void insert_row(const std::string& path) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.file_size = 3;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = "sha";
        info.phash = 0;
        info.width = 1;
        info.height = 1;
        info.metadata_json = "{}";
        db->insert(info);
    }

    void set_readonly(bool on) {
        sqlite3_exec(db->db_ptr(), on ? "PRAGMA query_only=ON" : "PRAGMA query_only=OFF", nullptr,
                     nullptr, nullptr);
    }
};

// ---- Happy paths ----

TEST_F(OrganizeExecTest, MoveRepointsRowAndMovesFile) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/true, /*op_id=*/0);

    EXPECT_FALSE(fs::exists(src));
    EXPECT_TRUE(fs::exists(dst));
    // DB row now points at dst.
    auto paths = db->get_all_images();
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0].path, dst.string());
}

TEST_F(OrganizeExecTest, CopyLeavesSourceAndTracksDest) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/false, /*op_id=*/0);

    EXPECT_TRUE(fs::exists(src));
    EXPECT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "AAA");
    EXPECT_EQ(db->count(), 2);  // source + copy
}

// ---- Compensator paths (DB write forced to fail after the disk op) ----

TEST_F(OrganizeExecTest, MoveCompensatesByMovingFileBackOnDbFailure) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());

    set_readonly(true);
    EXPECT_THROW(
        execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/true, /*op_id=*/0),
        std::exception);
    set_readonly(false);

    // File is back at the source; nothing left at the destination.
    EXPECT_TRUE(fs::exists(src));
    EXPECT_FALSE(fs::exists(dst));
    EXPECT_EQ(read_file(src), "AAA");
    // DB row still points at the original source.
    auto paths = db->get_all_images();
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0].path, src.string());
}

TEST_F(OrganizeExecTest, CopyRemovesOrphanOnDbFailureNoExisting) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());

    set_readonly(true);
    EXPECT_THROW(
        execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/false, /*op_id=*/0),
        std::exception);
    set_readonly(false);

    // Source intact; the copy we made was removed (no orphan).
    EXPECT_TRUE(fs::exists(src));
    EXPECT_FALSE(fs::exists(dst));
    EXPECT_EQ(db->count(), 1);  // only the source row
}

TEST_F(OrganizeExecTest, CopyRestoresPriorFileOnDbFailureOverwrite) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "NEW");
    write_file(dst, "ORIGINAL");  // a pre-existing file at the destination
    insert_row(src.string());

    set_readonly(true);
    EXPECT_THROW(
        execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/false, /*op_id=*/0),
        std::exception);
    set_readonly(false);

    // The prior destination file is restored byte-for-byte — not clobbered.
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "ORIGINAL");
    EXPECT_TRUE(fs::exists(src));
    // No stray backup left behind in the dest directory.
    int bak_count = 0;
    for (auto& e : fs::directory_iterator(dst.parent_path()))
        if (e.path().filename().string().find(".phig-bak.") != std::string::npos) bak_count++;
    EXPECT_EQ(bak_count, 0);
}

TEST_F(OrganizeExecTest, CopyOverwriteSuccessLeavesNoBackup) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "NEW");
    write_file(dst, "ORIGINAL");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/false, /*op_id=*/0);

    EXPECT_EQ(read_file(dst), "NEW");  // overwrite happened
    int bak_count = 0;
    for (auto& e : fs::directory_iterator(dst.parent_path()))
        if (e.path().filename().string().find(".phig-bak.") != std::string::npos) bak_count++;
    EXPECT_EQ(bak_count, 0);  // backup cleaned up on success
}

// ---- Logging integration ----

TEST_F(OrganizeExecTest, LogsEntryWhenOpIdProvided) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());

    int64_t op = db->log_begin_operation("cp", "phig cp dest", "2026-07-06T00:00:00Z");
    execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/false, op);
    db->log_finish_operation(op, "completed");

    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db->db_ptr(), "SELECT kind, src, dst FROM operation_entries WHERE op_id=?",
                       -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, op);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)), "copy");
    sqlite3_finalize(s);
}

TEST_F(OrganizeExecTest, NoOpSelfMoveDoesNothingAndLogsNothing) {
    fs::path src = dir / "src" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());
    int64_t op = db->log_begin_operation("mv", "phig mv", "2026-07-06T00:00:00Z");

    // dest == source: a file already at its target path.
    execute_organize_action(*db, src.string(), src.string(), /*is_move=*/true, op);
    db->log_finish_operation(op, "completed");

    EXPECT_TRUE(fs::exists(src));
    EXPECT_EQ(read_file(src), "AAA");
    // No entry logged for a no-op.
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE op_id=?", -1, &s,
                       nullptr);
    sqlite3_bind_int64(s, 1, op);
    sqlite3_step(s);
    EXPECT_EQ(sqlite3_column_int(s, 0), 0);
    sqlite3_finalize(s);
}

TEST_F(OrganizeExecTest, NoOpSelfCopyDoesNotCorruptFile) {
    fs::path src = dir / "src" / "a.jpg";
    write_file(src, "AAA");
    insert_row(src.string());
    // dest == source on a copy must not back-up-then-copy the file onto itself.
    execute_organize_action(*db, src.string(), src.string(), /*is_move=*/false, /*op_id=*/0);
    EXPECT_TRUE(fs::exists(src));
    EXPECT_EQ(read_file(src), "AAA");
}

// ---- carry_sidecars ----

TEST_F(OrganizeExecTest, CarryMovesStemSidecarBesideParent) {
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1.JPG";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    // Parent first (repoints the row so get_image_id(dst) works), then sidecars.
    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), /*is_move=*/true, /*op_id=*/0);

    EXPECT_FALSE(fs::exists(dir / "src" / "IMG_1.xmp"));
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1.xmp"));
}

TEST_F(OrganizeExecTest, CarryRenamesSidecarToMatchRenamedParent) {
    // Parent was renamed to avoid a conflict: IMG_1.JPG -> IMG_1-1.JPG.
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1-1.JPG";  // final (renamed) name
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), true, 0);

    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1-1.xmp"));
}

TEST_F(OrganizeExecTest, CarryHandlesAppendStyleRawSidecar) {
    fs::path src = dir / "src" / "IMG_1.CR2";
    fs::path dst = dir / "dest" / "IMG_1-1.CR2";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.CR2.xmp", "<xmp/>");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), true, 0);

    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1-1.CR2.xmp"));
}

TEST_F(OrganizeExecTest, CarryCopyLeavesSourceSidecar) {
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1.JPG";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), false, 0);
    carry_sidecars(*db, src.string(), dst.string(), /*is_move=*/false, 0);

    EXPECT_TRUE(fs::exists(dir / "src" / "IMG_1.xmp"));  // copy leaves source
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1.xmp"));
}

TEST_F(OrganizeExecTest, CarryLogsSidecarEntryWithParentId) {
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1.JPG";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    int64_t op = db->log_begin_operation("mv", "argv", "2026-07-11T00:00:00Z");
    execute_organize_action(*db, src.string(), dst.string(), true, op);
    carry_sidecars(*db, src.string(), dst.string(), true, op);
    db->log_finish_operation(op, "completed");

    // Exactly one move_sidecar entry, dst is the moved sidecar, restore_json
    // names the parent image id.
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db->db_ptr(),
                       "SELECT kind, dst, restore_json FROM operation_entries "
                       "WHERE kind='move_sidecar'",
                       -1, &st, nullptr);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    std::string kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    std::string edst = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    std::string rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    sqlite3_finalize(st);
    EXPECT_EQ(kind, "move_sidecar");
    EXPECT_EQ(edst, (dir / "dest" / "IMG_1.xmp").string());
    EXPECT_NE(rj.find("parent_image_id"), std::string::npos);
}

// ---- carry_sidecars: RAW's attached JPEG ----

TEST_F(OrganizeExecTest, CarryMovesAttachedJpegForRaw) {
    fs::path src = dir / "src" / "IMG_1.CR2";
    fs::path dst = dir / "dest" / "IMG_1.CR2";
    write_file(src, "RAWDATA");
    write_file(dir / "src" / "IMG_1.JPG", "JPEGDATA");
    insert_row(src.string());

    int64_t op = db->log_begin_operation("mv", "argv", "2026-07-11T00:00:00Z");
    execute_organize_action(*db, src.string(), dst.string(), true, op);
    carry_sidecars(*db, src.string(), dst.string(), /*is_move=*/true, op);
    db->log_finish_operation(op, "completed");

    EXPECT_FALSE(fs::exists(dir / "src" / "IMG_1.JPG"));
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1.JPG"));

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db->db_ptr(),
                       "SELECT kind, dst, restore_json FROM operation_entries "
                       "WHERE kind='move_sidecar'",
                       -1, &st, nullptr);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    std::string kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    std::string edst = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    std::string rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    sqlite3_finalize(st);
    EXPECT_EQ(kind, "move_sidecar");
    EXPECT_EQ(edst, (dir / "dest" / "IMG_1.JPG").string());
    EXPECT_NE(rj.find("parent_image_id"), std::string::npos);
}

TEST_F(OrganizeExecTest, CarryRenamesAttachedJpegToMatchRenamedRaw) {
    fs::path src = dir / "src" / "IMG_1.CR2";
    fs::path dst = dir / "dest" / "IMG_1-1.CR2";  // parent renamed to avoid a conflict
    write_file(src, "RAWDATA");
    write_file(dir / "src" / "IMG_1.JPG", "JPEGDATA");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), true, 0);

    EXPECT_FALSE(fs::exists(dir / "src" / "IMG_1.JPG"));
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1-1.JPG"));
}

TEST_F(OrganizeExecTest, CarryCopyLeavesSourceAttachedJpeg) {
    fs::path src = dir / "src" / "IMG_1.CR2";
    fs::path dst = dir / "dest" / "IMG_1.CR2";
    write_file(src, "RAWDATA");
    write_file(dir / "src" / "IMG_1.JPG", "JPEGDATA");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), false, 0);
    carry_sidecars(*db, src.string(), dst.string(), /*is_move=*/false, 0);

    EXPECT_TRUE(fs::exists(dir / "src" / "IMG_1.JPG"));  // copy leaves source
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1.JPG"));
}

TEST_F(OrganizeExecTest, NoAttachedJpegForNonRawParent) {
    // Parent is itself a JPEG (not a RAW): carry_sidecars must not try to carry
    // a "sibling jpeg" of a jpeg (would be a self-carry). No sidecars either, so
    // nothing should be moved or logged.
    fs::path src = dir / "src" / "IMG_2.JPG";
    fs::path dst = dir / "dest" / "IMG_2.JPG";
    write_file(src, "JPEGDATA");
    insert_row(src.string());

    int64_t op = db->log_begin_operation("mv", "argv", "2026-07-11T00:00:00Z");
    execute_organize_action(*db, src.string(), dst.string(), true, op);
    carry_sidecars(*db, src.string(), dst.string(), true, op);
    db->log_finish_operation(op, "completed");

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE op_id=?", -1,
                       &st, nullptr);
    sqlite3_bind_int64(st, 1, op);
    sqlite3_step(st);
    // Only the "move" entry from execute_organize_action, no sidecar entries.
    EXPECT_EQ(sqlite3_column_int(st, 0), 1);
    sqlite3_finalize(st);
}

// ---- Live Photo carry (plan 038) ----
//
// A Live Photo's video is an attachment of its still, so organize must move it
// alongside — otherwise `mv` files the still under its date and strands the
// motion where it was. Real exported files only: no committable fixture carries
// an Apple MakerNote, so this skips in CI.

TEST(OrganizeLivePhoto, MoveCarriesTheVideoWithItsStill) {
    const std::string samples = std::string(getenv("HOME") ?: "") + "/phig-live-samples";
    if (!fs::exists(samples + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples";

    fs::path root = make_temp_dir("organize-live");
    fs::path src = root / "src";
    fs::path dst = root / "dst" / "2025" / "10";
    fs::create_directories(src);
    fs::create_directories(dst);
    fs::copy_file(samples + "/IMG_1112.HEIC", src / "shot.heic");
    fs::copy_file(samples + "/IMG_1112.MOV", src / "motion.mov");

    // carry_sidecars runs after the parent has moved, which is why it reads the
    // still's metadata from the destination and searches the source directory.
    fs::rename(src / "shot.heic", dst / "renamed.heic");

    Database db((root / "phig.db").string());
    db.init_schema();
    carry_sidecars(db, (src / "shot.heic").string(), (dst / "renamed.heic").string(), true, 0);

    EXPECT_TRUE(fs::exists(dst / "renamed.mov"))
        << "the video must follow the still and take its new stem";
    EXPECT_FALSE(fs::exists(src / "motion.mov")) << "a move must not leave the motion behind";
    fs::remove_all(root);
}

TEST(OrganizeLivePhoto, LeavesAnUnrelatedVideoWhereItIs) {
    fs::path root = make_temp_dir("organize-live-plain");
    fs::path src = root / "src";
    fs::path dst = root / "dst";
    fs::create_directories(src);
    fs::create_directories(dst);
    fs::copy_file("tests/data/orient_1.heic", src / "photo.heic");
    fs::copy_file("tests/data/test_red.mov", src / "clip.mov");
    fs::rename(src / "photo.heic", dst / "photo.heic");

    Database db((root / "phig.db").string());
    db.init_schema();
    carry_sidecars(db, (src / "photo.heic").string(), (dst / "photo.heic").string(), true, 0);

    EXPECT_TRUE(fs::exists(src / "clip.mov")) << "an ordinary clip is not an attachment";
    fs::remove_all(root);
}

// ---- Live Photo carry: review findings (plan 038) ----
//
// These use the committed synthetic fixtures rather than exported personal
// media, so they run in CI. live_photo_still.jpg carries a MakerNote containing
// the same identifier live_photo.mov carries; the pairing code only ever tests
// containment, so this exercises the real paths.

namespace {
// A source directory holding a paired still and video under chosen names.
fs::path make_pair_dir(const fs::path& dir, const std::string& stem,
                       const std::string& video_stem) {
    fs::create_directories(dir);
    fs::copy_file("tests/data/live_photo_still.jpg", dir / (stem + ".jpg"));
    fs::copy_file("tests/data/live_photo.mov", dir / (video_stem + ".mov"));
    return dir / (stem + ".jpg");
}
}  // namespace

// A copy must never destroy bytes already at the carried destination. The
// parent's own destination is protected by resolve_conflict; this one is not
// seen by the conflict planner at all, and atomic_copy_file ends in a rename
// that replaces silently.
TEST(OrganizeLivePhotoSafety, CopyRefusesToClobberAnExistingVideoAtTheDestination) {
    fs::path root = make_temp_dir("lp-clobber");
    fs::path src = root / "src";
    fs::path dst = root / "dst";
    make_pair_dir(src, "shot", "shot");
    fs::create_directories(dst);

    // Something valuable already sitting exactly where the carry would land.
    fs::copy_file("tests/data/test_red.mov", dst / "shot.mov");
    const auto victim_size = fs::file_size(dst / "shot.mov");
    fs::copy_file(src / "shot.jpg", dst / "shot.jpg");

    Database db((root / "phig.db").string());
    db.init_schema();
    EXPECT_THROW(
        carry_sidecars(db, (src / "shot.jpg").string(), (dst / "shot.jpg").string(), false, 0),
        std::exception);
    EXPECT_EQ(fs::file_size(dst / "shot.mov"), victim_size)
        << "the pre-existing video was overwritten";
    fs::remove_all(root);
}

// Pairing says "this video belongs to that still". It does not say scan agreed
// to treat the video as an attachment — and on the migration path every
// pre-038 video still has its own row.
TEST(OrganizeLivePhotoSafety, DoesNotCarryAVideoThatHasItsOwnRow) {
    fs::path root = make_temp_dir("lp-indexed");
    fs::path src = root / "src";
    fs::path dst = root / "dst";
    make_pair_dir(src, "shot", "shot");
    fs::create_directories(dst);
    fs::copy_file(src / "shot.jpg", dst / "shot.jpg");

    Database db((root / "phig.db").string());
    db.init_schema();
    ImageInfo v;
    v.path = (src / "shot.mov").string();
    v.filename = "shot.mov";
    v.extension = "mov";
    v.media_type = "video";
    v.sha256 = "indexed-video";
    db.insert(v);

    carry_sidecars(db, (src / "shot.jpg").string(), (dst / "shot.jpg").string(), true, 0);
    EXPECT_TRUE(fs::exists(src / "shot.mov"))
        << "a video with its own row must not be moved as an attachment";
    fs::remove_all(root);
}

// The halves need not share a stem, so a path comparison would see
// lp_src != lp_dst and "carry" the video onto itself during an operation that
// moved nothing — renaming the user's file, or duplicating it.
TEST(OrganizeLivePhotoSafety, DoesNothingWhenTheParentDidNotMove) {
    fs::path root = make_temp_dir("lp-noop");
    fs::path dir = root / "same";
    make_pair_dir(dir, "shot", "motion");

    Database db((root / "phig.db").string());
    db.init_schema();
    carry_sidecars(db, (dir / "shot.jpg").string(), (dir / "shot.jpg").string(), true, 0);

    EXPECT_TRUE(fs::exists(dir / "motion.mov")) << "the user's video was renamed by a no-op";
    EXPECT_FALSE(fs::exists(dir / "shot.mov")) << "a no-op created a duplicate video";
    fs::remove_all(root);
}

// The positive path, in CI: an unindexed paired video follows its still.
TEST(OrganizeLivePhotoSafety, CarriesAnUnindexedPairedVideo) {
    fs::path root = make_temp_dir("lp-carry-ci");
    fs::path src = root / "src";
    fs::path dst = root / "dst";
    make_pair_dir(src, "shot", "motion");
    fs::create_directories(dst);
    fs::rename(src / "shot.jpg", dst / "renamed.jpg");

    Database db((root / "phig.db").string());
    db.init_schema();
    carry_sidecars(db, (src / "shot.jpg").string(), (dst / "renamed.jpg").string(), true, 0);

    EXPECT_TRUE(fs::exists(dst / "renamed.mov")) << "the video must follow and take the new stem";
    EXPECT_FALSE(fs::exists(src / "motion.mov"));
    fs::remove_all(root);
}

// Move with allow_overwrite renames the occupant aside before moving, mirroring
// what copy has always done. A database failure after the file has moved must
// put both files back: the source where it was, and the occupant where it was.
TEST_F(OrganizeExecTest, MoveRestoresPriorFileOnDbFailureWhenOverwriteAllowed) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "NEW");
    write_file(dst, "ORIGINAL");
    insert_row(src.string());

    set_readonly(true);
    EXPECT_THROW(execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/true,
                                         /*op_id=*/0, /*allow_overwrite=*/true),
                 std::exception);
    set_readonly(false);

    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "ORIGINAL") << "the occupant must be restored, not lost";
    ASSERT_TRUE(fs::exists(src));
    EXPECT_EQ(read_file(src), "NEW") << "the source must be back where it started";

    int bak_count = 0;
    for (auto& e : fs::directory_iterator(dst.parent_path()))
        if (e.path().filename().string().find(".phig-bak.") != std::string::npos) bak_count++;
    EXPECT_EQ(bak_count, 0) << "no stray backup left behind";
}

TEST_F(OrganizeExecTest, MoveOverwriteSuccessLeavesNoBackup) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "NEW");
    write_file(dst, "ORIGINAL");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/true, /*op_id=*/0,
                            /*allow_overwrite=*/true);

    EXPECT_EQ(read_file(dst), "NEW") << "the authorized overwrite replaced it";
    EXPECT_FALSE(fs::exists(src));
    int bak_count = 0;
    for (auto& e : fs::directory_iterator(dst.parent_path()))
        if (e.path().filename().string().find(".phig-bak.") != std::string::npos) bak_count++;
    EXPECT_EQ(bak_count, 0);
}

// Without the flag, move_file's no-clobber refusal stands. This is the guard
// that keeps an unauthorized run from destroying bytes.
TEST_F(OrganizeExecTest, MoveRefusesAnOccupiedDestinationWithoutAuthorization) {
    fs::path src = dir / "src" / "a.jpg";
    fs::path dst = dir / "dest" / "a.jpg";
    write_file(src, "NEW");
    write_file(dst, "ORIGINAL");
    insert_row(src.string());

    EXPECT_THROW(execute_organize_action(*db, src.string(), dst.string(), /*is_move=*/true,
                                         /*op_id=*/0),
                 std::exception);
    EXPECT_EQ(read_file(dst), "ORIGINAL");
    EXPECT_TRUE(fs::exists(src));
}
