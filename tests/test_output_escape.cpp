// CSV and JSON escaping for the CLI's machine-readable output (plan 078).

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "catalog.h"
#include "database.h"
#include "face_path.h"
#include "output_escape.h"
#include "paths.h"
#include "phig_binary.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

// ---- The helpers ----

TEST(CsvField, QuotesEveryFieldAndDoublesEmbeddedQuotes) {
    EXPECT_EQ(csv_field("plain"), "\"plain\"");
    EXPECT_EQ(csv_field(""), "\"\"");
    EXPECT_EQ(csv_field("a,b.jpg"), "\"a,b.jpg\"");
    EXPECT_EQ(csv_field("q\"uote"), "\"q\"\"uote\"");
    EXPECT_EQ(csv_field("two\nlines"), "\"two\nlines\"");
}

TEST(JsonString, EscapesQuotesBackslashesAndControlCharacters) {
    EXPECT_EQ(json_string("plain"), "\"plain\"");
    EXPECT_EQ(json_string("q\"uote"), "\"q\\\"uote\"");
    EXPECT_EQ(json_string("back\\slash"), "\"back\\\\slash\"");
    EXPECT_EQ(json_string("a\nb\tc\rd"), "\"a\\nb\\tc\\rd\"");
    EXPECT_EQ(json_string(std::string("x\x01y\x1fz", 5)), "\"x\\u0001y\\u001fz\"");
}

TEST(JsonString, KeepsValidUtf8) {
    EXPECT_EQ(json_string("caf\xC3\xA9"), "\"caf\xC3\xA9\"");            // é
    EXPECT_EQ(json_string("\xE2\x82\xAC"), "\"\xE2\x82\xAC\"");          // €
    EXPECT_EQ(json_string("\xF0\x9F\x93\xB7"), "\"\xF0\x9F\x93\xB7\"");  // 📷
}

TEST(JsonString, ReplacesInvalidUtf8WithTheReplacementCharacter) {
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_EQ(json_string("a\xFF"
                          "b"),
              "\"a" + fffd + "b\"");                                           // never valid
    EXPECT_EQ(json_string("\x80"), "\"" + fffd + "\"");                        // stray continuation
    EXPECT_EQ(json_string("\xC3"), "\"" + fffd + "\"");                        // truncated
    EXPECT_EQ(json_string("\xC0\xAF"), "\"" + fffd + fffd + "\"");             // overlong '/'
    EXPECT_EQ(json_string("\xED\xA0\x80"), "\"" + fffd + fffd + fffd + "\"");  // surrogate
    EXPECT_EQ(json_string("\xF4\x90\x80\x80"), "\"" + fffd + fffd + fffd + fffd + "\"");
}

// ---- Every writer, through the built binary ----

namespace {

// RFC 4180: fields separated by commas, a quoted field may hold commas, doubled
// quotes and newlines. Throws on a quote that is not closed.
std::vector<std::vector<std::string>> parse_csv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                field += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            row.push_back(field);
            field.clear();
        } else if (c == '\n') {
            row.push_back(field);
            field.clear();
            rows.push_back(row);
            row.clear();
        } else {
            field += c;
        }
    }
    if (quoted) throw std::runtime_error("unterminated quoted field");
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}

// SQLite's JSON functions are the parser: json_valid for the document, and
// json_extract for one value.
class Json {
   public:
    Json() { sqlite3_open(":memory:", &db_); }
    ~Json() { sqlite3_close(db_); }
    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;

    bool valid(const std::string& doc) {
        return query("SELECT json_valid(?1)", doc, "").value_or("0") == "1";
    }
    std::optional<std::string> extract(const std::string& doc, const std::string& path) {
        return query("SELECT json_extract(?1, ?2)", doc, path);
    }

   private:
    std::optional<std::string> query(const char* sql, const std::string& a, const std::string& b) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &s, nullptr);
        sqlite3_bind_text(s, 1, a.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_bind_parameter_count(s) > 1)
            sqlite3_bind_text(s, 2, b.c_str(), -1, SQLITE_TRANSIENT);
        std::optional<std::string> out;
        if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
            out = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
        return out;
    }
    sqlite3* db_ = nullptr;
};

}  // namespace

class OutputEscapeCliTest : public ::testing::Test {
   protected:
    fs::path phig_bin;
    fs::path dir;
    fs::path cat;
    // Names that broke the old writers: a comma for CSV, a quote and a
    // backslash for both.
    const std::vector<std::string> names = {"a,b.jpg", "q\"uote.jpg", "back\\slash.jpg"};
    std::vector<std::string> paths;

    void SetUp() override {
        phig_bin = phig_binary_path();
        if (phig_bin.empty()) {
            GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                            "build it with `cmake --build build` to run this test";
        }
        dir = make_temp_dir("output-escape");
        cat = dir / "cat";
        fs::create_directories(cat / "photos");
        Database db(catalog_paths(cat.string()).db);
        db.init_schema();
        for (const auto& name : names) {
            const fs::path file = cat / "photos" / name;
            std::ofstream(file) << "same bytes";
            ImageInfo info;
            info.path = file.string();
            info.filename = name;
            info.extension = ".jpg";
            info.sha256 = std::string(64, 'a');  // one exact-duplicate group
            info.metadata_json = R"({"DateTimeOriginal":"2019:07:04 10:00:00"})";
            db.insert(info);
            paths.push_back(file.string());
        }
        db.set_tag(paths, "favorite", true);
    }
    void TearDown() override {
        if (!dir.empty()) fs::remove_all(dir);
    }

    std::string run(const std::string& args) {
        PhigRun r = run_phig(phig_bin, dir, "--catalog '" + cat.string() + "' " + args);
        EXPECT_EQ(r.exit_code, 0) << args << "\n" << r.output;
        return r.output;
    }

    // The value of `column` in every data row of a CSV document.
    std::vector<std::string> csv_column(const std::string& text, const std::string& column) {
        auto rows = parse_csv(text);
        EXPECT_FALSE(rows.empty());
        if (rows.empty()) return {};
        const auto& header = rows[0];
        const auto it = std::find(header.begin(), header.end(), column);
        EXPECT_NE(it, header.end()) << column;
        const size_t idx = it - header.begin();
        std::vector<std::string> out;
        for (size_t i = 1; i < rows.size(); ++i) {
            EXPECT_EQ(rows[i].size(), header.size()) << "row " << i << " has a shifted column";
            if (idx < rows[i].size()) out.push_back(rows[i][idx]);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> sorted(std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    }
};

TEST_F(OutputEscapeCliTest, DuplicatesCsvRoundTripsEveryNameAndPath) {
    const std::string out = run("duplicates --type exact --format csv");
    EXPECT_EQ(csv_column(out, "filename"), sorted(names)) << out;
    EXPECT_EQ(csv_column(out, "path"), sorted(paths)) << out;
}

TEST_F(OutputEscapeCliTest, DuplicatesJsonIsValidAndRoundTripsEveryPath) {
    const std::string out = run("duplicates --type exact --format json");
    Json json;
    ASSERT_TRUE(json.valid(out)) << out;
    std::vector<std::string> got;
    for (int i = 0; i < 3; ++i) {
        got.push_back(
            json.extract(out, "$.groups[0].files[" + std::to_string(i) + "].path").value_or(""));
        EXPECT_TRUE(json.extract(out, "$.groups[0].files[" + std::to_string(i) + "].filename"));
    }
    EXPECT_EQ(sorted(got), sorted(paths));
}

TEST_F(OutputEscapeCliTest, SearchCsvRoundTripsEveryNameAndPath) {
    const std::string out = run("search --format csv");
    EXPECT_EQ(csv_column(out, "filename"), sorted(names)) << out;
    EXPECT_EQ(csv_column(out, "path"), sorted(paths)) << out;
}

TEST_F(OutputEscapeCliTest, SearchJsonIsValidAndRoundTripsEveryPath) {
    const std::string out = run("search --format json");
    Json json;
    ASSERT_TRUE(json.valid(out)) << out;
    std::vector<std::string> got;
    for (int i = 0; i < 3; ++i)
        got.push_back(json.extract(out, "$.results[" + std::to_string(i) + "].path").value_or(""));
    EXPECT_EQ(sorted(got), sorted(paths));
}

TEST_F(OutputEscapeCliTest, FavListCsvAndJsonRoundTripEveryPath) {
    EXPECT_EQ(csv_column(run("fav list --format csv"), "path"), sorted(paths));
    const std::string out = run("fav list --format json");
    Json json;
    ASSERT_TRUE(json.valid(out)) << out;
    EXPECT_EQ(json.extract(out, "$.count").value_or(""), "3");
}

// The face search has its own JSON writer. It needs a photo with a detectable
// face, scanned with --faces, so it runs only where the face path does.
TEST_F(OutputEscapeCliTest, FaceSearchJsonIsValidAndRoundTripsThePath) {
    if (std::string why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    // run_phig gives the child its own cache directory; point it at the models.
    fs::create_directories(dir / "xdg" / "cache" / "phig");
    fs::create_directory_symlink(models_dir(), dir / "xdg" / "cache" / "phig" / "models");
    const fs::path faces = dir / "faces";
    fs::create_directories(faces);
    const fs::path photo = faces / "q\"uote,face.jpg";
    fs::copy_file("tests/data/faces_small.jpg", photo);
    run("scan '" + faces.string() + "' --faces");

    // To a file: the search reports progress on the terminal around its results.
    const fs::path json_file = dir / "faces.json";
    run("search --face '" + photo.string() + "' --format json --face-threshold 2 --output '" +
        json_file.string() + "'");
    std::ifstream in(json_file);
    const std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json json;
    ASSERT_TRUE(json.valid(out)) << out;
    EXPECT_EQ(json.extract(out, "$.results[0].path").value_or(""), photo.string()) << out;
}
