#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "paths.h"
#include "phig/phig.h"

namespace {
// A scratch catalog directory; `path` is its database.
struct TempDbPath {
    std::string dir;
    std::string path;
    TempDbPath() {
        dir = (std::filesystem::temp_directory_path() /
               ("phig_abi_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this))))
                  .string();
        std::filesystem::create_directories(dir);
        path = dir + "/phig.db";
    }
    ~TempDbPath() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
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
// Clears phig's process-wide catalog when the test scope exits, including on
// an ASSERT_* early return.
struct CatalogGuard {
    ~CatalogGuard() { clear_catalog(); }
};
}  // namespace

TEST(Abi, VersionMajorMatchesHeader) {
    EXPECT_EQ(phig_abi_version() >> 16, PHIG_ABI_VERSION_MAJOR);
}

namespace {
// How many entry points the header exported when a given ABI version was
// current. Appended to, never edited: a row is a statement about a version that
// has already been published.
struct AbiSymbolCount {
    unsigned major;
    unsigned minor;
    size_t symbols;
};
constexpr AbiSymbolCount kAbiSymbolCounts[] = {
    {1, 3, 69},
    // 1.4 adds phig_query_add_match, phig_query_add_exclude, phig_query_ids
    // and phig_id_list_free — glob predicates on a query and resolving one to
    // ids (plan 053 decision 2). Additive, so the soname is unchanged.
    {1, 4, 73},
};

// Every exported entry point declared in the public header.
//
// An exported declaration is a line beginning `PHIG_API`; the name is the
// phig_-prefixed identifier on it that is followed by `(`. Deliberately not the
// looser "any phig_foo( anywhere in the file" that scripts/rules.py's coverage
// check uses, which also matches the prose in the doc comments.
std::set<std::string> exported_symbols(const std::string& header_text) {
    std::set<std::string> names;
    std::istringstream lines(header_text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("PHIG_API", 0) != 0) continue;
        for (size_t i = 0; i + 5 < line.size(); ++i) {
            if (line.compare(i, 5, "phig_") != 0) continue;
            if (i > 0 &&
                (std::isalnum(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == '_')) {
                continue;
            }
            size_t end = i;
            while (end < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) {
                ++end;
            }
            if (end < line.size() && line[end] == '(') names.insert(line.substr(i, end - i));
        }
    }
    return names;
}
}  // namespace

// Adding a symbol has to bump PHIG_ABI_VERSION_MINOR and the CMake VERSION, and
// nothing in the build makes that happen. The export lists match on the phig_
// prefix, so a new entry point ships exported with both numbers untouched, and
// scripts/rules.py's version check only compares the header against
// libphig/CMakeLists.txt — three numbers that still agree with each other.
//
// This is the missing trigger: the header's own exported declarations, counted
// against what was recorded for the minor it claims to be. Add a symbol and
// this fails until either the version moves and kAbiSymbolCounts gains a row,
// or somebody deliberately falsifies the row for the current one.
TEST(Abi, ExportedSymbolCountMatchesTheRecordedAbiVersion) {
    const std::filesystem::path header = "libphig/include/phig/phig.h";
    ASSERT_TRUE(std::filesystem::exists(header))
        << "run the suite from the source root (ctest does); cwd is "
        << std::filesystem::current_path();
    std::ifstream in(header);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(text.empty());

    const AbiSymbolCount* row = nullptr;
    for (const auto& candidate : kAbiSymbolCounts) {
        if (candidate.major == PHIG_ABI_VERSION_MAJOR &&
            candidate.minor == PHIG_ABI_VERSION_MINOR) {
            row = &candidate;
        }
    }
    ASSERT_NE(row, nullptr) << "ABI " << PHIG_ABI_VERSION_MAJOR << "." << PHIG_ABI_VERSION_MINOR
                            << " has no row in kAbiSymbolCounts — add one saying how many "
                               "symbols it exports";

    std::set<std::string> names = exported_symbols(text);
    EXPECT_EQ(names.size(), row->symbols)
        << "the header declares " << names.size() << " exported symbols and ABI "
        << PHIG_ABI_VERSION_MAJOR << "." << PHIG_ABI_VERSION_MINOR << " was recorded with "
        << row->symbols << ". Adding one bumps PHIG_ABI_VERSION_MINOR and "
        << "libphig/CMakeLists.txt's VERSION — never SOVERSION — and then gets a row here.";
}

// phig_op_progress is caller-allocated and phig_op_poll fills it, with no size
// and no version member, so a field added to it makes a newer libphig write
// past the end of an older client's buffer. tests/test_libphig_c_compile.c
// carries the same layout pin as negative-width bitfields, which fails the
// build rather than a run; this is the half the record can name and check 2 can
// see pass, and it is C++'s view of the same struct the C translation unit
// pins.
TEST(Abi, OpProgressGainsNoField) {
    EXPECT_EQ(offsetof(phig_op_progress, stage), 0u);
    EXPECT_EQ(offsetof(phig_op_progress, total),
              offsetof(phig_op_progress, done) + sizeof(uint64_t));
    EXPECT_EQ(offsetof(phig_op_progress, processed),
              offsetof(phig_op_progress, total) + sizeof(uint64_t));
    EXPECT_EQ(offsetof(phig_op_progress, skipped),
              offsetof(phig_op_progress, processed) + sizeof(uint64_t));
    EXPECT_EQ(offsetof(phig_op_progress, errors),
              offsetof(phig_op_progress, skipped) + sizeof(uint64_t));
    EXPECT_EQ(offsetof(phig_op_progress, path),
              offsetof(phig_op_progress, errors) + sizeof(uint64_t));
    EXPECT_EQ(offsetof(phig_op_progress, error),
              offsetof(phig_op_progress, path) + sizeof(const char*));
    EXPECT_EQ(sizeof(phig_op_progress), offsetof(phig_op_progress, error) + sizeof(const char*))
        << "phig_op_progress grew: a new counter is a new struct and a new poll symbol";
    // `done` is the only field whose offset no predecessor fixes: it sits at
    // the enum's size rounded up to alignof(uint64_t).
    EXPECT_EQ(
        offsetof(phig_op_progress, done),
        ((sizeof(phig_op_stage) + alignof(uint64_t) - 1) / alignof(uint64_t)) * alignof(uint64_t));
}

TEST(Abi, InitRejectsNullConfig) {
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(nullptr, &ctx), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitRejectsNullOut) {
    phig_config cfg{};
    cfg.catalog_dir = "/tmp/whatever";
    EXPECT_EQ(phig_init(&cfg, nullptr), PHIG_ERR_INVALID_ARG);
}

TEST(Abi, InitRejectsNullDbPath) {
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.catalog_dir = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitFailsOnUnopenableDatabase) {
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.catalog_dir = "/nonexistent-dir-xyz";
    cfg.mode = PHIG_READ_ONLY;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_DB);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, ReadWriteInitCreatesAMissingCatalog) {
    CatalogGuard guard;
    TempDbPath tmp;
    const std::string dir = tmp.dir + "/not-yet";
    phig_config cfg{};
    cfg.catalog_dir = dir.c_str();
    cfg.mode = PHIG_READ_WRITE;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_TRUE(std::filesystem::exists(dir + "/phig.db"));
    phig_shutdown(ctx);
}

// The installed catalog outlives the call, so a relative catalog_dir is
// resolved against the working directory at phig_init, not at each later use.
// Observed through phig_thumb_lookup: libphig keeps its own installed catalog.
TEST(Abi, InitInstallsARelativeCatalogAsAnAbsolutePath) {
    namespace fs = std::filesystem;
    CatalogGuard guard;
    TempDbPath tmp;
    make_db(tmp.path);
    const fs::path name = fs::path(tmp.dir).filename();
    const std::string relative = (fs::relative(tmp.dir, fs::current_path()) / ".." / name).string();
    ASSERT_FALSE(fs::path(relative).is_absolute());
    phig_config cfg{};
    cfg.catalog_dir = relative.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, std::string(64, 'a').c_str(), 512, buf, sizeof(buf), &exists),
              PHIG_OK);
    // fs::relative resolves symlinks (macOS's /var is /private/var), so the
    // expected prefix must be resolved the same way.
    const std::string thumbs = (fs::weakly_canonical(tmp.dir) / "thumbs/").string();
    EXPECT_EQ(std::string(buf).rfind(thumbs, 0), 0u) << buf;
    phig_shutdown(ctx);
}

TEST(Abi, InitSucceedsAndShutdownIsClean) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.catalog_dir = tmp.dir.c_str();
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
    cfg.catalog_dir = tmp.dir.c_str();
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
    cfg.catalog_dir = tmp.dir.c_str();
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
    cfg.catalog_dir = tmp.dir.c_str();
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
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'a');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
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
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    char buf[4096];
    bool exists = false;
    EXPECT_EQ(phig_thumb_lookup(ctx, "nothex", 512, buf, sizeof(buf), &exists),
              PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_thumb_lookup(ctx, nullptr, 512, buf, sizeof(buf), &exists),
              PHIG_ERR_INVALID_ARG);

    std::string sha(64, 'a');
    char tiny[8];
    EXPECT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, tiny, sizeof(tiny), &exists),
              PHIG_ERR_INVALID_ARG);
    phig_shutdown(ctx);
}

TEST(AbiThumb, ThumbnailsLiveInTheCatalog) {
    CatalogGuard guard;
    TempDbPath tmp;
    make_db(tmp.path);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'a');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_EQ(std::string(buf).rfind(tmp.dir + "/thumbs/", 0), 0u) << buf;
    EXPECT_FALSE(exists);

    phig_shutdown(ctx);
}

namespace {
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
    CatalogGuard guard;
    TempDbPath tmp;
    std::string sha(64, 'c');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/faces_small.jpg", "image", sha);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_FALSE(exists);

    ASSERT_EQ(phig_thumb_generate(ctx, id, 512), PHIG_OK);

    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_TRUE(exists);

    phig_shutdown(ctx);
}

TEST(AbiThumbGenerate, GeneratingTwiceStaysOkAndLeavesOneValidFile) {
    CatalogGuard guard;
    TempDbPath tmp;
    std::string sha(64, 'd');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/faces_small.jpg", "image", sha);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    ASSERT_EQ(phig_thumb_generate(ctx, id, 512), PHIG_OK);

    char buf[4096];
    bool exists = false;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_TRUE(exists);
    auto size_after_first = std::filesystem::file_size(buf);

    ASSERT_EQ(phig_thumb_generate(ctx, id, 512), PHIG_OK);
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_TRUE(exists);
    EXPECT_GT(std::filesystem::file_size(buf), 0u);
    EXPECT_EQ(std::filesystem::file_size(buf), size_after_first);

    phig_shutdown(ctx);
}

TEST(AbiThumbGenerate, NonexistentImageIdReturnsErrDb) {
    CatalogGuard guard;
    TempDbPath tmp;
    make_db(tmp.path);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    EXPECT_EQ(phig_thumb_generate(ctx, 999999, 512), PHIG_ERR_DB);

    phig_shutdown(ctx);
}

// The test that distinguishes a correct wrap of ThumbsEngine's worker from a
// naive generate_thumb(path, sha): a video row has no decodable pixels at
// `path` itself, only through extract_video_keyframe first.
TEST(AbiThumbGenerate, VideoRowProducesThumbnailViaKeyframe) {
    CatalogGuard guard;
    TempDbPath tmp;
    std::string sha(64, 'e');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/test_red.mov", "video", sha);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    ASSERT_EQ(phig_thumb_generate(ctx, id, 512), PHIG_OK);

    char buf[4096];
    bool exists = false;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_TRUE(exists);
    EXPECT_GT(std::filesystem::file_size(buf), 0u);

    phig_shutdown(ctx);
}

TEST(AbiThumbGenerate, ConcurrentGenerationOfSameRowAllSucceed) {
    CatalogGuard guard;
    TempDbPath tmp;
    std::string sha(64, 'f');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/faces_small.jpg", "image", sha);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::vector<phig_status> results(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] { results[t] = phig_thumb_generate(ctx, id, 512); });
    }
    for (auto& th : threads) th.join();

    for (auto st : results) EXPECT_EQ(st, PHIG_OK);

    char buf[4096];
    bool exists = false;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
    ASSERT_TRUE(exists);
    EXPECT_GT(std::filesystem::file_size(buf), 0u);

    phig_shutdown(ctx);
}

// Sibling of ConcurrentGenerationOfSameRowAllSucceed above, but the row's
// source file does not exist on disk (a dangling row: the file was moved or
// deleted after being indexed, an ordinary state for this kind of
// collection). Runs the 8 concurrent calls in a forked child via EXPECT_EXIT:
// a decoder-triggered exit() deep inside one of them would otherwise take the
// whole test binary down with it rather than just failing this one check.
// The child exits with a marker code only if every call returned normally, so
// an unexpected exit (silent death, code 0) shows up as a failed exit-code
// match instead of aborting the suite.
TEST(AbiThumbGenerate, ConcurrentGenerationOfDanglingRowReturnsNotFound) {
    CatalogGuard guard;
    TempDbPath tmp;
    std::string sha(64, '1');
    int64_t id = seed_thumb_row(tmp.path, "tests/data/does_not_exist.jpg", "image", sha);

    phig_config cfg{};
    cfg.catalog_dir = tmp.dir.c_str();
    cfg.mode = PHIG_READ_ONLY;

    constexpr int kMarkerExitCode = 42;
    EXPECT_EXIT(
        {
            phig_ctx* ctx = nullptr;
            if (phig_init(&cfg, &ctx) != PHIG_OK) _exit(1);

            constexpr int kThreads = 8;
            std::vector<std::thread> threads;
            std::vector<phig_status> results(kThreads);
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] { results[t] = phig_thumb_generate(ctx, id, 512); });
            }
            for (auto& th : threads) th.join();

            for (auto st : results) {
                if (st != PHIG_ERR_NOT_FOUND) _exit(2);
            }

            phig_shutdown(ctx);
            _exit(kMarkerExitCode);
        },
        ::testing::ExitedWithCode(kMarkerExitCode), "");
}
