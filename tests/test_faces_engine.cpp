#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "cancel.h"
#include "database.h"
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

bool models_or_skip() { return face_models_exist(); }

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
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
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
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
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
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
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

TEST(FacesEngineRun, PublishesModelLoadThenSelectedThenFiles) {
    // Ordering matters for rendering: the CLI prints "Loading face recognition
    // models (N instances)..." before the per-file lines, and model loading is
    // the slowest part of a small rebuild.
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
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
    EXPECT_EQ(p.stage, FacesStage::LoadingModels);
    EXPECT_EQ(p.total, 1u);  // one detector instance for parallel=1

    size_t files = 0;
    while (q.pop(p)) {
        if (p.stage == FacesStage::FileDone) ++files;
    }
    EXPECT_EQ(files, 2u);
}

TEST(FacesEngineRun, UndecodableFileIsSkippedWithoutFailingTheRun) {
    // A file that cannot be decoded must not abort the rebuild or write rows.
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
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

TEST(FacesEngineRun, SelectionFiltersRestrictWhatIsProcessed) {
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.matches.push_back((coll.dir / "f0.jpg").string());
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
}
