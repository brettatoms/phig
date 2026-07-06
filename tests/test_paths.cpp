#include <gtest/gtest.h>

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "paths.h"

namespace fs = std::filesystem;

// Saves and restores HOME + all three XDG base-dir env vars so each test can
// set them freely without leaking into other tests.
class PathsTest : public ::testing::Test {
   protected:
    struct Saved {
        const char* name;
        bool was_set;
        std::string value;
    };
    Saved vars[4] = {
        {"HOME", false, ""},
        {"XDG_CONFIG_HOME", false, ""},
        {"XDG_DATA_HOME", false, ""},
        {"XDG_CACHE_HOME", false, ""},
    };

    void SetUp() override {
        for (auto& v : vars) {
            const char* cur = std::getenv(v.name);
            v.was_set = cur != nullptr;
            if (cur) v.value = cur;
        }
        // Start each test from a known base: HOME set, XDG unset.
        setenv("HOME", "/home/tester", 1);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("XDG_DATA_HOME");
        unsetenv("XDG_CACHE_HOME");
        // No path overrides unless a test sets them.
        set_path_overrides(std::nullopt, std::nullopt, std::nullopt);
    }

    void TearDown() override {
        for (auto& v : vars) {
            if (v.was_set) {
                setenv(v.name, v.value.c_str(), 1);
            } else {
                unsetenv(v.name);
            }
        }
    }
};

// ---- Base resolvers honor XDG when set ----

TEST_F(PathsTest, ConfigDirUsesXdgWhenSet) {
    setenv("XDG_CONFIG_HOME", "/custom/config", 1);
    EXPECT_EQ(config_dir(), "/custom/config/phig");
}

TEST_F(PathsTest, DataDirUsesXdgWhenSet) {
    setenv("XDG_DATA_HOME", "/custom/data", 1);
    EXPECT_EQ(data_dir(), "/custom/data/phig");
}

TEST_F(PathsTest, CacheDirUsesXdgWhenSet) {
    setenv("XDG_CACHE_HOME", "/custom/cache", 1);
    EXPECT_EQ(cache_dir(), "/custom/cache/phig");
}

// ---- Base resolvers fall back to XDG defaults under HOME ----

TEST_F(PathsTest, ConfigDirFallsBackToHome) {
    EXPECT_EQ(config_dir(), "/home/tester/.config/phig");
}

TEST_F(PathsTest, DataDirFallsBackToHome) {
    EXPECT_EQ(data_dir(), "/home/tester/.local/share/phig");
}

TEST_F(PathsTest, CacheDirFallsBackToHome) { EXPECT_EQ(cache_dir(), "/home/tester/.cache/phig"); }

// ---- Empty-string env vars are treated as unset ----

TEST_F(PathsTest, EmptyXdgTreatedAsUnset) {
    setenv("XDG_CONFIG_HOME", "", 1);
    EXPECT_EQ(config_dir(), "/home/tester/.config/phig");
}

// ---- Derived paths ----

TEST_F(PathsTest, DefaultDbPathUnderData) {
    EXPECT_EQ(default_db_path(), "/home/tester/.local/share/phig/phig.db");
}

TEST_F(PathsTest, ModelsDirUnderCache) {
    EXPECT_EQ(models_dir(), "/home/tester/.cache/phig/models");
}

TEST_F(PathsTest, ThumbsDirUnderCache) {
    EXPECT_EQ(thumbs_dir(), "/home/tester/.cache/phig/thumbs");
}

TEST_F(PathsTest, ConfigPathUnderConfig) {
    EXPECT_EQ(config_path(), "/home/tester/.config/phig/config.ini");
}

// ---- Missing HOME with no XDG override throws ----

TEST_F(PathsTest, MissingHomeThrows) {
    unsetenv("HOME");
    EXPECT_THROW(config_dir(), std::runtime_error);
}

// ---- Tilde expansion ----

TEST_F(PathsTest, ExpandTildeReplacesLeadingTilde) {
    EXPECT_EQ(expand_tilde("~/photos"), "/home/tester/photos");
}

TEST_F(PathsTest, ExpandTildeLeavesNonTildeUnchanged) {
    EXPECT_EQ(expand_tilde("/abs/path"), "/abs/path");
    EXPECT_EQ(expand_tilde("relative/path"), "relative/path");
}

TEST_F(PathsTest, ExpandTildeOnlyLeadingTilde) { EXPECT_EQ(expand_tilde("/x/~/y"), "/x/~/y"); }

// ---- Path overrides (from config) take precedence over computed paths ----

TEST_F(PathsTest, PathOverridesTakePrecedence) {
    set_path_overrides("/opt/my.db", "/opt/models", "/opt/thumbs");
    EXPECT_EQ(default_db_path(), "/opt/my.db");
    EXPECT_EQ(models_dir(), "/opt/models");
    EXPECT_EQ(thumbs_dir(), "/opt/thumbs");
}

TEST_F(PathsTest, UnsetOverridesFallBackToComputed) {
    set_path_overrides(std::nullopt, std::nullopt, std::nullopt);
    EXPECT_EQ(default_db_path(), "/home/tester/.local/share/phig/phig.db");
    EXPECT_EQ(models_dir(), "/home/tester/.cache/phig/models");
    EXPECT_EQ(thumbs_dir(), "/home/tester/.cache/phig/thumbs");
}

TEST_F(PathsTest, PartialOverrideOnlyAffectsSetPaths) {
    set_path_overrides("/opt/only.db", std::nullopt, std::nullopt);
    EXPECT_EQ(default_db_path(), "/opt/only.db");
    EXPECT_EQ(models_dir(), "/home/tester/.cache/phig/models");
}

// ---- move_file ----

class MoveFileTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-move-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    static void write_file(const fs::path& p, const std::string& contents) {
        std::ofstream o(p);
        o << contents;
    }
    static std::string read_file(const fs::path& p) {
        std::ifstream in(p);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    // Device id of the filesystem holding p, or its parent if p doesn't exist.
    static dev_t device_of(const fs::path& p) {
        struct stat st{};
        if (stat(p.c_str(), &st) != 0) stat(p.parent_path().c_str(), &st);
        return st.st_dev;
    }
};

TEST_F(MoveFileTest, SameFilesystemMove) {
    fs::path src = dir / "a.txt", dst = dir / "b.txt";
    write_file(src, "hello");

    move_file(src.string(), dst.string());

    EXPECT_FALSE(fs::exists(src));
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "hello");
}

TEST_F(MoveFileTest, CreatesDestinationParents) {
    fs::path src = dir / "a.txt", dst = dir / "nested/deep/b.txt";
    write_file(src, "data");

    move_file(src.string(), dst.string());

    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "data");
}

TEST_F(MoveFileTest, OverwritesExistingDestination) {
    fs::path src = dir / "a.txt", dst = dir / "b.txt";
    write_file(src, "new");
    write_file(dst, "old");

    move_file(src.string(), dst.string());

    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "new");
    EXPECT_FALSE(fs::exists(src));
}

TEST_F(MoveFileTest, MissingSourceThrows) {
    fs::path src = dir / "does-not-exist.txt", dst = dir / "b.txt";
    EXPECT_THROW(move_file(src.string(), dst.string()), fs::filesystem_error);
}

// Genuine cross-device move: source on the system temp fs, dest on /dev/shm.
// rename(2) fails with EXDEV here, exercising the copy+remove fallback. Skipped
// when /dev/shm is unavailable or happens to be the same filesystem.
TEST_F(MoveFileTest, CrossDeviceFallsBackToCopyRemove) {
    fs::path shm = "/dev/shm";
    if (!fs::exists(shm) || device_of(dir) == device_of(shm)) {
        GTEST_SKIP() << "no distinct second filesystem available for EXDEV test";
    }
    fs::path shm_dir = shm / "phig-move-test";
    fs::remove_all(shm_dir);

    fs::path src = dir / "a.txt";
    fs::path dst = shm_dir / "sub/b.txt";
    write_file(src, "cross-device");

    move_file(src.string(), dst.string());

    EXPECT_FALSE(fs::exists(src));
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "cross-device");

    fs::remove_all(shm_dir);
}

// ---- resolve_conflict ----

class ResolveConflictTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-conflict-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    void touch(const fs::path& p) { std::ofstream o(p); }
};

TEST_F(ResolveConflictTest, NoConflictReturnsDestAndClaims) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed), dst.string());
    EXPECT_EQ(claimed.count(dst.string()), 1u);
}

TEST_F(ResolveConflictTest, SkipReturnsEmptyWhenFileExists) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed), "");
}

TEST_F(ResolveConflictTest, OverwriteReturnsDestWhenFileExists) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    EXPECT_EQ(resolve_conflict(dst.string(), "overwrite", claimed), dst.string());
}

TEST_F(ResolveConflictTest, RenameAppendsSuffixWhenFileExists) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    EXPECT_EQ(resolve_conflict(dst.string(), "rename", claimed), (dir / "a_1.jpg").string());
}

TEST_F(ResolveConflictTest, RenameSkipsAlreadyExistingSuffixes) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    touch(dir / "a_1.jpg");
    touch(dir / "a_2.jpg");
    EXPECT_EQ(resolve_conflict(dst.string(), "rename", claimed), (dir / "a_3.jpg").string());
}

// The bug: two source files resolving to the same destination in one run.
// Neither exists on disk at planning time, so only the `claimed` set catches it.
TEST_F(ResolveConflictTest, IntraRunCollisionRenameKeepsBoth) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";  // nothing on disk yet

    std::string first = resolve_conflict(dst.string(), "rename", claimed);
    std::string second = resolve_conflict(dst.string(), "rename", claimed);

    EXPECT_EQ(first, dst.string());
    EXPECT_EQ(second, (dir / "a_1.jpg").string());
    EXPECT_NE(first, second);  // both survive — no clobber
}

TEST_F(ResolveConflictTest, IntraRunCollisionSkipDropsSecond) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";

    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed), dst.string());
    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed), "");  // second skipped
}

// Even with overwrite, two DISTINCT sources on one path within a run must not
// clobber each other — the second is renamed so both files survive.
TEST_F(ResolveConflictTest, IntraRunCollisionOverwriteRenamesSecond) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";

    std::string first = resolve_conflict(dst.string(), "overwrite", claimed);
    std::string second = resolve_conflict(dst.string(), "overwrite", claimed);

    EXPECT_EQ(first, dst.string());
    EXPECT_EQ(second, (dir / "a_1.jpg").string());
}

// ---- atomic writes ----

class AtomicWriteTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-atomic-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    static void write_file(const fs::path& p, const std::string& c) {
        std::ofstream o(p, std::ios::binary);
        o << c;
    }
    static std::string read_file(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    // Any leftover ".phig-tmp" file in the tree — a residue leak.
    bool has_temp_residue() {
        for (auto& e : fs::recursive_directory_iterator(dir))
            if (e.is_regular_file() && e.path().string().find(".phig-tmp") != std::string::npos)
                return true;
        return false;
    }
};

TEST_F(AtomicWriteTest, WriteFileProducesExactBytes) {
    fs::path dst = dir / "sub/thumb.jpg";
    std::string bytes = "\x00\x01JPEGDATA\xff";
    ASSERT_TRUE(atomic_write_file(dst.string(), bytes.data(), bytes.size()));
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), bytes);
    EXPECT_FALSE(has_temp_residue());
}

TEST_F(AtomicWriteTest, WriteFileToUnwritableDirLeavesNothing) {
    // Parent path is a regular file, so create_directories/open fails.
    fs::path blocker = dir / "blocker";
    write_file(blocker, "x");
    fs::path dst = blocker / "cant.jpg";  // blocker is a file, not a dir
    std::string bytes = "data";
    EXPECT_FALSE(atomic_write_file(dst.string(), bytes.data(), bytes.size()));
    EXPECT_FALSE(fs::exists(dst));
    EXPECT_FALSE(has_temp_residue());
}

TEST_F(AtomicWriteTest, CopyFileSucceedsAndLeavesNoTemp) {
    fs::path src = dir / "src.bin", dst = dir / "out/dst.bin";
    write_file(src, "payload-payload");
    atomic_copy_file(src.string(), dst.string());
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "payload-payload");
    EXPECT_TRUE(fs::exists(src));  // copy, not move
    EXPECT_FALSE(has_temp_residue());
}

TEST_F(AtomicWriteTest, CopyFileFailureDoesNotTruncateExistingDest) {
    // A pre-existing good file at dest must survive if the copy fails, because
    // the copy goes to a temp and only renames on success — dest is never
    // opened for truncation. Force failure with a nonexistent source.
    fs::path dst = dir / "important.jpg";
    write_file(dst, "ORIGINAL-GOOD-DATA");
    fs::path missing = dir / "does-not-exist.src";

    EXPECT_THROW(atomic_copy_file(missing.string(), dst.string()), fs::filesystem_error);

    // The original is intact, not truncated or replaced.
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(read_file(dst), "ORIGINAL-GOOD-DATA");
    EXPECT_FALSE(has_temp_residue());
}

TEST_F(AtomicWriteTest, CopyFileOverwritesExistingDestOnSuccess) {
    fs::path src = dir / "new.bin", dst = dir / "old.bin";
    write_file(src, "NEW");
    write_file(dst, "OLD-AND-LONGER");
    atomic_copy_file(src.string(), dst.string());
    EXPECT_EQ(read_file(dst), "NEW");
    EXPECT_FALSE(has_temp_residue());
}
