#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <stdexcept>

#include "paths.h"

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
