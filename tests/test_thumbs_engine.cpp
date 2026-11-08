#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"
#include "temp_dir.h"
#include "thumbs.h"
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

        temp_dir = make_temp_dir("thumbs-engine");
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
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), false, PHIG_DEFAULT_THUMB_SIZE));
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), true, PHIG_DEFAULT_THUMB_SIZE));
}

TEST_F(ThumbsEngineTest, ForceRegeneratesEvenWhenTheThumbExists) {
    // A row with a cached thumb should still be regenerated under --force.
    // This test distinguishes `force || !exists` from `!exists`: if the
    // implementation checks the cache even when force=true, the test would fail.
    std::string sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    create_thumb_file(sha);
    ImageInfo hashed = with_sha(sha);
    EXPECT_TRUE(thumb_needs_generating(hashed, true, PHIG_DEFAULT_THUMB_SIZE));
}

TEST_F(ThumbsEngineTest, UnhashedRowIsSkippedBeforeTheCacheIsConsulted) {
    // Guards ordering: if the implementation checked thumb_exists("") first it
    // would stat a path built from an empty key. The empty-sha branch must win.
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), false, PHIG_DEFAULT_THUMB_SIZE));
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
    // Every row. These tests predate id selection and meant "the whole
    // collection" by setting no filter; an empty selection now means nothing,
    // so what they meant has to be said.
    std::vector<int64_t> all_ids() {
        Database::PageCriteria pc;
        pc.limit = -1;
        return db->ids_matching(pc);
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
    opts.ids = coll.all_ids();
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
    opts.ids = coll.all_ids();
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
    // Before the first run, not between the two: the first run is what puts the
    // thumbnails in the cache, and without a selection it would generate
    // nothing, leaving the forced run with nothing to regenerate and this test
    // asserting force works when it had never been exercised.
    opts.ids = coll.all_ids();
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
    opts.ids = coll.all_ids();
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

TEST(ThumbsEngineRun, UndecodableFilePublishesTheDecodeErrorOnProgress) {
    // The count already exists; the reason does not. generate_thumb collapses
    // "could not decode", "empty sha", "bad size", and "imencode failed" into a
    // single false, and the worker's catch(...) drops any exception message —
    // so 40 failed thumbnails give a user no way to tell one corrupt file from
    // a full disk. decode_image_with_warning already captures OpenCV's own text
    // per decode; route it instead of discarding it.
    ScratchThumbs coll(1);
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
    info.sha256 = std::string(63, '1') + "1";
    info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z";
    coll.db->insert(info);

    ThumbsOptions opts;
    opts.parallel = 1;
    opts.ids = coll.all_ids();
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<ThumbsProgress> q(64);

    ThumbsResult r = engine.run(tok, &q);
    q.close();

    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(r.generated, 1u);

    bool saw_error = false;
    ThumbsProgress p;
    while (q.pop(p)) {
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

TEST(ThumbsEngineRun, SelectionFiltersRestrictWhatIsProcessed) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    // The engine takes ids; the glob is resolved by whoever calls it, which for
    // the CLI is Database::ids_matching. Resolving here keeps this test's
    // subject — that a restricted selection restricts the run — while exercising
    // the surface the engine actually has.
    Database::PageCriteria pc;
    pc.filter.matches = {(coll.dir / "img0.jpg").string()};
    pc.limit = -1;
    opts.ids = coll.db->ids_matching(pc);
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

TEST(ThumbsEngineRun, AnExcludedRowGetsNoThumbnail) {
    // R74's predicate is pinned by FilterTest against passes_selection itself,
    // and DatabaseTest.ExcludeBeatsMatchJustLikePassesSelection pins that
    // ids_matching applies it in SQL. What neither can show is that a row left
    // out of the selection gets no thumbnail written for it — the engine now
    // acts on ids alone, so this is where "excluded" has to mean "no cache
    // entry" rather than merely "not counted".
    ScratchThumbs coll(2);
    ThumbsOptions opts;
    opts.parallel = 2;
    Database::PageCriteria pc;
    pc.filter.excludes = {"*img1*"};
    pc.limit = -1;
    opts.ids = coll.db->ids_matching(pc);
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
    EXPECT_EQ(r.generated, 1u);
    const std::string included = std::string(63, '0') + "0";
    const std::string excluded = std::string(63, '0') + "1";
    EXPECT_TRUE(thumb_exists(included, PHIG_DEFAULT_THUMB_SIZE));
    EXPECT_FALSE(thumb_exists(excluded, PHIG_DEFAULT_THUMB_SIZE))
        << "--exclude did not reach the cache write";
}

TEST(ThumbsEngineRun, AnEmptySelectionGeneratesNothing) {
    // Empty means nothing, not everything. Before ids, an empty match list
    // selected the whole collection, so a caller that forgot to set a selection
    // got a collection-wide run; now it gets a no-op.
    ScratchThumbs coll(3);
    ThumbsOptions opts;
    opts.ids = {};
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 0u);
    EXPECT_EQ(r.generated, 0u);
}

// ---- ensure_thumb: one row's thumbnail, generated on demand ----

class EnsureThumbTest : public ::testing::Test {
   protected:
    std::string orig_cache;
    fs::path temp_dir;
    std::unique_ptr<Database> db;

    void SetUp() override {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg) orig_cache = xdg;
        temp_dir = make_temp_dir("ensure-thumb");
        setenv("XDG_CACHE_HOME", temp_dir.c_str(), 1);
        db = std::make_unique<Database>((temp_dir / "phig.db").string());
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        if (orig_cache.empty()) {
            unsetenv("XDG_CACHE_HOME");
        } else {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        }
        fs::remove_all(temp_dir);
    }

    int64_t add_row(const std::string& path, const std::string& sha,
                    const std::string& media_type = "image") {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = fs::path(path).extension().string().substr(1);
        info.modified_at = "2021-01-01T00:00:00Z";
        info.sha256 = sha;
        info.media_type = media_type;
        db->insert(info);
        return db->get_image_id(path);
    }
};

TEST_F(EnsureThumbTest, GeneratesAMissingThumbnail) {
    std::string sha(64, 'a');
    int64_t id = add_row("tests/data/faces_small.jpg", sha);
    ASSERT_FALSE(thumb_exists(sha, 256));

    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Ok);
    EXPECT_TRUE(thumb_exists(sha, 256));
    EXPECT_EQ(get_thumb_path(sha, 256).rfind(temp_dir.string(), 0), 0u);
}

TEST_F(EnsureThumbTest, ACachedThumbnailNeedsNoSourceFile) {
    // The cached check comes before the source check, so a row whose file has
    // gone still reports its cached thumbnail.
    std::string sha(64, 'b');
    int64_t id = add_row("/nonexistent/b.jpg", sha);
    fs::path thumb = get_thumb_path(sha, 256);
    fs::create_directories(thumb.parent_path());
    std::ofstream(thumb) << "cached";

    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Ok);
}

TEST_F(EnsureThumbTest, AnUnknownIdOrAnUnhashedRowIsNoSuchRow) {
    int64_t unhashed = add_row("tests/data/faces_small.jpg", "");
    EXPECT_EQ(ensure_thumb(*db, 999999, 256), EnsureThumb::NoSuchRow);
    EXPECT_EQ(ensure_thumb(*db, unhashed, 256), EnsureThumb::NoSuchRow);
}

TEST_F(EnsureThumbTest, AMissingSourceIsSourceMissing) {
    int64_t id = add_row("/nonexistent/c.jpg", std::string(64, 'c'));
    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::SourceMissing);
}

TEST_F(EnsureThumbTest, AnUndecodableSourceIsFailed) {
    fs::path junk = temp_dir / "junk.jpg";
    std::ofstream(junk) << "not a jpeg";
    int64_t id = add_row(junk.string(), std::string(64, 'd'));
    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Failed);
    EXPECT_FALSE(thumb_exists(std::string(64, 'd'), 256));
}

TEST_F(EnsureThumbTest, AVideoRowIsGeneratedFromItsKeyframe) {
    std::string sha(64, 'e');
    int64_t id = add_row("tests/data/test_red.mov", sha, "video");
    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Ok);
    EXPECT_TRUE(thumb_exists(sha, 256));
}
