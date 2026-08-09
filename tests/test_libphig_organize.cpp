#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "database.h"
#include "phig/phig.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A scratch collection with `images` real files indexed under src/, plus an
// empty out/ to organize into.
struct OrganizeFixture {
    fs::path dir;
    std::string db_path;
    std::string out_dir;

    explicit OrganizeFixture(int images = 1) {
        dir = make_temp_dir("libphig-organize");
        db_path = (dir / "phig.db").string();
        out_dir = (dir / "out").string();
        fs::create_directories(dir / "src");

        Database db(db_path);
        db.init_schema();
        for (int i = 0; i < images; ++i) {
            std::string name = "img" + std::to_string(i) + ".jpg";
            fs::path p = dir / "src" / name;
            {
                std::ofstream f(p);
                f << name;
            }
            ImageInfo info;
            info.path = p.string();
            info.filename = name;
            info.extension = ".jpg";
            info.media_type = "image";
            info.sha256 = std::string(63, '0') + std::to_string(i);
            info.metadata_json = R"({"DateTimeOriginal":"2019:07:04 10:00:00"})";
            db.insert(info);
        }
    }
    ~OrganizeFixture() { fs::remove_all(dir); }
};

class AbiOrganizeTest : public ::testing::Test {
   protected:
    phig_ctx* ctx = nullptr;

    phig_ctx* open(const OrganizeFixture& fx) {
        phig_config cfg{};
        cfg.db_path = fx.db_path.c_str();
        cfg.mode = PHIG_READ_WRITE;
        EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
        return ctx;
    }

    void TearDown() override {
        if (ctx) phig_shutdown(ctx);
        ctx = nullptr;
    }

    // Plan against `fx`, waiting for the operation and taking the plan out.
    phig_organize_plan* make_plan(const OrganizeFixture& fx,
                                  phig_conflict_resolution policy = PHIG_CONFLICT_RENAME) {
        phig_organize_options opts{};
        opts.destination = fx.out_dir.c_str();
        opts.format = "%Y/%original";
        opts.on_conflict = policy;

        phig_op* op = nullptr;
        EXPECT_EQ(phig_op_organize_plan_start(ctx, &opts, &op), PHIG_OK);
        phig_op_wait(op);
        EXPECT_EQ(phig_op_status(op), PHIG_OP_DONE);

        phig_organize_plan* plan = nullptr;
        EXPECT_EQ(phig_op_organize_plan_take(op, &plan), PHIG_OK);
        phig_op_free(op);
        return plan;
    }
};

}  // namespace

TEST_F(AbiOrganizeTest, PlanListsOneItemPerImageAndWritesNothing) {
    OrganizeFixture fx(2);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);

    ASSERT_EQ(phig_organize_plan_count(plan), 2u);
    EXPECT_NE(std::string(phig_organize_plan_dest_at(plan, 0)).find("2019"), std::string::npos);
    EXPECT_FALSE(fs::exists(fx.out_dir)) << "planning must not create the destination tree";

    phig_organize_plan_free(plan);
}

// Decision 1: the plan outlives the operation that computed it. make_plan
// already frees the operation before returning, so every assertion here reads a
// plan whose operation is gone.
TEST_F(AbiOrganizeTest, PlanStaysUsableAfterTheOperationIsFreed) {
    OrganizeFixture fx(1);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);

    ASSERT_EQ(phig_organize_plan_count(plan), 1u);
    EXPECT_STRNE(phig_organize_plan_source_at(plan, 0), "");
    EXPECT_STRNE(phig_organize_plan_dest_at(plan, 0), "");

    phig_organize_plan_free(plan);
}

TEST_F(AbiOrganizeTest, SetResolutionRejectsANonConflictingItem) {
    OrganizeFixture fx(1);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);

    ASSERT_FALSE(phig_organize_plan_conflicts_at(plan, 0));
    EXPECT_EQ(phig_organize_plan_set_resolution(plan, 0, PHIG_CONFLICT_OVERWRITE),
              PHIG_ERR_INVALID_ARG);

    phig_organize_plan_free(plan);
}

TEST_F(AbiOrganizeTest, OutOfRangeAccessorsReturnDefaultsRatherThanCrashing) {
    OrganizeFixture fx(1);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);

    EXPECT_STREQ(phig_organize_plan_source_at(plan, 99), "");
    EXPECT_STREQ(phig_organize_plan_dest_at(plan, 99), "");
    EXPECT_FALSE(phig_organize_plan_conflicts_at(plan, 99));
    EXPECT_FALSE(phig_organize_plan_occupant_tracked_at(plan, 99));
    EXPECT_EQ(phig_organize_plan_set_resolution(plan, 99, PHIG_CONFLICT_SKIP),
              PHIG_ERR_INVALID_ARG);

    phig_organize_plan_free(plan);
}

TEST_F(AbiOrganizeTest, ConflictIsReportedWithWhetherPhigTracksTheOccupant) {
    OrganizeFixture fx(1);
    open(fx);
    // Occupy the destination with a file phig has no row for.
    fs::create_directories(fs::path(fx.out_dir) / "2019");
    {
        std::ofstream f(fs::path(fx.out_dir) / "2019" / "img0.jpg");
        f << "occupant";
    }

    phig_organize_plan* plan = make_plan(fx);
    ASSERT_EQ(phig_organize_plan_count(plan), 1u);
    EXPECT_TRUE(phig_organize_plan_conflicts_at(plan, 0));
    EXPECT_FALSE(phig_organize_plan_occupant_tracked_at(plan, 0));

    phig_organize_plan_free(plan);
}
