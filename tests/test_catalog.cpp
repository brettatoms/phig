#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "catalog.h"
#include "database.h"
#include "paths.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

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
    std::string saved_data, saved_cache, saved_state;

    void SetUp() override {
        saved_data = std::getenv("XDG_DATA_HOME");
        saved_cache = std::getenv("XDG_CACHE_HOME");
        saved_state = std::getenv("XDG_STATE_HOME");
        root = make_temp_dir("legacy-migration");
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
        setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
        setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
        clear_catalog();
    }
    void TearDown() override {
        setenv("XDG_DATA_HOME", saved_data.c_str(), 1);
        setenv("XDG_CACHE_HOME", saved_cache.c_str(), 1);
        setenv("XDG_STATE_HOME", saved_state.c_str(), 1);
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
