#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "paths.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

// Points HOME and the four XDG base directories into a per-process scratch
// directory and clears PHIG_CATALOG. The real face models and ~/phig-live-samples
// are linked back in, since tests read them at fixed paths under HOME and would
// otherwise skip. A test needing the models missing redirects XDG_CACHE_HOME.
class ScratchHome : public ::testing::Environment {
   public:
    void SetUp() override {
        std::error_code ec;
        const char* real_home = std::getenv("HOME");
        fs::path real_models;
        try {
            real_models = models_dir();
        } catch (const std::exception&) {
            // No HOME and no XDG_CACHE_HOME: there are no real models to keep.
        }
        root_ = make_temp_dir("scratch-home");
        const fs::path home = root_ / "home";
        set("HOME", home);
        set("XDG_CONFIG_HOME", home / ".config");
        set("XDG_DATA_HOME", home / ".local" / "share");
        set("XDG_CACHE_HOME", home / ".cache");
        set("XDG_STATE_HOME", home / ".local" / "state");
        ::unsetenv("PHIG_CATALOG");
        if (!real_models.empty() && fs::is_directory(real_models, ec)) {
            fs::create_directories(home / ".cache" / "phig", ec);
            fs::create_directory_symlink(real_models, home / ".cache" / "phig" / "models", ec);
        }
        if (real_home != nullptr) {
            const fs::path samples = fs::path(real_home) / "phig-live-samples";
            if (fs::is_directory(samples, ec)) {
                fs::create_directory_symlink(samples, home / "phig-live-samples", ec);
            }
        }
    }

    // remove_all removes the models symlink, not what it points at.
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

   private:
    static void set(const char* name, const fs::path& value) {
        fs::create_directories(value);
        ::setenv(name, value.c_str(), 1);
    }

    fs::path root_;
};

const ::testing::Environment* const kScratchHome =
    ::testing::AddGlobalTestEnvironment(new ScratchHome);

}  // namespace

// A test process must never resolve a path inside the developer's real home:
// a test that reaches a default path, directly or through a `phig` child,
// would otherwise write the real catalog's lock, log, trash or thumbnails.
TEST(ScratchHome, KeepsEveryDefaultPathInScratch) {
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_NE(std::string(home).find("phig-scratch-home-"), std::string::npos) << home;
    for (const char* var :
         {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        const char* value = std::getenv(var);
        ASSERT_NE(value, nullptr) << var;
        EXPECT_EQ(std::string(value).rfind(home, 0), 0u) << var << "=" << value;
    }
    EXPECT_EQ(std::getenv("PHIG_CATALOG"), nullptr);
    EXPECT_EQ(data_dir().rfind(home, 0), 0u) << data_dir();
    EXPECT_EQ(default_log_path().rfind(home, 0), 0u) << default_log_path();
}
