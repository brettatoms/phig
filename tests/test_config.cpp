#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "config.h"

namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string saved_home;
    bool had_home = false;

    void SetUp() override {
        const char* h = std::getenv("HOME");
        had_home = h != nullptr;
        if (h) saved_home = h;
        setenv("HOME", "/home/tester", 1);

        dir = fs::temp_directory_path() / "phig-test-config";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override {
        if (had_home)
            setenv("HOME", saved_home.c_str(), 1);
        else
            unsetenv("HOME");
        fs::remove_all(dir);
    }

    std::string write_ini(const std::string& content) {
        fs::path p = dir / "config.ini";
        std::ofstream(p) << content;
        return p.string();
    }
};

TEST_F(ConfigTest, AbsentFileReturnsDefaults) {
    Config c = load_config((dir / "nope.ini").string());
    EXPECT_FALSE(c.db.has_value());
    EXPECT_FALSE(c.parallel.has_value());
    EXPECT_FALSE(c.scan_recursive.has_value());
    EXPECT_FALSE(c.faces_threshold.has_value());
}

TEST_F(ConfigTest, ParsesAllRecognizedKeys) {
    std::string p = write_ini(
        "[paths]\n"
        "db = ~/photos/phig.db\n"
        "models_dir = ~/.cache/phig/models\n"
        "thumbs_dir = /var/thumbs\n"
        "[general]\n"
        "parallel = 4\n"
        "[scan]\n"
        "recursive = true\n"
        "on_error = fail\n"
        "media = video\n"
        "thumbs = yes\n"
        "faces = false\n"
        "[organize]\n"
        "format = %Y/%m/%original\n"
        "[duplicates]\n"
        "type = near\n"
        "threshold = 10\n"
        "[faces]\n"
        "threshold = 0.55\n");
    Config c = load_config(p);
    EXPECT_EQ(c.db.value(), "/home/tester/photos/phig.db");
    EXPECT_EQ(c.models_dir.value(), "/home/tester/.cache/phig/models");
    EXPECT_EQ(c.thumbs_dir.value(), "/var/thumbs");
    EXPECT_EQ(c.parallel.value(), 4);
    EXPECT_TRUE(c.scan_recursive.value());
    EXPECT_EQ(c.scan_on_error.value(), "fail");
    EXPECT_EQ(c.scan_media.value(), "video");
    EXPECT_TRUE(c.scan_thumbs.value());
    EXPECT_FALSE(c.scan_faces.value());
    EXPECT_EQ(c.organize_format.value(), "%Y/%m/%original");
    EXPECT_EQ(c.duplicates_type.value(), "near");
    EXPECT_EQ(c.duplicates_threshold.value(), 10);
    EXPECT_DOUBLE_EQ(c.faces_threshold.value(), 0.55);
}

TEST_F(ConfigTest, UnknownKeyIsIgnored) {
    std::string p = write_ini("[general]\nparallel = 2\nbogus = 1\n[nonsense]\nx = y\n");
    Config c = load_config(p);  // must not throw
    EXPECT_EQ(c.parallel.value(), 2);
}

TEST_F(ConfigTest, ExcludedKeyForceThrows) {
    EXPECT_THROW(load_config(write_ini("[scan]\nforce = true\n")), std::runtime_error);
}

TEST_F(ConfigTest, ExcludedKeyOnConflictThrows) {
    EXPECT_THROW(load_config(write_ini("[organize]\non_conflict = overwrite\n")),
                 std::runtime_error);
}

TEST_F(ConfigTest, ExcludedKeyDryRunThrows) {
    EXPECT_THROW(load_config(write_ini("[organize]\ndry_run = true\n")), std::runtime_error);
}

TEST_F(ConfigTest, ExcludedKeyIgnoreMountWarningThrows) {
    EXPECT_THROW(load_config(write_ini("[scan]\nignore_mount_warning = true\n")),
                 std::runtime_error);
}

TEST_F(ConfigTest, InvalidParallelThrows) {
    EXPECT_THROW(load_config(write_ini("[general]\nparallel = abc\n")), std::runtime_error);
}

TEST_F(ConfigTest, ParallelBelowOneThrows) {
    EXPECT_THROW(load_config(write_ini("[general]\nparallel = 0\n")), std::runtime_error);
}

TEST_F(ConfigTest, ThresholdOutOfRangeThrows) {
    EXPECT_THROW(load_config(write_ini("[duplicates]\nthreshold = 100\n")), std::runtime_error);
}

TEST_F(ConfigTest, InvalidBoolThrows) {
    EXPECT_THROW(load_config(write_ini("[scan]\nrecursive = maybe\n")), std::runtime_error);
}

TEST_F(ConfigTest, InvalidEnumThrows) {
    EXPECT_THROW(load_config(write_ini("[scan]\non_error = explode\n")), std::runtime_error);
}

TEST_F(ConfigTest, MalformedIniThrows) {
    EXPECT_THROW(load_config(write_ini("not a valid ini line\n")), std::runtime_error);
}

TEST_F(ConfigTest, HyphenatedKeysNormalized) {
    // Keys may be written with hyphens; treated the same as underscores.
    Config c = load_config(write_ini("[scan]\non-error = warn\n"));
    EXPECT_EQ(c.scan_on_error.value(), "warn");
}

TEST_F(ConfigTest, ParsesLogSection) {
    std::string p = write_ini("[log]\nenabled = false\nmax_entries = 50\n");
    Config cfg = load_config(p);
    ASSERT_TRUE(cfg.log_enabled.has_value());
    EXPECT_FALSE(*cfg.log_enabled);
    ASSERT_TRUE(cfg.log_max_entries.has_value());
    EXPECT_EQ(*cfg.log_max_entries, 50);
}

TEST_F(ConfigTest, LogDefaultsUnset) {
    std::string p = write_ini("[general]\nparallel = 4\n");
    Config cfg = load_config(p);
    EXPECT_FALSE(cfg.log_enabled.has_value());
    EXPECT_FALSE(cfg.log_max_entries.has_value());
}

TEST_F(ConfigTest, RejectsNegativeMaxEntries) {
    std::string p = write_ini("[log]\nmax_entries = -1\n");
    EXPECT_THROW(load_config(p), std::runtime_error);
}

TEST_F(ConfigTest, ParsesLogFileKeys) {
    std::string p = write_ini("[log]\nfile = false\nfile_level = warn\n");
    Config cfg = load_config(p);
    ASSERT_TRUE(cfg.log_file.has_value());
    EXPECT_FALSE(*cfg.log_file);
    ASSERT_TRUE(cfg.log_file_level.has_value());
    EXPECT_EQ(*cfg.log_file_level, "warn");
}

TEST_F(ConfigTest, LogFileKeysDefaultUnset) {
    std::string p = write_ini("[log]\nenabled = true\n");
    Config cfg = load_config(p);
    EXPECT_FALSE(cfg.log_file.has_value());
    EXPECT_FALSE(cfg.log_file_level.has_value());
}

TEST_F(ConfigTest, RejectsInvalidFileLevel) {
    std::string p = write_ini("[log]\nfile_level = debug\n");
    EXPECT_THROW(load_config(p), std::runtime_error);
}
