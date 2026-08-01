#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "settings.h"

namespace {
// Writes `content` to a unique temp file and removes it on scope exit.
struct TempFile {
    std::string path;
    explicit TempFile(const std::string& content) {
        path = (std::filesystem::temp_directory_path() /
                ("phig_settings_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".ini"))
                   .string();
        std::ofstream(path) << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string read() const {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
};
}  // namespace

TEST(Settings, ReadsSectionDottedKeys) {
    TempFile f("[paths]\ndb = /photos/phig.db\n\n[general]\nparallel = 4\n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("paths.db"), nullptr);
    EXPECT_EQ(*s.get("paths.db"), "/photos/phig.db");
    ASSERT_NE(s.get("general.parallel"), nullptr);
    EXPECT_EQ(*s.get("general.parallel"), "4");
}

TEST(Settings, AbsentKeyReturnsNull) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.get("paths.nope"), nullptr);
    EXPECT_EQ(s.get("nosuch.key"), nullptr);
    EXPECT_FALSE(s.has("paths.nope"));
    EXPECT_TRUE(s.has("paths.db"));
}

TEST(Settings, TrimsWhitespaceAroundKeyAndValue) {
    TempFile f("[paths]\n   db   =    /photos/phig.db   \n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("paths.db"), nullptr);
    EXPECT_EQ(*s.get("paths.db"), "/photos/phig.db");
}

TEST(Settings, AcceptsBothCommentMarkers) {
    TempFile f("# hash comment\n; semicolon comment\n[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.keys().size(), 1u);
    EXPECT_EQ(*s.get("paths.db"), "/x");
}

TEST(Settings, KeyBeforeAnySectionGetsBareName) {
    TempFile f("orphan = 1\n[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("orphan"), nullptr);
    EXPECT_EQ(*s.get("orphan"), "1");
}

TEST(Settings, LastDuplicateKeyWins) {
    TempFile f("[paths]\ndb = /first\ndb = /second\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(*s.get("paths.db"), "/second");
}

TEST(Settings, HandlesCrlfAndMissingTrailingNewline) {
    TempFile f("[paths]\r\ndb = /x\r\n[general]\nparallel = 2");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(*s.get("paths.db"), "/x");
    EXPECT_EQ(*s.get("general.parallel"), "2");
}

TEST(Settings, EmptyFileHasNoKeys) {
    TempFile f("");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.keys().size(), 0u);
}

TEST(Settings, MissingFileThrows) {
    EXPECT_THROW(Settings::load("/nonexistent-dir-xyz/config.ini"), std::runtime_error);
}

TEST(Settings, KeysAreInFileOrder) {
    TempFile f("[paths]\ndb = /x\nthumbs_dir = /t\n[general]\nparallel = 4\n");
    Settings s = Settings::load(f.path);
    std::vector<std::string> expected = {"paths.db", "paths.thumbs_dir", "general.parallel"};
    EXPECT_EQ(s.keys(), expected);
}

TEST(Settings, ValueKeepsInternalSpacesAndEquals) {
    TempFile f("[organize]\nformat = %Y/%m/%original\nodd = a = b\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(*s.get("organize.format"), "%Y/%m/%original");
    EXPECT_EQ(*s.get("organize.odd"), "a = b");  // split on the FIRST '='
}
