#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "face_path.h"
#include "faces_engine.h"
#include "fault_injection.h"
#include "models.h"
#include "paths.h"
#include "phig_binary.h"
#include "progress_queue.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {
// Alternating face fixtures at different dimensions (320x320 and 448x280).
// Same-size images would let a shared detector pass, because the detector
// caches its input size and only reconfigures when the size changes.
struct ScratchFaces {
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    explicit ScratchFaces(int n) {
        dir = fs::temp_directory_path() / ("phig_faces_engine_" + std::to_string(::getpid()) + "_" +
                                           std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
        // The sha prefix is unique per fixture instance (the pointer value
        // baked into `dir`) so rows from different tests never collide.
        std::string prefix = std::to_string(reinterpret_cast<uintptr_t>(this));
        for (int i = 0; i < n; ++i) {
            const char* src =
                (i % 2 == 0) ? "tests/data/faces_small.jpg" : "tests/data/faces_large.jpg";
            fs::path p = dir / ("f" + std::to_string(i) + ".jpg");
            fs::copy_file(src, p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            std::string sha = prefix + std::to_string(i);
            info.sha256 = sha.substr(0, 63) + std::to_string(i % 10);
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db->insert(info);
        }
    }
    // Every row. These tests predate id selection and meant "the whole
    // collection" by setting no filter; an empty selection now means nothing,
    // so what they meant has to be said.
    std::vector<int64_t> all_ids() const {
        Database::PageCriteria pc;
        pc.limit = -1;
        return db->ids_matching(pc);
    }

    ~ScratchFaces() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Total rows in the faces table. Database exposes insert_face and
// delete_faces_for_image but no per-image getter, so read the table directly.
size_t count_face_rows(const ScratchFaces& coll) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(coll.db_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << "could not open " << coll.db_path;
        if (raw) sqlite3_close(raw);
        return 0;
    }
    sqlite3_stmt* stmt = nullptr;
    size_t n = 0;
    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM faces", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            n = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
        }
    } else {
        ADD_FAILURE() << "prepare failed: " << sqlite3_errmsg(raw);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    return n;
}

struct FaceRow {
    int64_t id;
    int x, y, width, height;
    int64_t person_id;  // -1 when unlabelled
};

// One image's face rows in id order, read straight from the table.
std::vector<FaceRow> faces_of(Database& db, int64_t image_id) {
    std::vector<FaceRow> out;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db.db_ptr(),
                           "SELECT id, x, y, width, height, COALESCE(person_id, -1) "
                           "FROM faces WHERE image_id = ? ORDER BY id",
                           -1, &s, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << "prepare failed: " << sqlite3_errmsg(db.db_ptr());
        return out;
    }
    sqlite3_bind_int64(s, 1, image_id);
    while (sqlite3_step(s) == SQLITE_ROW) {
        out.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int(s, 1),
                       sqlite3_column_int(s, 2), sqlite3_column_int(s, 3), sqlite3_column_int(s, 4),
                       sqlite3_column_int64(s, 5)});
    }
    sqlite3_finalize(s);
    return out;
}
}  // namespace

TEST(FacesEngineRun, DetectsFacesInEveryImageAcrossWorkers) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    opts.ids = coll.all_ids();
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<FacesProgress> q(64);

    FacesResult r = engine.run(tok, &q);
    q.close();

    EXPECT_EQ(r.total_selected, 4u);
    // 6 is the deterministic count for two faces_small.jpg + two
    // faces_large.jpg copies (confirmed across repeated runs); a partial-
    // processing bug that only ran one of the four images would report fewer.
    EXPECT_EQ(r.faces_detected, 6u);
    EXPECT_TRUE(r.completed);

    size_t files = 0;
    FacesProgress p;
    while (q.pop(p)) {
        if (p.stage == FacesStage::FileDone) ++files;
    }
    EXPECT_EQ(files, 4u);
}

TEST(FacesEngineRun, RerunReplacesRatherThanAccumulatesFaceRows) {
    // The delete-then-reinsert contract. If the delete were dropped, a second
    // rebuild would double every image's face count.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    opts.ids = coll.all_ids();
    CancelToken tok;

    FacesResult first = FacesEngine(opts, *coll.db).run(tok, nullptr);
    FacesResult second = FacesEngine(opts, *coll.db).run(tok, nullptr);

    // Without detections both runs report zero and the comparison below passes
    // whether or not the delete happened.
    ASSERT_GT(first.faces_detected, 0u);
    EXPECT_EQ(second.faces_detected, first.faces_detected);

    // And the stored rows match the reported count, not twice it. Database has
    // no per-image face getter, so count directly. The db handle must be closed
    // first — this opens a second connection to the same file.
    size_t stored = count_face_rows(coll);
    EXPECT_EQ(stored, second.faces_detected);
}

TEST(FacesEngineRun, RebuildKeepsALabelOnTheFaceItNamed) {
    // R60. The named face is deleted and re-detected in the same transaction;
    // the label has to land on the re-detected face with the same box, not on
    // another face in the image and not nowhere.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(2);  // one faces_small.jpg, one faces_large.jpg
    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.ids = coll.all_ids();
    CancelToken tok;
    ASSERT_GT(FacesEngine(opts, *coll.db).run(tok, nullptr).faces_detected, 0u);

    // Name the second face of an image that has at least two, so a label that
    // landed on the wrong face is distinguishable from one that landed right.
    int64_t image_id = -1;
    std::vector<FaceRow> before;
    for (int64_t id : opts.ids) {
        before = faces_of(*coll.db, id);
        if (before.size() >= 2) {
            image_id = id;
            break;
        }
    }
    ASSERT_GE(image_id, 0) << "no fixture image has two faces";
    const FaceRow named = before[1];
    int64_t ada = coll.db->get_or_create_person("Ada");
    ASSERT_TRUE(coll.db->set_face_person(named.id, ada));

    FacesResult r = FacesEngine(opts, *coll.db).run(tok, nullptr);
    EXPECT_EQ(r.labels_dropped, 0u);

    std::vector<FaceRow> after = faces_of(*coll.db, image_id);
    ASSERT_EQ(after.size(), before.size());
    bool found = false;
    for (const FaceRow& f : after) {
        if (f.x == named.x && f.y == named.y && f.width == named.width &&
            f.height == named.height) {
            found = true;
            EXPECT_EQ(f.person_id, ada) << "the re-detected face lost its label";
            EXPECT_NE(f.id, named.id) << "the face row was not replaced";
        }
    }
    EXPECT_TRUE(found) << "no re-detected face has the named face's box";
}

TEST(FacesEngineRun, RebuildCountsALabelItCouldNotCarry) {
    // A labelled face no detection overlaps: the label is dropped, and the
    // drop is counted rather than silent.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(1);
    int64_t image_id = coll.db->get_image_id((coll.dir / "f0.jpg").string());
    ASSERT_GT(image_id, 0);
    FaceInfo phantom{};
    phantom.embedding.fill(0.5f);
    phantom.x = 0;
    phantom.y = 0;
    phantom.width = 4;
    phantom.height = 4;
    int64_t face_id = coll.db->insert_face(image_id, phantom);
    ASSERT_TRUE(coll.db->set_face_person(face_id, coll.db->get_or_create_person("Ada")));

    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.ids = coll.all_ids();
    CancelToken tok;
    FacesResult r = FacesEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_GT(r.faces_detected, 0u);
    EXPECT_EQ(r.labels_dropped, 1u);
    for (const auto& p : coll.db->list_people()) {
        if (p.name == "Ada") EXPECT_EQ(p.face_count, 0);
    }
}

TEST(FacesEngineRun, RebuildOverUnlabelledFacesDropsNothing) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(2);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    opts.ids = coll.all_ids();
    CancelToken tok;
    FacesEngine(opts, *coll.db).run(tok, nullptr);
    FacesResult r = FacesEngine(opts, *coll.db).run(tok, nullptr);
    EXPECT_GT(r.faces_detected, 0u);
    EXPECT_EQ(r.labels_dropped, 0u);
}

TEST(FacesEngineRun, AlreadyCancelledDetectsNothingAndReportsIncomplete) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    tok.cancel();

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.faces_detected, 0u);
}

TEST(FacesEngineRun, AlreadyCancelledOverAnEmptySelectionReportsIncomplete) {
    // The empty-selection return must not precede the cancellation check: an
    // already-cancelled run with nothing selected still has to report
    // completed = false.
    ScratchFaces coll(0);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    tok.cancel();

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 0u);
    EXPECT_FALSE(r.completed);
}

TEST(FacesEngineRun, PublishesModelLoadThenSelectedThenFiles) {
    // Ordering matters for rendering: the CLI prints "Loading face recognition
    // models (N instances)..." before the per-file lines, and model loading is
    // the slowest part of a small rebuild.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(2);
    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.ids = coll.all_ids();
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<FacesProgress> q(64);

    engine.run(tok, &q);
    q.close();

    FacesProgress p;
    ASSERT_TRUE(q.pop(p));
    EXPECT_EQ(p.stage, FacesStage::Selected);
    EXPECT_EQ(p.total, 2u);

    ASSERT_TRUE(q.pop(p));
    EXPECT_EQ(p.stage, FacesStage::LoadingFaceModels);
    EXPECT_EQ(p.total, 1u);  // one detector instance for parallel=1

    size_t files = 0;
    while (q.pop(p)) {
        if (p.stage == FacesStage::FileDone) ++files;
    }
    EXPECT_EQ(files, 2u);
}

TEST(FacesEngineRun, UndecodableFileIsSkippedWithoutFailingTheRun) {
    // A file that cannot be decoded must not abort the rebuild or write rows.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(2);
    fs::path junk = coll.dir / "broken.jpg";
    {
        std::ofstream(junk) << "not an image";
    }
    ImageInfo info;
    info.path = junk.string();
    info.filename = "broken.jpg";
    info.extension = "jpg";
    info.file_size = fs::file_size(junk);
    info.media_type = "image";
    std::string prefix = std::to_string(reinterpret_cast<uintptr_t>(&coll));
    info.sha256 = (prefix + std::string(63, '9')).substr(0, 63) + "9";
    info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z";
    coll.db->insert(info);

    FacesRebuildOptions opts;
    opts.parallel = 2;
    opts.ids = coll.all_ids();
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r;
    EXPECT_NO_THROW(r = engine.run(tok, nullptr));
    EXPECT_EQ(r.total_selected, 3u);
    EXPECT_TRUE(r.completed);
}

TEST(FacesEngineRun, UndecodableFileIsCountedAsAnError) {
    // Skipping the file is right; skipping it silently is not. Without a count,
    // an unreadable image is indistinguishable from one with no faces in it, so
    // a rebuild over a collection with unreadable files reports total success.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(2);
    fs::path junk = coll.dir / "broken.jpg";
    {
        std::ofstream(junk) << "not an image";
    }
    ImageInfo info;
    info.path = junk.string();
    info.filename = "broken.jpg";
    info.extension = "jpg";
    info.file_size = fs::file_size(junk);
    info.media_type = "image";
    std::string prefix = std::to_string(reinterpret_cast<uintptr_t>(&coll));
    info.sha256 = (prefix + std::string(63, '8')).substr(0, 63) + "8";
    info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z";
    coll.db->insert(info);

    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.ids = coll.all_ids();
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 3u);
    EXPECT_EQ(r.errors, 1u);
    EXPECT_TRUE(r.completed);  // one bad file does not fail the run
}

TEST(FacesEngineRun, UndecodableFilePublishesTheDecodeErrorOnProgress) {
    // The reason already exists — decode_image_with_warning captures OpenCV's
    // own message per decode — and is currently discarded. Route it to the
    // caller the way ScanProgress::error does, so a client can say which file
    // failed and why rather than only how many did.
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(1);
    fs::path junk = coll.dir / "broken.jpg";
    {
        std::ofstream(junk) << "not an image";
    }
    ImageInfo info;
    info.path = junk.string();
    info.filename = "broken.jpg";
    info.extension = "jpg";
    info.file_size = fs::file_size(junk);
    info.media_type = "image";
    std::string prefix = std::to_string(reinterpret_cast<uintptr_t>(&coll));
    info.sha256 = (prefix + std::string(63, '7')).substr(0, 63) + "7";
    info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z";
    coll.db->insert(info);

    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.ids = coll.all_ids();
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<FacesProgress> q(64);

    engine.run(tok, &q);
    q.close();

    bool saw_error = false;
    FacesProgress p;
    while (q.pop(p)) {
        if (p.stage != FacesStage::FileDone) continue;
        if (p.path != junk.string()) {
            EXPECT_TRUE(p.error.empty()) << "decodable file reported an error: " << p.error;
            continue;
        }
        saw_error = !p.error.empty();
        EXPECT_NE(p.error.find("broken.jpg"), std::string::npos)
            << "error text should name the file: " << p.error;
    }
    EXPECT_TRUE(saw_error) << "no error text published for the undecodable file";
}

TEST(FacesEngineRun, SelectionFiltersRestrictWhatIsProcessed) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 1;
    // The engine takes ids; the glob is resolved by whoever calls it, which for
    // the CLI is Database::ids_matching.
    Database::PageCriteria pc;
    pc.filter.matches = {(coll.dir / "f0.jpg").string()};
    pc.limit = -1;
    opts.ids = coll.db->ids_matching(pc);
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
}

// A lock-free rebuild runs beside `phig rm`, so an image row can vanish while a
// worker is mid-file. Reading the image id outside the write transaction leaves
// a window where the row is gone by the time the face insert runs, and that
// insert violates faces.image_id's foreign key. parallel_for_each keeps the
// first exception and sets `failed`, which every other worker checks at the top
// of its loop -- so one deleted image abandons the entire rebuild rather than
// skipping one file.
TEST(FacesEngineRun, ImageDeletedMidRunIsSkippedRatherThanAbortingTheRun) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(40);

    std::vector<std::string> paths;
    {
        sqlite3_stmt* s = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(coll.db->db_ptr(), "SELECT path FROM images ORDER BY id", -1,
                                     &s, nullptr),
                  SQLITE_OK);
        while (sqlite3_step(s) == SQLITE_ROW) {
            paths.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)));
        }
        sqlite3_finalize(s);
    }
    ASSERT_EQ(paths.size(), 40u);

    FacesRebuildOptions opts;
    opts.parallel = 4;
    opts.ids = coll.all_ids();
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    // The gap between reading an image id and opening the write transaction is
    // two statements wide, so a deleter that merely runs alongside never lands
    // in it -- measured, 10 of 10 runs miss. What makes it reachable is the
    // thing that widens it: BEGIN IMMEDIATE waits on the busy timeout. This
    // deleter holds the write lock, so a worker reads a live image id and then
    // blocks; the row is deleted and committed while it waits; it wakes up and
    // writes faces for an image that is gone. That is the same window `phig rm`
    // hits, held open long enough to observe.
    std::atomic<bool> running{true};
    Database other(coll.db_path);
    std::thread deleter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        other.begin_transaction();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        for (const auto& p : paths) {
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(other.db_ptr(), "DELETE FROM images WHERE path = ?", -1, &s,
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }
        other.commit_transaction();
        running.store(false);
    });

    ProgressQueue<FacesProgress> q;
    FacesResult r;
    std::string threw;
    try {
        r = engine.run(tok, &q);
    } catch (const std::exception& e) {
        threw = e.what();
    }
    running.store(false);
    deleter.join();
    q.close();

    EXPECT_TRUE(threw.empty()) << "the rebuild aborted instead of skipping: " << threw;

    // "Nothing threw" alone would pass a run whose timing missed the race
    // entirely, which is the same verdict a broken guard gets on a lucky run.
    // The engine counts a lost row as an error and publishes the reason, so
    // both are asserted: a run that never enters the window now fails.
    EXPECT_GT(r.errors, 0u) << "no file was skipped -- the race window was never entered";
    size_t vanished = 0;
    FacesProgress p;
    while (q.try_pop(p)) {
        if (p.error.find("image no longer in the database") != std::string::npos) vanished++;
    }
    EXPECT_GT(vanished, 0u) << "no progress line named a row that vanished mid-run";
}

// A rebuild deletes an image's faces before re-detecting them, and an empty
// match list selects the whole collection -- so a flag the parser does not
// recognize has to stop the command, not run it over everything. That guard is
// in argv parsing, which lives in cli/main.cpp and is not linked into
// phig-tests, so this drives the built binary as a subprocess.
TEST(FacesEngineRun, CliRejectsAnUnknownFlagWithoutTouchingFaceData) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }

    ScratchFaces coll(2);

    // One face row carrying a person's name. Written directly rather than
    // detected: what is being pinned is that the command leaves it alone, and
    // that holds on a machine with no face models too.
    int64_t image_id = coll.db->get_image_id((coll.dir / "f0.jpg").string());
    ASSERT_GT(image_id, 0);
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.x = 1;
    face.y = 2;
    face.width = 10;
    face.height = 10;
    int64_t face_id = coll.db->insert_face(image_id, face);
    ASSERT_GT(face_id, 0);
    int64_t person_id = coll.db->get_or_create_person("Ada");
    ASSERT_GT(person_id, 0);
    ASSERT_TRUE(coll.db->set_face_person(face_id, person_id));

    PhigRun run = run_phig(phig_bin, coll.dir,
                           "face rebuild --mtach '" + (coll.dir / "f0.jpg").string() +
                               "' --catalog '" + coll.dir.string() + "'");
    int exit_code = run.exit_code;
    const std::string& output = run.output;

    EXPECT_EQ(exit_code, 1) << "output was:\n" << output;
    EXPECT_NE(output.find("Unknown option: --mtach"), std::string::npos) << "output was:\n"
                                                                         << output;

    // The assertions that matter: the exit code alone would still pass if the
    // command destroyed the data on its way out. Nothing was re-detected, and
    // the labeled row is the same row -- faces.id is AUTOINCREMENT, so a
    // deleted id is never reissued and this cannot match a replacement.
    EXPECT_EQ(count_face_rows(coll), 1u) << "output was:\n" << output;
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(coll.db->db_ptr(), "SELECT person_id FROM faces WHERE id = ?", -1,
                                 &s, nullptr),
              SQLITE_OK);
    sqlite3_bind_int64(s, 1, face_id);
    int step = sqlite3_step(s);
    int64_t labeled_as = step == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    EXPECT_EQ(step, SQLITE_ROW) << "the named face was deleted; output was:\n" << output;
    EXPECT_EQ(labeled_as, person_id);
}

TEST(FacesEngineRun, CliRebuildWarnsWhenItDropsALabel) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    ScratchFaces coll(1);
    int64_t image_id = coll.db->get_image_id((coll.dir / "f0.jpg").string());
    ASSERT_GT(image_id, 0);
    FaceInfo phantom{};
    phantom.embedding.fill(0.5f);
    phantom.width = 4;
    phantom.height = 4;
    int64_t face_id = coll.db->insert_face(image_id, phantom);
    ASSERT_TRUE(coll.db->set_face_person(face_id, coll.db->get_or_create_person("Ada")));

    // The child's cache is redirected into coll.dir; give it the models this process found.
    fs::create_directories(coll.dir / "xdg" / "cache" / "phig");
    fs::create_directory_symlink(models_dir(), coll.dir / "xdg" / "cache" / "phig" / "models");
    PhigRun run = run_phig(phig_bin, coll.dir,
                           "face rebuild --match '" + (coll.dir / "f0.jpg").string() +
                               "' --catalog '" + coll.dir.string() + "'");
    int exit_code = run.exit_code;
    const std::string& output = run.output;

    EXPECT_EQ(exit_code, 0) << "output was:\n" << output;
    EXPECT_NE(output.find("1 person label(s) dropped"), std::string::npos) << "output was:\n"
                                                                           << output;
}

// ---- Selection (R74) ----

TEST(FacesEngineRun, AnExcludedImagesFacesAreLeftAlone) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(2);

    // A named face on the image the run is told to skip. A rebuild deletes an
    // image's faces before re-detecting them, so an --exclude that failed to
    // reach the engine takes this row and its label with it -- which is the
    // half of R74 that no test of passes_selection can show.
    int64_t excluded = coll.db->get_image_id((coll.dir / "f1.jpg").string());
    ASSERT_GT(excluded, 0);
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.width = 10;
    face.height = 10;
    int64_t face_id = coll.db->insert_face(excluded, face);
    ASSERT_GT(face_id, 0);
    int64_t person_id = coll.db->get_or_create_person("Ada");
    ASSERT_GT(person_id, 0);
    ASSERT_TRUE(coll.db->set_face_person(face_id, person_id));

    FacesRebuildOptions opts;
    opts.parallel = 2;
    Database::PageCriteria pc;
    pc.filter.excludes = {"*f1*"};
    pc.limit = -1;
    opts.ids = coll.db->ids_matching(pc);
    CancelToken tok;
    FacesResult r = FacesEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
    EXPECT_GT(r.faces_detected, 0u) << "the included image was not processed either";

    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(coll.db->db_ptr(), "SELECT person_id FROM faces WHERE id = ?", -1,
                                 &s, nullptr),
              SQLITE_OK);
    sqlite3_bind_int64(s, 1, face_id);
    int step = sqlite3_step(s);
    int64_t labeled_as = step == SQLITE_ROW ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    EXPECT_EQ(step, SQLITE_ROW) << "--exclude did not reach the delete: the face row is gone";
    EXPECT_EQ(labeled_as, person_id);
}

// ---- The other entry points that build a detector (R72) ----

namespace {
// Points models_dir() at an empty directory for the scope. models_dir() is
// cache_dir() + "/models" unless overridden, so this drives the missing-models
// refusal on a machine that has the models installed -- which is the only way
// these three cases run anywhere but a bare CI leg. The subprocess cases get
// the same condition from run_phig's redirected XDG_CACHE_HOME.
struct MissingModels {
    explicit MissingModels(const fs::path& empty_dir) {
        fs::create_directories(empty_dir);
        set_models_dir(empty_dir.string());
    }
    ~MissingModels() { set_models_dir(std::nullopt); }
};

// What a refusal has to name. Above the OpenCV floor the models are what is
// missing; below it the version check fires first and the models are beside the
// point, so the same test runs on both legs and asserts the reason that applies.
std::string expected_detector_refusal() {
    return PHIG_FACES_OPENCV_OK ? "models download" : PHIG_FACES_MIN_OPENCV;
}
}  // namespace

// Both of R72's existing witnesses are scan-path. FacesEngine::run builds its
// detectors after selection and before the first worker, on the same claim: the
// run establishes it can detect before it writes.
TEST(FacesEngineRun, RefusesTheRunWhenNoDetectorCanBeBuiltAndWritesNothing) {
    ScratchFaces coll(2);
    int64_t image_id = coll.db->get_image_id((coll.dir / "f0.jpg").string());
    ASSERT_GT(image_id, 0);
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.width = 10;
    face.height = 10;
    ASSERT_GT(coll.db->insert_face(image_id, face), 0);

    MissingModels missing(coll.dir / "empty-models");
    FacesRebuildOptions opts;
    opts.parallel = 2;
    CancelToken tok;
    opts.ids = coll.all_ids();
    std::string err = thrown_message([&] { FacesEngine(opts, *coll.db).run(tok, nullptr); });

    EXPECT_NE(err.find(expected_detector_refusal()), std::string::npos)
        << "the refusal must name what is missing: [" << err << "]";
    EXPECT_EQ(count_face_rows(coll), 1u)
        << "a rebuild deletes an image's faces first, so this run wrote before it checked";
}

TEST(FacesEngineRun, CliFaceNameRefusesWhenNoDetectorCanBeBuilt) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    ScratchFaces coll(1);
    PhigRun run = run_phig(phig_bin, coll.dir,
                           "face name '" + (coll.dir / "f0.jpg").string() + "' Ada --catalog '" +
                               coll.dir.string() + "'");

    EXPECT_EQ(run.exit_code, 1) << run.output;
    EXPECT_NE(run.output.find(expected_detector_refusal()), std::string::npos) << run.output;
    EXPECT_TRUE(coll.db->list_people().empty())
        << "the command named somebody before it found out it could not detect";
    EXPECT_EQ(count_face_rows(coll), 0u);
}

TEST(FacesEngineRun, CliSearchByFaceRefusesWhenNoDetectorCanBeBuilt) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    ScratchFaces coll(1);
    // A copy under a name no row carries, so a listed result is the only thing
    // that can put "f0.jpg" in the output.
    fs::path query = coll.dir / "query.jpg";
    fs::copy_file(coll.dir / "f0.jpg", query);

    PhigRun run =
        run_phig(phig_bin, coll.dir,
                 "search --face '" + query.string() + "' --catalog '" + coll.dir.string() + "'");

    EXPECT_EQ(run.exit_code, 1) << run.output;
    EXPECT_NE(run.output.find(expected_detector_refusal()), std::string::npos) << run.output;
    EXPECT_EQ(run.output.find("f0.jpg"), std::string::npos)
        << "the search ran anyway: " << run.output;
}

TEST(FacesEngineRun, AnEmptySelectionDetectsNothing) {
    // Matters more here than anywhere else in the codebase: a rebuild deletes
    // every selected image's face rows, including the person labels R60 records
    // as unrecoverable. An unset selection must not mean the whole collection.
    //
    // Needs no face models: an empty selection never reaches a detector, which
    // is why this one has no face_path_blocker skip.
    ScratchFaces coll(3);
    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.ids = {};
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 0u);
    EXPECT_EQ(r.faces_detected, 0u);
}
