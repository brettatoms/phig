#include <gtest/gtest.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
    Saved vars[5] = {
        {"HOME", false, ""},           {"XDG_CONFIG_HOME", false, ""}, {"XDG_DATA_HOME", false, ""},
        {"XDG_CACHE_HOME", false, ""}, {"XDG_STATE_HOME", false, ""},
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
        unsetenv("XDG_STATE_HOME");
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

TEST_F(PathsTest, StateDirHonorsXdgStateHome) {
    setenv("XDG_STATE_HOME", "/tmp/xdg-state-test", 1);
    EXPECT_EQ(state_dir(), "/tmp/xdg-state-test/phig");
}

TEST_F(PathsTest, StateDirFallsBackToHome) {
    EXPECT_EQ(state_dir(), "/home/tester/.local/state/phig");
}

TEST_F(PathsTest, DefaultLogPathUnderStateDir) {
    EXPECT_EQ(default_log_path(), state_dir() + "/phig.log");
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

TEST_F(MoveFileTest, RefusesToOverwriteExistingDest) {
    fs::path src = dir / "src.jpg", dst = dir / "dst.jpg";
    write_file(src, "SOURCE");
    write_file(dst, "PRECIOUS");

    EXPECT_THROW(move_file(src.string(), dst.string()), fs::filesystem_error);

    // Both files intact, contents untouched.
    EXPECT_EQ(read_file(src), "SOURCE");
    EXPECT_EQ(read_file(dst), "PRECIOUS");
}

TEST_F(MoveFileTest, CrossDeviceRefusesToOverwriteExistingDest) {
    fs::path shm = "/dev/shm";
    if (!fs::exists(shm) || device_of(dir) == device_of(shm)) {
        GTEST_SKIP() << "no distinct second filesystem available for EXDEV test";
    }
    fs::path shm_dir = shm / "phig-move-test";
    fs::remove_all(shm_dir);
    fs::create_directories(shm_dir);

    fs::path src = dir / "src.jpg", dst = shm_dir / "dst.jpg";
    write_file(src, "SOURCE");
    write_file(dst, "PRECIOUS");

    EXPECT_THROW(move_file(src.string(), dst.string()), fs::filesystem_error);
    EXPECT_EQ(read_file(src), "SOURCE");
    EXPECT_EQ(read_file(dst), "PRECIOUS");

    fs::remove_all(shm_dir);
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

// Genuine cross-device move where the copy itself fails (unreadable source),
// so rename_noreplace returns EXDEV but fs::copy_file then errors. The thrown
// exception must carry the real copy error, not a clobbered "success" from the
// temp-file cleanup remove.
TEST_F(MoveFileTest, CrossDeviceCopyFailureReportsRealError) {
    fs::path shm = "/dev/shm";
    if (!fs::exists(shm) || device_of(dir) == device_of(shm)) {
        GTEST_SKIP() << "no distinct second filesystem available for EXDEV test";
    }
    if (geteuid() == 0) {
        GTEST_SKIP() << "running as root: unreadable-file permission checks don't apply";
    }
    fs::path shm_dir = shm / "phig-move-test";
    fs::remove_all(shm_dir);
    fs::create_directories(shm_dir);

    fs::path src = dir / "unreadable.jpg";
    fs::path dst = shm_dir / "dst.jpg";
    write_file(src, "SOURCE");
    fs::permissions(src, fs::perms::none);

    bool threw = false;
    try {
        move_file(src.string(), dst.string());
    } catch (const fs::filesystem_error& e) {
        threw = true;
        EXPECT_NE(e.code().value(), 0);
        EXPECT_NE(e.code(), std::error_code());
    }
    EXPECT_TRUE(threw);

    fs::permissions(src, fs::perms::owner_all);
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

    // Default predicate: nothing is protected.
    static bool no_protect(const std::string&) { return false; }
};

TEST_F(ResolveConflictTest, NoConflictReturnsDestAndClaims) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed, no_protect), dst.string());
    EXPECT_EQ(claimed.count(dst.string()), 1u);
}

TEST_F(ResolveConflictTest, SkipReturnsEmptyWhenFileExists) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed, no_protect), "");
}

TEST_F(ResolveConflictTest, OverwriteReturnsDestWhenFileExists) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    EXPECT_EQ(resolve_conflict(dst.string(), "overwrite", claimed, no_protect), dst.string());
}

TEST_F(ResolveConflictTest, RenameAppendsSuffixWhenFileExists) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    EXPECT_EQ(resolve_conflict(dst.string(), "rename", claimed, no_protect),
              (dir / "a_1.jpg").string());
}

TEST_F(ResolveConflictTest, RenameSkipsAlreadyExistingSuffixes) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";
    touch(dst);
    touch(dir / "a_1.jpg");
    touch(dir / "a_2.jpg");
    EXPECT_EQ(resolve_conflict(dst.string(), "rename", claimed, no_protect),
              (dir / "a_3.jpg").string());
}

// The bug: two source files resolving to the same destination in one run.
// Neither exists on disk at planning time, so only the `claimed` set catches it.
TEST_F(ResolveConflictTest, IntraRunCollisionRenameKeepsBoth) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";  // nothing on disk yet

    std::string first = resolve_conflict(dst.string(), "rename", claimed, no_protect);
    std::string second = resolve_conflict(dst.string(), "rename", claimed, no_protect);

    EXPECT_EQ(first, dst.string());
    EXPECT_EQ(second, (dir / "a_1.jpg").string());
    EXPECT_NE(first, second);  // both survive — no clobber
}

TEST_F(ResolveConflictTest, IntraRunCollisionSkipDropsSecond) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";

    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed, no_protect), dst.string());
    EXPECT_EQ(resolve_conflict(dst.string(), "skip", claimed, no_protect), "");  // second skipped
}

// Even with overwrite, two DISTINCT sources on one path within a run must not
// clobber each other — the second is renamed so both files survive.
TEST_F(ResolveConflictTest, IntraRunCollisionOverwriteRenamesSecond) {
    std::set<std::string> claimed;
    fs::path dst = dir / "a.jpg";

    std::string first = resolve_conflict(dst.string(), "overwrite", claimed, no_protect);
    std::string second = resolve_conflict(dst.string(), "overwrite", claimed, no_protect);

    EXPECT_EQ(first, dst.string());
    EXPECT_EQ(second, (dir / "a_1.jpg").string());
}

// audit #1 (critical): under overwrite, a dest equal to another image's
// not-yet-moved source is PROTECTED — resolve_conflict must rename around it,
// never return it, so the original is not clobbered.
TEST_F(ResolveConflictTest, OverwriteDoesNotClobberProtectedSource) {
    std::set<std::string> claimed;
    fs::path dst = dir / "IMG.jpg";  // == some other image's source path
    touch(dst);                      // the original exists on disk
    auto protect = [&](const std::string& p) { return p == dst.string(); };

    std::string r = resolve_conflict(dst.string(), "overwrite", claimed, protect);
    EXPECT_NE(r, dst.string());                  // did NOT choose the protected path
    EXPECT_EQ(r, (dir / "IMG_1.jpg").string());  // renamed around it
}

// audit #4: under overwrite, an untracked on-disk file at the dest is protected
// exactly like a source, so it survives.
TEST_F(ResolveConflictTest, OverwriteDoesNotClobberUntrackedFile) {
    std::set<std::string> claimed;
    fs::path dst = dir / "untracked.jpg";
    touch(dst);
    auto protect = [&](const std::string& p) { return p == dst.string(); };

    EXPECT_EQ(resolve_conflict(dst.string(), "overwrite", claimed, protect),
              (dir / "untracked_1.jpg").string());
}

// A protected path even blocks the rename candidates: if IMG_1.jpg is also
// protected, resolution skips to the next free, unprotected name.
TEST_F(ResolveConflictTest, RenameSkipsProtectedCandidates) {
    std::set<std::string> claimed;
    fs::path dst = dir / "IMG.jpg";
    touch(dst);
    fs::path taken1 = dir / "IMG_1.jpg";
    auto protect = [&](const std::string& p) { return p == dst.string() || p == taken1.string(); };
    EXPECT_EQ(resolve_conflict(dst.string(), "rename", claimed, protect),
              (dir / "IMG_2.jpg").string());
}

// A file resolving to its OWN source path is not protected against itself.
TEST_F(ResolveConflictTest, SelfSourceNotProtected) {
    std::set<std::string> claimed;
    fs::path dst = dir / "self.jpg";
    touch(dst);
    // Caller's predicate returns false for the file's own source (self excluded).
    EXPECT_EQ(resolve_conflict(dst.string(), "overwrite", claimed, no_protect), dst.string());
}

// ---- paths_safe_to_delete (scan cleanup guard) ----

class SafeToDeleteTest : public ::testing::Test {
   protected:
    fs::path root;

    void SetUp() override {
        root = fs::temp_directory_path() / "phig-safedelete-test";
        fs::remove_all(root);
        fs::create_directories(root);
    }
    void TearDown() override { fs::remove_all(root); }

    void touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream o(p);
    }
    static bool contains(const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    }
};

TEST_F(SafeToDeleteTest, PresentFilesAreNotDeletable) {
    fs::path a = root / "a.jpg";
    touch(a);
    auto del = paths_safe_to_delete({a.string()});
    EXPECT_TRUE(del.empty());
}

TEST_F(SafeToDeleteTest, GenuinelyMissingFileWithPresentParentIsDeletable) {
    // Parent dir exists, file within it is gone -> a real deletion.
    fs::path missing = root / "album" / "gone.jpg";
    fs::create_directories(missing.parent_path());  // parent present, file absent
    auto del = paths_safe_to_delete({missing.string()});
    ASSERT_EQ(del.size(), 1u);
    EXPECT_EQ(del[0], missing.string());
}

TEST_F(SafeToDeleteTest, MissingFileUnderAbsentParentIsKept) {
    // The whole subtree (parent dir) is gone -> looks like an unmounted mount,
    // NOT an individual deletion. Must be kept.
    fs::path under_unmounted = root / "removable" / "2020" / "img.jpg";
    // Do NOT create the parent dirs.
    auto del = paths_safe_to_delete({under_unmounted.string()});
    EXPECT_TRUE(del.empty());
}

TEST_F(SafeToDeleteTest, PartialUnmountKeepsUnmountedSubtreeDeletesRealMissing) {
    // The bug scenario: most files present, one subtree unmounted, one file
    // genuinely deleted from a present directory.
    fs::path present1 = root / "local" / "a.jpg";
    fs::path present2 = root / "local" / "b.jpg";
    touch(present1);
    touch(present2);

    fs::path real_deleted = root / "local" / "deleted.jpg";  // parent present, file gone

    fs::path unmounted = root / "mnt2020" / "x.jpg";  // parent dir absent

    auto del = paths_safe_to_delete(
        {present1.string(), present2.string(), real_deleted.string(), unmounted.string()});

    // Only the genuinely-deleted file (present parent) is removed.
    ASSERT_EQ(del.size(), 1u);
    EXPECT_TRUE(contains(del, real_deleted.string()));
    EXPECT_FALSE(contains(del, unmounted.string()));  // unmounted subtree preserved
    EXPECT_FALSE(contains(del, present1.string()));
    EXPECT_FALSE(contains(del, present2.string()));
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

// ---- trash_dir and trash_path_for ----

TEST(TrashPathTest, TrashDirUnderDataHome) {
    fs::path tmp = fs::temp_directory_path() / "phig-trashdir-test";
    fs::remove_all(tmp);
    setenv("XDG_DATA_HOME", tmp.c_str(), 1);
    EXPECT_EQ(trash_dir(), (tmp / "phig" / "trash").string());
    unsetenv("XDG_DATA_HOME");
    fs::remove_all(tmp);
}

TEST(TrashPathTest, TrashPathForInsertsRandomHexAndIsUnique) {
    fs::path tmp = fs::temp_directory_path() / "phig-trashpath-test";
    fs::remove_all(tmp);
    setenv("XDG_DATA_HOME", tmp.c_str(), 1);

    // Shape: <trash_dir>/<stem>.<8 hex>.<ext>, and the path does not exist yet
    // (trash_path_for never scans/creates — it just names).
    std::string p = trash_path_for("IMG_1234.jpg");
    fs::path pp(p);
    EXPECT_EQ(pp.parent_path(), fs::path(trash_dir()));
    EXPECT_FALSE(fs::exists(p));
    // filename is IMG_1234.<hex>.jpg
    std::string name = pp.filename().string();
    ASSERT_EQ(name.rfind("IMG_1234.", 0), 0u);  // starts with stem + '.'
    ASSERT_TRUE(name.size() > std::string("IMG_1234..jpg").size());
    // The extension is preserved as the final component.
    EXPECT_EQ(pp.extension().string(), ".jpg");
    // The token between stem and extension is 8 lowercase-hex chars.
    std::string mid = name.substr(std::string("IMG_1234.").size());  // "<hex>.jpg"
    std::string hex = mid.substr(0, mid.size() - std::string(".jpg").size());
    EXPECT_EQ(hex.size(), 8u);
    EXPECT_TRUE(std::all_of(hex.begin(), hex.end(),
                            [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));

    // Two calls for the same name yield different paths (random, no counter).
    std::string q = trash_path_for("IMG_1234.jpg");
    EXPECT_NE(p, q);

    // A name with no extension still gets a hex token appended.
    std::string noext = trash_path_for("NOEXT");
    std::string noext_name = fs::path(noext).filename().string();
    EXPECT_EQ(noext_name.rfind("NOEXT.", 0), 0u);
    EXPECT_EQ(noext_name.size(), std::string("NOEXT.").size() + 8u);

    unsetenv("XDG_DATA_HOME");
    fs::remove_all(tmp);
}

// ---- temp_sibling concurrency (data race test) ----

TEST(TempSiblingConcurrency, NoCollisionUnderThreads) {
    namespace fs = std::filesystem;
    const fs::path dest = fs::temp_directory_path() / "phig-tmpsib" / "IMG_1.jpg";
    const int threads = 8;
    const int per_thread = 2000;

    std::vector<std::vector<std::string>> results(threads);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            results[t].reserve(per_thread);
            for (int i = 0; i < per_thread; ++i) results[t].push_back(temp_sibling(dest).string());
        });
    }
    for (auto& th : pool) th.join();

    std::set<std::string> uniq;
    size_t total = 0;
    for (auto& r : results)
        for (auto& s : r) {
            uniq.insert(s);
            ++total;
        }

    // Every generated temp path must be distinct.
    EXPECT_EQ(uniq.size(), total);
}
