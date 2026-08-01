#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "config.h"
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

// A 0-byte file must not synthesize a phantom blank line (regression test:
// the guard in load()'s line-splitting loop previously suppressed its own
// empty-content check, producing one Line with text == "" for a 0-byte file).
// keys() can't distinguish "" from "\n" through the public API alone since
// both are Other lines with zero keys, so this only proves keys() stays
// empty; see the Task 1 report for why a stronger round-trip test waits on
// Task 2's save().
TEST(Settings, EmptyFileProducesNoLines) {
    TempFile f("");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.keys().size(), 0u);
    EXPECT_FALSE(s.has(""));
}

TEST(Settings, DirectoryPathThrows) {
    EXPECT_THROW(Settings::load(std::filesystem::temp_directory_path().string()),
                 std::runtime_error);
}

TEST(SettingsWriter, RewritesOneKeyAndLeavesEverythingElseByteIdentical) {
    const std::string original =
        "# my photo collection\n"
        "[paths]\n"
        "db = ~/photos/phig.db\n"
        "# thumbs live on the SSD\n"
        "thumbs_dir = /ssd/thumbs\n"
        "\n"
        "[general]\n"
        "parallel = 4\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.set("general.parallel", "8");
    s.save(f.path);

    const std::string expected =
        "# my photo collection\n"
        "[paths]\n"
        "db = ~/photos/phig.db\n"
        "# thumbs live on the SSD\n"
        "thumbs_dir = /ssd/thumbs\n"
        "\n"
        "[general]\n"
        "parallel = 8\n";
    EXPECT_EQ(f.read(), expected);
}

TEST(SettingsWriter, PreservesUnknownKeys) {
    const std::string original = "[paths]\ndb = /x\n[weird]\nsomething_phig_does_not_know = yes\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/y");
    s.save(f.path);
    EXPECT_NE(f.read().find("something_phig_does_not_know = yes"), std::string::npos);
}

TEST(SettingsWriter, PreservesTrailingCommentOnARewrittenLine) {
    // '#' is never an inline comment marker per inih (only whole-line); ';'
    // is, but only when preceded by whitespace. Use ';' here to exercise a
    // genuine inline comment.
    TempFile f("[general]\nparallel = 4  ; tuned for this box\n");
    Settings s = Settings::load(f.path);
    s.set("general.parallel", "8");
    EXPECT_EQ(f.read().empty(), false);
    s.save(f.path);
    std::string out = f.read();
    EXPECT_NE(out.find("; tuned for this box"), std::string::npos);
    EXPECT_NE(out.find("parallel = 8"), std::string::npos);
}

TEST(SettingsWriter, AppendsNewKeyToExistingSection) {
    TempFile f("[paths]\ndb = /x\n\n[general]\nparallel = 4\n");
    Settings s = Settings::load(f.path);
    s.set("paths.models_dir", "/models");
    s.save(f.path);
    const std::string expected =
        "[paths]\ndb = /x\nmodels_dir = /models\n\n[general]\nparallel = 4\n";
    EXPECT_EQ(f.read(), expected);
}

TEST(SettingsWriter, CreatesMissingSectionAtEndOfFile) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    s.set("faces.threshold", "0.6");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\ndb = /x\n\n[faces]\nthreshold = 0.6\n");
}

TEST(SettingsWriter, UnsetRemovesOnlyThatLine) {
    TempFile f("[paths]\n# keep me\ndb = /x\nthumbs_dir = /t\n");
    Settings s = Settings::load(f.path);
    s.unset("paths.db");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\n# keep me\nthumbs_dir = /t\n");
    EXPECT_FALSE(s.has("paths.db"));
}

TEST(SettingsWriter, UnsetOfAbsentKeyIsANoOp) {
    const std::string original = "[paths]\ndb = /x\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.unset("paths.nope");
    s.save(f.path);
    EXPECT_EQ(f.read(), original);
}

TEST(SettingsWriter, RoundTripsWithoutChangesByteIdentical) {
    const std::string original =
        "; leading semicolon comment\n"
        "\n"
        "[paths]\n"
        "   db   =   ~/photos/phig.db   \n"
        "\n"
        "# trailing comment\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.save(f.path);
    EXPECT_EQ(f.read(), original);
}

TEST(SettingsWriter, SavingLeavesNoTempSibling) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/y");
    s.save(f.path);
    auto dir = std::filesystem::path(f.path).parent_path();
    auto stem = std::filesystem::path(f.path).filename().string();
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string name = e.path().filename().string();
        if (name != stem && name.find(stem) != std::string::npos) {
            ADD_FAILURE() << "leftover temp sibling: " << name;
        }
    }
}

TEST(SettingsWriter, SetOnEmptyFileCreatesSection) {
    TempFile f("");
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/x");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\ndb = /x\n");
}

TEST(SettingsWriter, GetReflectsSetBeforeSave) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/y");
    ASSERT_NE(s.get("paths.db"), nullptr);
    EXPECT_EQ(*s.get("paths.db"), "/y");
    s.set("general.parallel", "2");
    ASSERT_NE(s.get("general.parallel"), nullptr);
    EXPECT_EQ(*s.get("general.parallel"), "2");
}

// A bare `=` line inside a section registers under "<section>." (empty key
// name; Task 1 behavior). set() on a real key must never match that line via
// a coincidental key collision.
TEST(SettingsWriter, SetNeverRewritesABareEqualsLine) {
    TempFile f("[paths]\n=\ndb = /x\n");
    Settings s = Settings::load(f.path);
    ASSERT_TRUE(s.has("paths."));
    s.set("paths.db", "/y");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\n=\ndb = /y\n");
}

// Round-trip: set(key, get(key)) must be a no-op across repeated saves. This
// is the shape a GUI's read-modify-write actually takes; using a fresh
// literal (as every test above does) can't catch a comment getting
// re-appended on every save.
TEST(SettingsWriter, SetOfGetResultDoesNotDoubleTheComment) {
    TempFile f("[general]\nparallel = 4  ; tuned\n");
    Settings s1 = Settings::load(f.path);
    s1.set("general.parallel", *s1.get("general.parallel"));
    s1.save(f.path);
    std::string after_first = f.read();

    Settings s2 = Settings::load(f.path);
    s2.set("general.parallel", *s2.get("general.parallel"));
    s2.save(f.path);
    std::string after_second = f.read();

    EXPECT_EQ(after_first, after_second);
    EXPECT_EQ(after_second, "[general]\nparallel = 4  ; tuned\n");
}

// '#' never starts an inline comment per inih (INI_INLINE_COMMENT_PREFIXES is
// ';' only); it's part of the value. set() must not weld the old value's
// tail onto the new one.
TEST(SettingsWriter, SetOnValueContainingHashDoesNotWeldOldTail) {
    TempFile f("[s]\nformat = %Y#%m\n");
    Settings s = Settings::load(f.path);
    ASSERT_EQ(*s.get("s.format"), "%Y#%m");
    s.set("s.format", "%d-%m");
    s.save(f.path);
    Settings reloaded = Settings::load(f.path);
    EXPECT_EQ(*reloaded.get("s.format"), "%d-%m");
}

// A ';' not preceded by whitespace is part of the value, not a comment
// marker (inih requires a preceding whitespace character).
TEST(SettingsWriter, SemicolonWithoutPrecedingSpaceIsPartOfValue) {
    TempFile f("[s]\nlist = a;b;c\n");
    Settings s = Settings::load(f.path);
    ASSERT_EQ(*s.get("s.list"), "a;b;c");
    s.set("s.list", "x;y");
    s.save(f.path);
    Settings reloaded = Settings::load(f.path);
    EXPECT_EQ(*reloaded.get("s.list"), "x;y");
}

// A genuine inline comment (';' preceded by whitespace) survives a set()
// exactly once, and the value itself is exactly what was set.
TEST(SettingsWriter, GenuineInlineCommentSurvivesExactlyOnce) {
    TempFile f("[s]\nk = v ; note\n");
    Settings s = Settings::load(f.path);
    ASSERT_EQ(*s.get("s.k"), "v");
    s.set("s.k", "w");
    s.save(f.path);
    std::string out = f.read();
    EXPECT_EQ(out, "[s]\nk = w ; note\n");

    Settings reloaded = Settings::load(f.path);
    EXPECT_EQ(*reloaded.get("s.k"), "w");
}

// libphig's Settings and phig's own inih-based config loader must agree on
// where a value ends and an inline comment begins, since both read the same
// file for different purposes.
TEST(SettingsWriter, AgreesWithInihOnInlineCommentSplit) {
    TempFile f("[organize]\nformat = %Y#%m ; keep this comment\n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("organize.format"), nullptr);

    Config c = load_config(f.path);
    ASSERT_TRUE(c.organize_format.has_value());
    EXPECT_EQ(*s.get("organize.format"), *c.organize_format);
}

// set(key, get(key)) on a line with an empty value and an inline comment
// must not widen the gap between '=' and the comment on every save. A
// single round isn't enough to catch this — the defect (if present)
// compounds, so this iterates and requires stability from the second save
// onward.
TEST(SettingsWriter, SetOfGetResultOnEmptyValueWithCommentDoesNotCompound) {
    TempFile f("[general]\nparallel =  ; not yet tuned\n");
    std::string previous;
    for (int i = 0; i < 4; ++i) {
        Settings s = Settings::load(f.path);
        s.set("general.parallel", *s.get("general.parallel"));
        s.save(f.path);
        std::string out = f.read();
        if (i > 0) EXPECT_EQ(out, previous) << "diverged on save " << (i + 1);
        previous = out;
    }
}

// Same defect, reachable when the value is empty and the line is nothing
// but a bare comment marker.
TEST(SettingsWriter, SetOfGetResultOnBareMarkerCommentDoesNotCompound) {
    TempFile f("[s]\nk = ;\n");
    std::string previous;
    for (int i = 0; i < 4; ++i) {
        Settings s = Settings::load(f.path);
        s.set("s.k", *s.get("s.k"));
        s.save(f.path);
        std::string out = f.read();
        if (i > 0) EXPECT_EQ(out, previous) << "diverged on save " << (i + 1);
        previous = out;
    }
}
