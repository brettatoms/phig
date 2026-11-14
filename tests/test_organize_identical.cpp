// Organize onto a destination that already holds the same bytes (plan 076).

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>

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
#include "fault_injection.h"
#include "hasher.h"
#include "organize_engine.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

class OrganizeIdenticalTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = make_temp_dir("organize-identical");
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

    // A row describing the file at `p` as scan would: its real sha256, and an EXIF
    // date so every fixture files under out/2019/.
    int64_t track(const fs::path& p) {
        ImageInfo info;
        info.path = p.string();
        info.filename = p.filename().string();
        info.extension = p.extension().string();
        info.file_size = static_cast<int64_t>(fs::file_size(p));
        info.sha256 = compute_sha256(p.string());
        info.metadata_json = R"({"DateTimeOriginal":"2019:07:04 10:00:00"})";
        db->insert(info);
        return db->get_image_id(p.string());
    }

    fs::path out() const { return dir / "out"; }
    fs::path dest_for(const std::string& name) const { return out() / "2019" / name; }

    OrganizeOptions opts(const std::vector<int64_t>& ids, ConflictResolution policy) {
        OrganizeOptions o;
        o.destination = out().string();
        o.format = "%Y/%original";
        o.ids = ids;
        o.on_conflict = policy;
        o.db_path = db_path;
        o.log_enabled = true;
        return o;
    }

    int entries(const std::string& kind) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db->db_ptr(), "SELECT COUNT(*) FROM operation_entries WHERE kind = ?",
                           -1, &s, nullptr);
        sqlite3_bind_text(s, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
        sqlite3_finalize(s);
        return n;
    }

    // Where the activity log says `src` was trashed to, or "" when it was not.
    std::string trashed_to(const fs::path& src) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db->db_ptr(),
                           "SELECT dst FROM operation_entries WHERE kind = 'trash' AND src = ?", -1,
                           &s, nullptr);
        sqlite3_bind_text(s, 1, src.c_str(), -1, SQLITE_TRANSIENT);
        std::string dst;
        if (sqlite3_step(s) == SQLITE_ROW) {
            dst = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        }
        sqlite3_finalize(s);
        return dst;
    }

    std::vector<std::string> files_in(const fs::path& d) {
        std::vector<std::string> names;
        if (!fs::exists(d)) return names;
        for (const auto& e : fs::directory_iterator(d)) names.push_back(e.path().filename());
        std::sort(names.begin(), names.end());
        return names;
    }

    static ino_t inode_of(const fs::path& p) {
        struct stat st{};
        stat(p.c_str(), &st);
        return st.st_ino;
    }
};

TEST_F(OrganizeIdenticalTest, MoveOntoAnIdenticalUntrackedFileTrashesTheSourceAndAdoptsTheDest) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t id = track(src);

    // Skip would leave the source in place for an ordinary conflict. Identical
    // content is not a conflict, so the policy does not apply.
    OrganizeEngine engine(opts({id}, ConflictResolution::Skip), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.at(0).identical);
    EXPECT_FALSE(plan.at(0).conflicts);
    EXPECT_EQ(plan.at(0).dest, dest_for("a.jpg").string());

    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(r.identical, 1u);
    EXPECT_EQ(r.applied, 0u);
    EXPECT_EQ(r.skipped, 0u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_TRUE(r.completed);
    EXPECT_FALSE(fs::exists(src));
    EXPECT_EQ(read(dest_for("a.jpg")), "SAME BYTES");
    EXPECT_EQ(files_in(out() / "2019"), std::vector<std::string>{"a.jpg"});

    // The row now describes the destination file, and keeps its id.
    EXPECT_EQ(db->get_image_id(dest_for("a.jpg").string()), id);
    EXPECT_EQ(db->count(), 1);

    const std::string trashed = trashed_to(src);
    ASSERT_FALSE(trashed.empty()) << "no trash entry for the source";
    EXPECT_EQ(read(trashed), "SAME BYTES");
    EXPECT_EQ(entries("adopt"), 1);
    EXPECT_EQ(entries("move"), 0);
}

TEST_F(OrganizeIdenticalTest, MoveOntoAnIdenticalTrackedFileCarriesMetadataAndDropsTheSourceRow) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t src_id = track(src);
    const int64_t dst_id = track(dest_for("a.jpg"));
    db->set_tag({src.string()}, "trip", true);
    db->set_tag({src.string()}, "favorite", true);
    db->set_gallery({src.string()}, "trip", false);
    db->set_tag({dest_for("a.jpg").string()}, "other", true);

    OrganizeEngine engine(opts({src_id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(0).identical);

    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(r.identical, 1u);
    EXPECT_FALSE(fs::exists(src));
    EXPECT_EQ(files_in(out() / "2019"), std::vector<std::string>{"a.jpg"});
    EXPECT_EQ(db->count(), 1);
    EXPECT_EQ(db->get_image_id(src.string()), -1);
    EXPECT_EQ(db->get_image_id(dest_for("a.jpg").string()), dst_id);

    auto tags = db->get_tags_for_image(dest_for("a.jpg").string());
    std::sort(tags.begin(), tags.end());
    EXPECT_EQ(tags, (std::vector<std::string>{"favorite", "other", "trip"}));
    auto rows = db->get_images_by_ids({dst_id});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].gallery, "trip");

    EXPECT_EQ(entries("relink"), 1);
    EXPECT_EQ(entries("purge_row"), 1) << "the source row was deleted without a restore snapshot";
    EXPECT_FALSE(trashed_to(src).empty());
}

TEST_F(OrganizeIdenticalTest, DifferentGalleriesFallBackToTheConflictPolicy) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t src_id = track(src);
    track(dest_for("a.jpg"));
    db->set_gallery({src.string()}, "trip", false);
    db->set_gallery({dest_for("a.jpg").string()}, "2010", false);

    OrganizeEngine engine(opts({src_id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_FALSE(plan.at(0).identical);
    EXPECT_TRUE(plan.at(0).conflicts);
    EXPECT_NE(plan.at(0).dest, dest_for("a.jpg").string()) << "rename policy should apply";
}

TEST_F(OrganizeIdenticalTest, NamedFacesOnTheSourceAndFacesOnTheDestFallBack) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t src_id = track(src);
    const int64_t dst_id = track(dest_for("a.jpg"));
    FaceInfo face{};
    face.width = face.height = 10;
    const int64_t named = db->insert_face(src_id, face);
    ASSERT_TRUE(db->set_face_person(named, db->get_or_create_person("Ann")));
    db->insert_face(dst_id, face);

    OrganizeEngine engine(opts({src_id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_FALSE(plan.at(0).identical);
    EXPECT_TRUE(plan.at(0).conflicts);
}

TEST_F(OrganizeIdenticalTest, TwoIdenticalSourcesPlannedToOneDestinationLeaveOneFile) {
    const fs::path a = dir / "one" / "x.jpg";
    const fs::path b = dir / "two" / "x.jpg";
    write(a, "SAME BYTES");
    write(b, "SAME BYTES");
    const int64_t a_id = track(a);
    const int64_t b_id = track(b);

    OrganizeEngine engine(opts({a_id, b_id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_FALSE(plan.at(0).identical);
    EXPECT_TRUE(plan.at(1).identical);
    EXPECT_EQ(plan.at(1).dest, dest_for("x.jpg").string());

    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(r.applied, 1u);
    EXPECT_EQ(r.identical, 1u);
    EXPECT_EQ(files_in(out() / "2019"), std::vector<std::string>{"x.jpg"});
    EXPECT_FALSE(fs::exists(a));
    EXPECT_FALSE(fs::exists(b));
    EXPECT_EQ(db->count(), 1);
}

TEST_F(OrganizeIdenticalTest, WhenTheFirstOfTwoIdenticalSourcesFailsTheSecondMovesNormally) {
    const fs::path a = dir / "one" / "x.jpg";
    const fs::path b = dir / "two" / "x.jpg";
    write(a, "SAME BYTES");
    write(b, "SAME BYTES");
    const int64_t a_id = track(a);
    const int64_t b_id = track(b);

    OrganizeEngine engine(opts({a_id, b_id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(1).identical);
    fs::remove(plan.at(0).source);  // the first item's move will fail

    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(r.applied, 1u);
    EXPECT_EQ(r.identical, 0u);
    EXPECT_EQ(read(dest_for("x.jpg")), "SAME BYTES");
    EXPECT_FALSE(fs::exists(plan.at(1).source));
    EXPECT_EQ(db->get_image_id(dest_for("x.jpg").string()),
              plan.at(1).source == a.string() ? a_id : b_id);
}

TEST_F(OrganizeIdenticalTest, ASourceEditedAfterPlanningIsSkippedNotTrashed) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t id = track(src);

    OrganizeEngine engine(opts({id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(0).identical);
    write(src, "EDITED BYTES");

    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(r.skipped, 1u);
    EXPECT_EQ(r.identical, 0u);
    EXPECT_EQ(read(src), "EDITED BYTES");
    EXPECT_EQ(read(dest_for("a.jpg")), "SAME BYTES");
    EXPECT_EQ(db->get_image_id(src.string()), id);
    EXPECT_TRUE(trashed_to(src).empty());
}

TEST_F(OrganizeIdenticalTest, ASidecarWithDifferentBytesAtTheDestinationFallsBack) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dir / "src" / "a.xmp", "SOURCE SIDECAR");
    write(dest_for("a.jpg"), "SAME BYTES");
    write(dest_for("a.xmp"), "OTHER SIDECAR");
    const int64_t id = track(src);

    OrganizeEngine engine(opts({id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_FALSE(plan.at(0).identical);
    EXPECT_TRUE(plan.at(0).conflicts);
}

TEST_F(OrganizeIdenticalTest, AMissingSidecarIsCarriedAndAnIdenticalOneIsTrashed) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dir / "src" / "a.xmp", "SAME SIDECAR");
    write(dir / "src" / "a.json", "ONLY AT SOURCE");
    write(dest_for("a.jpg"), "SAME BYTES");
    write(dest_for("a.xmp"), "SAME SIDECAR");
    const int64_t id = track(src);

    OrganizeEngine engine(opts({id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(0).identical);

    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(r.identical, 1u);
    EXPECT_EQ(r.attachment_errors, 0u);
    EXPECT_EQ(files_in(dir / "src"), std::vector<std::string>{});
    EXPECT_EQ(files_in(out() / "2019"), (std::vector<std::string>{"a.jpg", "a.json", "a.xmp"}));
    EXPECT_EQ(read(dest_for("a.json")), "ONLY AT SOURCE");
    EXPECT_FALSE(trashed_to(dir / "src" / "a.xmp").empty());
}

TEST_F(OrganizeIdenticalTest, CopyOntoAnIdenticalUntrackedFileWritesNothingAndTracksIt) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const ino_t before = inode_of(dest_for("a.jpg"));
    const int64_t id = track(src);

    OrganizeEngine engine(opts({id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(0).identical);

    OrganizeResult r = engine.apply(plan, /*is_move=*/false, cancel, nullptr);

    EXPECT_EQ(r.identical, 1u);
    EXPECT_EQ(r.files_written, 0u);
    EXPECT_EQ(inode_of(dest_for("a.jpg")), before) << "the destination was rewritten";
    EXPECT_TRUE(fs::exists(src));
    EXPECT_EQ(db->count(), 2);
    EXPECT_GE(db->get_image_id(dest_for("a.jpg").string()), 0);
    // An undo of a `copy` entry would delete the destination, which phig did not write.
    EXPECT_EQ(entries("copy"), 0);
    EXPECT_EQ(entries("adopt"), 1);
}

TEST_F(OrganizeIdenticalTest, CopyOntoAnIdenticalTrackedFileChangesNothing) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t id = track(src);
    track(dest_for("a.jpg"));

    OrganizeEngine engine(opts({id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizeResult r =
        engine.apply(engine.plan(cancel, nullptr), /*is_move=*/false, cancel, nullptr);

    EXPECT_EQ(r.identical, 1u);
    EXPECT_EQ(db->count(), 2);
    EXPECT_EQ(files_in(out() / "2019"), std::vector<std::string>{"a.jpg"});
    EXPECT_EQ(entries("copy"), 0);
}

TEST_F(OrganizeIdenticalTest, AFailedCommitBringsTheTrashedSourceBack) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(dest_for("a.jpg"), "SAME BYTES");
    const int64_t src_id = track(src);
    track(dest_for("a.jpg"));

    OrganizeEngine engine(opts({src_id}, ConflictResolution::Rename), *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(0).identical);

    BlockedImageDeletes blocker(db->db_ptr(), src.string());
    OrganizeResult r = engine.apply(plan, /*is_move=*/true, cancel, nullptr);
    ASSERT_GT(blocker.fired(), 0) << "the fault never fired, so this test pins nothing";

    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(r.identical, 0u);
    EXPECT_EQ(read(src), "SAME BYTES") << "the source was not brought back from the trash";
    EXPECT_EQ(db->get_image_id(src.string()), src_id);
    EXPECT_EQ(db->count(), 2);
}

// ---- The older-mtime rule (plan 076 decision 3) ----

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

class OrganizeIdenticalMtimeTest : public OrganizeIdenticalTest {
   protected:
    // A row as scan writes one for a file with no EXIF date: it files by mtime.
    int64_t track_undated(const fs::path& p) {
        ImageInfo info;
        info.path = p.string();
        info.filename = p.filename().string();
        info.extension = p.extension().string();
        info.file_size = static_cast<int64_t>(fs::file_size(p));
        info.sha256 = compute_sha256(p.string());
        info.metadata_json = "{}";
        info.modified_at = format_time(fs::last_write_time(p));
        db->insert(info);
        return db->get_image_id(p.string());
    }

    std::string row_modified_at(int64_t id) {
        auto rows = db->get_images_by_ids({id});
        return rows.empty() ? "" : rows[0].modified_at;
    }

    // An undated source with mtime `src_time`, moved onto an identical untracked
    // file with mtime `dst_time`. `%original` keeps the destination fixed rather
    // than dated by the very mtime under test.
    OrganizeResult move_undated(fs::file_time_type src_time, fs::file_time_type dst_time,
                                int64_t& id) {
        const fs::path src = dir / "src" / "a.jpg";
        write(src, "SAME BYTES");
        write(out() / "a.jpg", "SAME BYTES");
        fs::last_write_time(src, src_time);
        fs::last_write_time(out() / "a.jpg", dst_time);
        id = track_undated(src);
        OrganizeOptions o = opts({id}, ConflictResolution::Rename);
        o.format = "%original";
        OrganizeEngine engine(o, *db);
        CancelToken cancel;
        OrganizePlan plan = engine.plan(cancel, nullptr);
        EXPECT_TRUE(plan.at(0).identical);
        return engine.apply(plan, /*is_move=*/true, cancel, nullptr);
    }
};

TEST_F(OrganizeIdenticalMtimeTest, AnUndatedDestinationTakesTheSourcesOlderMtime) {
    int64_t id = 0;
    OrganizeResult r = move_undated(at(2011, 5, 6), at(2026, 7, 6), id);

    ASSERT_EQ(r.identical, 1u);
    EXPECT_EQ(fs::last_write_time(out() / "a.jpg"), at(2011, 5, 6));
    EXPECT_EQ(row_modified_at(id), format_time(at(2011, 5, 6)));
    auto rows = db->get_images_by_ids({id});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].effective_date.substr(0, 10), "2011-05-06");
    EXPECT_EQ(entries("set_mtime"), 1);
}

TEST_F(OrganizeIdenticalMtimeTest, ANewerSourceMtimeIsNotTaken) {
    int64_t id = 0;
    move_undated(at(2026, 7, 6), at(2011, 5, 6), id);

    EXPECT_EQ(fs::last_write_time(out() / "a.jpg"), at(2011, 5, 6));
    // The row moved onto the destination describes the file that is there.
    EXPECT_EQ(row_modified_at(id), format_time(at(2011, 5, 6)));
    EXPECT_EQ(entries("set_mtime"), 0);
}

TEST_F(OrganizeIdenticalMtimeTest, ImplausibleSourceTimesAreIgnored) {
    for (auto bogus : {at(1970, 1, 1), at(1980, 1, 1)}) {
        int64_t id = 0;
        move_undated(bogus, at(2026, 7, 6), id);
        EXPECT_EQ(fs::last_write_time(out() / "a.jpg"), at(2026, 7, 6))
            << format_time(bogus) << " was taken";
        EXPECT_EQ(entries("set_mtime"), 0);
        fs::remove_all(out());
        db->delete_paths({out() / "a.jpg"});
    }
}

TEST_F(OrganizeIdenticalMtimeTest, AnExifDatedDestinationKeepsItsMtime) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(out() / "a.jpg", "SAME BYTES");
    fs::last_write_time(src, at(2011, 5, 6));
    fs::last_write_time(out() / "a.jpg", at(2026, 7, 6));
    const int64_t id = track(src);  // EXIF DateTimeOriginal 2019

    OrganizeOptions o = opts({id}, ConflictResolution::Rename);
    o.format = "%original";
    OrganizeEngine engine(o, *db);
    CancelToken cancel;
    OrganizeResult r = engine.apply(engine.plan(cancel, nullptr), true, cancel, nullptr);

    ASSERT_EQ(r.identical, 1u);
    EXPECT_EQ(fs::last_write_time(out() / "a.jpg"), at(2026, 7, 6));
    EXPECT_EQ(row_modified_at(id), format_time(at(2026, 7, 6)));
    EXPECT_EQ(entries("set_mtime"), 0);
}

TEST_F(OrganizeIdenticalMtimeTest, AFailedCommitRestoresTheDestinationsMtime) {
    const fs::path src = dir / "src" / "a.jpg";
    write(src, "SAME BYTES");
    write(out() / "a.jpg", "SAME BYTES");
    fs::last_write_time(src, at(2011, 5, 6));
    fs::last_write_time(out() / "a.jpg", at(2026, 7, 6));
    const int64_t id = track_undated(src);
    track_undated(out() / "a.jpg");

    OrganizeOptions o = opts({id}, ConflictResolution::Rename);
    o.format = "%original";
    OrganizeEngine engine(o, *db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.at(0).identical);
    BlockedImageDeletes blocker(db->db_ptr(), src.string());
    OrganizeResult r = engine.apply(plan, true, cancel, nullptr);
    ASSERT_GT(blocker.fired(), 0);

    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(fs::last_write_time(out() / "a.jpg"), at(2026, 7, 6));
    EXPECT_EQ(fs::last_write_time(src), at(2011, 5, 6));
}
