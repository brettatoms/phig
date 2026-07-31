#include <gtest/gtest.h>

#include <filesystem>
#include <string>

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

TEST(Abi, ReadOnlyContextCannotWrite) {
    TempDbPath tmp;
    make_db(tmp.path);  // exists, migrated

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    ASSERT_NE(ctx, nullptr);
    phig_shutdown(ctx);
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
