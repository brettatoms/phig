#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "phig/phig.h"

namespace {
struct TempIni {
    std::string path;
    explicit TempIni(const std::string& content) {
        path = (std::filesystem::temp_directory_path() /
                ("phig_abi_ini_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".ini"))
                   .string();
        std::ofstream(path) << content;
    }
    ~TempIni() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string read() const {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};
}  // namespace

TEST(AbiSettings, LoadsAndReadsStringValues) {
    TempIni f("[paths]\ndb = /photos/phig.db\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(phig_settings_get(s, PHIG_KEY_DB, "unset"), "/photos/phig.db");
    EXPECT_STREQ(phig_settings_get(s, "paths.nope", "fallback"), "fallback");
    EXPECT_TRUE(phig_settings_has(s, PHIG_KEY_DB));
    EXPECT_FALSE(phig_settings_has(s, "paths.nope"));
    phig_settings_free(s);
}

TEST(AbiSettings, ExpandsTildeOnRead) {
    TempIni f("[paths]\ndb = ~/photos/phig.db\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    const char* v = phig_settings_get(s, PHIG_KEY_DB, "");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(std::string(v).find('~'), std::string::npos);
    EXPECT_NE(std::string(v).find("/photos/phig.db"), std::string::npos);
    phig_settings_free(s);
}

TEST(AbiSettings, IntAndBoolParsingWithFallbacks) {
    TempIni f(
        "[general]\nparallel = 8\n"
        "[log]\nenabled = yes\nfile = FALSE\nmax_entries = notanumber\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    EXPECT_EQ(phig_settings_get_int(s, PHIG_KEY_PARALLEL, 1), 8);
    EXPECT_EQ(phig_settings_get_int(s, "general.missing", 42), 42);
    EXPECT_EQ(phig_settings_get_int(s, PHIG_KEY_LOG_MAX, 99), 99);  // unparseable
    EXPECT_TRUE(phig_settings_get_bool(s, PHIG_KEY_LOG_ENABLED, false));
    EXPECT_FALSE(phig_settings_get_bool(s, PHIG_KEY_LOG_FILE, true));
    EXPECT_TRUE(phig_settings_get_bool(s, "log.missing", true));
    phig_settings_free(s);
}

TEST(AbiSettings, MissingFileYieldsNotFoundWithUsableHandle) {
    auto missing = (std::filesystem::temp_directory_path() /
                    ("phig_absent_" + std::to_string(::getpid()) + ".ini"))
                       .string();
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    phig_settings* s = nullptr;
    EXPECT_EQ(phig_settings_load(missing.c_str(), &s), PHIG_ERR_NOT_FOUND);
    ASSERT_NE(s, nullptr);  // usable empty handle
    EXPECT_EQ(phig_settings_count(s), 0u);
    EXPECT_EQ(phig_settings_set(s, PHIG_KEY_DB, "/new"), PHIG_OK);
    EXPECT_EQ(phig_settings_save(s, missing.c_str()), PHIG_OK);
    EXPECT_TRUE(std::filesystem::exists(missing));
    phig_settings_free(s);
    std::filesystem::remove(missing, ec);
}

TEST(AbiSettings, UnreadableFileYieldsErrDbWithNullHandle) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "running as root: chmod 000 does not deny access";
    }
    TempIni f("[paths]\ndb = /x\n");
    ASSERT_EQ(::chmod(f.path.c_str(), 0), 0);

    phig_settings* s = reinterpret_cast<phig_settings*>(1);  // sentinel
    EXPECT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_ERR_DB);
    EXPECT_EQ(s, nullptr);

    ::chmod(f.path.c_str(), 0644);  // let TempIni's destructor remove it
}

TEST(AbiSettings, EnumerationReturnsKeysInFileOrder) {
    TempIni f("[paths]\ndb = /x\nthumbs_dir = /t\n[general]\nparallel = 4\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_EQ(phig_settings_count(s), 3u);
    EXPECT_STREQ(phig_settings_key_at(s, 0), "paths.db");
    EXPECT_STREQ(phig_settings_key_at(s, 1), "paths.thumbs_dir");
    EXPECT_STREQ(phig_settings_key_at(s, 2), "general.parallel");
    EXPECT_EQ(phig_settings_key_at(s, 3), nullptr);
    EXPECT_EQ(phig_settings_key_at(s, 9999), nullptr);
    phig_settings_free(s);
}

TEST(AbiSettings, EnumerationPointersSurviveWholeLoop) {
    // Keys must be at least 16 characters to exceed libstdc++'s small-string
    // optimization buffer; shorter keys live inside the vector's element
    // array and mask a use-after-free if key_cache is rebuilt mid-enumeration.
    TempIni f(
        "[paths]\nthumbs_dir = /t\n"
        "[general]\nsome_long_key_name = 1\n"
        "[log]\nyet_another_long_key = 2\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    size_t n = phig_settings_count(s);
    ASSERT_EQ(n, 3u);

    std::vector<const char*> ptrs(n);
    for (size_t i = 0; i < n; i++) {
        ptrs[i] = phig_settings_key_at(s, i);
        ASSERT_NE(ptrs[i], nullptr);
    }
    // Only now, after the whole loop has run, dereference every pointer.
    EXPECT_STREQ(ptrs[0], "paths.thumbs_dir");
    EXPECT_STREQ(ptrs[1], "general.some_long_key_name");
    EXPECT_STREQ(ptrs[2], "log.yet_another_long_key");
    phig_settings_free(s);
}

TEST(AbiSettings, SetSaveRoundTripPreservesComments) {
    TempIni f("# keep me\n[paths]\ndb = /x\n[general]\nparallel = 4\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_EQ(phig_settings_set(s, PHIG_KEY_PARALLEL, "8"), PHIG_OK);
    ASSERT_EQ(phig_settings_save(s, nullptr), PHIG_OK);  // NULL = loaded path
    std::string out = f.read();
    EXPECT_NE(out.find("# keep me"), std::string::npos);
    EXPECT_NE(out.find("parallel = 8"), std::string::npos);
    phig_settings_free(s);
}

TEST(AbiSettings, UnsetRemovesKey) {
    TempIni f("[paths]\ndb = /x\nthumbs_dir = /t\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_EQ(phig_settings_unset(s, PHIG_KEY_DB), PHIG_OK);
    EXPECT_FALSE(phig_settings_has(s, PHIG_KEY_DB));
    EXPECT_EQ(phig_settings_unset(s, "paths.nope"), PHIG_OK);  // absent = no-op
    phig_settings_free(s);
}

TEST(AbiSettings, ToConfigFillsPathsAndLeavesModeAlone) {
    TempIni f("[paths]\ndb = /photos/phig.db\nthumbs_dir = /ssd/thumbs\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);

    phig_config cfg{};
    cfg.mode = PHIG_READ_WRITE;  // caller's choice; must survive
    ASSERT_EQ(phig_settings_to_config(s, &cfg), PHIG_OK);
    ASSERT_NE(cfg.db_path, nullptr);
    EXPECT_STREQ(cfg.db_path, "/photos/phig.db");
    ASSERT_NE(cfg.thumbs_dir, nullptr);
    EXPECT_STREQ(cfg.thumbs_dir, "/ssd/thumbs");
    EXPECT_EQ(cfg.mode, PHIG_READ_WRITE);
    EXPECT_EQ(cfg.models_dir, nullptr);  // not in the file, left untouched
    phig_settings_free(s);
}

TEST(AbiSettings, ToConfigDoesNotClobberPreSeededFields) {
    TempIni f("[general]\nparallel = 4\n");  // no [paths] at all
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);

    phig_config cfg{};
    cfg.db_path = "/caller/seeded.db";
    ASSERT_EQ(phig_settings_to_config(s, &cfg), PHIG_OK);
    EXPECT_STREQ(cfg.db_path, "/caller/seeded.db");
    phig_settings_free(s);
}

TEST(AbiSettings, NullArgumentsAreRejected) {
    TempIni f("[paths]\ndb = /x\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);

    EXPECT_EQ(phig_settings_load(f.path.c_str(), nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_set(nullptr, "a.b", "v"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_set(s, nullptr, "v"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_set(s, "a.b", nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_unset(nullptr, "a.b"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_save(nullptr, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_to_config(nullptr, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_to_config(s, nullptr), PHIG_ERR_INVALID_ARG);

    // Readers must tolerate NULL and return the fallback, not crash.
    EXPECT_STREQ(phig_settings_get(nullptr, "a.b", "fb"), "fb");
    EXPECT_EQ(phig_settings_get_int(nullptr, "a.b", 7), 7);
    EXPECT_TRUE(phig_settings_get_bool(nullptr, "a.b", true));
    EXPECT_FALSE(phig_settings_has(nullptr, "a.b"));
    EXPECT_EQ(phig_settings_count(nullptr), 0u);
    EXPECT_EQ(phig_settings_key_at(nullptr, 0), nullptr);
    EXPECT_STREQ(phig_settings_get(s, nullptr, "fb"), "fb");
    phig_settings_free(nullptr);  // must not crash
    phig_settings_free(s);
}

TEST(AbiSettings, LoadsDefaultPathWhenNull) {
    // NULL path means phig's default config location. It may or may not exist
    // on this machine; either outcome is valid, and the handle must be usable.
    phig_settings* s = nullptr;
    phig_status st = phig_settings_load(nullptr, &s);
    EXPECT_TRUE(st == PHIG_OK || st == PHIG_ERR_NOT_FOUND);
    ASSERT_NE(s, nullptr);
    (void)phig_settings_count(s);
    phig_settings_free(s);
}
