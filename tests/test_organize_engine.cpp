#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "cancel.h"
#include "database.h"
#include "organize_engine.h"
#include "phig_binary.h"
#include "progress_queue.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

class OrganizeEngineTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;

    void SetUp() override {
        dir = make_temp_dir("organize-engine");
        db_path = (dir / "phig.db").string();
    }
    void TearDown() override { fs::remove_all(dir); }

    // A real file on disk plus its row, so planning sees what production sees.
    // The date goes in metadata_json: get_best_date reads EXIF DateTimeOriginal
    // first and falls back to mtime, so a created_at would be ignored and every
    // fixture would file under today's date instead of the one it names.
    void add_image(Database& db, const std::string& name, const std::string& exif_date) {
        fs::path p = dir / "src" / name;
        fs::create_directories(p.parent_path());
        {
            std::ofstream f(p);
            f << name;
        }
        ImageInfo info;
        info.path = p.string();
        info.filename = name;
        info.extension = p.extension().string();
        info.sha256 = name;  // unique per file is all this needs
        info.metadata_json = R"({"DateTimeOriginal":")" + exif_date + R"("})";
        db.insert(info);
    }

    OrganizeOptions base_opts() {
        OrganizeOptions o;
        o.destination = (dir / "out").string();
        o.format = "%Y/%original";
        o.on_conflict = ConflictResolution::Rename;
        o.db_path = db_path;
        o.log_enabled = false;
        return o;
    }
};

TEST_F(OrganizeEngineTest, PlanProducesOneActionPerSelectedImage) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2020:01:02 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 2u);
    std::string all = plan.at(0).dest + "|" + plan.at(1).dest;
    EXPECT_NE(all.find("2019"), std::string::npos);
    EXPECT_NE(all.find("2020"), std::string::npos);
}

TEST_F(OrganizeEngineTest, PlanWritesNothingToDiskOrDatabase) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    const int64_t rows_before = db.count();

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_EQ(plan.size(), 1u);
    EXPECT_EQ(db.count(), rows_before);
    EXPECT_FALSE(fs::exists(dir / "out")) << "plan must not create the destination tree";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "plan must not move sources";
}

TEST_F(OrganizeEngineTest, PlanHonorsMatchAndExclude) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "keep.jpg", "2019:07:04 10:00:00");
    add_image(db, "drop.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts();
    o.excludes.push_back("*drop*");
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    EXPECT_NE(plan.at(0).source.find("keep.jpg"), std::string::npos);
}

TEST_F(OrganizeEngineTest, PlanFlagsAnOccupiedDestinationAsConflicting) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.at(0).conflicts);
    EXPECT_FALSE(plan.at(0).occupant_tracked) << "the occupant has no row";
}

TEST_F(OrganizeEngineTest, SetResolutionRejectsANonConflictingItem) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    ASSERT_FALSE(plan.at(0).conflicts);
    EXPECT_FALSE(plan.set_resolution(0, ConflictResolution::Overwrite));
    EXPECT_FALSE(plan.at(0).overridden);
}

TEST_F(OrganizeEngineTest, SetResolutionRejectsAnIndexOutOfRange) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    EXPECT_FALSE(plan.set_resolution(99, ConflictResolution::Skip));
}

// Overriding to Overwrite must produce the plain destination, not a renamed one.
// Without the bypass, is_protected sees the existing file and resolve_conflict
// renames around it, silently discarding the caller's choice.
TEST_F(OrganizeEngineTest, OverridingToOverwriteTargetsTheOccupiedPath) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    ASSERT_TRUE(plan.at(0).conflicts);
    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Overwrite));
    EXPECT_TRUE(plan.at(0).overridden);
    EXPECT_EQ(plan.at(0).dest, (dir / "out" / "2019" / "a.jpg").string());
}

TEST_F(OrganizeEngineTest, OverridingToSkipEmptiesTheDestination) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Skip));
    EXPECT_TRUE(plan.at(0).dest.empty());
}

// The one bypass that must never be granted: the occupying file is another
// image's source, still waiting to be moved.
TEST_F(OrganizeEngineTest, SetResolutionRefusesToTargetABatchSource) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    // This file sits exactly where src/a.jpg wants to land, and is itself indexed.
    fs::path collide = dir / "out" / "2019" / "a.jpg";
    fs::create_directories(collide.parent_path());
    {
        std::ofstream f(collide);
        f << "b";
    }
    ImageInfo b;
    b.path = collide.string();
    b.filename = "a.jpg";
    b.extension = ".jpg";
    b.sha256 = "b-sha";
    b.metadata_json = R"({"DateTimeOriginal":"2021:05:05 10:00:00"})";
    db.insert(b);

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    size_t idx = plan.size();
    for (size_t i = 0; i < plan.size(); i++) {
        if (plan.at(i).source.find("/src/") != std::string::npos) idx = i;
    }
    ASSERT_LT(idx, plan.size());
    ASSERT_TRUE(plan.at(idx).conflicts);
    EXPECT_TRUE(plan.at(idx).occupant_tracked);
    EXPECT_FALSE(plan.set_resolution(idx, ConflictResolution::Overwrite))
        << "overwriting another image's un-moved source must be refused";
    EXPECT_TRUE(fs::exists(collide));
}

TEST_F(OrganizeEngineTest, ApplyMovesFilesAndUpdatesRows) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    EXPECT_EQ(result.errors, 0u);
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "a.jpg"));
    EXPECT_FALSE(fs::exists(dir / "src" / "a.jpg"));
    EXPECT_GE(db.get_image_id((dir / "out" / "2019" / "a.jpg").string()), 0);
}

TEST_F(OrganizeEngineTest, ApplyCopyLeavesTheSourceInPlace) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, /*is_move=*/false, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg"));
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "a.jpg"));
}

TEST_F(OrganizeEngineTest, ApplySkipsItemsWithNoDestination) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    {
        std::ofstream f(dir / "src" / "a.xmp");
        f << "sidecar";
    }
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeOptions o = base_opts();
    o.on_conflict = ConflictResolution::Skip;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 0u);
    EXPECT_EQ(result.skipped, 1u);
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "a skipped source stays put";
    // An attachment is carried only after its parent's own action returned, so
    // a parent that was never acted on carries nothing.
    EXPECT_TRUE(fs::exists(dir / "src" / "a.xmp")) << "a skipped parent's sidecar stays put";
    EXPECT_EQ(result.files_written, 0u) << "a skipped parent writes no files at all";
}

// Sidecars move with their parent and are counted separately, so the denominator
// a caller draws stays the photo count.
TEST_F(OrganizeEngineTest, SidecarsCountInFilesWrittenNotInApplied) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    {
        std::ofstream f(dir / "src" / "a.xmp");
        f << "sidecar";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u) << "one photo";
    EXPECT_EQ(result.files_written, 2u) << "the photo and its sidecar";
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "a.xmp"));
}

TEST_F(OrganizeEngineTest, AlreadyCancelledAppliesNothingAndReportsIncomplete) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    cancel.cancel();
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 0u);
    EXPECT_FALSE(result.completed);
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg"));
}

TEST_F(OrganizeEngineTest, ApplyPublishesPlannedThenOneUpdatePerFile) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    ProgressQueue<OrganizeProgress> q;
    OrganizePlan plan = engine.plan(cancel, &q);
    engine.apply(plan, true, cancel, &q);
    q.close();

    OrganizeProgress p;
    int planned = 0, file_done = 0;
    while (q.try_pop(p)) {
        if (p.stage == OrganizeStage::Planned) planned++;
        if (p.stage == OrganizeStage::FileDone) file_done++;
    }
    EXPECT_EQ(planned, 1);
    EXPECT_EQ(file_done, 2);
}

// Today `--on-conflict overwrite` never replaces a file that exists on disk:
// is_protected covers any existing path and resolve_conflict renames around it.
// Extraction must not quietly turn overwrite into a destructive policy.
TEST_F(OrganizeEngineTest, OverwritePolicyStillRenamesAroundAnExistingFile) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeOptions o = base_opts();
    o.on_conflict = ConflictResolution::Overwrite;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    engine.apply(plan, true, cancel, nullptr);

    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_EQ(content, "occupant") << "run-level overwrite must not clobber an existing file";
}

// 041's decision 4, inherited here: the plan is advisory and apply's own check
// is authoritative, so a destination occupied between plan and apply is not
// silently overwritten.
TEST_F(OrganizeEngineTest, ApplySkipsAnItemWhoseDestinationBecameOccupied) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    {
        std::ofstream f(dir / "src" / "a.xmp");
        f << "sidecar";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_FALSE(plan.at(0).conflicts);

    // Someone else writes the destination after planning.
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "latecomer";
    }

    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 0u);
    EXPECT_EQ(result.skipped, 1u);
    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_EQ(content, "latecomer") << "the file that arrived first must survive";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg"));
    // The parent's action never ran, so its attachment must not have moved
    // either -- an orphaned sidecar in the destination would name a photo that
    // is not there.
    EXPECT_TRUE(fs::exists(dir / "src" / "a.xmp")) << "a skipped parent's sidecar stays put";
    EXPECT_FALSE(fs::exists(dir / "out" / "2019" / "a.xmp"));
    EXPECT_EQ(result.files_written, 0u) << "a skipped parent writes no files at all";
}

// An explicit override is the caller's authority to replace, so re-verification
// must not undo it.
TEST_F(OrganizeEngineTest, ApplyHonorsAnOverrideAgainstAFileThatStillExists) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Overwrite));

    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_NE(content, "occupant") << "an explicit override replaces the occupant";
}

TEST_F(OrganizeEngineTest, CopyHonorsAPerItemOverrideToo) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    {
        std::ofstream f(dir / "out" / "2019" / "a.jpg");
        f << "occupant";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Overwrite));

    OrganizeResult result = engine.apply(plan, /*is_move=*/false, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_NE(content, "occupant") << "an override applies to copy as well as move";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "copy still leaves the source";
}

// A RAW's same-basename JPEG is an attachment, not a row of its own, so it is
// carried like a sidecar and counts as work done without moving the denominator.
TEST_F(OrganizeEngineTest, AttachedJpegForARawCountsInFilesWritten) {
    Database db(db_path);
    db.init_schema();
    fs::create_directories(dir / "src");
    {
        std::ofstream f(dir / "src" / "IMG_1.CR2");
        f << "RAWDATA";
    }
    {
        std::ofstream f(dir / "src" / "IMG_1.JPG");
        f << "JPEGDATA";
    }
    ImageInfo raw;
    raw.path = (dir / "src" / "IMG_1.CR2").string();
    raw.filename = "IMG_1.CR2";
    raw.extension = ".CR2";
    raw.sha256 = "raw-sha";
    raw.metadata_json = R"({"DateTimeOriginal":"2019:07:04 10:00:00"})";
    db.insert(raw);

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u) << "one photo";
    EXPECT_EQ(result.files_written, 2u) << "the RAW and its attached JPEG";
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "IMG_1.CR2"));
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "IMG_1.JPG"));
    EXPECT_FALSE(fs::exists(dir / "src" / "IMG_1.JPG"));
}

TEST_F(OrganizeEngineTest, ADestinationThatBecameFreeIsSimplyWritten) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    fs::path occupied = dir / "out" / "2019" / "a.jpg";
    {
        std::ofstream f(occupied);
        f << "occupant";
    }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    // Planning renamed around the occupant. It then goes away on its own.
    ASSERT_TRUE(plan.at(0).conflicts);
    std::string planned_dest = plan.at(0).dest;
    ASSERT_NE(planned_dest, occupied.string());
    fs::remove(occupied);

    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    EXPECT_EQ(result.skipped, 0u);
    EXPECT_TRUE(fs::exists(planned_dest))
        << "apply writes the destination it planned, not the one that freed up";
    EXPECT_FALSE(fs::exists(occupied)) << "apply must not opportunistically retarget";
}

// Cancelling partway must leave the logged operation marked interrupted rather
// than lingering as in_progress, and disk and database agreeing on every file
// that did move.
TEST_F(OrganizeEngineTest, CancellationPartwayMarksTheOperationInterrupted) {
    Database db(db_path);
    db.init_schema();
    for (int i = 0; i < 40; i++) {
        add_image(db, "f" + std::to_string(i) + ".jpg", "2019:07:04 10:00:00");
    }

    OrganizeOptions o = base_opts();
    o.log_enabled = true;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ProgressQueue<OrganizeProgress> q(1024);
    std::thread canceller([&] {
        OrganizeProgress p;
        while (q.pop(p)) {
            if (p.stage == OrganizeStage::FileDone && p.done >= 3) {
                cancel.cancel();
                break;
            }
        }
    });
    OrganizeResult result = engine.apply(plan, true, cancel, &q);
    q.close();
    canceller.join();

    EXPECT_FALSE(result.completed);
    EXPECT_LT(result.applied, 40u) << "cancellation must stop short of the whole batch";

    sqlite3_stmt* st = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.db_ptr(), "SELECT status FROM operations ORDER BY id DESC", -1,
                                 &st, nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), "interrupted");
    sqlite3_finalize(st);

    // Every row the run did move must point at a file that exists.
    for (const auto& img : db.get_all_images()) {
        EXPECT_TRUE(fs::exists(img.path)) << img.path << " is recorded but missing";
    }
}

// ---- The dry run below cli/ (R21, R74) ----

namespace {
// One integer out of a scratch database, over a connection of this test's own.
// The child process has already exited, so nothing is racing the read.
int64_t scalar(const std::string& db_path, const char* sql) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(db_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << "could not open " << db_path;
        if (raw) sqlite3_close(raw);
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    int64_t n = -1;
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int64(stmt, 0);
    } else {
        ADD_FAILURE() << "query failed: " << sqlite3_errmsg(raw);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    return n;
}
}  // namespace

// OrganizeEngine::plan writes nothing, which the test above pins. The command
// that calls it opens the database read-write and migrates it before it looks
// at --dry-run, so the preview is side-effect-free only below cli/ — and that
// half has never been asserted, only measured by hand. cmd_organize lives in
// cli/main.cpp and is not linked into phig-tests, so the binary is the way in.
TEST_F(OrganizeEngineTest, CliDryRunOverAnAbsentDatabaseStillCreatesAndMigratesIt) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    const fs::path absent = dir / "absent.db";
    const fs::path dest = dir / "out";
    ASSERT_FALSE(fs::exists(absent));

    PhigRun run = run_phig(phig_bin, dir,
                           "cp '" + dest.string() + "' --dry-run --db '" + absent.string() + "'");

    // Exit 1, not 0: a database with no rows is refused before the preview is
    // printed. The refusal is beside the point — the file it just created is
    // the point.
    EXPECT_EQ(run.exit_code, 1) << run.output;
    EXPECT_NE(run.output.find("Database is empty"), std::string::npos) << run.output;
    EXPECT_FALSE(fs::exists(dest)) << "a preview must not create the destination tree";

    ASSERT_TRUE(fs::exists(absent)) << "measured: the preview leaves a database behind";
    Database opened(absent.string());
    EXPECT_EQ(opened.schema_version(), Database::expected_schema_version())
        << "the preview created the file and migrated it, both before --dry-run was read";
}

TEST_F(OrganizeEngineTest, CliDryRunPreviewsEveryCopyAndWritesNothingElse) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    {
        Database db(db_path);
        db.init_schema();
        add_image(db, "a.jpg", "2019:07:04 10:00:00");
        add_image(db, "b.jpg", "2020:01:02 10:00:00");
    }
    const fs::path preview_dest = dir / "preview";

    PhigRun preview = run_phig(
        phig_bin, dir, "cp '" + preview_dest.string() + "' --dry-run --db '" + db_path + "'");

    EXPECT_EQ(preview.exit_code, 0) << preview.output;
    EXPECT_NE(preview.output.find("[DRY RUN]"), std::string::npos) << preview.output;
    EXPECT_NE(preview.output.find("2019/07"), std::string::npos) << preview.output;
    EXPECT_NE(preview.output.find("2020/01"), std::string::npos) << preview.output;

    EXPECT_FALSE(fs::exists(preview_dest)) << "the preview created the destination tree";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "the preview moved a source";
    EXPECT_EQ(scalar(db_path, "SELECT COUNT(*) FROM images"), 2);
    EXPECT_EQ(scalar(db_path, "SELECT COUNT(*) FROM operations"), 0)
        << "the preview opened a logged operation";
    EXPECT_EQ(scalar(db_path, "SELECT COUNT(*) FROM operation_entries"), 0);

    // The control. Without it every assertion above would also hold for a `cp`
    // that had simply found nothing to do, and the test would pin the fixture
    // rather than the flag.
    const fs::path applied_dest = dir / "applied";
    PhigRun applied =
        run_phig(phig_bin, dir, "cp '" + applied_dest.string() + "' --db '" + db_path + "'");
    EXPECT_EQ(applied.exit_code, 0) << applied.output;
    EXPECT_TRUE(fs::exists(applied_dest / "2019" / "07" / "a.jpg")) << applied.output;
    EXPECT_TRUE(fs::exists(applied_dest / "2020" / "01" / "b.jpg")) << applied.output;
}

TEST_F(OrganizeEngineTest, ApplyLeavesAnExcludedFileAndItsRowAlone) {
    // PlanHonorsMatchAndExclude pins the plan. This pins the move: the file the
    // caller excluded is still where it was and its row still points at it,
    // which is what R74 claims and what no test of passes_selection can show.
    Database db(db_path);
    db.init_schema();
    add_image(db, "moved.jpg", "2019:07:04 10:00:00");
    add_image(db, "spared.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts();
    o.excludes.push_back("*spared*");
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    EXPECT_EQ(result.errors, 0u);
    const std::string spared = (dir / "src" / "spared.jpg").string();
    EXPECT_FALSE(fs::exists(dir / "src" / "moved.jpg")) << "the matched file was not moved";
    EXPECT_TRUE(fs::exists(spared)) << "--exclude did not reach the move";

    bool row_still_at_source = false;
    for (const auto& img : db.get_all_images()) {
        if (img.path == spared) row_still_at_source = true;
    }
    EXPECT_TRUE(row_still_at_source) << "the excluded row was rewritten to a destination";
}
