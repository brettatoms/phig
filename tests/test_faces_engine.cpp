#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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
#include "models.h"
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
}  // namespace

TEST(FacesEngineRun, DetectsFacesInEveryImageAcrossWorkers) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
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
    CancelToken tok;

    FacesResult first = FacesEngine(opts, *coll.db).run(tok, nullptr);
    FacesResult second = FacesEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(second.faces_detected, first.faces_detected);

    // And the stored rows match the reported count, not twice it. Database has
    // no per-image face getter, so count directly. The db handle must be closed
    // first — this opens a second connection to the same file.
    size_t stored = count_face_rows(coll);
    EXPECT_EQ(stored, second.faces_detected);
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
    opts.matches.push_back((coll.dir / "f0.jpg").string());
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

    std::string threw;
    try {
        engine.run(tok, nullptr);
    } catch (const std::exception& e) {
        threw = e.what();
    }
    running.store(false);
    deleter.join();

    EXPECT_TRUE(threw.empty()) << "the rebuild aborted instead of skipping: " << threw;
}
