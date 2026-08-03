#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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
