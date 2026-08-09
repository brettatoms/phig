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

TEST_F(AbiOrganizeTest, MoveAppliesThePlanAndTheSourceIsGone) {
    OrganizeFixture fx(1);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);
    std::string source = phig_organize_plan_source_at(plan, 0);

    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_move_start(ctx, plan, &apply_op), PHIG_OK);
    phig_op_wait(apply_op);
    ASSERT_EQ(phig_op_status(apply_op), PHIG_OP_DONE);

    phig_organize_result r{};
    ASSERT_EQ(phig_op_organize_result(apply_op, &r), PHIG_OK);
    EXPECT_EQ(r.applied, 1u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_TRUE(r.completed);
    EXPECT_FALSE(fs::exists(source));
    EXPECT_TRUE(fs::exists(fs::path(fx.out_dir) / "2019" / "img0.jpg"));

    phig_op_free(apply_op);
    phig_organize_plan_free(plan);
}

TEST_F(AbiOrganizeTest, CopyAppliesThePlanAndTheSourceRemains) {
    OrganizeFixture fx(1);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);
    std::string source = phig_organize_plan_source_at(plan, 0);

    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_copy_start(ctx, plan, &apply_op), PHIG_OK);
    phig_op_wait(apply_op);

    phig_organize_result r{};
    ASSERT_EQ(phig_op_organize_result(apply_op, &r), PHIG_OK);
    EXPECT_EQ(r.applied, 1u);
    EXPECT_TRUE(fs::exists(source)) << "copy leaves the original in place";
    EXPECT_TRUE(fs::exists(fs::path(fx.out_dir) / "2019" / "img0.jpg"));

    phig_op_free(apply_op);
    phig_organize_plan_free(plan);
}

// Decision 1 on the apply side: apply snapshots the plan at start, so a caller
// may free it immediately. Run under ASan, this is what catches a borrow where
// a copy was intended.
TEST_F(AbiOrganizeTest, PlanMayBeFreedImmediatelyAfterApplyStarts) {
    OrganizeFixture fx(3);
    open(fx);
    phig_organize_plan* plan = make_plan(fx);

    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_copy_start(ctx, plan, &apply_op), PHIG_OK);
    phig_organize_plan_free(plan);  // before the operation finishes
    phig_op_wait(apply_op);
    EXPECT_EQ(phig_op_status(apply_op), PHIG_OP_DONE);

    phig_organize_result r{};
    ASSERT_EQ(phig_op_organize_result(apply_op, &r), PHIG_OK);
    EXPECT_EQ(r.applied, 3u);

    phig_op_free(apply_op);
}

TEST_F(AbiOrganizeTest, ResultIsRefusedForAPlanOperation) {
    OrganizeFixture fx(1);
    open(fx);
    phig_organize_options opts{};
    opts.destination = fx.out_dir.c_str();
    opts.format = "%Y/%original";
    opts.on_conflict = PHIG_CONFLICT_RENAME;

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_organize_plan_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_organize_result r{};
    EXPECT_EQ(phig_op_organize_result(op, &r), PHIG_ERR_INVALID_ARG)
        << "a plan operation produces a plan, not an organize result";
    phig_op_free(op);
}

// An override taken through the ABI has to survive both the planner's guard and
// move_file's refusal to overwrite.
TEST_F(AbiOrganizeTest, OverriddenOverwriteReplacesAnUntrackedOccupant) {
    OrganizeFixture fx(1);
    open(fx);
    fs::create_directories(fs::path(fx.out_dir) / "2019");
    fs::path occupied = fs::path(fx.out_dir) / "2019" / "img0.jpg";
    {
        std::ofstream f(occupied);
        f << "occupant";
    }

    phig_organize_plan* plan = make_plan(fx);
    ASSERT_TRUE(phig_organize_plan_conflicts_at(plan, 0));
    ASSERT_EQ(phig_organize_plan_set_resolution(plan, 0, PHIG_CONFLICT_OVERWRITE), PHIG_OK);
    EXPECT_STREQ(phig_organize_plan_dest_at(plan, 0), occupied.c_str());

    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_move_start(ctx, plan, &apply_op), PHIG_OK);
    phig_op_wait(apply_op);

    phig_organize_result r{};
    ASSERT_EQ(phig_op_organize_result(apply_op, &r), PHIG_OK);
    EXPECT_EQ(r.applied, 1u);
    std::ifstream f(occupied);
    std::string content;
    f >> content;
    EXPECT_NE(content, "occupant") << "the authorized overwrite replaced it";

    phig_op_free(apply_op);
    phig_organize_plan_free(plan);
}

// The gap that hid a broken switch: every other ABI test here waits rather than
// polls, so nothing exercised phig_op_poll for an organize operation at all.
TEST_F(AbiOrganizeTest, PollReportsPlannedThenOneUpdatePerParent) {
    OrganizeFixture fx(3);
    open(fx);

    phig_organize_options opts{};
    opts.destination = fx.out_dir.c_str();
    opts.format = "%Y/%original";
    opts.on_conflict = PHIG_CONFLICT_RENAME;

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_organize_plan_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    int planned = 0;
    uint64_t planned_total = 0;
    phig_op_progress p{};
    while (phig_op_poll(op, &p)) {
        if (p.stage == PHIG_STAGE_SELECTED) {
            planned++;
            planned_total = p.total;
        }
    }
    EXPECT_EQ(planned, 1) << "planning publishes exactly one Selected update";
    EXPECT_EQ(planned_total, 3u);

    phig_organize_plan* plan = nullptr;
    ASSERT_EQ(phig_op_organize_plan_take(op, &plan), PHIG_OK);
    phig_op_free(op);

    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_copy_start(ctx, plan, &apply_op), PHIG_OK);
    phig_op_wait(apply_op);

    int file_done = 0;
    uint64_t last_done = 0, last_total = 0, last_processed = 0;
    while (phig_op_poll(apply_op, &p)) {
        if (p.stage == PHIG_STAGE_FILE_DONE) {
            file_done++;
            last_done = p.done;
            last_total = p.total;
            last_processed = p.processed;
        }
    }
    EXPECT_EQ(file_done, 3) << "one update per parent";
    EXPECT_EQ(last_done, 3u);
    EXPECT_EQ(last_total, 3u) << "total counts parents, not files";
    EXPECT_EQ(last_processed, 3u) << "processed counts files written";

    phig_op_free(apply_op);
    phig_organize_plan_free(plan);
}
