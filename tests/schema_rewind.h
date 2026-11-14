#pragma once

#include <sqlite3.h>

#include <string>

namespace phig_test {

// Turns a current database into a genuine v7 one: images and faces back to a
// plain INTEGER PRIMARY KEY, faces without the v10 provenance columns, version
// stamped 7. Rows are copied with their ids, so whatever the caller wrote,
// orphans included, is what the v7 database holds.
inline std::string rewind_to_v7(sqlite3* db) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, R"(
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
    )",
                                nullptr, nullptr, &err);
    std::string failure = rc == SQLITE_OK ? "" : (err ? err : "unknown sqlite error");
    sqlite3_free(err);
    return failure;
}

}  // namespace phig_test
