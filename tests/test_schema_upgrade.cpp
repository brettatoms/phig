#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "catalog.h"
#include "database.h"
#include "phig_binary.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// Every row a query returns, one string per row with columns joined by '|'
// and NULL spelled out, so two snapshots compare with one EXPECT_EQ.
std::vector<std::string> dump(sqlite3* db, const std::string& sql) {
    std::vector<std::string> rows;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << sql << ": " << sqlite3_errmsg(db);
        return rows;
    }
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string row;
        for (int c = 0; c < sqlite3_column_count(s); c++) {
            if (c > 0) row += '|';
            const auto* text = sqlite3_column_text(s, c);
            row += text ? reinterpret_cast<const char*>(text) : "NULL";
        }
        rows.push_back(row);
    }
    sqlite3_finalize(s);
    return rows;
}

// The tables an upgrade from v7 must carry across, in columns a v7 database
// and a current one share. Orphans included: nothing here filters on a parent.
struct Snapshot {
    std::vector<std::string> images, faces, vec_faces, image_tags, tags, people, operations,
        operation_entries;
    bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot(sqlite3* db) {
    Snapshot s;
    s.images = dump(db,
                    "SELECT id, path, sha256, file_size, gallery, metadata, effective_date "
                    "FROM images ORDER BY id");
    s.faces = dump(db,
                   "SELECT id, image_id, x, y, width, height, person_id, hex(embedding) "
                   "FROM faces ORDER BY id");
    s.vec_faces = dump(db, "SELECT face_id, hex(embedding) FROM vec_faces ORDER BY face_id");
    s.image_tags = dump(db, "SELECT image_id, tag_id FROM image_tags ORDER BY image_id, tag_id");
    s.tags = dump(db, "SELECT id, name FROM tags ORDER BY id");
    s.people = dump(db, "SELECT id, name FROM people ORDER BY id");
    s.operations = dump(db, "SELECT id, command, status FROM operations ORDER BY id");
    s.operation_entries =
        dump(db, "SELECT id, op_id, kind, src, dst FROM operation_entries ORDER BY id");
    return s;
}

void exec_or_fail(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        ADD_FAILURE() << (err ? err : "?") << "\n" << sql;
    }
    sqlite3_free(err);
}

ImageInfo image_at(const std::string& path, const std::string& sha) {
    ImageInfo info;
    info.path = path;
    info.filename = fs::path(path).filename().string();
    info.extension = "jpg";
    info.file_size = 1000;
    info.sha256 = sha;
    info.metadata_json = R"({"Make":"Canon"})";
    info.modified_at = "2024-01-01T00:00:00Z";
    return info;
}

FaceInfo face_of(float v) {
    FaceInfo f;
    f.embedding.fill(v);
    f.x = f.y = f.width = f.height = 4;
    return f;
}

// Removes rows the way a delete made before foreign keys were enforced did:
// with the pragma off, so the parent goes and its children stay.
void orphan(sqlite3* db, const std::string& delete_sql) {
    exec_or_fail(db, "PRAGMA foreign_keys=OFF");
    exec_or_fail(db, delete_sql.c_str());
    exec_or_fail(db, "PRAGMA foreign_keys=ON");
}

// Turns a current database into a genuine v7 one: images and faces back to a
// plain INTEGER PRIMARY KEY, faces without the v10 provenance columns, version
// stamped 7. Rows are copied with their ids, so whatever the caller wrote,
// orphans included, is what the v7 database holds.
void rewind_to_v7(sqlite3* db) {
    exec_or_fail(db, R"(
        PRAGMA foreign_keys=OFF;
        BEGIN;
        CREATE TABLE images_old (
            id          INTEGER PRIMARY KEY,
            path        TEXT NOT NULL UNIQUE,
            filename    TEXT NOT NULL,
            extension   TEXT NOT NULL,
            file_size   INTEGER NOT NULL,
            created_at  TEXT,
            modified_at TEXT,
            sha256      TEXT NOT NULL,
            phash       INTEGER NOT NULL,
            width       INTEGER,
            height      INTEGER,
            metadata    TEXT,
            media_type  TEXT NOT NULL DEFAULT 'image',
            duration_ms INTEGER,
            gallery     TEXT,
            blur_score  REAL,
            effective_date TEXT
        );
        INSERT INTO images_old SELECT id, path, filename, extension, file_size, created_at,
            modified_at, sha256, phash, width, height, metadata, media_type, duration_ms,
            gallery, blur_score, effective_date FROM images;
        DROP TABLE images;
        ALTER TABLE images_old RENAME TO images;
        CREATE INDEX idx_phash ON images(phash);
        CREATE INDEX idx_sha256 ON images(sha256);
        CREATE INDEX idx_path ON images(path);
        CREATE INDEX idx_gallery ON images(gallery);
        CREATE INDEX idx_effective_date ON images(effective_date DESC, id DESC);
        CREATE TABLE faces_old (
            id          INTEGER PRIMARY KEY,
            image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
            embedding   BLOB NOT NULL,
            x INTEGER, y INTEGER, width INTEGER, height INTEGER,
            person_id   INTEGER REFERENCES people(id)
        );
        INSERT INTO faces_old SELECT id, image_id, embedding, x, y, width, height, person_id
            FROM faces;
        DROP TABLE faces;
        ALTER TABLE faces_old RENAME TO faces;
        CREATE INDEX idx_faces_image ON faces(image_id);
        UPDATE meta SET value = '7' WHERE key = 'schema_version';
        COMMIT;
        PRAGMA foreign_keys=ON;
    )");
}

// A v7 catalog holding the rows plan 066 found in a real one: faces, a tag and
// an activity-log entry whose parents are gone. Two images survive, with a tag,
// a favorite, a gallery, a labelled face and an unlabelled one.
void make_v7_with_orphans(const std::string& db_path) {
    Database db(db_path);
    db.init_schema();
    db.insert(image_at("/p/keep.jpg", "sha_keep"));
    db.insert(image_at("/p/fav.jpg", "sha_fav"));
    db.insert(image_at("/p/gone.jpg", "sha_gone"));
    const int64_t keep = db.get_image_id("/p/keep.jpg");
    const int64_t fav = db.get_image_id("/p/fav.jpg");
    const int64_t gone = db.get_image_id("/p/gone.jpg");
    db.set_tag({"/p/keep.jpg", "/p/gone.jpg"}, "holiday", true);
    db.set_tag({"/p/fav.jpg"}, "favorite", true);
    ASSERT_EQ(db.set_gallery_by_ids({keep}, "trip"), 1);

    const int64_t alice = db.get_or_create_person("Alice");
    ASSERT_TRUE(db.set_face_person(db.insert_face(keep, face_of(0.25f)), alice));
    db.insert_face(fav, face_of(0.5f));
    // The orphans-to-be: one face labelled, one not, each with its vector.
    ASSERT_TRUE(db.set_face_person(db.insert_face(gone, face_of(0.75f)), alice));
    db.insert_face(gone, face_of(0.875f));

    const int64_t op = db.log_begin_operation("mv", "phig mv /dest", "2024-01-01T00:00:00Z");
    Database::OperationEntry e;
    e.kind = "move";
    e.src = "/p/gone.jpg";
    e.dst = "/dest/gone.jpg";
    db.begin_transaction();
    db.log_add_entry(op, e);
    db.commit_transaction();
    db.log_finish_operation(op, "completed");

    orphan(db.db_ptr(), "DELETE FROM images WHERE id = " + std::to_string(gone));
    orphan(db.db_ptr(), "DELETE FROM operations WHERE id = " + std::to_string(op));
    rewind_to_v7(db.db_ptr());
}

std::vector<fs::path> backups_in(const fs::path& dir) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().find("-backup-") != std::string::npos) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

class SchemaUpgradeTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    void SetUp() override {
        dir = make_temp_dir("schema-upgrade");
        db_path = (dir / "phig.db").string();
    }
    void TearDown() override {
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(dir, ec);
    }
};

}  // namespace

// Plan 066 found 36 faces rows with no images row in a real catalog. Every
// upgrade step checked the whole database for violations, so those rows, which
// no step created, failed the v8 step and every catalog holding one was stuck
// at v7.
TEST_F(SchemaUpgradeTest, AV7CatalogWithOrphanRowsUpgradesAndKeepsEveryRow) {
    make_v7_with_orphans(db_path);
    Snapshot before;
    size_t orphans_before = 0;
    {
        Database v7(db_path, /*read_only=*/true);
        ASSERT_EQ(v7.schema_version(), 7);
        before = snapshot(v7.db_ptr());
        orphans_before = v7.foreign_key_violations().size();
    }
    ASSERT_EQ(orphans_before, 4u) << "two faces, one image_tags row, one operation_entries row";

    auto db = open_catalog_db(db_path, CatalogAccess::Write);
    EXPECT_EQ(db->schema_version(), Database::expected_schema_version());
    EXPECT_EQ(snapshot(db->db_ptr()), before) << "a row or a value changed in the upgrade";
    EXPECT_EQ(db->foreign_key_violations().size(), orphans_before)
        << "the orphans are carried through, neither repaired nor multiplied";

    EXPECT_EQ(db->get_tags_for_image("/p/keep.jpg"), std::vector<std::string>{"holiday"});
    EXPECT_EQ(db->get_tags_for_image("/p/fav.jpg"), std::vector<std::string>{"favorite"});
    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].name, "Alice");
    std::array<float, 128> probe;
    probe.fill(0.25f);
    EXPECT_EQ(db->find_closest_face(probe, 0.1f), 1) << "the labelled face's vector is not found";
}

// The same comparison that lets existing orphans through must still stop a
// step that strands a row of its own, with those orphans present beside it.
TEST_F(SchemaUpgradeTest, AStepThatAddsAViolationFailsAndRollsBackBesideExistingOrphans) {
    make_v7_with_orphans(db_path);
    auto db = open_catalog_db(db_path, CatalogAccess::Write);
    const int current = Database::expected_schema_version();
    const auto orphans = db->foreign_key_violations();
    const Snapshot before = snapshot(db->db_ptr());

    try {
        db->apply_migration_step(current + 1, "DELETE FROM images WHERE path = '/p/keep.jpg'");
        FAIL() << "a step that stranded keep.jpg's tag and face committed";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("v" + std::to_string(current + 1)), std::string::npos) << msg;
        EXPECT_NE(msg.find("2 new foreign key violations"), std::string::npos) << msg;
        EXPECT_NE(msg.find("faces"), std::string::npos) << msg;
        EXPECT_NE(msg.find("image_tags"), std::string::npos) << msg;
    }
    EXPECT_EQ(db->schema_version(), current);
    EXPECT_EQ(snapshot(db->db_ptr()), before) << "the failed step was not rolled back";
    EXPECT_EQ(db->foreign_key_violations(), orphans);
}

TEST_F(SchemaUpgradeTest, TheBackupIsWrittenBeforeTheFirstStepAndHoldsTheOldDatabase) {
    make_v7_with_orphans(db_path);
    Snapshot before;
    {
        Database v7(db_path, /*read_only=*/true);
        before = snapshot(v7.db_ptr());
    }

    auto db = open_catalog_db(db_path, CatalogAccess::Write);
    const auto backups = backups_in(dir);
    ASSERT_EQ(backups.size(), 1u);
    EXPECT_EQ(db->upgrade_backup_path(), backups[0].string());
    const std::string name = backups[0].filename().string();
    // phig.db.v7-backup-YYYYMMDDTHHMMSSZ
    const std::string prefix = "phig.db.v7-backup-";
    ASSERT_EQ(name.rfind(prefix, 0), 0u) << name;
    const std::string stamp = name.substr(prefix.size());
    ASSERT_EQ(stamp.size(), 16u) << name;
    EXPECT_EQ(stamp[8], 'T') << name;
    EXPECT_EQ(stamp[15], 'Z') << name;

    Database backup(backups[0].string(), /*read_only=*/true);
    EXPECT_EQ(backup.schema_version(), 7) << "the copy was taken after a step ran";
    EXPECT_EQ(snapshot(backup.db_ptr()), before) << "the copy differs from the v7 database";
    EXPECT_FALSE(before.vec_faces.empty());
}

TEST_F(SchemaUpgradeTest, NoBackupIsWrittenWhenNoStepIsPending) {
    {
        auto created = open_catalog_db(db_path, CatalogAccess::Create);
    }
    auto db = open_catalog_db(db_path, CatalogAccess::Write);
    EXPECT_TRUE(db->upgrade_backup_path().empty());
    EXPECT_TRUE(backups_in(dir).empty());
}

// The name is claimed, never taken over: a file already there keeps its
// bytes and the backup goes beside it.
TEST_F(SchemaUpgradeTest, AnExistingFileAtTheBackupsNameIsLeftAlone) {
    make_v7_with_orphans(db_path);
    // The name carries the second the copy is taken, so occupy this one and the
    // next few.
    std::vector<fs::path> occupied;
    const std::time_t now = std::time(nullptr);
    for (int i = 0; i < 5; i++) {
        const std::time_t t = now + i;
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
        occupied.push_back(dir / ("phig.db.v7-backup-" + std::string(buf)));
        std::ofstream(occupied.back()) << "not phig's";
    }

    auto db = open_catalog_db(db_path, CatalogAccess::Write);
    EXPECT_EQ(db->schema_version(), Database::expected_schema_version());
    for (const auto& p : occupied) {
        std::ifstream in(p);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "not phig's") << p << " was overwritten";
    }
    const fs::path written = db->upgrade_backup_path();
    EXPECT_TRUE(std::find(occupied.begin(), occupied.end(), written) == occupied.end()) << written;
    Database backup(written.string(), /*read_only=*/true);
    EXPECT_EQ(backup.schema_version(), 7);
}

TEST_F(SchemaUpgradeTest, WhenTheBackupCannotBeWrittenNoStepRuns) {
    if (::geteuid() == 0) GTEST_SKIP() << "root writes into a read-only directory";
    make_v7_with_orphans(db_path);
    Snapshot before;
    {
        Database v7(db_path, /*read_only=*/true);
        before = snapshot(v7.db_ptr());
    }

    // Keeps the -wal and -shm files open, so the upgrading connection can open
    // the database in a directory where it cannot create a file.
    Database holder(db_path);
    fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    try {
        open_catalog_db(db_path, CatalogAccess::Write);
        FAIL() << "the upgrade ran without a backup";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find((dir / "phig.db.v7-backup-").string()), std::string::npos) << msg;
        EXPECT_NE(msg.find("ermission denied"), std::string::npos) << msg;
    }
    fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add);

    EXPECT_EQ(holder.schema_version(), 7) << "a step ran before the backup existed";
    EXPECT_EQ(snapshot(holder.db_ptr()), before);
    EXPECT_TRUE(backups_in(dir).empty());
}

namespace {

class SchemaUpgradeCliTest : public SchemaUpgradeTest {
   protected:
    fs::path phig_bin;
    void SetUp() override {
        phig_bin = phig_binary_path();
        if (phig_bin.empty()) GTEST_SKIP() << "phig binary not built";
        SchemaUpgradeTest::SetUp();
    }
};

}  // namespace

TEST_F(SchemaUpgradeCliTest, ScanUpgradesAV7CatalogWithOrphansAndNamesTheBackup) {
    const fs::path catalog = dir / "catalog";
    fs::create_directories(catalog);
    make_v7_with_orphans((catalog / "phig.db").string());
    const fs::path photos = dir / "photos";
    fs::create_directories(photos);
    fs::copy_file("tests/data/faces_small.jpg", photos / "f.jpg");

    PhigRun r = run_phig(phig_bin, dir,
                         "--catalog '" + catalog.string() + "' scan '" + photos.string() + "'");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    const auto backups = backups_in(catalog);
    ASSERT_EQ(backups.size(), 1u) << r.output;
    EXPECT_NE(r.output.find("[warning]"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find(backups[0].string()), std::string::npos) << r.output;

    Database db((catalog / "phig.db").string(), /*read_only=*/true);
    EXPECT_EQ(db.schema_version(), Database::expected_schema_version());
    EXPECT_GE(db.get_image_id((photos / "f.jpg").string()), 1) << "the scan did not index";
}
