#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "cancel.h"
#include "database.h"
#include "organize_engine.h"
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
