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
