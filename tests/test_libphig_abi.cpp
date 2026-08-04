#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "paths.h"
#include "phig/phig.h"

namespace {
struct TempDbPath {
    std::string path;
    TempDbPath() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_abi_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                   .string();
    }
    ~TempDbPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
};
}  // namespace

// Creates a real phig database at `path` via the C++ API.
// Defined in test_libphig_query.cpp.
void make_db(const std::string& path);

namespace {
// Reads back PRAGMA journal_mode via a throwaway raw connection, independent
// of libphig, to confirm a read-only phig_init did not change it.
std::string journal_mode_of(const std::string& path) {
    sqlite3* raw = nullptr;
    sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(raw, "PRAGMA journal_mode", -1, &stmt, nullptr);
    std::string mode;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    return mode;
}
}  // namespace

namespace {
// Restores phig's process-wide path overrides when the test scope exits,
// including on an ASSERT_* early return.
struct PathOverrideGuard {
    ~PathOverrideGuard() { set_path_overrides(std::nullopt, std::nullopt, std::nullopt); }
};
}  // namespace

TEST(Abi, VersionMajorMatchesHeader) {
    EXPECT_EQ(phig_abi_version() >> 16, PHIG_ABI_VERSION_MAJOR);
}

TEST(Abi, InitRejectsNullConfig) {
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(nullptr, &ctx), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitRejectsNullOut) {
    phig_config cfg{};
    cfg.db_path = "/tmp/whatever.db";
    EXPECT_EQ(phig_init(&cfg, nullptr), PHIG_ERR_INVALID_ARG);
}

TEST(Abi, InitRejectsNullDbPath) {
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.db_path = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitFailsOnUnopenableDatabase) {
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.db_path = "/nonexistent-dir-xyz/phig.db";
    cfg.mode = PHIG_READ_ONLY;
    EXPECT_NE(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitSucceedsAndShutdownIsClean) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    ASSERT_NE(ctx, nullptr);
    EXPECT_STREQ(phig_last_error(ctx), "");
    phig_shutdown(ctx);
}

TEST(Abi, ShutdownToleratesNull) {
    phig_shutdown(nullptr);  // must not crash
}

TEST(Abi, LastErrorOnNullCtxReturnsEmptyString) {
    const char* msg = phig_last_error(nullptr);
    ASSERT_NE(msg, nullptr);
    EXPECT_STREQ(msg, "");
}

TEST(Abi, ReadOnlyInitDoesNotCreateDatabase) {
    TempDbPath tmp;  // path does not exist yet
    ASSERT_FALSE(std::filesystem::exists(tmp.path));

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    EXPECT_NE(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_EQ(ctx, nullptr);
    // The whole point: a read-only open must not create the file.
    EXPECT_FALSE(std::filesystem::exists(tmp.path));
}

// The query ABI isn't exposed yet, so this can't prove a read-only context
// can read application data through libphig itself. Instead it pins the
// observable facts that matter for this task: init on an existing migrated
// database succeeds, reports no error, and leaves the database's journal
// mode (and therefore the file) unmutated by the read-only open.
TEST(Abi, ReadOnlyInitSucceedsOnExistingDatabase) {
    TempDbPath tmp;
    make_db(tmp.path);  // exists, migrated, WAL mode
    ASSERT_EQ(journal_mode_of(tmp.path), "wal");

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    ASSERT_NE(ctx, nullptr);
    EXPECT_STREQ(phig_last_error(ctx), "");
    phig_shutdown(ctx);

    EXPECT_EQ(journal_mode_of(tmp.path), "wal");
}

TEST(Abi, ReadWriteInitStillCreatesDatabase) {
    TempDbPath tmp;
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_WRITE;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_TRUE(std::filesystem::exists(tmp.path));
    phig_shutdown(ctx);
}

TEST(AbiThumb, ReturnsPathAndReportsMissing) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'a');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_NE(buf[0], '\0');
    EXPECT_FALSE(exists);  // nothing generated for this digest
    // Path must contain the digest so it is stable and cache-keyed by content.
    EXPECT_NE(std::string(buf).find(sha), std::string::npos);
    phig_shutdown(ctx);
}

TEST(AbiThumb, RejectsBadDigestAndSmallBuffer) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    char buf[4096];
    bool exists = false;
    EXPECT_EQ(phig_thumb_lookup(ctx, "nothex", buf, sizeof(buf), &exists), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_thumb_lookup(ctx, nullptr, buf, sizeof(buf), &exists), PHIG_ERR_INVALID_ARG);

    std::string sha(64, 'a');
    char tiny[8];
    EXPECT_EQ(phig_thumb_lookup(ctx, sha.c_str(), tiny, sizeof(tiny), &exists),
              PHIG_ERR_INVALID_ARG);
    phig_shutdown(ctx);
}

TEST(AbiThumb, HonorsConfiguredThumbsDir) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    make_db(tmp.path);
    auto custom =
        (std::filesystem::temp_directory_path() / ("phig_thumbs_" + std::to_string(::getpid())))
            .string();

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = custom.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'a');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_EQ(std::string(buf).rfind(custom, 0), 0u);  // path starts with custom dir
    EXPECT_FALSE(exists);

    phig_shutdown(ctx);
}

TEST(AbiThumb, NullThumbsDirLeavesDefaultInPlace) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    make_db(tmp.path);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = nullptr;  // must not clobber an existing override
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'b');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_NE(std::string(buf).find("thumbs"), std::string::npos);

    phig_shutdown(ctx);
}

namespace {
// A scratch thumbs_dir, removed on scope exit, so generation tests never
// write into a real ~/.cache/phig/thumbs.
struct ScratchThumbsDir {
    std::string path;
    ScratchThumbsDir() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_thumb_gen_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this))))
                   .string();
    }
    ~ScratchThumbsDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// Inserts one row with a real fixture file behind it (so generation has real
// pixels to decode) and returns its id.
int64_t seed_thumb_row(const std::string& db_path, const std::string& src_fixture,
                       const std::string& media_type, const std::string& sha) {
    Database db(db_path);
    db.init_schema();
    ImageInfo info;
    info.path = src_fixture;
    info.filename = src_fixture.substr(src_fixture.rfind('/') + 1);
    info.extension = src_fixture.substr(src_fixture.rfind('.') + 1);
    info.modified_at = "2021-01-01T00:00:00Z";
    info.sha256 = sha;
    info.media_type = media_type;
    db.insert(info);
    return db.get_image_id(src_fixture);
}
}  // namespace

TEST(AbiThumbGenerate, GeneratesFromMissThenLookupReportsExists) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    ScratchThumbsDir thumbs;
    std::string sha(64, 'c');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/faces_small.jpg", "image", sha);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = thumbs.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_FALSE(exists);

    ASSERT_EQ(phig_thumb_generate(ctx, id), PHIG_OK);

    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_TRUE(exists);

    phig_shutdown(ctx);
}

TEST(AbiThumbGenerate, GeneratingTwiceStaysOkAndLeavesOneValidFile) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    ScratchThumbsDir thumbs;
    std::string sha(64, 'd');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/faces_small.jpg", "image", sha);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = thumbs.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    ASSERT_EQ(phig_thumb_generate(ctx, id), PHIG_OK);

    char buf[4096];
    bool exists = false;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_TRUE(exists);
    auto size_after_first = std::filesystem::file_size(buf);

    ASSERT_EQ(phig_thumb_generate(ctx, id), PHIG_OK);
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_TRUE(exists);
    EXPECT_GT(std::filesystem::file_size(buf), 0u);
    EXPECT_EQ(std::filesystem::file_size(buf), size_after_first);

    phig_shutdown(ctx);
}

TEST(AbiThumbGenerate, NonexistentImageIdReturnsErrDb) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    ScratchThumbsDir thumbs;
    make_db(tmp.path);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = thumbs.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    EXPECT_EQ(phig_thumb_generate(ctx, 999999), PHIG_ERR_DB);

    phig_shutdown(ctx);
}

// The test that distinguishes a correct wrap of ThumbsEngine's worker from a
// naive generate_thumb(path, sha): a video row has no decodable pixels at
// `path` itself, only through extract_video_keyframe first.
TEST(AbiThumbGenerate, VideoRowProducesThumbnailViaKeyframe) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    ScratchThumbsDir thumbs;
    std::string sha(64, 'e');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/test_red.mov", "video", sha);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = thumbs.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    ASSERT_EQ(phig_thumb_generate(ctx, id), PHIG_OK);

    char buf[4096];
    bool exists = false;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_TRUE(exists);
    EXPECT_GT(std::filesystem::file_size(buf), 0u);

    phig_shutdown(ctx);
}

TEST(AbiThumbGenerate, ConcurrentGenerationOfSameRowAllSucceed) {
    PathOverrideGuard guard;
    TempDbPath tmp;
    ScratchThumbsDir thumbs;
    std::string sha(64, 'f');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/faces_small.jpg", "image", sha);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = thumbs.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::vector<phig_status> results(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] { results[t] = phig_thumb_generate(ctx, id); });
    }
    for (auto& th : threads) th.join();

    for (auto st : results) EXPECT_EQ(st, PHIG_OK);

    char buf[4096];
    bool exists = false;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_TRUE(exists);
    EXPECT_GT(std::filesystem::file_size(buf), 0u);

    phig_shutdown(ctx);
}
