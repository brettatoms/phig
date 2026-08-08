// Isolated repro for plan 046 (vec_faces delete failure). Not part of the
// build. Reproduces phig's connection setup and DDL exactly, then runs the
// statement that throws "Failed to delete face vectors: SQL logic error".
//
// Run this on a machine where the three DatabaseTest cases fail, and report
// what it prints along with sqlite3_libversion(). On macOS (Apple SQLite
// 3.51.0 and Homebrew 3.53.4) all four variants succeed, which is why the
// SQLite the platform provides is now the prime suspect rather than anything
// in phig's own setup.
//
//   clang -std=c11 -O2 -c -DSQLITE_CORE -DSQLITE_VEC_STATIC \
//       -Ivendor/sqlite-vec vendor/sqlite-vec/sqlite-vec.c -o /tmp/sv.o
//   clang++ -std=c++20 -O2 -DSQLITE_CORE -DSQLITE_VEC_STATIC \
//       -Ivendor/sqlite-vec docs/plans/2026-08-07-046-vec-delete-repro.cpp \
//       /tmp/sv.o -lsqlite3 -o /tmp/vecdel
//   /tmp/vecdel        # DELETE with no transaction open
//   /tmp/vecdel txn    # DELETE inside BEGIN IMMEDIATE
//
// To test a different SQLite than the default, add its -I/-L to both commands.

#include <sqlite3.h>

#include <array>
#include <cstdio>
#include <filesystem>

#include "sqlite-vec.h"

static void ex(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::printf("  setup failed [%s]: %s\n", sql, err ? err : "?");
    }
}

int main(int argc, char** argv) {
    // File-backed, not :memory: -- WAL is a no-op on an in-memory database, so
    // an in-memory repro is not phig's setup.
    bool in_txn = argc > 1 && argv[1][0] == 't';
    std::printf("sqlite %s%s\n", sqlite3_libversion(),
                in_txn ? ", DELETE inside BEGIN IMMEDIATE" : ", DELETE with no txn");
    std::filesystem::create_directories("/tmp/phig046");
    std::remove("/tmp/phig046/vecdel.db");
    std::remove("/tmp/phig046/vecdel.db-wal");
    std::remove("/tmp/phig046/vecdel.db-shm");
    sqlite3* db = nullptr;
    sqlite3_open("/tmp/phig046/vecdel.db", &db);

    // core/database.cpp open sequence, in order.
    sqlite3_vec_init(db, nullptr, nullptr);
    ex(db, "PRAGMA journal_mode=WAL");
    sqlite3_busy_timeout(db, 30000);
    ex(db, "PRAGMA foreign_keys=ON");

    ex(db,
       "CREATE TABLE images (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
       " sha256 TEXT NOT NULL, phash INTEGER NOT NULL)");
    ex(db, "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)");
    ex(db,
       "CREATE TABLE faces (id INTEGER PRIMARY KEY,"
       " image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
       " embedding BLOB NOT NULL, x INTEGER, y INTEGER, width INTEGER, height INTEGER,"
       " person_id INTEGER REFERENCES people(id))");
    ex(db, "CREATE INDEX idx_faces_image ON faces(image_id)");
    ex(db, "CREATE VIRTUAL TABLE vec_faces USING vec0(face_id INTEGER PRIMARY KEY,"
           " embedding float[128])");

    ex(db, "INSERT INTO images(id, path, sha256, phash) VALUES (1, '/a.jpg', 'sha', 0)");
    std::array<float, 128> e{};
    e.fill(0.25f);
    {
        sqlite3_stmt* s;
        sqlite3_prepare_v2(db,
                           "INSERT INTO faces(image_id, embedding, x, y, width, height)"
                           " VALUES (1, ?, 1, 2, 3, 4)",
                           -1, &s, nullptr);
        sqlite3_bind_blob(s, 1, e.data(), 512, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
        sqlite3_prepare_v2(db, "INSERT INTO vec_faces(face_id, embedding) VALUES (?, ?)", -1, &s,
                           nullptr);
        sqlite3_bind_int64(s, 1, sqlite3_last_insert_rowid(db));
        sqlite3_bind_blob(s, 2, e.data(), 512, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    // The statement that throws "Failed to delete face vectors: SQL logic error".
    const char* sql = "DELETE FROM vec_faces WHERE face_id IN (SELECT id FROM faces WHERE"
                      " image_id = ?)";
    if (in_txn) ex(db, "BEGIN IMMEDIATE TRANSACTION");
    sqlite3_stmt* s;
    int rc = sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    std::printf("  prepare: rc=%d (%s)\n", rc, rc == SQLITE_OK ? "ok" : sqlite3_errmsg(db));
    sqlite3_bind_int64(s, 1, 1);
    rc = sqlite3_step(s);
    std::printf("  step:    rc=%d %s (%s)\n", rc, rc == SQLITE_DONE ? "DONE" : "FAILED",
                sqlite3_errmsg(db));
    sqlite3_finalize(s);
    std::printf("  vec_faces rows left: ");
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM vec_faces", -1, &s, nullptr);
    if (sqlite3_step(s) == SQLITE_ROW) std::printf("%lld\n", sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);
    return 0;
}
