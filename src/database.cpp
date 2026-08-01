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
#include "dates.h"
#include "sqlite-vec.h"

namespace fs = std::filesystem;

// Schema version this code expects. Bump when adding migrations.
static const int CURRENT_SCHEMA_VERSION = 6;

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
    // v6: indexed effective_date for date-sorted pagination without a full
    // scan/JSON-parse of every row. The ADD COLUMN happens in C++ (see the
    // migration loop) because a DB created fresh at a later CURRENT_SCHEMA_VERSION
    // and then rolled back to v4/v5 for testing already has the column, and
    // SQLite's ALTER TABLE has no IF NOT EXISTS.
    R"(UPDATE images SET effective_date = COALESCE(
                   CASE WHEN json_valid(metadata) THEN json_extract(metadata, '$.DateTimeOriginal') END,
                   CASE WHEN json_valid(metadata) THEN json_extract(metadata, '$.DateTimeDigitized') END,
                   modified_at);
       UPDATE images SET effective_date =
                   substr(effective_date,1,4) || '-' || substr(effective_date,6,2) || '-' ||
                   substr(effective_date,9)
               WHERE effective_date LIKE '____:__:__%';
       CREATE INDEX IF NOT EXISTS idx_effective_date
               ON images(effective_date DESC, id DESC);)",
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

// Escape a string for embedding in the hand-built restore_json snapshot.
std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                o += c;
        }
    }
    return o;
}
}  // namespace

std::string compute_effective_date(const std::string& metadata_json,
                                   const std::string& modified_at) {
    std::string d;
    if (!metadata_json.empty()) {
        d = json_get(metadata_json, "DateTimeOriginal");
        if (d.empty()) d = json_get(metadata_json, "DateTimeDigitized");
    }
    if (d.empty()) d = modified_at;

    // Normalize EXIF ':' date separators (positions 4 and 7, e.g.
    // "2021:05:04 10:00:00") to '-' so all values sort in one family
    // alongside modified_at's native "YYYY-MM-DD ..." form. The time
    // portion's colons are left untouched.
    if (d.size() >= 10 && d[4] == ':' && d[7] == ':') {
        d[4] = '-';
        d[7] = '-';
    }
    return d;
}

Database::Database(const std::string& path, bool read_only) {
    // Read-only opens create nothing — neither the database nor its parent
    // directory.
    fs::path db_path(path);
    if (!read_only && db_path.has_parent_path()) {
        fs::create_directories(db_path.parent_path());
    }

    // Read-only callers must not create a database; read-write keeps the
    // historical create-if-missing behavior.
    const int flags =
        read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("Failed to open database: " + err);
    }

    if (!read_only) {
        // Enable WAL mode for better concurrent read/write performance
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=NORMAL");
    }
    // Wait (rather than fail instantly with SQLITE_BUSY) when another phig
    // process holds the single WAL writer lock.
    sqlite3_busy_timeout(db_, 30000);
    // Enforce declared FK cascades (faces/image_tags ON DELETE CASCADE). SQLite
    // defaults foreign_keys OFF per-connection, so this must be set every open.
    // Connection-local; does not write to the database file.
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
            // predates it (idempotent — safe if already present). Do this in
            // ONE transaction: a crash partway must not leave a versionless
            // partial schema that a later run misreads as still pre-migration
            // (whose re-run would then race the ALTER TABLE probe below).
            exec("BEGIN IMMEDIATE TRANSACTION");
            try {
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
                    int rc = sqlite3_prepare_v2(db_, "SELECT person_id FROM faces LIMIT 0", -1,
                                                &stmt, nullptr);
                    if (rc != SQLITE_OK) {
                        exec(
                            "ALTER TABLE faces ADD COLUMN person_id INTEGER REFERENCES people(id)");
                    } else {
                        sqlite3_finalize(stmt);
                    }
                }
                set_schema_version(1);
                exec("COMMIT");
            } catch (...) {
                exec("ROLLBACK");
                throw;
            }
            db_version = 1;
            // Fall through to migration path below
        } else {
            // Truly fresh database — create the full schema at the latest
            // version in ONE transaction: a crash partway must not leave a
            // versionless partial schema that a later run misreads as a
            // pre-migration v1 DB (whose v2 migration would then fail).
            exec("BEGIN IMMEDIATE TRANSACTION");
            try {
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
                        blur_score  REAL,
                        effective_date TEXT
                    )
                )");
                exec("CREATE INDEX IF NOT EXISTS idx_phash ON images(phash)");
                exec("CREATE INDEX IF NOT EXISTS idx_sha256 ON images(sha256)");
                exec("CREATE INDEX IF NOT EXISTS idx_path ON images(path)");
                exec("CREATE INDEX IF NOT EXISTS idx_gallery ON images(gallery)");
                exec(
                    "CREATE INDEX IF NOT EXISTS idx_effective_date "
                    "ON images(effective_date DESC, id DESC)");

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
                exec("COMMIT");
            } catch (...) {
                exec("ROLLBACK");
                throw;
            }
        }
    }

    if (db_version > 0 && db_version < CURRENT_SCHEMA_VERSION) {
        // Run pending migrations. Each step takes the write lock up front
        // (IMMEDIATE) and re-reads the version inside the transaction, so if a
        // concurrent process already applied this step we skip it instead of
        // re-running non-idempotent DDL (e.g. ALTER TABLE ADD COLUMN).
        for (int v = db_version + 1; v <= CURRENT_SCHEMA_VERSION; v++) {
            exec("BEGIN IMMEDIATE TRANSACTION");
            try {
                if (get_schema_version() >= v) {
                    // Another process already applied v while we waited.
                    exec("COMMIT");
                    continue;
                }
                if (v == 6) {
                    // Probe first (SQLite lacks IF NOT EXISTS for ALTER TABLE):
                    // a DB created fresh at a later CURRENT_SCHEMA_VERSION already
                    // has this column via init_schema()'s CREATE TABLE.
                    sqlite3_stmt* stmt;
                    int rc = sqlite3_prepare_v2(db_, "SELECT effective_date FROM images LIMIT 0",
                                                -1, &stmt, nullptr);
                    if (rc != SQLITE_OK) {
                        exec("ALTER TABLE images ADD COLUMN effective_date TEXT");
                    } else {
                        sqlite3_finalize(stmt);
                    }
                }
                if (!migrations[v].empty()) exec(migrations[v]);
                set_schema_version(v);
                exec("COMMIT");
            } catch (...) {
                exec("ROLLBACK");
                throw;
            }
        }
    }
    // else: db_version == CURRENT_SCHEMA_VERSION, nothing to do
}

void Database::insert(const ImageInfo& info) {
    // If the row exists and its content changed, its face data (boxes +
    // embeddings, incl. vec_faces) describes pixels that no longer exist —
    // clear it. Unchanged content keeps faces and their person labels.
    {
        const char* sel = "SELECT id, sha256 FROM images WHERE path = ?";
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db_, sel, -1, &s, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare pre-insert select: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        sqlite3_bind_text(s, 1, info.path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(s, 0);
            const char* old_sha = reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
            bool content_changed = !old_sha || info.sha256 != old_sha;
            sqlite3_finalize(s);
            if (content_changed) delete_faces_for_image(id);
        } else {
            sqlite3_finalize(s);
        }
    }

    // Upsert, NOT INSERT OR REPLACE: REPLACE deletes the row, which (with
    // foreign_keys=ON) cascades away image_tags and faces and assigns a new
    // id. DO UPDATE keeps the id — tags survive — and never touches gallery.
    const char* sql = R"(
        INSERT INTO images
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score, effective_date)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(path) DO UPDATE SET
            filename = excluded.filename,
            extension = excluded.extension,
            file_size = excluded.file_size,
            created_at = excluded.created_at,
            modified_at = excluded.modified_at,
            sha256 = excluded.sha256,
            phash = excluded.phash,
            width = excluded.width,
            height = excluded.height,
            metadata = excluded.metadata,
            media_type = excluded.media_type,
            duration_ms = excluded.duration_ms,
            blur_score = excluded.blur_score,
            effective_date = excluded.effective_date
    )";

    std::string effective_date = info.effective_date.empty()
                                     ? compute_effective_date(info.metadata_json, info.modified_at)
                                     : info.effective_date;

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
    sqlite3_bind_text(stmt, 15, effective_date.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert: " + std::string(sqlite3_errmsg(db_)));
    }
}

void Database::insert_batch(const std::vector<ImageInfo>& infos) {
    exec("BEGIN IMMEDIATE TRANSACTION");
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
    // Same vec_faces cleanup as update_path_in_txn: the REPLACE below deletes
    // any stale row tracking dest_path, and FK cascade can't reach vec_faces.
    {
        int64_t stale_id = get_image_id(dest_path);
        if (stale_id >= 0) delete_faces_for_image(stale_id);
    }

    const char* sql = R"(
        INSERT OR REPLACE INTO images
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score, effective_date)
        SELECT ?, ?, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score, effective_date
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

    exec("BEGIN IMMEDIATE TRANSACTION");
    auto fail = [&](const std::string& what) {
        exec("ROLLBACK");
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };

    // Walk operations newest-first, accumulating their entry counts. Keep the
    // newest operations that fit under the cap; everything older is pruned in one
    // bulk DELETE (entries cascade via the operation_entries FK). Two invariants:
    //   - ALWAYS keep the newest operation, even if it alone exceeds the cap — the
    //     just-recorded run must never delete its own rollback record (review [2]).
    //   - Single pass + single delete, so pruning is O(n), not O(n^2) (review [8]).
    int64_t cutoff = -1;  // delete operations with id <= cutoff; -1 = delete none
    {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id, entry_count FROM operations ORDER BY id DESC", -1,
                               &s, nullptr) != SQLITE_OK)
            fail("Failed to prepare prune scan");
        int64_t kept = 0;
        bool first = true;
        while (sqlite3_step(s) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(s, 0);
            int64_t entries = sqlite3_column_int64(s, 1);
            // Keep the newest op unconditionally; keep further ops only while they
            // still fit. The first op that would overflow becomes the cutoff, and
            // every older op is pruned with it.
            if (first || kept + entries <= max_entries) {
                kept += entries;
                first = false;
            } else {
                cutoff = id;
                break;
            }
        }
        sqlite3_finalize(s);
    }

    if (cutoff >= 0) {
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM operations WHERE id <= ?", -1, &d, nullptr) !=
            SQLITE_OK)
            fail("Failed to prepare op delete");
        sqlite3_bind_int64(d, 1, cutoff);
        int rc = sqlite3_step(d);
        sqlite3_finalize(d);
        if (rc != SQLITE_DONE) fail("Failed to prune operations");
    }

    exec("COMMIT");
}

void Database::begin_transaction() { exec("BEGIN IMMEDIATE TRANSACTION"); }

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

void Database::update_path_in_txn(const std::string& old_path, const std::string& new_path) {
    // No-op move (source == dest): the DELETE-then-UPDATE below would delete the
    // row (as the "stale dest") and then find nothing to repoint, losing the
    // entry for a file that's still on disk. Nothing to do here.
    if (old_path == new_path) return;

    // The file has just been moved to new_path, overwriting whatever was there.
    // If a row already tracks new_path (e.g. it was scanned in a prior run), that
    // row is now stale: drop it first so repointing old_path doesn't hit the
    // UNIQUE(path) constraint. Assumes the caller has opened a transaction.
    auto fail = [&](const std::string& what) {
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };

    // vec_faces is a vec0 virtual table and unreachable by FK cascade; clear
    // the stale dest row's face data explicitly before dropping the row.
    {
        int64_t stale_id = get_image_id(new_path);
        if (stale_id >= 0) delete_faces_for_image(stale_id);
    }

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
}

void Database::update_path(const std::string& old_path, const std::string& new_path) {
    if (old_path == new_path) return;
    exec("BEGIN IMMEDIATE TRANSACTION");
    try {
        update_path_in_txn(old_path, new_path);
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
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

    exec("BEGIN IMMEDIATE TRANSACTION");
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

int Database::delete_paths_logged(const std::vector<std::string>& paths, int64_t op_id) {
    if (paths.empty()) return 0;
    exec("BEGIN IMMEDIATE TRANSACTION");
    auto fail = [&](const std::string& what) {
        exec("ROLLBACK");
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };

    static const char* cols[] = {"path",        "filename",    "extension",  "file_size",
                                 "created_at",  "modified_at", "sha256",     "phash",
                                 "width",       "height",      "metadata",   "media_type",
                                 "duration_ms", "gallery",     "blur_score", "effective_date"};
    const int kNumCols = 16;

    int deleted = 0;
    for (const auto& path : paths) {
        // Snapshot the row (the columns insert/insert_copy carry) for restore.
        std::string restore;
        {
            const char* sel =
                "SELECT path, filename, extension, file_size, created_at, modified_at, "
                "sha256, phash, width, height, metadata, media_type, duration_ms, gallery, "
                "blur_score, effective_date FROM images WHERE path = ?";
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(db_, sel, -1, &s, nullptr) != SQLITE_OK)
                fail("Failed to prepare snapshot select");
            sqlite3_bind_text(s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) != SQLITE_ROW) {
                sqlite3_finalize(s);
                continue;  // not present — nothing to delete/log
            }
            restore = "{";
            for (int i = 0; i < kNumCols; i++) {
                if (i) restore += ",";
                restore += "\"";
                restore += cols[i];
                restore += "\":";
                const unsigned char* txt = sqlite3_column_text(s, i);
                if (!txt) {
                    restore += "null";
                } else {
                    restore += "\"";
                    restore += json_escape(reinterpret_cast<const char*>(txt));
                    restore += "\"";
                }
            }
            sqlite3_finalize(s);

            // The snapshot captures only the images row, not the image's faces /
            // vec_faces embeddings (deleted below). Record how many faces are lost
            // so a future undo can honestly report the row is only partially
            // restorable rather than silently dropping face data (review [4]).
            int64_t fid = get_image_id(path);
            int64_t face_count = 0;
            if (fid >= 0) {
                sqlite3_stmt* fs = nullptr;
                if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM faces WHERE image_id = ?", -1,
                                       &fs, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(fs, 1, fid);
                    if (sqlite3_step(fs) == SQLITE_ROW) face_count = sqlite3_column_int64(fs, 0);
                    sqlite3_finalize(fs);
                }
            }
            if (face_count > 0) restore += ",\"_faces_omitted\":" + std::to_string(face_count);
            restore += "}";
        }

        // vec_faces / faces are not reachable by cascade from a vec0 table; clear
        // them explicitly first, mirroring delete_paths.
        int64_t id = get_image_id(path);
        if (id >= 0) delete_faces_for_image(id);

        if (op_id > 0) log_add_entry(op_id, {"purge_row", path, "", restore});

        const char* del = "DELETE FROM images WHERE path = ?";
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, del, -1, &d, nullptr) != SQLITE_OK)
            fail("Failed to prepare delete");
        sqlite3_bind_text(d, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(d);
        sqlite3_finalize(d);
        if (rc != SQLITE_DONE) fail("Failed to delete path '" + path + "'");
        deleted++;
    }

    exec("COMMIT");
    return deleted;
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

std::vector<std::string> Database::get_paths_for_sha256(const std::string& sha256,
                                                        const std::string& exclude_path) {
    const char* sql = "SELECT path FROM images WHERE sha256 = ? AND path != ? ORDER BY path";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare sha256 lookup: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, exclude_path.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<std::string> paths;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) paths.emplace_back(text);
    }
    sqlite3_finalize(stmt);
    return paths;
}

void Database::transfer_user_metadata(int64_t from_id, int64_t to_id) {
    // Tags: union — the target keeps its own and gains the source's.
    {
        const char* sql =
            "INSERT OR IGNORE INTO image_tags (image_id, tag_id) "
            "SELECT ?, tag_id FROM image_tags WHERE image_id = ?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to prepare tag transfer: " +
                                     std::string(sqlite3_errmsg(db_)));
        sqlite3_bind_int64(stmt, 1, to_id);
        sqlite3_bind_int64(stmt, 2, from_id);
        step_write_or_throw(db_, stmt, "Failed to transfer tags");
    }

    // Gallery: fill only an empty target — a user-set gallery on the target wins.
    {
        const char* sql =
            "UPDATE images SET gallery = (SELECT gallery FROM images WHERE id = ?) "
            "WHERE id = ? AND gallery IS NULL";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to prepare gallery transfer: " +
                                     std::string(sqlite3_errmsg(db_)));
        sqlite3_bind_int64(stmt, 1, from_id);
        sqlite3_bind_int64(stmt, 2, to_id);
        step_write_or_throw(db_, stmt, "Failed to transfer gallery");
    }

    // Faces: repoint only when the target has none. Checked in C++ first — a
    // NOT EXISTS in the UPDATE's WHERE would see the rows it just moved and
    // stop after one. Fresh detections on the target win over stale ones.
    {
        int64_t target_faces = 0;
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM faces WHERE image_id = ?", -1, &c,
                               nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to prepare face count: " +
                                     std::string(sqlite3_errmsg(db_)));
        sqlite3_bind_int64(c, 1, to_id);
        if (sqlite3_step(c) == SQLITE_ROW) target_faces = sqlite3_column_int64(c, 0);
        sqlite3_finalize(c);

        if (target_faces == 0) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE faces SET image_id = ? WHERE image_id = ?", -1,
                                   &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error("Failed to prepare face transfer: " +
                                         std::string(sqlite3_errmsg(db_)));
            sqlite3_bind_int64(stmt, 1, to_id);
            sqlite3_bind_int64(stmt, 2, from_id);
            step_write_or_throw(db_, stmt, "Failed to transfer faces");
        }
    }
}

std::vector<ImageInfo> Database::get_all_images(const std::string& path_prefix) {
    std::string sql =
        "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, "
        "width, height, metadata, media_type, duration_ms, gallery, "
        "EXISTS(SELECT 1 FROM image_tags it JOIN tags t ON t.id = it.tag_id "
        "WHERE it.image_id = images.id AND t.name = 'favorite'), blur_score, effective_date "
        "FROM images";
    std::optional<std::string> upper;
    if (!path_prefix.empty()) {
        upper = prefix_upper_bound(path_prefix);
        sql += " WHERE path >= ?";
        if (upper) sql += " AND path < ?";
    }
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, 1, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (upper) sqlite3_bind_text(stmt, 2, upper->c_str(), -1, SQLITE_TRANSIENT);
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
        info.effective_date = text(16);
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
    }
    std::optional<std::string> upper;
    if (!path_prefix.empty()) {
        upper = prefix_upper_bound(path_prefix);
        std::string bound = upper ? " AND path < ?" : "";
        sql =
            "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, "
            "phash, width, height, metadata, media_type, duration_ms FROM images "
            "WHERE path >= ?" +
            bound + " AND sha256 IN (SELECT sha256 FROM images WHERE path >= ?" + bound +
            " GROUP BY sha256 HAVING COUNT(*) > 1) ORDER BY sha256, path";
    }
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (!path_prefix.empty()) {
        int i = 1;
        sqlite3_bind_text(stmt, i++, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (upper) sqlite3_bind_text(stmt, i++, upper->c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, i++, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (upper) sqlite3_bind_text(stmt, i++, upper->c_str(), -1, SQLITE_TRANSIENT);
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
    exec("BEGIN IMMEDIATE TRANSACTION");
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

int Database::set_tag_by_ids(const std::vector<int64_t>& ids, const std::string& tag, bool add) {
    if (ids.empty()) return 0;
    int affected = 0;
    begin_transaction();
    try {
        if (add) {
            const char* ins_tag = "INSERT OR IGNORE INTO tags (name) VALUES (?)";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, ins_tag, -1, &st, nullptr) != SQLITE_OK) {
                throw std::runtime_error("prepare tag insert: " + std::string(sqlite3_errmsg(db_)));
            }
            sqlite3_bind_text(st, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
            step_write_or_throw(db_, st, "Failed to create tag");
        }
        // The add-path's SELECT filters to images that exist so an unknown id
        // is silently skipped rather than tripping image_tags' FK constraint
        // (INSERT OR IGNORE does not suppress foreign-key violations).
        const char* sql = add ? "INSERT OR IGNORE INTO image_tags (image_id, tag_id) "
                                "SELECT ?, id FROM tags WHERE name = ? "
                                "AND EXISTS (SELECT 1 FROM images WHERE id = ?)"
                              : "DELETE FROM image_tags WHERE image_id = ? AND tag_id = "
                                "(SELECT id FROM tags WHERE name = ?)";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            throw std::runtime_error("prepare image_tags: " + std::string(sqlite3_errmsg(db_)));
        }
        for (int64_t id : ids) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
            if (add) sqlite3_bind_int64(st, 3, id);
            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                throw std::runtime_error("apply tag: " + std::string(sqlite3_errmsg(db_)));
            }
            affected += sqlite3_changes(db_);
        }
        sqlite3_finalize(st);
        commit_transaction();
    } catch (...) {
        rollback_transaction();
        throw;
    }
    return affected;
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
    exec("BEGIN IMMEDIATE TRANSACTION");
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
    exec("BEGIN IMMEDIATE TRANSACTION");
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

int Database::set_gallery_by_ids(const std::vector<int64_t>& ids, const std::string& gallery) {
    if (ids.empty()) return 0;
    int affected = 0;
    begin_transaction();
    try {
        const char* sql = "UPDATE images SET gallery = ? WHERE id = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            throw std::runtime_error("prepare gallery update: " + std::string(sqlite3_errmsg(db_)));
        }
        for (int64_t id : ids) {
            sqlite3_reset(st);
            if (gallery.empty()) {
                sqlite3_bind_null(st, 1);
            } else {
                sqlite3_bind_text(st, 1, gallery.c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int64(st, 2, id);
            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                throw std::runtime_error("apply gallery: " + std::string(sqlite3_errmsg(db_)));
            }
            affected += sqlite3_changes(db_);
        }
        sqlite3_finalize(st);
        commit_transaction();
    } catch (...) {
        rollback_transaction();
        throw;
    }
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

void Database::build_filter_sql(const SearchCriteria& c, std::string& sql,
                                std::vector<std::pair<std::string, bool>>& binds) {
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
}

void Database::bind_filter_values(sqlite3_stmt* stmt,
                                  const std::vector<std::pair<std::string, bool>>& binds,
                                  int& idx) {
    for (const auto& b : binds) {
        if (b.second) {
            sqlite3_bind_int64(stmt, idx++, std::stoll(b.first));
        } else {
            sqlite3_bind_text(stmt, idx++, b.first.c_str(), -1, SQLITE_TRANSIENT);
        }
    }
}

std::vector<ImageInfo> Database::search(const SearchCriteria& c) {
    std::string sql =
        "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, "
        "width, height, metadata, media_type, duration_ms, blur_score FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;  // {value, is_int}
    build_filter_sql(c, sql, binds);

    sql += " ORDER BY path";

    if (c.limit > 0) {
        sql += " LIMIT " + std::to_string(c.limit);
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare search: " + std::string(sqlite3_errmsg(db_)));
    }

    int idx = 1;
    bind_filter_values(stmt, binds, idx);

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

namespace {
const char* sort_column(Database::SortKey k) {
    switch (k) {
        case Database::SortKey::Path:
            return "path";
        case Database::SortKey::Size:
            return "file_size";
        case Database::SortKey::Filename:
            return "filename";
        case Database::SortKey::Date:
        default:
            return "effective_date";
    }
}
}  // namespace

// Column order must match the SELECT list in search_page().
ImageInfo Database::row_to_image_info(sqlite3_stmt* stmt) {
    auto text = [&](int col) -> std::string {
        const unsigned char* p = sqlite3_column_text(stmt, col);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    ImageInfo info;
    info.id = sqlite3_column_int64(stmt, 0);
    info.path = text(1);
    info.filename = text(2);
    info.extension = text(3);
    info.file_size = sqlite3_column_int64(stmt, 4);
    info.created_at = text(5);
    info.modified_at = text(6);
    info.effective_date = text(7);
    info.sha256 = text(8);
    info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 9));
    info.width = sqlite3_column_int(stmt, 10);
    info.height = sqlite3_column_int(stmt, 11);
    info.metadata_json = text(12);
    info.media_type = text(13);
    info.duration_ms = sqlite3_column_int64(stmt, 14);
    info.gallery = text(15);
    info.blur_score =
        sqlite3_column_type(stmt, 16) == SQLITE_NULL ? -1.0 : sqlite3_column_double(stmt, 16);
    info.favorite = sqlite3_column_int(stmt, 17) != 0;
    return info;
}

std::vector<ImageInfo> Database::search_page(const PageCriteria& c) {
    std::string sql =
        "SELECT id, path, filename, extension, file_size, created_at, modified_at, "
        "effective_date, sha256, phash, width, height, metadata, media_type, "
        "duration_ms, gallery, blur_score, "
        "EXISTS(SELECT 1 FROM image_tags it JOIN tags t ON t.id = it.tag_id "
        "WHERE it.image_id = images.id AND t.name = 'favorite') "
        "FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;
    build_filter_sql(c.filter, sql, binds);

    const char* dir = c.descending ? "DESC" : "ASC";
    // id is the tiebreaker so equal sort keys never shuffle between pages.
    sql += std::string(" ORDER BY ") + sort_column(c.sort) + " " + dir + ", id " + dir +
           " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare page query: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    int idx = 1;
    bind_filter_values(stmt, binds, idx);
    sqlite3_bind_int(stmt, idx++, c.limit);
    sqlite3_bind_int64(stmt, idx++, c.offset);

    std::vector<ImageInfo> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_image_info(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t Database::count_matching(const SearchCriteria& filter) {
    std::string sql = "SELECT COUNT(*) FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;
    build_filter_sql(filter, sql, binds);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare count: " + std::string(sqlite3_errmsg(db_)));
    }
    int idx = 1;
    bind_filter_values(stmt, binds, idx);
    int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

bool Database::get_metadata_json(int64_t id, std::string& out) {
    const char* sql = "SELECT metadata FROM images WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare metadata query: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int64(stmt, 1, id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(stmt, 0);
        out = p ? reinterpret_cast<const char*>(p) : "";
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
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
