// Resolving exact duplicates: keep one file per group, trash the rest (plan 077).

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "dates.h"
#include "duplicates_resolve.h"
#include "fault_injection.h"
#include "hasher.h"
#include "invalid_option.h"
#include "phig_binary.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// Noon UTC on a date, as a file time. One reading of each clock, so two calls
// for the same date compare equal.
fs::file_time_type at(int year, int month, int day) {
    static const auto file_now = fs::file_time_type::clock::now();
    static const auto sys_now = std::chrono::system_clock::now();
    const auto sys =
        std::chrono::sys_days{std::chrono::year{year} / month / day} + std::chrono::hours(12);
    return file_now - std::chrono::duration_cast<fs::file_time_type::duration>(sys_now - sys);
}

}  // namespace

class DuplicatesResolveTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = make_temp_dir("dup-resolve");
        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }
    void TearDown() override {
        db.reset();
        fs::remove_all(dir);
    }

    static void write(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << contents;
    }
    static std::string read(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // A file with `contents` and a row for it as scan writes one. `dated` gives it
    // an EXIF date; otherwise it is dated by its mtime.
    int64_t add(const fs::path& p, const std::string& contents,
                fs::file_time_type mtime = at(2015, 1, 1), bool dated = true) {
        write(p, contents);
        fs::last_write_time(p, mtime);
        ImageInfo info;
        info.path = p.string();
        info.filename = p.filename().string();
        info.extension = p.extension().string();
        info.file_size = static_cast<int64_t>(contents.size());
        info.sha256 = compute_sha256(p.string());
        info.modified_at = format_time(mtime);
        info.metadata_json = dated ? R"({"DateTimeOriginal":"2019:07:04 10:00:00"})" : "{}";
        db->insert(info);
        return db->get_image_id(p.string());
    }

    std::vector<int64_t> all_ids() {
        Database::PageCriteria pc;
        pc.limit = -1;
        return db->ids_matching(pc);
    }

    ResolveOptions opts(std::vector<KeepRule> keep = default_keep_rules()) {
        ResolveOptions o;
        o.ids = all_ids();
        o.keep = std::move(keep);
        return o;
    }

    DuplicatesPlan plan_with(const ResolveOptions& o) { return DuplicatesResolver(o, *db).plan(); }

    std::string keeper_path(const ResolveGroup& g) { return g.members.at(g.keeper).path; }

    int entries(const std::string& kind) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE kind = ?",
                           -1, &s, nullptr);
        sqlite3_bind_text(s, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
        sqlite3_finalize(s);
        return n;
    }

    std::string last_operation_status() {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(
            db->db_ptr(),
            "SELECT command || ':' || status FROM operations ORDER BY id DESC LIMIT 1", -1, &s,
            nullptr);
        std::string out;
        if (sqlite3_step(s) == SQLITE_ROW)
            out = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
        return out;
    }
};

// ---- Keep rules ----

TEST_F(DuplicatesResolveTest, ParsesTheFourRulesAndRefusesAnythingElse) {
    EXPECT_EQ(parse_keep_rule("unsuffixed").kind, KeepRule::Kind::Unsuffixed);
    EXPECT_EQ(parse_keep_rule("oldest").kind, KeepRule::Kind::Oldest);
    EXPECT_EQ(parse_keep_rule("shortest").kind, KeepRule::Kind::Shortest);
    KeepRule under = parse_keep_rule("under:/mnt/photos");
    EXPECT_EQ(under.kind, KeepRule::Kind::Under);
    EXPECT_EQ(under.dir, "/mnt/photos");
    EXPECT_THROW(parse_keep_rule("under:"), InvalidOption);
    EXPECT_THROW(parse_keep_rule("newest"), InvalidOption);
    EXPECT_THROW(parse_keep_rule(""), InvalidOption);
}

TEST_F(DuplicatesResolveTest, UnsuffixedKeepsTheOriginalNameOverItsCopy) {
    add(dir / "p" / "a_1.jpg", "X", at(2011, 1, 1));  // older, but a copy's name
    add(dir / "p" / "a.jpg", "X", at(2020, 1, 1));

    DuplicatesPlan plan = plan_with(opts());
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(keeper_path(plan.at(0)), (dir / "p" / "a.jpg").string());
    EXPECT_EQ(plan.at(0).reason, KeepReason::Unsuffixed);
}

TEST_F(DuplicatesResolveTest, ASuffixIsOnlyACopyWhenTheBaseNameIsInTheGroup) {
    // x_1.jpg is not a copy of anything here, so unsuffixed cannot choose and
    // oldest does.
    add(dir / "p" / "x_1.jpg", "X", at(2011, 1, 1));
    add(dir / "p" / "y.jpg", "X", at(2020, 1, 1));

    DuplicatesPlan plan = plan_with(opts());
    EXPECT_EQ(keeper_path(plan.at(0)), (dir / "p" / "x_1.jpg").string());
    EXPECT_EQ(plan.at(0).reason, KeepReason::Oldest);
}

TEST_F(DuplicatesResolveTest, OldestIgnoresAnImplausibleTime) {
    add(dir / "p" / "epoch.jpg", "X", at(1970, 1, 2));
    add(dir / "p" / "real.jpg", "X", at(2011, 1, 1));
    add(dir / "p" / "copy.jpg", "X", at(2020, 1, 1));

    DuplicatesPlan plan = plan_with(opts({parse_keep_rule("oldest")}));
    EXPECT_EQ(keeper_path(plan.at(0)), (dir / "p" / "real.jpg").string());
}

TEST_F(DuplicatesResolveTest, UnderPrefersTheNamedTreeAndEarlierDirectoriesWin) {
    add(dir / "old" / "a.jpg", "X");
    add(dir / "library" / "a.jpg", "X");
    add(dir / "staging" / "a.jpg", "X");

    DuplicatesPlan plan = plan_with(opts({parse_keep_rule("under:" + (dir / "library").string()),
                                          parse_keep_rule("under:" + (dir / "staging").string())}));
    EXPECT_EQ(keeper_path(plan.at(0)), (dir / "library" / "a.jpg").string());
    EXPECT_EQ(plan.at(0).reason, KeepReason::Under);

    // A directory that is only a name prefix of another is not "under" it.
    DuplicatesPlan prefix = plan_with(opts({parse_keep_rule("under:" + (dir / "lib").string())}));
    EXPECT_NE(prefix.at(0).reason, KeepReason::Under);
}

TEST_F(DuplicatesResolveTest, ShortestAndThenTheLowestIdBreakTies) {
    // The longest path has the lowest id, so it is kept only if shortest does nothing.
    add(dir / "p" / "longer.jpg", "X");
    const int64_t bb = add(dir / "p" / "bb.jpg", "X");
    add(dir / "p" / "aa.jpg", "X");

    DuplicatesPlan plan = plan_with(opts());
    // Same mtimes, no suffixes: shortest narrows to bb and aa, and the lower id wins.
    EXPECT_EQ(plan.at(0).members.at(plan.at(0).keeper).id, bb);
    EXPECT_EQ(plan.at(0).reason, KeepReason::LowestId);

    DuplicatesPlan again = plan_with(opts());
    EXPECT_EQ(keeper_path(again.at(0)), keeper_path(plan.at(0))) << "a plan is deterministic";
}

TEST_F(DuplicatesResolveTest, AGroupIsShownWhenAnyMemberIsSelected) {
    const int64_t a = add(dir / "p" / "a.jpg", "X");
    add(dir / "q" / "b.jpg", "X");
    add(dir / "p" / "lonely.jpg", "Y");

    ResolveOptions o = opts();
    o.ids = {a};
    DuplicatesPlan plan = plan_with(o);
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan.at(0).members.size(), 2u) << "the unselected copy is still a member";

    o.ids = {};
    EXPECT_EQ(plan_with(o).size(), 0u) << "an empty selection selects nothing";
}

// ---- Resolving ----

TEST_F(DuplicatesResolveTest, AResolvedGroupLeavesOneFileAndCarriesTheMetadata) {
    const fs::path keep = dir / "p" / "a.jpg";
    const fs::path copy = dir / "trip" / "a_copy.jpg";
    const int64_t keep_id = add(keep, "X", at(2011, 1, 1));
    add(copy, "X", at(2020, 1, 1));
    db->set_tag({copy.string()}, "trip", true);
    db->set_tag({copy.string()}, "favorite", true);
    db->set_gallery({copy.string()}, "trip", false);
    db->set_tag({keep.string()}, "other", true);

    ResolveOptions o = opts();
    DuplicatesResolver resolver(o, *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(keeper_path(plan.at(0)), keep.string());
    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);

    EXPECT_EQ(r.resolved, 1u);
    EXPECT_EQ(r.trashed, 1u);
    EXPECT_TRUE(r.completed);
    EXPECT_TRUE(fs::exists(keep));
    EXPECT_FALSE(fs::exists(copy));
    EXPECT_EQ(db->count(), 1);

    auto tags = db->get_tags_for_image(keep.string());
    std::sort(tags.begin(), tags.end());
    EXPECT_EQ(tags, (std::vector<std::string>{"favorite", "other", "trip"}));
    EXPECT_EQ(db->get_images_by_ids({keep_id}).at(0).gallery, "trip");

    EXPECT_EQ(entries("trash"), 1);
    EXPECT_EQ(entries("relink"), 1);
    EXPECT_EQ(entries("purge_row"), 1);
    EXPECT_EQ(last_operation_status(), "duplicates-resolve:completed");
}

TEST_F(DuplicatesResolveTest, AnUndatedKeeperTakesTheOldestPlausibleMtimeInTheGroup) {
    const fs::path keep = dir / "p" / "a.jpg";
    const int64_t keep_id = add(keep, "X", at(2026, 7, 6), /*dated=*/false);
    add(dir / "p" / "a_1.jpg", "X", at(2011, 5, 6), false);
    add(dir / "p" / "a_2.jpg", "X", at(1970, 1, 2), false);

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(keeper_path(plan.at(0)), keep.string());
    CancelToken cancel;
    resolver.apply(plan, cancel, nullptr);

    EXPECT_EQ(fs::last_write_time(keep), at(2011, 5, 6));
    EXPECT_EQ(db->get_images_by_ids({keep_id}).at(0).modified_at, format_time(at(2011, 5, 6)));
    EXPECT_EQ(entries("set_mtime"), 1);
}

TEST_F(DuplicatesResolveTest, ARemovedMembersSidecarIsCarriedToTheKeeper) {
    const fs::path keep = dir / "p" / "a.jpg";
    add(keep, "X");
    add(dir / "q" / "b.jpg", "X", at(2020, 1, 1));
    write(dir / "q" / "b.xmp", "SIDECAR");

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(keeper_path(plan.at(0)), keep.string());
    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);

    EXPECT_EQ(r.attachment_errors, 0u);
    EXPECT_EQ(read(dir / "p" / "a.xmp"), "SIDECAR");
    EXPECT_FALSE(fs::exists(dir / "q" / "b.xmp"));
}

// ---- Groups left alone ----

TEST_F(DuplicatesResolveTest, DifferentGalleriesLeaveTheGroupAlone) {
    const fs::path a = dir / "p" / "a.jpg";
    const fs::path b = dir / "q" / "b.jpg";
    add(a, "X");
    add(b, "X");
    db->set_gallery({a.string()}, "2010", false);
    db->set_gallery({b.string()}, "trip", false);

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    EXPECT_EQ(plan.at(0).blocker, ResolveBlocker::DifferentGalleries);
    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);
    EXPECT_EQ(r.skipped, 1u);
    EXPECT_TRUE(fs::exists(a));
    EXPECT_TRUE(fs::exists(b));
    EXPECT_EQ(db->count(), 2);
}

TEST_F(DuplicatesResolveTest, NamedFacesThatCannotMoveLeaveTheGroupAlone) {
    const int64_t keep = add(dir / "p" / "a.jpg", "X");
    const int64_t other = add(dir / "p" / "a_1.jpg", "X");
    FaceInfo face{};
    face.width = face.height = 10;
    db->insert_face(keep, face);
    ASSERT_TRUE(db->set_face_person(db->insert_face(other, face), db->get_or_create_person("Ann")));

    DuplicatesPlan plan = plan_with(opts());
    EXPECT_EQ(plan.at(0).blocker, ResolveBlocker::NamedFaces);
}

TEST_F(DuplicatesResolveTest, ADifferentSidecarBesideTheKeeperLeavesTheGroupAlone) {
    add(dir / "p" / "a.jpg", "X");
    write(dir / "p" / "a.xmp", "KEEPER'S");
    add(dir / "p" / "a_1.jpg", "X");
    write(dir / "p" / "a_1.xmp", "THE COPY'S");

    DuplicatesPlan plan = plan_with(opts());
    EXPECT_EQ(plan.at(0).blocker, ResolveBlocker::AttachmentDiffers);
}

TEST_F(DuplicatesResolveTest, AGroupWithOneFileLeftOnDiskIsLeftAlone) {
    add(dir / "p" / "a.jpg", "X");
    add(dir / "p" / "a_1.jpg", "X");
    fs::remove(dir / "p" / "a_1.jpg");

    DuplicatesPlan plan = plan_with(opts());
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan.at(0).blocker, ResolveBlocker::TooFewOnDisk);
}

// ---- Re-verification, overrides, failure, cancellation ----

TEST_F(DuplicatesResolveTest, AMemberEditedAfterPlanningIsNotTrashed) {
    const fs::path keep = dir / "p" / "a.jpg";
    const fs::path edited = dir / "p" / "a_1.jpg";
    const fs::path copy = dir / "p" / "a_2.jpg";
    add(keep, "X");
    add(edited, "X");
    add(copy, "X");

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(keeper_path(plan.at(0)), keep.string());
    write(edited, "EDITED");
    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);

    EXPECT_EQ(r.resolved, 1u);
    EXPECT_EQ(r.trashed, 1u);
    EXPECT_EQ(read(edited), "EDITED");
    EXPECT_FALSE(fs::exists(copy));
    EXPECT_GE(db->get_image_id(edited.string()), 0);
}

TEST_F(DuplicatesResolveTest, AnEditedKeeperLeavesTheGroupAlone) {
    const fs::path keep = dir / "p" / "a.jpg";
    add(keep, "X");
    add(dir / "p" / "a_1.jpg", "X");

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    write(keep, "EDITED");
    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);

    EXPECT_EQ(r.skipped, 1u);
    EXPECT_TRUE(fs::exists(dir / "p" / "a_1.jpg"));
}

TEST_F(DuplicatesResolveTest, SetKeeperAndSetSkipChangeWhatApplyDoes) {
    add(dir / "p" / "a.jpg", "X");
    add(dir / "p" / "a_1.jpg", "X");
    add(dir / "q" / "b.jpg", "Y");
    add(dir / "q" / "b_1.jpg", "Y");

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(plan.size(), 2u);
    size_t xg = keeper_path(plan.at(0)).find("/p/") != std::string::npos ? 0 : 1;
    size_t copy_index = 1 - plan.at(xg).keeper;
    ASSERT_TRUE(plan.set_keeper(xg, copy_index));
    EXPECT_EQ(plan.at(xg).reason, KeepReason::Chosen);
    ASSERT_TRUE(plan.set_skip(1 - xg));
    EXPECT_FALSE(plan.set_keeper(1 - xg, 0)) << "a skipped group cannot be edited";
    EXPECT_FALSE(plan.set_keeper(xg, 9));
    EXPECT_FALSE(plan.set_skip(9));

    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);
    EXPECT_EQ(r.resolved, 1u);
    EXPECT_EQ(r.skipped, 1u);
    EXPECT_FALSE(fs::exists(dir / "p" / "a.jpg"));
    EXPECT_TRUE(fs::exists(dir / "p" / "a_1.jpg"));
    EXPECT_TRUE(fs::exists(dir / "q" / "b.jpg"));
    EXPECT_TRUE(fs::exists(dir / "q" / "b_1.jpg"));
}

TEST_F(DuplicatesResolveTest, AFailedCommitBringsTheTrashedFilesBack) {
    const fs::path keep = dir / "p" / "a.jpg";
    const fs::path copy = dir / "p" / "a_1.jpg";
    add(keep, "X", at(2026, 7, 6), false);
    add(copy, "X", at(2011, 5, 6), false);

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(keeper_path(plan.at(0)), keep.string());
    BlockedImageDeletes blocker(db->db_ptr(), copy.string());
    CancelToken cancel;
    ResolveResult r = resolver.apply(plan, cancel, nullptr);
    ASSERT_GT(blocker.fired(), 0) << "the fault never fired, so this test pins nothing";

    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(r.trashed, 0u);
    EXPECT_FALSE(r.completed);
    EXPECT_EQ(read(copy), "X");
    EXPECT_EQ(fs::last_write_time(keep), at(2026, 7, 6)) << "the keeper's mtime was not restored";
    EXPECT_EQ(db->count(), 2);
}

namespace {
// Registered on the test's connection, and called by a trigger when the first
// group's removed row is deleted, so the token is set inside that group and the
// engine sees it before the second.
void cancel_from_sql(sqlite3_context* ctx, int, sqlite3_value**) {
    static_cast<CancelToken*>(sqlite3_user_data(ctx))->cancel();
    sqlite3_result_null(ctx);
}
}  // namespace

TEST_F(DuplicatesResolveTest, CancellingBetweenGroupsFinishesTheCurrentOneAndStops) {
    add(dir / "p" / "a.jpg", "X");
    add(dir / "p" / "a_1.jpg", "X");
    add(dir / "q" / "b.jpg", "Y");
    add(dir / "q" / "b_1.jpg", "Y");

    DuplicatesResolver resolver(opts(), *db);
    DuplicatesPlan plan = resolver.plan();
    ASSERT_EQ(plan.size(), 2u);
    const ResolveGroup& first = plan.at(0);
    const std::string first_removed = first.members.at(1 - first.keeper).path;
    const ResolveGroup& second = plan.at(1);

    CancelToken cancel;
    ASSERT_EQ(sqlite3_create_function_v2(db->db_ptr(), "test_cancel", 0, SQLITE_UTF8, &cancel,
                                         &cancel_from_sql, nullptr, nullptr, nullptr),
              SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(db->db_ptr(),
                     ("CREATE TRIGGER cancel_after_first AFTER DELETE ON images WHEN OLD.path "
                      "= '" +
                      first_removed + "' BEGIN SELECT test_cancel(); END")
                         .c_str(),
                     nullptr, nullptr, nullptr),
        SQLITE_OK);

    ResolveResult r = resolver.apply(plan, cancel, nullptr);

    EXPECT_TRUE(r.cancelled);
    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.resolved, 1u);
    EXPECT_FALSE(fs::exists(first_removed)) << "the group in progress was not finished";
    for (const auto& m : second.members) EXPECT_TRUE(fs::exists(m.path)) << m.path;
    EXPECT_EQ(last_operation_status(), "duplicates-resolve:interrupted");
}

// ---- The command ----

class DuplicatesResolveCliTest : public DuplicatesResolveTest {
   protected:
    fs::path phig_bin;
    void SetUp() override {
        DuplicatesResolveTest::SetUp();
        phig_bin = phig_binary_path();
        if (phig_bin.empty()) GTEST_SKIP() << "phig binary not built";
        add(dir / "p" / "a.jpg", "X");
        add(dir / "p" / "a_1.jpg", "X");
    }
    PhigRun run(const std::string& args) {
        return run_phig(phig_bin, dir,
                        "--catalog '" + dir.string() + "' duplicates resolve " + args);
    }
};

TEST_F(DuplicatesResolveCliTest, DryRunPrintsThePlanAndWritesNothing) {
    PhigRun r = run("--dry-run");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_NE(r.output.find("[DRY RUN]"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("a_1.jpg"), std::string::npos) << r.output;
    EXPECT_TRUE(fs::exists(dir / "p" / "a_1.jpg"));
    EXPECT_EQ(last_operation_status(), "");
}

TEST_F(DuplicatesResolveCliTest, WithoutYesAndNoTerminalItRefusesAndWritesNothing) {
    PhigRun r = run("");
    EXPECT_EQ(r.exit_code, 1) << r.output;
    EXPECT_TRUE(fs::exists(dir / "p" / "a_1.jpg"));
    EXPECT_EQ(last_operation_status(), "");
}

TEST_F(DuplicatesResolveCliTest, YesResolvesTheGroup) {
    PhigRun r = run("--yes");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_TRUE(fs::exists(dir / "p" / "a.jpg"));
    EXPECT_FALSE(fs::exists(dir / "p" / "a_1.jpg"));
}

TEST_F(DuplicatesResolveCliTest, DryRunJsonIsValid) {
    // With --format json the command writes the document and nothing else.
    PhigRun r = run("--dry-run --format json");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    sqlite3* mem = nullptr;
    sqlite3_open(":memory:", &mem);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(mem, "SELECT json_valid(?1), json_extract(?1, '$.groups[0].keep')", -1, &s,
                       nullptr);
    sqlite3_bind_text(s, 1, r.output.c_str(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s, 0), 1) << r.output;
    const unsigned char* keep = sqlite3_column_text(s, 1);
    EXPECT_EQ(keep ? std::string(reinterpret_cast<const char*>(keep)) : "",
              (dir / "p" / "a.jpg").string());
    sqlite3_finalize(s);
    sqlite3_close(mem);
}

TEST_F(DuplicatesResolveCliTest, AnUnknownKeepRuleIsRefused) {
    PhigRun r = run("--dry-run --keep newest");
    EXPECT_EQ(r.exit_code, 1) << r.output;
}
