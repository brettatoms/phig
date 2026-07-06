#include "database.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#ifndef SQLITE_VEC_STATIC
#define SQLITE_VEC_STATIC
#endif
#ifndef SQLITE_CORE
#define SQLITE_CORE
#endif
#include "sqlite-vec.h"

namespace fs = std::filesystem;

// Schema version this code expects. Bump when adding migrations.
static const int CURRENT_SCHEMA_VERSION = 5;

// migrations[N] upgrades from version N-1 to version N.
// Version 1 = initial schema, created directly by init_schema().
// Version 2 = video support (media_type + duration_ms columns).
// Version 3 = tags/image_tags + gallery column + seeded favorite tag.
static const std::vector<std::string> migrations = {
    "",  // placeholder for index 0 (no version 0)
    "",  // v1: initial schema, created fresh by init_schema()
    // v2: rename exif -> metadata, add video support columns
    R"(ALTER TABLE images RENAME COLUMN exif TO metadata;
       ALTER TABLE images ADD COLUMN media_type TEXT NOT NULL DEFAULT 'image';
       ALTER TABLE images ADD COLUMN duration_ms INTEGER;)",
    // v3: tags & galleries & favorites
    R"(CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
       CREATE TABLE image_tags (
         image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
         tag_id   INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
         PRIMARY KEY (image_id, tag_id));
       INSERT OR IGNORE INTO tags(name) VALUES ('favorite');
       ALTER TABLE images ADD COLUMN gallery TEXT;
       CREATE INDEX idx_gallery ON images(gallery);)",
    // v4: blur/sharpness score
    "ALTER TABLE images ADD COLUMN blur_score REAL;",
    // v5: activity log (operations + per-file entries) for audit/rollback
    R"(CREATE TABLE operations (
         id          INTEGER PRIMARY KEY,
         ts          TEXT NOT NULL,
         command     TEXT NOT NULL,
         argv        TEXT,
         status      TEXT NOT NULL,
         undoes_id   INTEGER REFERENCES operations(id),
         entry_count INTEGER NOT NULL DEFAULT 0);
       CREATE TABLE operation_entries (
         id           INTEGER PRIMARY KEY,
         op_id        INTEGER NOT NULL REFERENCES operations(id) ON DELETE CASCADE,
         kind         TEXT NOT NULL,
         src          TEXT,
         dst          TEXT,
         restore_json TEXT);
       CREATE INDEX idx_op_entries ON operation_entries(op_id);)",
};

namespace {
// Step a prepared DML statement to completion, finalize it, and throw if the
// step did not report SQLITE_DONE — so a failed write (e.g. SQLITE_BUSY, a
// constraint violation) surfaces as an error instead of being silently ignored
// and reported as success. `stmt` is finalized in all paths.
void step_write_or_throw(sqlite3* db, sqlite3_stmt* stmt, const char* what) {
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(db));
    }
}
}  // namespace

Database::Database(const std::string& path) {
    // Ensure parent directory exists
    fs::path db_path(path);
    if (db_path.has_parent_path()) {
        fs::create_directories(db_path.parent_path());
    }

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("Failed to open database: " + err);
    }

    // Enable WAL mode for better concurrent read/write performance
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    // Wait (rather than fail instantly with SQLITE_BUSY) when another phig
    // process holds the single WAL writer lock.
    sqlite3_busy_timeout(db_, 30000);
    // Enforce declared FK cascades (faces/image_tags ON DELETE CASCADE). SQLite
    // defaults foreign_keys OFF per-connection, so this must be set every open.
    exec("PRAGMA foreign_keys=ON");

    // Initialize sqlite-vec extension
    sqlite3_vec_init(db_, nullptr, nullptr);
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

int Database::get_schema_version() {
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key = 'schema_version'", -1,
                                &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;  // meta table doesn't exist = fresh DB
    }
    rc = sqlite3_step(stmt);
    int version = 0;
    if (rc == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

void Database::set_schema_version(int version) {
    exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)");
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(
        db_, "INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)", -1, &stmt,
        nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to set schema version: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int(stmt, 1, version);
    step_write_or_throw(db_, stmt, "Failed to write schema version");
}

void Database::init_schema() {
    int db_version = get_schema_version();

    if (db_version > CURRENT_SCHEMA_VERSION) {
        throw std::runtime_error("Database schema (v" + std::to_string(db_version) +
                                 ") is newer than this version of phig (v" +
                                 std::to_string(CURRENT_SCHEMA_VERSION) + "). Please update phig.");
    }

    if (db_version == 0) {
        // Check if this is a pre-migration DB (has images table but no meta table)
        bool has_images_table = false;
        {
            sqlite3_stmt* stmt;
            int rc = sqlite3_prepare_v2(
                db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='images'", -1, &stmt,
                nullptr);
            if (rc == SQLITE_OK) {
                has_images_table = (sqlite3_step(stmt) == SQLITE_ROW);
                sqlite3_finalize(stmt);
            }
        }

        if (has_images_table) {
            // Pre-migration DB — set version to 1, then run migrations forward.
            // Re-establish the face-recognition schema that the old init_schema
            // used to guarantee on every startup, in case the existing DB
            // predates it (idempotent — safe if already present).
            exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)");
            exec(R"(
                CREATE TABLE IF NOT EXISTS faces (
                    id          INTEGER PRIMARY KEY,
                    image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                    embedding   BLOB NOT NULL,
                    x           INTEGER,
                    y           INTEGER,
                    width       INTEGER,
                    height      INTEGER
                )
            )");
            exec("CREATE INDEX IF NOT EXISTS idx_faces_image ON faces(image_id)");
            exec(R"(
                CREATE TABLE IF NOT EXISTS people (
                    id   INTEGER PRIMARY KEY,
                    name TEXT NOT NULL UNIQUE
                )
            )");
            exec(R"(
                CREATE VIRTUAL TABLE IF NOT EXISTS vec_faces USING vec0(
                    face_id INTEGER PRIMARY KEY,
                    embedding float[128]
                )
            )");
            // Add person_id to faces if not present (SQLite lacks
            // IF NOT EXISTS for ALTER TABLE, so probe first).
            {
                sqlite3_stmt* stmt;
                int rc = sqlite3_prepare_v2(db_, "SELECT person_id FROM faces LIMIT 0", -1, &stmt,
                                            nullptr);
                if (rc != SQLITE_OK) {
                    exec("ALTER TABLE faces ADD COLUMN person_id INTEGER REFERENCES people(id)");
                } else {
                    sqlite3_finalize(stmt);
                }
            }
            set_schema_version(1);
            db_version = 1;
            // Fall through to migration path below
        } else {
            // Truly fresh database — create full schema at latest version
            exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)");

            exec(R"(
                CREATE TABLE IF NOT EXISTS images (
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
                    blur_score  REAL
                )
            )");
            exec("CREATE INDEX IF NOT EXISTS idx_phash ON images(phash)");
            exec("CREATE INDEX IF NOT EXISTS idx_sha256 ON images(sha256)");
            exec("CREATE INDEX IF NOT EXISTS idx_path ON images(path)");
            exec("CREATE INDEX IF NOT EXISTS idx_gallery ON images(gallery)");

            exec(R"(
                CREATE TABLE IF NOT EXISTS tags (
                    id   INTEGER PRIMARY KEY,
                    name TEXT NOT NULL UNIQUE
                )
            )");
            exec(R"(
                CREATE TABLE IF NOT EXISTS image_tags (
                    image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                    tag_id   INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
                    PRIMARY KEY (image_id, tag_id)
                )
            )");
            exec("INSERT OR IGNORE INTO tags(name) VALUES ('favorite')");

            exec(R"(
                CREATE TABLE IF NOT EXISTS faces (
                    id          INTEGER PRIMARY KEY,
                    image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                    embedding   BLOB NOT NULL,
                    x           INTEGER,
                    y           INTEGER,
                    width       INTEGER,
                    height      INTEGER,
                    person_id   INTEGER REFERENCES people(id)
                )
            )");
            exec("CREATE INDEX IF NOT EXISTS idx_faces_image ON faces(image_id)");

            exec(R"(
                CREATE TABLE IF NOT EXISTS people (
                    id   INTEGER PRIMARY KEY,
                    name TEXT NOT NULL UNIQUE
                )
            )");

            exec(R"(
                CREATE VIRTUAL TABLE IF NOT EXISTS vec_faces USING vec0(
                    face_id INTEGER PRIMARY KEY,
                    embedding float[128]
                )
            )");

            exec(R"(
                CREATE TABLE IF NOT EXISTS operations (
                    id          INTEGER PRIMARY KEY,
                    ts          TEXT NOT NULL,
                    command     TEXT NOT NULL,
                    argv        TEXT,
                    status      TEXT NOT NULL,
                    undoes_id   INTEGER REFERENCES operations(id),
                    entry_count INTEGER NOT NULL DEFAULT 0
                )
            )");
            exec(R"(
                CREATE TABLE IF NOT EXISTS operation_entries (
                    id           INTEGER PRIMARY KEY,
                    op_id        INTEGER NOT NULL REFERENCES operations(id) ON DELETE CASCADE,
                    kind         TEXT NOT NULL,
                    src          TEXT,
                    dst          TEXT,
                    restore_json TEXT
                )
            )");
            exec("CREATE INDEX IF NOT EXISTS idx_op_entries ON operation_entries(op_id)");

            set_schema_version(CURRENT_SCHEMA_VERSION);
        }
    }

    if (db_version > 0 && db_version < CURRENT_SCHEMA_VERSION) {
        // Run pending migrations
        for (int v = db_version + 1; v <= CURRENT_SCHEMA_VERSION; v++) {
            if (!migrations[v].empty()) {
                exec("BEGIN TRANSACTION");
                try {
                    exec(migrations[v]);
                    set_schema_version(v);
                    exec("COMMIT");
                } catch (...) {
                    exec("ROLLBACK");
                    throw;
                }
            } else {
                set_schema_version(v);
            }
        }
    }
    // else: db_version == CURRENT_SCHEMA_VERSION, nothing to do
}

void Database::insert(const ImageInfo& info) {
    const char* sql = R"(
        INSERT OR REPLACE INTO images
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, info.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, info.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, info.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, info.file_size);
    sqlite3_bind_text(stmt, 5, info.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, info.modified_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, info.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(info.phash));
    sqlite3_bind_int(stmt, 9, info.width);
    sqlite3_bind_int(stmt, 10, info.height);
    sqlite3_bind_text(stmt, 11, info.metadata_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, info.media_type.c_str(), -1, SQLITE_TRANSIENT);
    if (info.duration_ms > 0) {
        sqlite3_bind_int64(stmt, 13, info.duration_ms);
    } else {
        sqlite3_bind_null(stmt, 13);
    }
    if (info.blur_score >= 0.0) {
        sqlite3_bind_double(stmt, 14, info.blur_score);
    } else {
        sqlite3_bind_null(stmt, 14);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert: " + std::string(sqlite3_errmsg(db_)));
    }
}

void Database::insert_batch(const std::vector<ImageInfo>& infos) {
    exec("BEGIN TRANSACTION");
    try {
        for (const auto& info : infos) {
            insert(info);
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

void Database::insert_copy(const std::string& source_path, const std::string& dest_path) {
    const char* sql = R"(
        INSERT OR REPLACE INTO images
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score)
        SELECT ?, ?, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score
        FROM images WHERE path = ?
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert_copy: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    fs::path dp(dest_path);
    sqlite3_bind_text(stmt, 1, dest_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dp.filename().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert copy: " + std::string(sqlite3_errmsg(db_)));
    }
}

int64_t Database::log_begin_operation(const std::string& command, const std::string& argv,
                                      const std::string& ts) {
    const char* sql =
        "INSERT INTO operations (ts, command, argv, status, entry_count) "
        "VALUES (?, ?, ?, 'in_progress', 0)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to prepare log_begin_operation: " +
                                 std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_text(stmt, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, command.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, argv.c_str(), -1, SQLITE_TRANSIENT);
    step_write_or_throw(db_, stmt, "Failed to insert operation");
    return sqlite3_last_insert_rowid(db_);
}

void Database::log_add_entry(int64_t op_id, const OperationEntry& entry) {
    {
        const char* sql =
            "INSERT INTO operation_entries (op_id, kind, src, dst, restore_json) "
            "VALUES (?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to prepare log_add_entry: " +
                                     std::string(sqlite3_errmsg(db_)));
        sqlite3_bind_int64(stmt, 1, op_id);
        sqlite3_bind_text(stmt, 2, entry.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, entry.src.c_str(), -1, SQLITE_TRANSIENT);
        if (entry.dst.empty())
            sqlite3_bind_null(stmt, 4);
        else
            sqlite3_bind_text(stmt, 4, entry.dst.c_str(), -1, SQLITE_TRANSIENT);
        if (entry.restore_json.empty())
            sqlite3_bind_null(stmt, 5);
        else
            sqlite3_bind_text(stmt, 5, entry.restore_json.c_str(), -1, SQLITE_TRANSIENT);
        step_write_or_throw(db_, stmt, "Failed to insert operation entry");
    }
    {
        const char* sql = "UPDATE operations SET entry_count = entry_count + 1 WHERE id = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to prepare entry_count bump: " +
                                     std::string(sqlite3_errmsg(db_)));
        sqlite3_bind_int64(stmt, 1, op_id);
        step_write_or_throw(db_, stmt, "Failed to bump entry_count");
    }
}

void Database::log_finish_operation(int64_t op_id, const std::string& status) {
    const char* sql = "UPDATE operations SET status = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to prepare log_finish_operation: " +
                                 std::string(sqlite3_errmsg(db_)));
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, op_id);
    step_write_or_throw(db_, stmt, "Failed to finish operation");
}

void Database::log_prune(int max_entries) {
    if (max_entries <= 0) return;  // unlimited

    exec("BEGIN TRANSACTION");
    auto fail = [&](const std::string& what) {
        exec("ROLLBACK");
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };

    auto total_entries = [&]() -> int64_t {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM operation_entries", -1, &s, nullptr) !=
            SQLITE_OK)
            fail("Failed to prepare entry count");
        int64_t c = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : 0;
        sqlite3_finalize(s);
        return c;
    };

    // Drop whole operations oldest-first until under the cap. entries cascade via
    // the operation_entries FK (foreign_keys is ON per-connection).
    while (total_entries() > max_entries) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM operations ORDER BY id ASC LIMIT 1", -1, &s,
                               nullptr) != SQLITE_OK)
            fail("Failed to prepare oldest-op query");
        if (sqlite3_step(s) != SQLITE_ROW) {  // no operations left
            sqlite3_finalize(s);
            break;
        }
        int64_t oldest = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);

        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM operations WHERE id = ?", -1, &d, nullptr) !=
            SQLITE_OK)
            fail("Failed to prepare op delete");
        sqlite3_bind_int64(d, 1, oldest);
        int rc = sqlite3_step(d);
        sqlite3_finalize(d);
        if (rc != SQLITE_DONE) fail("Failed to prune operation");
    }

    exec("COMMIT");
}

void Database::begin_transaction() { exec("BEGIN TRANSACTION"); }

void Database::commit_transaction() { exec("COMMIT"); }

void Database::rollback_transaction() { exec("ROLLBACK"); }

std::optional<std::string> Database::get_modified_at(const std::string& path) {
    const char* sql = "SELECT modified_at FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);

    std::optional<std::string> result;
    if (rc == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            result = std::string(text);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

std::optional<std::string> Database::get_sha256(const std::string& path) {
    const char* sql = "SELECT sha256 FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);

    std::optional<std::string> result;
    if (rc == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            result = std::string(text);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

void Database::update_path(const std::string& old_path, const std::string& new_path) {
    // No-op move (source == dest): the DELETE-then-UPDATE below would delete the
    // row (as the "stale dest") and then find nothing to repoint, losing the
    // entry for a file that's still on disk. Nothing to do here.
    if (old_path == new_path) return;

    // The file has just been moved to new_path, overwriting whatever was there.
    // If a row already tracks new_path (e.g. it was scanned in a prior run), that
    // row is now stale: drop it first so repointing old_path doesn't hit the
    // UNIQUE(path) constraint. Do both in one transaction so it's all-or-nothing.
    exec("BEGIN TRANSACTION");

    auto fail = [&](const std::string& what) {
        exec("ROLLBACK");
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };

    {
        const char* del = "DELETE FROM images WHERE path = ?";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, del, -1, &stmt, nullptr) != SQLITE_OK)
            fail("Failed to prepare delete");
        sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) fail("Failed to clear existing dest row");
    }

    {
        const char* upd = "UPDATE images SET path = ?, filename = ? WHERE path = ?";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, upd, -1, &stmt, nullptr) != SQLITE_OK)
            fail("Failed to prepare update");
        fs::path p(new_path);
        sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, p.filename().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, old_path.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) fail("Failed to update path");
    }

    exec("COMMIT");
}

// Smallest string strictly greater than every string beginning with `prefix`:
// increment the last byte. Returns nullopt when no finite upper bound exists
// (empty prefix, or prefix is all 0xFF), meaning "no upper bound".
static std::optional<std::string> prefix_upper_bound(const std::string& prefix) {
    std::string upper = prefix;
    while (!upper.empty()) {
        auto& last = reinterpret_cast<unsigned char&>(upper.back());
        if (last != 0xFF) {
            last++;
            return upper;
        }
        upper.pop_back();  // 0xFF can't be incremented; drop and carry
    }
    return std::nullopt;
}

std::vector<std::string> Database::get_paths_with_prefix(const std::string& prefix) {
    // Range comparison, NOT LIKE: a path prefix can contain '_' or '%' (very
    // common in real directory names), which LIKE would treat as wildcards and
    // over-match sibling rows. `path >= prefix AND path < upper` is exact and
    // still uses the idx_path index.
    auto upper = prefix_upper_bound(prefix);
    std::string sql = "SELECT path FROM images WHERE path >= ?";
    if (upper) sql += " AND path < ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, prefix.c_str(), -1, SQLITE_TRANSIENT);
    if (upper) sqlite3_bind_text(stmt, 2, upper->c_str(), -1, SQLITE_TRANSIENT);

    std::vector<std::string> paths;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            paths.emplace_back(text);
        }
    }

    sqlite3_finalize(stmt);
    return paths;
}

void Database::delete_paths(const std::vector<std::string>& paths) {
    if (paths.empty()) return;

    exec("BEGIN TRANSACTION");
    const char* sql = "DELETE FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        exec("ROLLBACK");
        throw std::runtime_error("Failed to prepare delete: " + std::string(sqlite3_errmsg(db_)));
    }

    for (const auto& path : paths) {
        // vec_faces is a vec0 virtual table and cannot be an FK child, so
        // ON DELETE CASCADE never reaches it — clean it (and faces) explicitly
        // before dropping the image row. image_tags cascades via foreign_keys.
        int64_t id = get_image_id(path);
        if (id >= 0) delete_faces_for_image(id);

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            exec("ROLLBACK");
            throw std::runtime_error("Failed to delete path '" + path +
                                     "': " + std::string(sqlite3_errmsg(db_)));
        }
    }

    sqlite3_finalize(stmt);
    exec("COMMIT");
}

std::vector<std::string> Database::purge(const std::string& path_prefix, const std::string& glob) {
    // First, find matching entries
    std::string sql = "SELECT path, filename FROM images WHERE 1=1";
    if (!path_prefix.empty()) {
        sql += " AND path LIKE ? || '%'";
    }
    if (!glob.empty()) {
        // Convert glob to SQL LIKE: * -> %, ? -> _
        std::string like_pattern;
        for (char c : glob) {
            if (c == '*')
                like_pattern += '%';
            else if (c == '?')
                like_pattern += '_';
            else if (c == '%' || c == '_') {
                like_pattern += '\\';
                like_pattern += c;
            } else
                like_pattern += c;
        }
        sql += " AND filename LIKE ? ESCAPE '\\'";
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    int bind_idx = 1;
    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!glob.empty()) {
        std::string like_pattern;
        for (char c : glob) {
            if (c == '*')
                like_pattern += '%';
            else if (c == '?')
                like_pattern += '_';
            else if (c == '%' || c == '_') {
                like_pattern += '\\';
                like_pattern += c;
            } else
                like_pattern += c;
        }
        sqlite3_bind_text(stmt, bind_idx++, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<std::string> matched_paths;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) matched_paths.emplace_back(text);
    }
    sqlite3_finalize(stmt);

    return matched_paths;
}

int64_t Database::count() {
    const char* sql = "SELECT COUNT(*) FROM images";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare count: " + std::string(sqlite3_errmsg(db_)));
    }

    rc = sqlite3_step(stmt);
    int64_t result = 0;
    if (rc == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::set<std::string> Database::get_all_sha256s() {
    const char* sql =
        "SELECT DISTINCT sha256 FROM images WHERE sha256 IS NOT NULL AND sha256 != ''";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    std::set<std::string> result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) result.insert(text);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ImageInfo> Database::get_all_images(const std::string& path_prefix) {
    std::string sql =
        "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, "
        "width, height, metadata, media_type, duration_ms, gallery, "
        "EXISTS(SELECT 1 FROM image_tags it JOIN tags t ON t.id = it.tag_id "
        "WHERE it.image_id = images.id AND t.name = 'favorite'), blur_score FROM images";
    if (!path_prefix.empty()) {
        sql += " WHERE path LIKE ? || '%'";
    }
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, 1, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ImageInfo> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ImageInfo info;
        auto text = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        info.path = text(0);
        info.filename = text(1);
        info.extension = text(2);
        info.file_size = sqlite3_column_int64(stmt, 3);
        info.created_at = text(4);
        info.modified_at = text(5);
        info.sha256 = text(6);
        info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        info.width = sqlite3_column_int(stmt, 8);
        info.height = sqlite3_column_int(stmt, 9);
        info.metadata_json = text(10);
        info.media_type = text(11);
        info.duration_ms = sqlite3_column_int64(stmt, 12);
        info.gallery = text(13);
        info.favorite = sqlite3_column_int(stmt, 14) != 0;
        info.blur_score =
            sqlite3_column_type(stmt, 15) == SQLITE_NULL ? -1.0 : sqlite3_column_double(stmt, 15);
        results.push_back(std::move(info));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<std::vector<ImageInfo>> Database::get_exact_duplicates(const std::string& path_prefix) {
    std::string sql;
    if (path_prefix.empty()) {
        sql = R"(
            SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms
            FROM images
            WHERE sha256 IN (SELECT sha256 FROM images GROUP BY sha256 HAVING COUNT(*) > 1)
            ORDER BY sha256, path
        )";
    } else {
        sql = R"(
            SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms
            FROM images
            WHERE path LIKE ? || '%'
              AND sha256 IN (SELECT sha256 FROM images WHERE path LIKE ? || '%' GROUP BY sha256 HAVING COUNT(*) > 1)
            ORDER BY sha256, path
        )";
    }
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, 1, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ImageInfo> flat;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ImageInfo info;
        auto text = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        info.path = text(0);
        info.filename = text(1);
        info.extension = text(2);
        info.file_size = sqlite3_column_int64(stmt, 3);
        info.created_at = text(4);
        info.modified_at = text(5);
        info.sha256 = text(6);
        info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        info.width = sqlite3_column_int(stmt, 8);
        info.height = sqlite3_column_int(stmt, 9);
        info.metadata_json = text(10);
        info.media_type = text(11);
        info.duration_ms = sqlite3_column_int64(stmt, 12);
        flat.push_back(std::move(info));
    }
    sqlite3_finalize(stmt);

    // Group by sha256
    std::vector<std::vector<ImageInfo>> groups;
    for (size_t i = 0; i < flat.size();) {
        size_t j = i;
        while (j < flat.size() && flat[j].sha256 == flat[i].sha256) j++;
        groups.emplace_back(flat.begin() + i, flat.begin() + j);
        i = j;
    }
    return groups;
}

int64_t Database::get_image_id(const std::string& path) {
    const char* sql = "SELECT id FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    int64_t id = -1;
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

int Database::set_tag(const std::vector<std::string>& paths, const std::string& tag, bool add) {
    if (paths.empty()) {
        return 0;
    }

    // On add, create the tag if it doesn't exist. Then resolve its id.
    if (add) {
        sqlite3_stmt* ins = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO tags(name) VALUES (?)", -1, &ins,
                               nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare tag insert: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        sqlite3_bind_text(ins, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
        step_write_or_throw(db_, ins, "Failed to create tag");
    }

    int64_t tag_id = -1;
    {
        sqlite3_stmt* sel = nullptr;
        sqlite3_prepare_v2(db_, "SELECT id FROM tags WHERE name = ?", -1, &sel, nullptr);
        sqlite3_bind_text(sel, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(sel) == SQLITE_ROW) {
            tag_id = sqlite3_column_int64(sel, 0);
        }
        sqlite3_finalize(sel);
    }
    if (tag_id < 0) {
        return 0;  // removing a tag that was never created: nothing to do
    }

    const char* sql = add ? "INSERT OR IGNORE INTO image_tags(image_id, tag_id) VALUES (?, ?)"
                          : "DELETE FROM image_tags WHERE image_id = ? AND tag_id = ?";

    int affected = 0;
    exec("BEGIN TRANSACTION");
    for (const auto& path : paths) {
        int64_t image_id = get_image_id(path);
        if (image_id < 0) {
            continue;  // path not in the DB
        }
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_int64(stmt, 1, image_id);
        sqlite3_bind_int64(stmt, 2, tag_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        affected++;
    }
    exec("COMMIT");
    return affected;
}

std::vector<std::string> Database::get_tags_for_image(const std::string& path) {
    std::vector<std::string> tags;
    const char* sql =
        "SELECT t.name FROM tags t "
        "JOIN image_tags it ON it.tag_id = t.id "
        "JOIN images i ON i.id = it.image_id "
        "WHERE i.path = ? ORDER BY t.name";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        tags.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return tags;
}

std::vector<std::pair<std::string, int64_t>> Database::get_tag_counts() {
    std::vector<std::pair<std::string, int64_t>> counts;
    const char* sql =
        "SELECT t.name, COUNT(it.image_id) FROM tags t "
        "LEFT JOIN image_tags it ON it.tag_id = t.id "
        "GROUP BY t.id ORDER BY t.name";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        counts.emplace_back(std::move(name), sqlite3_column_int64(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return counts;
}

int Database::set_gallery(const std::vector<std::string>& paths, const std::string& gallery,
                          bool no_overwrite) {
    if (paths.empty()) {
        return 0;
    }
    const char* sql = no_overwrite
                          ? "UPDATE images SET gallery = ? WHERE path = ? AND gallery IS NULL"
                          : "UPDATE images SET gallery = ? WHERE path = ?";
    int affected = 0;
    exec("BEGIN TRANSACTION");
    for (const auto& path : paths) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, gallery.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        affected += sqlite3_changes(db_);
    }
    exec("COMMIT");
    return affected;
}

int Database::clear_gallery(const std::vector<std::string>& paths) {
    if (paths.empty()) {
        return 0;
    }
    int affected = 0;
    exec("BEGIN TRANSACTION");
    for (const auto& path : paths) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(
            db_, "UPDATE images SET gallery = NULL WHERE path = ? AND gallery IS NOT NULL", -1,
            &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        affected += sqlite3_changes(db_);
    }
    exec("COMMIT");
    return affected;
}

std::vector<std::pair<std::string, int64_t>> Database::get_gallery_counts() {
    std::vector<std::pair<std::string, int64_t>> counts;
    const char* sql =
        "SELECT gallery, COUNT(*) FROM images WHERE gallery IS NOT NULL "
        "GROUP BY gallery ORDER BY gallery";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        counts.emplace_back(std::move(name), sqlite3_column_int64(stmt, 1));
    }
    sqlite3_finalize(stmt);
    return counts;
}

bool Database::has_faces(const std::string& path) {
    int64_t image_id = get_image_id(path);
    if (image_id < 0) return false;

    const char* sql = "SELECT COUNT(*) FROM faces WHERE image_id = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, image_id);
    rc = sqlite3_step(stmt);
    int64_t count = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count > 0;
}

void Database::delete_faces_for_image(int64_t image_id) {
    // Delete from vec_faces first (references face ids)
    const char* vec_sql =
        "DELETE FROM vec_faces WHERE face_id IN (SELECT id FROM faces WHERE image_id = ?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, vec_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare vec_faces delete: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int64(stmt, 1, image_id);
    step_write_or_throw(db_, stmt, "Failed to delete face vectors");

    // Delete from faces
    const char* sql = "DELETE FROM faces WHERE image_id = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare faces delete: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int64(stmt, 1, image_id);
    step_write_or_throw(db_, stmt, "Failed to delete faces");
}

void Database::insert_face(int64_t image_id, const FaceInfo& face) {
    // Insert into faces table
    const char* sql = R"(
        INSERT INTO faces (image_id, embedding, x, y, width, height)
        VALUES (?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare face insert: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_int64(stmt, 1, image_id);
    sqlite3_bind_blob(stmt, 2, face.embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, face.x);
    sqlite3_bind_int(stmt, 4, face.y);
    sqlite3_bind_int(stmt, 5, face.width);
    sqlite3_bind_int(stmt, 6, face.height);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert face: " + std::string(sqlite3_errmsg(db_)));
    }

    // Get the face id
    int64_t face_id = sqlite3_last_insert_rowid(db_);

    // Insert into vec_faces for KNN search
    const char* vec_sql = "INSERT INTO vec_faces(face_id, embedding) VALUES (?, ?)";
    rc = sqlite3_prepare_v2(db_, vec_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare vec insert: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_int64(stmt, 1, face_id);
    sqlite3_bind_blob(stmt, 2, face.embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert vec face: " + std::string(sqlite3_errmsg(db_)));
    }
}

std::vector<Database::FaceSearchResult> Database::search_faces(
    const std::array<float, 128>& embedding, float threshold, int limit) {
    const char* sql = R"(
        SELECT f.id, i.path, v.distance, f.x, f.y, f.width, f.height
        FROM vec_faces v
        JOIN faces f ON f.id = v.face_id
        JOIN images i ON i.id = f.image_id
        WHERE v.embedding MATCH ?
          AND k = ?
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare face search: " +
                                 std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_blob(stmt, 1, embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 100);

    std::vector<FaceSearchResult> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        float dist = static_cast<float>(sqlite3_column_double(stmt, 2));
        if (dist > threshold) continue;

        FaceSearchResult r;
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.path = path ? path : "";
        r.distance = dist;
        r.face_x = sqlite3_column_int(stmt, 3);
        r.face_y = sqlite3_column_int(stmt, 4);
        r.face_w = sqlite3_column_int(stmt, 5);
        r.face_h = sqlite3_column_int(stmt, 6);
        results.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::get_or_create_person(const std::string& name) {
    // Try to find existing
    const char* sql = "SELECT id FROM people WHERE name = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);

    // Create new
    const char* insert_sql = "INSERT INTO people (name) VALUES (?)";
    rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to create person: " + std::string(sqlite3_errmsg(db_)));
    }
    return sqlite3_last_insert_rowid(db_);
}

void Database::set_face_person(int64_t face_id, int64_t person_id) {
    const char* sql = "UPDATE faces SET person_id = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare update: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int64(stmt, 1, person_id);
    sqlite3_bind_int64(stmt, 2, face_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int64_t Database::find_closest_face(const std::array<float, 128>& embedding, float threshold) {
    // Use vec_faces to find nearest face
    const char* sql = R"(
        SELECT face_id, distance FROM vec_faces
        WHERE embedding MATCH ? AND k = 1
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare face query: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_blob(stmt, 1, embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    int64_t face_id = -1;
    if (rc == SQLITE_ROW) {
        float dist = static_cast<float>(sqlite3_column_double(stmt, 1));
        if (dist <= threshold) {
            face_id = sqlite3_column_int64(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return face_id;
}

std::vector<Database::PersonInfo> Database::list_people() {
    const char* sql = R"(
        SELECT p.id, p.name, COUNT(f.id) as face_count
        FROM people p
        LEFT JOIN faces f ON f.person_id = p.id
        GROUP BY p.id
        ORDER BY p.name
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    std::vector<PersonInfo> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersonInfo p;
        p.id = sqlite3_column_int64(stmt, 0);
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        p.name = name ? name : "";
        p.face_count = sqlite3_column_int64(stmt, 2);
        results.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<std::string> Database::get_images_for_person(const std::string& name) {
    const char* sql = R"(
        SELECT DISTINCT i.path
        FROM images i
        JOIN faces f ON f.image_id = i.id
        JOIN people p ON p.id = f.person_id
        WHERE p.name = ?
        ORDER BY i.path
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<std::string> paths;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (path) paths.emplace_back(path);
    }
    sqlite3_finalize(stmt);
    return paths;
}

std::string Database::auto_label_face(int64_t face_id, const std::array<float, 128>& embedding,
                                      float threshold) {
    // Get all reference embeddings from named faces
    const char* sql = R"(
        SELECT f.embedding, p.id, p.name
        FROM faces f
        JOIN people p ON p.id = f.person_id
        WHERE f.person_id IS NOT NULL
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return "";

    float best_dist = threshold;
    int64_t best_person_id = -1;
    std::string best_name;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const float* ref_data = static_cast<const float*>(sqlite3_column_blob(stmt, 0));
        if (!ref_data) continue;

        // Compute distance
        float dist = 0;
        for (int i = 0; i < 128; i++) {
            float diff = embedding[i] - ref_data[i];
            dist += diff * diff;
        }
        dist = std::sqrt(dist);

        if (dist < best_dist) {
            best_dist = dist;
            best_person_id = sqlite3_column_int64(stmt, 1);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            best_name = name ? name : "";
        }
    }
    sqlite3_finalize(stmt);

    if (best_person_id >= 0) {
        set_face_person(face_id, best_person_id);
        return best_name;
    }
    return "";
}

std::vector<ImageInfo> Database::search(const SearchCriteria& c) {
    std::string sql =
        "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, "
        "width, height, metadata, media_type, duration_ms, blur_score FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;  // {value, is_int}

    if (!c.extension.empty()) {
        sql += " AND LOWER(extension) = LOWER(?)";
        binds.push_back({c.extension, false});
    }
    if (!c.after.empty()) {
        // Compare against best available date
        std::string meta_date = c.after;
        if (meta_date.size() >= 10) {
            meta_date[4] = ':';
            meta_date[7] = ':';
        }
        sql +=
            " AND COALESCE(json_extract(metadata, '$.DateTimeOriginal'), json_extract(metadata, "
            "'$.DateTimeDigitized'), modified_at) >= ?";
        binds.push_back({meta_date, false});
    }
    if (!c.before.empty()) {
        std::string meta_date = c.before;
        if (meta_date.size() >= 10) {
            meta_date[4] = ':';
            meta_date[7] = ':';
        }
        sql +=
            " AND COALESCE(json_extract(metadata, '$.DateTimeOriginal'), json_extract(metadata, "
            "'$.DateTimeDigitized'), modified_at) <= ?";
        binds.push_back({meta_date, false});
    }
    if (!c.camera.empty()) {
        sql += " AND json_extract(metadata, '$.Model') LIKE '%' || ? || '%'";
        binds.push_back({c.camera, false});
    }
    if (!c.make.empty()) {
        sql += " AND json_extract(metadata, '$.Make') LIKE '%' || ? || '%'";
        binds.push_back({c.make, false});
    }
    if (c.min_size >= 0) {
        sql += " AND file_size >= " + std::to_string(c.min_size);
    }
    if (c.max_size >= 0) {
        sql += " AND file_size <= " + std::to_string(c.max_size);
    }
    if (!c.media.empty()) {
        sql += " AND media_type = ?";
        binds.push_back({c.media, false});
    }
    // Duration filters: images have NULL duration_ms and always pass, so a
    // duration bound narrows videos without silently dropping every image.
    if (c.min_duration_ms >= 0) {
        sql += " AND (duration_ms IS NULL OR duration_ms >= " + std::to_string(c.min_duration_ms) +
               ")";
    }
    if (c.max_duration_ms >= 0) {
        sql += " AND (duration_ms IS NULL OR duration_ms <= " + std::to_string(c.max_duration_ms) +
               ")";
    }
    if (!c.gallery.empty()) {
        sql += " AND gallery = ?";
        binds.push_back({c.gallery, false});
    }
    if (!c.tags.empty()) {
        // Image must have ALL requested tags (AND semantics).
        sql +=
            " AND id IN (SELECT it.image_id FROM image_tags it "
            "JOIN tags t ON t.id = it.tag_id WHERE t.name IN (";
        for (size_t i = 0; i < c.tags.size(); i++) {
            sql += (i == 0) ? "?" : ",?";
            binds.push_back({c.tags[i], false});
        }
        sql += ") GROUP BY it.image_id HAVING COUNT(DISTINCT t.id) = " +
               std::to_string(c.tags.size()) + ")";
    }
    // Sharpness bounds (NULL/unscored rows are excluded by the comparison).
    if (c.min_sharpness >= 0) {
        sql += " AND blur_score >= " + std::to_string(c.min_sharpness);
    }
    if (c.max_sharpness >= 0) {
        sql += " AND blur_score <= " + std::to_string(c.max_sharpness);
    }

    sql += " ORDER BY path";

    if (c.limit > 0) {
        sql += " LIMIT " + std::to_string(c.limit);
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare search: " + std::string(sqlite3_errmsg(db_)));
    }

    for (size_t i = 0; i < binds.size(); i++) {
        sqlite3_bind_text(stmt, i + 1, binds[i].first.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ImageInfo> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ImageInfo info;
        auto text = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        info.path = text(0);
        info.filename = text(1);
        info.extension = text(2);
        info.file_size = sqlite3_column_int64(stmt, 3);
        info.created_at = text(4);
        info.modified_at = text(5);
        info.sha256 = text(6);
        info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        info.width = sqlite3_column_int(stmt, 8);
        info.height = sqlite3_column_int(stmt, 9);
        info.metadata_json = text(10);
        info.media_type = text(11);
        info.duration_ms = sqlite3_column_int64(stmt, 12);
        info.blur_score =
            sqlite3_column_type(stmt, 13) == SQLITE_NULL ? -1.0 : sqlite3_column_double(stmt, 13);
        results.push_back(std::move(info));
    }

    sqlite3_finalize(stmt);
    return results;
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg);
    }
}
