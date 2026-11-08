#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

#include "catalog.h"
#include "config.h"
#include "database.h"
#include "paths.h"
#include "phig_binary.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

// One environment variable's state, so a test can restore "unset" as well as a value.
struct SavedEnv {
    std::string name;
    std::optional<std::string> value;
    explicit SavedEnv(const char* n) : name(n) {
        if (const char* v = std::getenv(n)) value = v;
    }
    void restore() const {
        if (value)
            setenv(name.c_str(), value->c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// XDG_DATA_HOME, XDG_CACHE_HOME and XDG_STATE_HOME inside one temp directory,
// restored afterwards, so the default catalog and the legacy locations are
// all scratch.
class LegacyMigrationTest : public ::testing::Test {
   protected:
    fs::path root;
    std::optional<SavedEnv> saved_data, saved_cache, saved_state;

    void SetUp() override {
        saved_data.emplace("XDG_DATA_HOME");
        saved_cache.emplace("XDG_CACHE_HOME");
        saved_state.emplace("XDG_STATE_HOME");
        root = make_temp_dir("legacy-migration");
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
        setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
        setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
        clear_catalog();
    }
    void TearDown() override {
        saved_data->restore();
        saved_cache->restore();
        saved_state->restore();
        clear_catalog();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // The default catalog, with a database so it exists.
    CatalogPaths make_default_catalog() {
        CatalogPaths c = catalog_paths(default_catalog_dir());
        fs::create_directories(c.dir);
        Database db(c.db);
        db.init_schema();
        return c;
    }

    fs::path legacy_thumb() const {
        return root / "cache" / "phig" / "thumbs" / "512" / "ab" / (std::string(64, 'a') + ".jpg");
    }
    fs::path legacy_log() const { return root / "state" / "phig" / "phig.log"; }
    fs::path legacy_rotated_log() const { return root / "state" / "phig" / "phig.1.log"; }
};

}  // namespace

TEST_F(LegacyMigrationTest, MovesThumbnailsAndLogIntoTheDefaultCatalog) {
    CatalogPaths c = make_default_catalog();
    write_file(legacy_thumb(), "thumb");
    write_file(legacy_log(), "log line\n");
    write_file(legacy_rotated_log(), "older line\n");

    LegacyMigration m = migrate_legacy_catalog_files(c);

    EXPECT_EQ(m.moved.size(), 3u);
    EXPECT_TRUE(m.kept_both.empty());
    EXPECT_TRUE(m.failed.empty());
    EXPECT_EQ(read_file(fs::path(c.thumbs) / "512" / "ab" / (std::string(64, 'a') + ".jpg")),
              "thumb");
    EXPECT_EQ(read_file(c.log), "log line\n");
    EXPECT_EQ(read_file(fs::path(c.dir) / "phig.1.log"), "older line\n");
    EXPECT_FALSE(fs::exists(root / "cache" / "phig" / "thumbs"));
    EXPECT_FALSE(fs::exists(legacy_log()));
    EXPECT_TRUE(fs::exists(fs::path(c.thumbs) / "CACHEDIR.TAG"));
}

TEST_F(LegacyMigrationTest, ASecondRunMovesNothing) {
    CatalogPaths c = make_default_catalog();
    write_file(legacy_thumb(), "thumb");
    migrate_legacy_catalog_files(c);
    LegacyMigration again = migrate_legacy_catalog_files(c);
    EXPECT_TRUE(again.moved.empty());
    EXPECT_TRUE(again.kept_both.empty());
    EXPECT_TRUE(again.failed.empty());
}

TEST_F(LegacyMigrationTest, LeavesBothWhenTheCatalogAlreadyHasThem) {
    CatalogPaths c = make_default_catalog();
    write_file(legacy_thumb(), "legacy");
    write_file(fs::path(c.thumbs) / "512" / "cd" / (std::string(64, 'c') + ".jpg"), "current");
    write_file(legacy_log(), "legacy log\n");
    write_file(c.log, "current log\n");

    LegacyMigration m = migrate_legacy_catalog_files(c);

    EXPECT_TRUE(m.moved.empty());
    EXPECT_EQ(m.kept_both.size(), 2u);
    EXPECT_TRUE(fs::exists(legacy_thumb()));
    EXPECT_EQ(read_file(c.log), "current log\n");
    EXPECT_EQ(read_file(legacy_log()), "legacy log\n");
}

TEST_F(LegacyMigrationTest, DoesNothingForACatalogThatDoesNotExist) {
    CatalogPaths c = catalog_paths(default_catalog_dir());  // no phig.db
    write_file(legacy_thumb(), "thumb");
    LegacyMigration m = migrate_legacy_catalog_files(c);
    EXPECT_TRUE(m.moved.empty());
    EXPECT_FALSE(fs::exists(c.dir)) << "a missing catalog must not be created";
    EXPECT_TRUE(fs::exists(legacy_thumb()));
}

TEST_F(LegacyMigrationTest, DoesNothingForANamedCatalog) {
    make_default_catalog();
    CatalogPaths named = catalog_paths((root / "named").string());
    fs::create_directories(named.dir);
    {
        Database db(named.db);
        db.init_schema();
    }
    write_file(legacy_thumb(), "thumb");
    LegacyMigration m = migrate_legacy_catalog_files(named);
    EXPECT_TRUE(m.moved.empty());
    EXPECT_TRUE(fs::exists(legacy_thumb()));
    EXPECT_FALSE(fs::exists(named.thumbs));
}

// Moving across filesystems copies into a temporary sibling, renames it into
// place and removes the source. Needs XDG_CACHE_HOME on another device:
// /dev/shm, when it is a separate filesystem from the temp directory.
TEST_F(LegacyMigrationTest, MovesThumbnailsAcrossFilesystems) {
    struct stat a{}, b{};
    if (!fs::exists("/dev/shm") || ::stat("/dev/shm", &a) != 0 || ::stat(root.c_str(), &b) != 0 ||
        a.st_dev == b.st_dev) {
        GTEST_SKIP() << "needs /dev/shm on a different filesystem from " << root;
    }
    const fs::path shm_cache = make_temp_dir("legacy-cache", "/dev/shm");
    setenv("XDG_CACHE_HOME", shm_cache.c_str(), 1);
    CatalogPaths c = make_default_catalog();
    const fs::path legacy =
        shm_cache / "phig" / "thumbs" / "512" / "ab" / (std::string(64, 'a') + ".jpg");
    write_file(legacy, "thumb");

    LegacyMigration m = migrate_legacy_catalog_files(c);

    EXPECT_EQ(m.moved.size(), 1u);
    EXPECT_TRUE(m.failed.empty());
    EXPECT_EQ(read_file(fs::path(c.thumbs) / "512" / "ab" / (std::string(64, 'a') + ".jpg")),
              "thumb");
    EXPECT_FALSE(fs::exists(shm_cache / "phig" / "thumbs"));
    std::error_code ec;
    fs::remove_all(shm_cache, ec);
}

namespace {

// Sets the version stamp in meta, leaving the tables as they are. Enough to
// make a read see an older or newer catalog without building one.
void stamp_schema_version(const std::string& db_path, int version) {
    Database db(db_path);
    const std::string sql =
        "UPDATE meta SET value = '" + std::to_string(version) + "' WHERE key = 'schema_version'";
    ASSERT_EQ(sqlite3_exec(db.db_ptr(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
}

class OpenCatalogTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    void SetUp() override {
        dir = make_temp_dir("open-catalog");
        db_path = (dir / "phig.db").string();
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void make_current() {
        Database db(db_path);
        db.init_schema();
    }
};

CatalogError::Kind kind_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const CatalogError& e) {
        return e.kind();
    }
    ADD_FAILURE() << "expected a CatalogError";
    return CatalogError::Kind::Missing;
}

}  // namespace

TEST_F(OpenCatalogTest, ReadAndWriteRefuseAMissingCatalogAndCreateNothing) {
    const fs::path missing = dir / "nowhere";
    const std::string missing_db = (missing / "phig.db").string();
    EXPECT_EQ(kind_of([&] { open_catalog_db(missing_db, CatalogAccess::Read); }),
              CatalogError::Kind::Missing);
    EXPECT_EQ(kind_of([&] { open_catalog_db(missing_db, CatalogAccess::Write); }),
              CatalogError::Kind::Missing);
    EXPECT_EQ(kind_of([&] { require_catalog(missing_db); }), CatalogError::Kind::Missing);
    EXPECT_FALSE(fs::exists(missing));
}

TEST_F(OpenCatalogTest, CreateMakesAMissingCatalog) {
    const std::string fresh = (dir / "fresh" / "phig.db").string();
    auto db = open_catalog_db(fresh, CatalogAccess::Create);
    EXPECT_EQ(db->schema_version(), Database::expected_schema_version());
    EXPECT_TRUE(fs::exists(fresh));
}

TEST_F(OpenCatalogTest, ReadRefusesAnOutdatedCatalogAndLeavesItsVersion) {
    make_current();
    const int older = Database::expected_schema_version() - 1;
    stamp_schema_version(db_path, older);
    EXPECT_EQ(kind_of([&] { open_catalog_db(db_path, CatalogAccess::Read); }),
              CatalogError::Kind::Outdated);
    Database after(db_path, /*read_only=*/true);
    EXPECT_EQ(after.schema_version(), older);
}

TEST_F(OpenCatalogTest, EveryAccessRefusesANewerCatalog) {
    make_current();
    stamp_schema_version(db_path, Database::expected_schema_version() + 1);
    for (CatalogAccess a : {CatalogAccess::Read, CatalogAccess::Write, CatalogAccess::Create}) {
        EXPECT_EQ(kind_of([&] { open_catalog_db(db_path, a); }), CatalogError::Kind::Newer);
    }
}

TEST_F(OpenCatalogTest, ReadOpensACurrentCatalogReadOnly) {
    make_current();
    auto db = open_catalog_db(db_path, CatalogAccess::Read);
    EXPECT_EQ(db->schema_version(), Database::expected_schema_version());
    EXPECT_NE(sqlite3_exec(db->db_ptr(), "CREATE TABLE should_fail (x INTEGER)", nullptr, nullptr,
                           nullptr),
              SQLITE_OK);
}

TEST_F(OpenCatalogTest, TheMessageNamesTheCatalogDirectory) {
    const std::string missing_db = (dir / "nowhere" / "phig.db").string();
    try {
        open_catalog_db(missing_db, CatalogAccess::Read);
        FAIL() << "expected a CatalogError";
    } catch (const CatalogError& e) {
        EXPECT_NE(std::string(e.what()).find((dir / "nowhere").string()), std::string::npos)
            << e.what();
    }
}

namespace {

// XDG_DATA_HOME and PHIG_CATALOG are each restored to their prior value, or to
// unset, afterwards.
class ResolveCatalogTest : public ::testing::Test {
   protected:
    std::optional<SavedEnv> saved_data, saved_catalog;
    void SetUp() override {
        saved_data.emplace("XDG_DATA_HOME");
        saved_catalog.emplace("PHIG_CATALOG");
        setenv("XDG_DATA_HOME", "/xdg/data", 1);
        unsetenv("PHIG_CATALOG");
    }
    void TearDown() override {
        saved_data->restore();
        saved_catalog->restore();
    }
};

}  // namespace

TEST_F(ResolveCatalogTest, DefaultIsTheDataDirectory) {
    ResolvedCatalog r = resolve_catalog_dir(std::nullopt, Config{});
    EXPECT_EQ(r.dir, "/xdg/data/phig");
    EXPECT_EQ(r.source, CatalogSource::Default);
}

TEST_F(ResolveCatalogTest, ConfigBeatsTheDefault) {
    Config cfg;
    cfg.catalog = "/from/config";
    EXPECT_EQ(resolve_catalog_dir(std::nullopt, cfg).dir, "/from/config");
    EXPECT_EQ(resolve_catalog_dir(std::nullopt, cfg).source, CatalogSource::Config);
}

TEST_F(ResolveCatalogTest, EnvironmentBeatsConfig) {
    Config cfg;
    cfg.catalog = "/from/config";
    setenv("PHIG_CATALOG", "/from/env", 1);
    ResolvedCatalog r = resolve_catalog_dir(std::nullopt, cfg);
    EXPECT_EQ(r.dir, "/from/env");
    EXPECT_EQ(r.source, CatalogSource::Environment);
}

TEST_F(ResolveCatalogTest, FlagBeatsEverything) {
    Config cfg;
    cfg.catalog = "/from/config";
    setenv("PHIG_CATALOG", "/from/env", 1);
    ResolvedCatalog r = resolve_catalog_dir(std::string("/from/flag"), cfg);
    EXPECT_EQ(r.dir, "/from/flag");
    EXPECT_EQ(r.source, CatalogSource::Flag);
}

TEST_F(ResolveCatalogTest, EmptyEnvironmentCountsAsUnset) {
    setenv("PHIG_CATALOG", "", 1);
    EXPECT_EQ(resolve_catalog_dir(std::nullopt, Config{}).source, CatalogSource::Default);
}

TEST_F(ResolveCatalogTest, RelativeAndTildePathsBecomeAbsolute) {
    const fs::path cwd = fs::current_path();
    EXPECT_EQ(resolve_catalog_dir(std::string("rel/cat"), Config{}).dir,
              (cwd / "rel" / "cat").lexically_normal().string());
    const std::string home = std::getenv("HOME");
    EXPECT_EQ(resolve_catalog_dir(std::string("~/cat"), Config{}).dir, home + "/cat");
}

namespace {

class CatalogCliTest : public ::testing::Test {
   protected:
    fs::path phig_bin;
    fs::path dir;
    void SetUp() override {
        phig_bin = phig_binary_path();
        if (phig_bin.empty()) GTEST_SKIP() << "phig binary not built";
        dir = make_temp_dir("catalog-cli");
    }
    void TearDown() override {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
    PhigRun run(const fs::path& catalog, const std::string& args) {
        return run_phig(phig_bin, dir, "--catalog '" + catalog.string() + "' " + args);
    }
    // A catalog at this build's schema with one row whose file exists.
    fs::path make_catalog(const std::string& name) {
        const fs::path c = dir / name;
        fs::create_directories(c / "photos");
        const fs::path photo = c / "photos" / "a.jpg";
        std::ofstream(photo) << "x";
        Database db((c / "phig.db").string());
        db.init_schema();
        ImageInfo info;
        info.path = photo.string();
        info.filename = "a.jpg";
        info.extension = "jpg";
        info.file_size = 1;
        info.sha256 = std::string(64, 'a');
        info.metadata_json = "{}";
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        db.insert(info);
        return c;
    }
};

}  // namespace

TEST_F(CatalogCliTest, ReadsRefuseAMissingCatalogAndCreateNothing) {
    const fs::path missing = dir / "missing";
    for (const char* args :
         {"search --count", "duplicates", "tag list", "gallery list", "fav list", "face list"}) {
        PhigRun r = run(missing, args);
        EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
        EXPECT_NE(r.output.find("no phig catalog at " + missing.string()), std::string::npos)
            << args << ": " << r.output;
        EXPECT_FALSE(fs::exists(missing)) << args << " created the catalog";
    }
}

TEST_F(CatalogCliTest, ReadsRefuseAnOutdatedCatalogAndLeaveItsVersion) {
    const fs::path c = make_catalog("old");
    const std::string db_path = (c / "phig.db").string();
    const int older = Database::expected_schema_version() - 1;
    stamp_schema_version(db_path, older);
    for (const char* args : {"search --count", "duplicates", "cp '/tmp/never' --dry-run",
                             "rm --match '*' --dry-run", "thumbs clean --dry-run"}) {
        PhigRun r = run(c, args);
        EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
        Database after(db_path, /*read_only=*/true);
        EXPECT_EQ(after.schema_version(), older) << args << " migrated the catalog";
    }
}

TEST_F(CatalogCliTest, WritesRefuseAMissingCatalogAndCreateNothing) {
    const fs::path missing = dir / "missing";
    for (const char* args :
         {"tag add holiday --match '*'", "gallery set trip --match '*'", "fav add --match '*'",
          "rm --match '*' --yes", "cp '/tmp/never'", "trash empty --yes", "thumbs rebuild",
          "thumbs clean", "face rebuild", "face identify Ada"}) {
        PhigRun r = run(missing, args);
        EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
        EXPECT_FALSE(fs::exists(missing)) << args << " created the catalog";
    }
}

TEST_F(CatalogCliTest, ScanCreatesAMissingCatalog) {
    const fs::path photos = dir / "photos";
    fs::create_directories(photos);
    fs::copy_file("tests/data/faces_small.jpg", photos / "f.jpg");
    const fs::path fresh = dir / "fresh";
    PhigRun r = run(fresh, "scan '" + photos.string() + "'");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_TRUE(fs::exists(fresh / "phig.db"));
    PhigRun count = run(fresh, "search --count");
    EXPECT_EQ(count.exit_code, 0) << count.output;
    EXPECT_NE(count.output.find('1'), std::string::npos) << count.output;
}

TEST_F(CatalogCliTest, RmTrashesIntoItsOwnCatalog) {
    const fs::path a = make_catalog("a");
    const fs::path b = make_catalog("b");
    PhigRun r = run(a, "rm --match '*' --yes");
    ASSERT_EQ(r.exit_code, 0) << r.output;
    EXPECT_FALSE(fs::exists(a / "photos" / "a.jpg"));
    ASSERT_TRUE(fs::exists(a / "trash"));
    EXPECT_FALSE(fs::is_empty(a / "trash"));
    EXPECT_FALSE(fs::exists(b / "trash"));
    EXPECT_FALSE(fs::exists(phig_scratch_db_path(dir).parent_path() / "trash"))
        << "trash went to the default catalog";
}

TEST_F(CatalogCliTest, TrashEmptyLeavesAnotherCatalogsTrash) {
    const fs::path a = make_catalog("a");
    const fs::path b = make_catalog("b");
    fs::create_directories(a / "trash");
    std::ofstream(a / "trash" / "kept.jpg") << "x";
    fs::create_directories(b / "trash");
    std::ofstream(b / "trash" / "gone.jpg") << "x";
    PhigRun r = run(b, "trash empty --yes");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_FALSE(fs::exists(b / "trash" / "gone.jpg")) << "the control: B's own trash is emptied";
    EXPECT_TRUE(fs::exists(a / "trash" / "kept.jpg"));
}

TEST_F(CatalogCliTest, ThumbsCleanLeavesAnotherCatalogsThumbnails) {
    const fs::path a = make_catalog("a");
    const fs::path b = make_catalog("b");
    const std::string orphan = std::string(64, 'f') + ".jpg";
    const fs::path a_orphan = a / "thumbs" / "512" / "ff" / orphan;
    const fs::path b_thumb = b / "thumbs" / "512" / "ff" / orphan;
    for (const fs::path& p : {a_orphan, b_thumb}) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }
    PhigRun r = run(a, "thumbs clean");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_FALSE(fs::exists(a_orphan)) << "the control: A's own orphan is removed";
    EXPECT_TRUE(fs::exists(b_thumb));
}
