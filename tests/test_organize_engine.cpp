#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "cancel.h"
#include "database.h"
#include "invalid_option.h"
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

    // add_image with extra raw JSON members after the date, e.g. R"("Model":"x")". The
    // value goes in unescaped, as json_get reads it back.
    void add_image_with(Database& db, const std::string& name, const std::string& members) {
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
        info.sha256 = name;
        info.metadata_json = R"({"DateTimeOriginal":"2019:07:04 10:00:00",)" + members + "}";
        db.insert(info);
    }

    // Every planned destination, after checking it lies under `root`.
    std::vector<std::string> planned_dests(const OrganizePlan& plan, const fs::path& root) {
        std::vector<std::string> out;
        const std::string prefix = root.string() + "/";
        for (size_t i = 0; i < plan.size(); ++i) {
            const std::string d = fs::path(plan.at(i).dest).lexically_normal().string();
            EXPECT_TRUE(d.starts_with(prefix)) << d << " is outside " << root;
            out.push_back(plan.at(i).dest);
        }
        return out;
    }

    // Every row. These tests predate id selection and meant "the whole
    // collection" by setting no filter; an empty selection now means nothing,
    // so what they meant has to be said.
    std::vector<int64_t> all_ids(Database& db) {
        Database::PageCriteria pc;
        pc.limit = -1;
        return db.ids_matching(pc);
    }

    // base_opts() with every row selected. Tests that construct an engine
    // straight from base_opts() meant the whole collection, which an empty
    // selection no longer means.
    OrganizeOptions base_opts(Database& db) {
        OrganizeOptions o = base_opts();
        o.ids = all_ids(db);
        return o;
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_EQ(plan.size(), 1u);
    EXPECT_EQ(db.count(), rows_before);
    EXPECT_FALSE(fs::exists(dir / "out")) << "plan must not create the destination tree";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "plan must not move sources";
}

// An empty destination would plan every path relative to the process's working
// directory, and applying the plan would move files there (R73).
TEST_F(OrganizeEngineTest, AnEmptyDestinationOrFormatIsRefusedBeforePlanning) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    CancelToken cancel;

    OrganizeOptions no_dest = base_opts(db);
    no_dest.destination = "";
    EXPECT_THROW(OrganizeEngine(no_dest, db).plan(cancel, nullptr), InvalidOption);

    OrganizeOptions no_format = base_opts(db);
    no_format.format = "";
    EXPECT_THROW(OrganizeEngine(no_format, db).plan(cancel, nullptr), InvalidOption);
}

// A file's EXIF Make and Model are substituted into its destination. They are labels,
// not paths: a separator in one becomes '_', so a file cannot steer where it is put
// (R87).
TEST_F(OrganizeEngineTest, AMakeOrModelThatClimbsOrIsAbsoluteStaysInsideTheDestination) {
    Database db(db_path);
    db.init_schema();
    add_image_with(db, "a.jpg", R"("Model":"../../x")");
    add_image_with(db, "b.jpg", R"("Make":"/tmp/abs")");

    OrganizeOptions opts = base_opts(db);
    opts.format = "%camera/%make/%original";
    CancelToken cancel;
    OrganizePlan plan = OrganizeEngine(opts, db).plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 2u);
    std::vector<std::string> dests = planned_dests(plan, dir / "out");
    std::sort(dests.begin(), dests.end());
    EXPECT_EQ(dests[0], (dir / "out" / ".._.._x" / "unknown-make" / "a.jpg").string());
    EXPECT_EQ(dests[1], (dir / "out" / "unknown-camera" / "_tmp_abs" / "b.jpg").string());
}

TEST_F(OrganizeEngineTest, AGalleryNameWithASlashPlansOneFolder) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    ASSERT_EQ(db.set_gallery({(dir / "src" / "a.jpg").string()}, "trips/2019", false), 1);

    OrganizeOptions opts = base_opts(db);
    opts.format = "%gallery/%original";
    CancelToken cancel;
    OrganizePlan plan = OrganizeEngine(opts, db).plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(planned_dests(plan, dir / "out")[0], (dir / "out" / "trips_2019" / "a.jpg").string());
}

TEST_F(OrganizeEngineTest, AValueThatCleansToNothingOrADotTakesItsPlaceholder) {
    Database db(db_path);
    db.init_schema();
    add_image_with(db, "a.jpg", "\"Model\":\"..\",\"Make\":\"\x01\x02\"");
    ASSERT_EQ(db.set_gallery({(dir / "src" / "a.jpg").string()}, ".", false), 1);

    OrganizeOptions opts = base_opts(db);
    opts.format = "%camera/%make/%gallery/%original";
    CancelToken cancel;
    OrganizePlan plan = OrganizeEngine(opts, db).plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(planned_dests(plan, dir / "out")[0],
              (dir / "out" / "unknown-camera" / "unknown-make" / "no-gallery" / "a.jpg").string());
}

// The format is the caller's own input, so one that could leave the destination, or
// that uses a token phig does not know, is refused before planning (R73's shape).
TEST_F(OrganizeEngineTest, AFormatThatLeavesTheDestinationOrHasAnUnknownTokenIsRefused) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    CancelToken cancel;

    for (const std::string fmt :
         {"/abs/%original", "%Y/../../%original", "%y/%original", "%Y/%original%"}) {
        OrganizeOptions opts = base_opts(db);
        opts.format = fmt;
        EXPECT_THROW(OrganizeEngine(opts, db).plan(cancel, nullptr), InvalidOption) << fmt;
    }
    // Literal text beside the tokens is still a format, and a destination given with a
    // trailing separator still contains what is planned under it.
    OrganizeOptions ok = base_opts(db);
    ok.destination += "/";
    ok.format = "photos-%Y/%m-%d/%original";
    EXPECT_EQ(OrganizeEngine(ok, db).plan(cancel, nullptr).size(), 1u);
}

// The 2026-11-10 measurement, repeated through the CLI: a Model of '../../escaped' used
// to put the copy two directories above the destination.
TEST_F(OrganizeEngineTest, CliCopyKeepsAFileWhoseModelClimbsInsideTheDestination) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    {
        Database db(db_path);
        db.init_schema();
        add_image_with(db, "a.jpg", R"("Model":"../../escaped")");
    }
    const fs::path dest = dir / "out" / "dest";

    PhigRun run = run_phig(
        phig_bin, dir,
        "cp '" + dest.string() + "' --format '%camera/%original' --catalog '" + dir.string() + "'");

    EXPECT_EQ(run.exit_code, 0) << run.output;
    EXPECT_TRUE(fs::exists(dest / ".._.._escaped" / "a.jpg")) << run.output;
    EXPECT_FALSE(fs::exists(dir / "escaped")) << run.output;
}

TEST_F(OrganizeEngineTest, CliCopyCarriesTagsAndGalleryToTheCopy) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    const fs::path src = dir / "src" / "a.jpg";
    {
        Database db(db_path);
        db.init_schema();
        add_image(db, "a.jpg", "2019:07:04 10:00:00");
        db.set_tag({src.string()}, "trip", true);
        db.set_gallery({src.string()}, "summer", false);
    }
    const fs::path copy = dir / "out" / "2019" / "a.jpg";
    const std::string catalog = " --catalog '" + dir.string() + "'";

    PhigRun cp = run_phig(phig_bin, dir,
                          "cp '" + (dir / "out").string() + "' --format '%Y/%original'" + catalog);
    ASSERT_EQ(cp.exit_code, 0) << cp.output;
    ASSERT_TRUE(fs::exists(copy)) << cp.output;

    for (const std::string filter : {"--tag trip", "--gallery summer"}) {
        PhigRun found = run_phig(phig_bin, dir, "search " + filter + " --porcelain" + catalog);
        EXPECT_EQ(found.exit_code, 0) << found.output;
        EXPECT_NE(found.output.find(copy.string()), std::string::npos)
            << filter << " did not find the copy: " << found.output;
        EXPECT_NE(found.output.find(src.string()), std::string::npos)
            << filter << " no longer finds the original: " << found.output;
    }
}

TEST_F(OrganizeEngineTest, PlanHonorsItsIdSelection) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "keep.jpg", "2019:07:04 10:00:00");
    add_image(db, "drop.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts();
    // The engine takes ids; the glob is resolved by whoever calls it, which for
    // cp and mv is cmd_organize via Database::ids_matching.
    Database::PageCriteria pc;
    pc.filter.excludes = {"*drop*"};
    pc.limit = -1;
    o.ids = db.ids_matching(pc);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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
    o.ids = all_ids(db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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

    OrganizeEngine engine(base_opts(db), db);
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
    o.ids = all_ids(db);
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    // The canceller races apply's loop, and losing that race fails every
    // assertion below: apply checks cancel.stop() once per file and otherwise
    // runs flat out, ProgressQueue::push never blocks, so nothing couples the
    // two. Measured, apply moves these 40 files in ~34ms, so the canceller has
    // to act inside that window — and a thread that has not been scheduled even
    // once will not.
    //
    // Two things keep the window wide. `started` makes the main thread wait
    // until the canceller has actually run, so apply never races a thread that
    // does not exist yet; after that the canceller is blocked in pop() and a
    // condvar notify wakes it, which is far more reliable than first-scheduling
    // a new thread on a saturated machine. And it cancels on the FIRST FileDone
    // rather than the third, spending none of the window waiting for events it
    // does not need.
    //
    // This is still a timing-dependent test, not a deterministic one. If it
    // ever flakes again, widen the window with more files rather than loosening
    // an assertion — each one here is load-bearing.
    ProgressQueue<OrganizeProgress> q(1024);
    std::atomic<bool> started{false};
    std::thread canceller([&] {
        started.store(true, std::memory_order_release);
        OrganizeProgress p;
        while (q.pop(p)) {
            if (p.stage == OrganizeStage::FileDone) {
                cancel.cancel();
                break;
            }
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    OrganizeResult result = engine.apply(plan, true, cancel, &q);
    q.close();
    canceller.join();

    EXPECT_FALSE(result.completed);
    EXPECT_TRUE(result.cancelled);
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

// ---- A run's counts and terminal status (R27, R48) ----

namespace {
std::string newest_operation_status(Database& db) {
    sqlite3_stmt* st = nullptr;
    EXPECT_EQ(sqlite3_prepare_v2(db.db_ptr(), "SELECT status FROM operations ORDER BY id DESC", -1,
                                 &st, nullptr),
              SQLITE_OK);
    std::string out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        out = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}
}  // namespace

TEST_F(OrganizeEngineTest, AnAttachmentNotCarriedCountsItsItemOnceAndInterruptsTheRun) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    std::ofstream(dir / "src" / "a.xmp") << "sidecar";

    OrganizeOptions o = base_opts(db);
    o.destination = (dir / "dest").string();
    o.log_enabled = true;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_EQ(plan.size(), 1u);
    const fs::path occupied = fs::path(plan.at(0).dest).parent_path() / "a.xmp";
    fs::create_directories(occupied.parent_path());
    std::ofstream(occupied) << "OCCUPANT";

    ProgressQueue<OrganizeProgress> q(1024);
    OrganizeResult result = engine.apply(plan, false, cancel, &q);
    q.close();

    EXPECT_EQ(result.applied, 1u);
    EXPECT_EQ(result.errors, 0u);
    EXPECT_EQ(result.attachment_errors, 1u);
    EXPECT_FALSE(result.completed);
    EXPECT_FALSE(result.cancelled);

    std::ifstream in(occupied);
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "OCCUPANT");

    OrganizeProgress p;
    size_t ticks = 0;
    while (q.pop(p)) {
        if (p.stage != OrganizeStage::FileDone) continue;
        ticks++;
        EXPECT_LE(p.done, p.total);
    }
    EXPECT_GE(ticks, 1u);
    EXPECT_EQ(newest_operation_status(db), "interrupted");
}

TEST_F(OrganizeEngineTest, AFileThatFailsInterruptsTheRun) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts(db);
    o.log_enabled = true;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_EQ(plan.size(), 2u);
    fs::remove(plan.at(0).source);

    OrganizeResult result = engine.apply(plan, false, cancel, nullptr);
    EXPECT_EQ(result.applied, 1u);
    EXPECT_EQ(result.errors, 1u);
    EXPECT_FALSE(result.completed);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(newest_operation_status(db), "interrupted");
}

// A destination whose directory cannot be searched makes the occupancy check
// itself fail. That counts as an item error; it must not throw out of apply and
// leave the operation in_progress.
TEST_F(OrganizeEngineTest, ADestinationThatCannotBeCheckedCountsAsAnErrorAndTheRunEnds) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores directory permissions";
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2020:07:04 10:00:00");

    OrganizeOptions o = base_opts(db);
    o.log_enabled = true;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_EQ(plan.size(), 2u);

    const fs::path locked = dir / "out" / "2019";
    fs::create_directories(locked);
    fs::permissions(locked, fs::perms::none);

    OrganizeResult result;
    EXPECT_NO_THROW(result = engine.apply(plan, false, cancel, nullptr));
    fs::permissions(locked, fs::perms::owner_all);

    EXPECT_EQ(result.errors, 1u);
    EXPECT_EQ(result.applied, 1u);
    EXPECT_FALSE(result.completed);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(newest_operation_status(db), "interrupted");
}

TEST_F(OrganizeEngineTest, ACleanRunIsCompleted) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts(db);
    o.log_enabled = true;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, false, cancel, nullptr);
    EXPECT_EQ(result.applied, 2u);
    EXPECT_TRUE(result.completed);
    EXPECT_FALSE(result.cancelled);
    EXPECT_EQ(result.attachment_errors, 0u);
    EXPECT_EQ(newest_operation_status(db), "completed");
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
// that calls it refuses a missing catalog before opening it, so the preview
// creates nothing. cmd_organize lives in cli/main.cpp and is not linked into
// phig-tests, so the binary is the way in.
TEST_F(OrganizeEngineTest, CliDryRunOverAMissingCatalogRefusesAndCreatesNothing) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    const fs::path absent = dir / "absent";
    const fs::path dest = dir / "out";
    ASSERT_FALSE(fs::exists(absent));

    PhigRun run = run_phig(
        phig_bin, dir, "cp '" + dest.string() + "' --dry-run --catalog '" + absent.string() + "'");

    EXPECT_EQ(run.exit_code, 1) << run.output;
    EXPECT_NE(run.output.find("no phig catalog at"), std::string::npos) << run.output;
    EXPECT_FALSE(fs::exists(dest)) << "a preview must not create the destination tree";
    EXPECT_FALSE(fs::exists(absent));
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

    PhigRun preview =
        run_phig(phig_bin, dir,
                 "cp '" + preview_dest.string() + "' --dry-run --catalog '" + dir.string() + "'");

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
    PhigRun applied = run_phig(
        phig_bin, dir, "cp '" + applied_dest.string() + "' --catalog '" + dir.string() + "'");
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
    Database::PageCriteria pc;
    pc.filter.excludes = {"*spared*"};
    pc.limit = -1;
    o.ids = db.ids_matching(pc);
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

TEST_F(OrganizeEngineTest, AnEmptySelectionPlansNothing) {
    // Empty means nothing, not everything. cp and mv move real files, so a
    // caller that named no subject getting a whole-collection move is the
    // failure this rule exists to prevent.
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts();
    o.ids = {};
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_EQ(plan.size(), 0u);
}

namespace {

// Scans `src_dir` into the catalog at `catalog`, failing the test on a nonzero exit.
void scan_into(const fs::path& bin, const fs::path& scratch, const fs::path& catalog,
               const fs::path& src_dir) {
    PhigRun run = run_phig(
        bin, scratch,
        "--catalog '" + catalog.string() + "' scan '" + src_dir.string() + "' --recursive");
    ASSERT_EQ(run.exit_code, 0) << run.output;
}

// A JPEG with no EXIF or sidecar date, so organize can only date it by mtime.
fs::path undated_photo_from_2011(const fs::path& dir) {
    fs::path p = dir / "undated.jpg";
    fs::create_directories(dir);
    fs::copy_file("tests/data/jpeg/baseline.jpg", p);
    const auto now = fs::file_time_type::clock::now();
    const auto sys_now = std::chrono::system_clock::now();
    // 2011-05-06 12:00:00 UTC.
    const auto target =
        std::chrono::sys_days{std::chrono::year{2011} / 5 / 6} + std::chrono::hours(12);
    fs::last_write_time(p, now - (sys_now - target));
    return p;
}

}  // namespace

// The date is lost on the next scan rather than by the copy itself: the copied row
// keeps the source's modified_at, the rescan sees the file's real mtime differ and
// adopts it. So the test copies, rescans, and organizes again.
TEST_F(OrganizeEngineTest, CliCopyKeepsAnUndatedPhotosMtimeThroughARescan) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    const fs::path catalog = dir / "catalog";
    undated_photo_from_2011(dir / "src");
    scan_into(phig_bin, dir, catalog, dir / "src");

    const fs::path copied = dir / "copied";
    PhigRun cp =
        run_phig(phig_bin, dir,
                 "--catalog '" + catalog.string() + "' cp '" + copied.string() +
                     "' --format '%Y/%original' --match '" + (dir / "src").string() + "/*'");
    ASSERT_EQ(cp.exit_code, 0) << cp.output;
    ASSERT_TRUE(fs::exists(copied / "2011" / "undated.jpg")) << cp.output;

    scan_into(phig_bin, dir, catalog, copied);
    const fs::path again = dir / "again";
    PhigRun cp2 = run_phig(phig_bin, dir,
                           "--catalog '" + catalog.string() + "' cp '" + again.string() +
                               "' --format '%Y/%original' --match '" + copied.string() + "/**'");
    ASSERT_EQ(cp2.exit_code, 0) << cp2.output;
    EXPECT_TRUE(fs::exists(again / "2011" / "undated.jpg")) << cp2.output;
}

TEST_F(OrganizeEngineTest, CliCrossDeviceMoveKeepsAnUndatedPhotosMtimeThroughARescan) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    const fs::path shm = "/dev/shm";
    struct stat a{}, b{};
    if (!fs::exists(shm) || stat(dir.c_str(), &a) != 0 || stat(shm.c_str(), &b) != 0 ||
        a.st_dev == b.st_dev) {
        GTEST_SKIP() << "no distinct second filesystem available for EXDEV test";
    }
    const fs::path other = make_temp_dir("organize-exdev-mtime", shm);
    const fs::path catalog = dir / "catalog";
    undated_photo_from_2011(dir / "src");
    scan_into(phig_bin, dir, catalog, dir / "src");

    PhigRun mv =
        run_phig(phig_bin, dir,
                 "--catalog '" + catalog.string() + "' mv '" + other.string() +
                     "' --format '%Y/%original' --match '" + (dir / "src").string() + "/*'");
    ASSERT_EQ(mv.exit_code, 0) << mv.output;
    ASSERT_TRUE(fs::exists(other / "2011" / "undated.jpg")) << mv.output;

    scan_into(phig_bin, dir, catalog, other);
    const fs::path again = dir / "again";
    PhigRun cp = run_phig(phig_bin, dir,
                          "--catalog '" + catalog.string() + "' cp '" + again.string() +
                              "' --format '%Y/%original' --match '" + other.string() + "/**'");
    EXPECT_EQ(cp.exit_code, 0) << cp.output;
    EXPECT_TRUE(fs::exists(again / "2011" / "undated.jpg")) << cp.output;

    fs::remove_all(other);
}
