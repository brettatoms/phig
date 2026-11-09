#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "catalog.h"
#include "database.h"
#include "paths.h"
#include "phig_binary.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

const std::string kKeep = "IRREPLACEABLE";

}  // namespace

// --output on search, duplicates and fav list never destroys what is at the
// path (R69). cli/main.cpp is not linked into phig-tests, so each case drives
// the built binary against a catalog under the test's temp directory.
class CliOutputTest : public ::testing::Test {
   protected:
    fs::path phig_bin;
    fs::path dir;
    fs::path cat;
    std::vector<std::string> row_paths;

    void SetUp() override {
        phig_bin = phig_binary_path();
        if (phig_bin.empty()) {
            GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                            "build it with `cmake --build build` to run this test";
        }
        dir = make_temp_dir("cli-output");
        cat = dir / "cat";
        fs::create_directories(cat / "photos");

        Database db(catalog_paths(cat.string()).db);
        db.init_schema();
        std::vector<int64_t> ids;
        for (const char* name : {"one.jpg", "two.jpg"}) {
            fs::path file = cat / "photos" / name;
            spit(file, "same bytes");
            ImageInfo info;
            info.path = file.string();
            info.filename = name;
            info.extension = "jpg";
            info.file_size = 10;
            info.media_type = "image";
            info.sha256 = "shared-sha";
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            info.metadata_json = "{}";
            db.insert(info);
            ids.push_back(db.get_image_id(info.path));
            ASSERT_GT(ids.back(), 0);
            row_paths.push_back(info.path);
        }
        FaceInfo face{};
        face.embedding.fill(0.5f);
        face.width = 10;
        face.height = 10;
        int64_t face_id = db.insert_face(ids[0], face);
        ASSERT_GT(face_id, 0);
        int64_t person = db.get_or_create_person("Ada");
        ASSERT_GT(person, 0);
        ASSERT_TRUE(db.set_face_person(face_id, person));
    }

    void TearDown() override {
        if (dir.empty()) return;
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    PhigRun run(const std::string& args, const std::optional<fs::path>& stdin_file = std::nullopt) {
        return run_phig(phig_bin, dir, "--catalog '" + cat.string() + "' " + args, std::nullopt,
                        stdin_file);
    }

    const std::vector<std::string>& commands() {
        static const std::vector<std::string> c = {"search", "duplicates", "search --person Ada",
                                                   "fav list"};
        return c;
    }
};

TEST_F(CliOutputTest, OutputRefusesAnOccupiedPath) {
    const fs::path out = dir / "out.txt";
    for (const auto& args : commands()) {
        spit(out, kKeep);
        PhigRun r = run(args + " --output '" + out.string() + "'");
        EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
        EXPECT_NE(r.output.find("already exists"), std::string::npos) << args << ": " << r.output;
        EXPECT_EQ(slurp(out), kKeep) << args;
    }
}

TEST_F(CliOutputTest, OutputOverwriteReplacesTheFile) {
    const fs::path out = dir / "out.txt";
    for (const auto& args : commands()) {
        spit(out, kKeep);
        PhigRun r = run(args + " --output '" + out.string() + "' --overwrite");
        EXPECT_EQ(r.exit_code, 0) << args << ": " << r.output;
        std::string got = slurp(out);
        if (args == "search" || args == "duplicates") {
            EXPECT_NE(got.find(row_paths[0]), std::string::npos) << args << ": " << got;
        } else {
            EXPECT_NE(got, kKeep) << args;
        }
    }
}

TEST_F(CliOutputTest, OutputCreatesAMissingFile) {
    const fs::path out = dir / "new.txt";
    PhigRun r = run("search --output '" + out.string() + "'");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    ASSERT_TRUE(fs::exists(out));
    EXPECT_NE(slurp(out).find(row_paths[0]), std::string::npos);
}

TEST_F(CliOutputTest, OutputOverwriteReplacesASymlinkNotItsTarget) {
    const fs::path victim = dir / "victim.txt";
    const fs::path out = dir / "out.txt";
    spit(victim, kKeep);
    fs::create_symlink(victim, out);
    PhigRun r = run("search --output '" + out.string() + "' --overwrite");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_TRUE(fs::is_regular_file(fs::symlink_status(out)));
    EXPECT_EQ(slurp(victim), kKeep);
}

TEST_F(CliOutputTest, OutputRefusesADanglingSymlink) {
    const fs::path out = dir / "out.txt";
    fs::create_symlink(dir / "nowhere.txt", out);
    PhigRun r = run("search --output '" + out.string() + "'");
    EXPECT_EQ(r.exit_code, 1) << r.output;
    EXPECT_TRUE(fs::is_symlink(fs::symlink_status(out)));
    EXPECT_FALSE(fs::exists(dir / "nowhere.txt"));
}

TEST_F(CliOutputTest, CountWithOutputIsAnError) {
    for (const char* args : {"search --count", "fav list --count"}) {
        const fs::path out = dir / "counted.txt";
        PhigRun r = run(std::string(args) + " --output '" + out.string() + "'");
        EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
        EXPECT_FALSE(fs::exists(out)) << args;
    }
}

TEST_F(CliOutputTest, OverwriteWithoutOutputIsAnError) {
    PhigRun r = run("search --overwrite");
    EXPECT_EQ(r.exit_code, 1) << r.output;
    EXPECT_NE(r.output.find("--overwrite needs --output"), std::string::npos) << r.output;
}

namespace {
// TMPDIR pointed at a scratch directory for the scope, so a test can see what
// the child creates in its temporary directory.
struct ScopedTmpdir {
    std::optional<std::string> saved;
    explicit ScopedTmpdir(const fs::path& p) {
        if (const char* v = std::getenv("TMPDIR")) saved = v;
        fs::create_directories(p);
        setenv("TMPDIR", p.c_str(), 1);
    }
    ~ScopedTmpdir() {
        if (saved)
            setenv("TMPDIR", saved->c_str(), 1);
        else
            unsetenv("TMPDIR");
    }
};
}  // namespace

TEST_F(CliOutputTest, StdinQueryNeverWritesThroughAPlantedPath) {
    const fs::path tmp = dir / "tmp";
    ScopedTmpdir scoped(tmp);
    const fs::path victim = dir / "victim.txt";
    std::ofstream(victim) << "IRREPLACEABLE USER DATA";
    fs::create_symlink(victim, tmp / "phig-stdin-image");

    PhigRun r = run("search --similar -", fs::path("tests/data/faces_small.jpg"));
    EXPECT_EQ(r.exit_code, 0) << r.output;
    std::ifstream in(victim);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "IRREPLACEABLE USER DATA");
    EXPECT_TRUE(fs::is_symlink(tmp / "phig-stdin-image"));
    size_t entries = 0;
    for (const auto& e : fs::directory_iterator(tmp)) {
        (void)e;
        entries++;
    }
    EXPECT_EQ(entries, 1u) << "the staged file was not removed";
}

TEST_F(CliOutputTest, EmptyStdinCreatesNothing) {
    const fs::path tmp = dir / "tmp";
    ScopedTmpdir scoped(tmp);
    PhigRun r = run("search --similar -");  // stdin is /dev/null
    EXPECT_EQ(r.exit_code, 1) << r.output;
    EXPECT_NE(r.output.find("No data received on stdin"), std::string::npos) << r.output;
    EXPECT_TRUE(fs::is_empty(tmp));
}
