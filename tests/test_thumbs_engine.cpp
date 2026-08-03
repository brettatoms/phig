#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"
#include "thumbs_engine.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {
ImageInfo with_sha(const std::string& sha) {
    ImageInfo info;
    info.path = "/tmp/nonexistent/x.jpg";
    info.sha256 = sha;
    return info;
}
}  // namespace

class ThumbsEngineTest : public ::testing::Test {
   protected:
    std::string orig_cache;
    std::string temp_dir;

    void SetUp() override {
        // Save original XDG_CACHE_HOME and redirect to temp
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg) orig_cache = xdg;

        temp_dir = fs::temp_directory_path() / "phig-test-thumbs-engine";
        fs::create_directories(temp_dir);
        setenv("XDG_CACHE_HOME", temp_dir.c_str(), 1);
    }

    void TearDown() override {
        // Restore
        if (orig_cache.empty()) {
            unsetenv("XDG_CACHE_HOME");
        } else {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        }
        fs::remove_all(temp_dir);
    }

    // Create a cached thumbnail file for the given SHA
    void create_thumb_file(const std::string& sha) {
        std::string thumb_path = temp_dir + "/phig/thumbs/" + sha.substr(0, 2);
        fs::create_directories(thumb_path);
        std::ofstream f(thumb_path + "/" + sha + ".jpg");
        f << "fake jpeg data";
    }
};

TEST_F(ThumbsEngineTest, EmptyShaIsSkippedEvenUnderForce) {
    // A row with no hash has no cache key, so there is nowhere to write a
    // thumbnail. --force must not override this: it is not a staleness
    // question, it is a missing-identifier question.
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), false));
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), true));
}

TEST_F(ThumbsEngineTest, ForceRegeneratesEvenWhenTheThumbExists) {
    // A row with a cached thumb should still be regenerated under --force.
    // This test distinguishes `force || !exists` from `!exists`: if the
    // implementation checks the cache even when force=true, the test would fail.
    std::string sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    create_thumb_file(sha);
    ImageInfo hashed = with_sha(sha);
    EXPECT_TRUE(thumb_needs_generating(hashed, true));
}

TEST_F(ThumbsEngineTest, UnhashedRowIsSkippedBeforeTheCacheIsConsulted) {
    // Guards ordering: if the implementation checked thumb_exists("") first it
    // would stat a path built from an empty key. The empty-sha branch must win.
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), false));
}

namespace {
// A scratch collection with N copies of a real fixture, scanned into a database.
//
// Redirects XDG_CACHE_HOME to a scratch directory for its lifetime: the fixed,
// low-entropy shas below (all-zeros plus an index) would otherwise collide
// with the real thumbnail cache and with each other across test instances.
struct ScratchThumbs {
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;
    std::string orig_cache;
    bool had_orig_cache;

    explicit ScratchThumbs(int n) {
        dir = fs::temp_directory_path() / ("phig_thumbs_engine_" + std::to_string(::getpid()) +
                                           "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);

        const char* xdg = std::getenv("XDG_CACHE_HOME");
        had_orig_cache = xdg != nullptr;
        if (xdg) orig_cache = xdg;
        setenv("XDG_CACHE_HOME", dir.c_str(), 1);

        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
        for (int i = 0; i < n; ++i) {
            fs::path p = dir / ("img" + std::to_string(i) + ".jpg");
            fs::copy_file("tests/data/faces_small.jpg", p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            // Distinct shas so each row gets its own cache entry.
            info.sha256 = std::string(63, '0') + std::to_string(i);
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db->insert(info);
        }
    }
    ~ScratchThumbs() {
        db.reset();
        if (had_orig_cache) {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        } else {
            unsetenv("XDG_CACHE_HOME");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
}  // namespace

TEST(ThumbsEngineRun, GeneratesOneThumbnailPerSelectedImage) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.generated, 4u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_EQ(r.total_selected, 4u);
    EXPECT_TRUE(r.completed);
}

TEST(ThumbsEngineRun, SecondRunSkipsEverythingItAlreadyGenerated) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    CancelToken tok;

    ThumbsEngine(opts, *coll.db).run(tok, nullptr);
    ThumbsResult second = ThumbsEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(second.skipped, 4u);
    EXPECT_EQ(second.generated, 0u);
}

TEST(ThumbsEngineRun, ForceRegeneratesWhatTheSecondRunWouldSkip) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    CancelToken tok;

    ThumbsEngine(opts, *coll.db).run(tok, nullptr);
    opts.force = true;
    ThumbsResult forced = ThumbsEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(forced.generated, 4u);
    EXPECT_EQ(forced.skipped, 0u);
}

TEST(ThumbsEngineRun, AlreadyCancelledGeneratesNothingAndReportsIncomplete) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;
    tok.cancel();

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.generated, 0u);
}

TEST(ThumbsEngineRun, AlreadyCancelledOverAnEmptySelectionReportsIncomplete) {
    // parallel_for_each short-circuits to true on an empty range, so an
    // already-cancelled run with nothing selected must not fall through to
    // that default and report completed.
    ScratchThumbs coll(0);
    ThumbsOptions opts;
    opts.parallel = 2;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;
    tok.cancel();

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 0u);
    EXPECT_FALSE(r.completed);
}

TEST(ThumbsEngineRun, PublishesSelectedCountBeforeAnyFileCompletes) {
    // The Selected update must arrive first, so a caller can size a progress
    // bar before the first file lands. Asserts order, not just presence.
    ScratchThumbs coll(3);
    ThumbsOptions opts;
    opts.parallel = 1;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<ThumbsProgress> q(64);

    engine.run(tok, &q);
    q.close();

    ThumbsProgress first;
    ASSERT_TRUE(q.pop(first));
    EXPECT_EQ(first.stage, ThumbsStage::Selected);
    EXPECT_EQ(first.total, 3u);

    size_t file_done = 0;
    ThumbsProgress p;
    while (q.pop(p)) {
        if (p.stage == ThumbsStage::FileDone) ++file_done;
    }
    EXPECT_EQ(file_done, 3u);
}

TEST(ThumbsEngineRun, SelectionFiltersRestrictWhatIsProcessed) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    opts.matches.push_back((coll.dir / "img0.jpg").string());
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
    EXPECT_EQ(r.generated, 1u);
}

TEST(ThumbsEngineRun, PublishesNothingWhenGivenANullQueue) {
    // The null-queue path is the one libphig's synchronous callers use; it must
    // not be a crash.
    ScratchThumbs coll(2);
    ThumbsOptions opts;
    opts.parallel = 1;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    EXPECT_NO_THROW(engine.run(tok, nullptr));
}
