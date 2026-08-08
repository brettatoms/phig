// Measurement harness for plan 045 (faces.id rowid reuse). Not part of the
// build; it exists so the numbers in plans/045-face-rowid-reuse.md can be
// re-derived rather than trusted.
//
// Answers, against real SQLite + phig's vendored sqlite-vec, on a schema
// copied from core/database.cpp:
//   1. rowid reuse with and without AUTOINCREMENT
//   2. per-insert cost of AUTOINCREMENT on the faces write path
//   3. cost and correctness of the v8 table-copy migration at scale
//   4. what a reused id does when a vec_faces row outlives its faces row
//   5. why the same recipe is unsafe on the other bare-rowid tables
//
// Link the SQLite phig links, not a newer one from a package manager -- on
// macOS CMake resolves to Apple's /usr/lib/libsqlite3.dylib via the SDK .tbd.
//
//   clang -std=c11 -O2 -c -DSQLITE_CORE -DSQLITE_VEC_STATIC \
//       -Ivendor/sqlite-vec vendor/sqlite-vec/sqlite-vec.c -o /tmp/sqlite-vec.o
//   clang++ -std=c++20 -O2 -DSQLITE_CORE -DSQLITE_VEC_STATIC \
//       -Ivendor/sqlite-vec docs/plans/2026-08-07-045-rowid-reuse-bench.cpp \
//       /tmp/sqlite-vec.o -lsqlite3 -o /tmp/bench
//   /tmp/bench [insert_rows] [migration_rows] [reps]     # default 100000 1000000 3
//
// Timings must be taken on an otherwise idle machine. A concurrent build makes
// phase 2 report AUTOINCREMENT as *faster* than a plain rowid, which it is not.

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "sqlite-vec.h"

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

[[noreturn]] void die(const char* what, sqlite3* db) {
    std::fprintf(stderr, "FAIL %s: %s\n", what, db ? sqlite3_errmsg(db) : "?");
    std::exit(1);
}

void ex(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::fprintf(stderr, "FAIL exec [%s]: %s\n", sql, err ? err : "?");
        std::exit(1);
    }
}

int64_t scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) die(sql, db);
    int64_t v = -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

// phig's connection setup: WAL, busy timeout, foreign keys on, vec registered.
sqlite3* open_fresh(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) die("open", db);
    sqlite3_vec_init(db, nullptr, nullptr);
    ex(db, "PRAGMA journal_mode=WAL");
    sqlite3_busy_timeout(db, 30000);
    ex(db, "PRAGMA foreign_keys=ON");
    return db;
}

// faces/images/people/vec_faces exactly as core/database.cpp declares them.
// `autoinc` picks the id declaration under test.
void create_schema(sqlite3* db, bool autoinc) {
    ex(db,
       "CREATE TABLE images (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
       " sha256 TEXT NOT NULL, phash INTEGER NOT NULL)");
    ex(db, "CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)");
    std::string faces = std::string("CREATE TABLE faces (id INTEGER PRIMARY KEY") +
                        (autoinc ? " AUTOINCREMENT" : "") +
                        ", image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
                        " embedding BLOB NOT NULL, x INTEGER, y INTEGER, width INTEGER,"
                        " height INTEGER, person_id INTEGER REFERENCES people(id))";
    ex(db, faces.c_str());
    ex(db, "CREATE INDEX idx_faces_image ON faces(image_id)");
    ex(db,
       "CREATE VIRTUAL TABLE vec_faces USING vec0("
       "face_id INTEGER PRIMARY KEY, embedding float[128])");
    ex(db, "INSERT INTO images(id, path, sha256, phash) VALUES (1, '/p/a.jpg', 'sha', 0)");
    ex(db, "INSERT INTO people(id, name) VALUES (1, 'Alice')");
}

std::array<float, 128> emb(float seed) {
    std::array<float, 128> e{};
    e.fill(seed);
    return e;
}

// core/database.cpp:1602 insert_face: faces row, then vec_faces keyed on its id.
int64_t insert_face(sqlite3* db, const std::array<float, 128>& e, bool with_vec = true) {
    sqlite3_stmt* s;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO faces(image_id, embedding, x, y, width, height)"
                           " VALUES (1, ?, 1, 2, 3, 4)",
                           -1, &s, nullptr) != SQLITE_OK)
        die("prep face", db);
    sqlite3_bind_blob(s, 1, e.data(), 512, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_DONE) die("insert face", db);
    sqlite3_finalize(s);
    int64_t id = sqlite3_last_insert_rowid(db);
    if (!with_vec) return id;
    if (sqlite3_prepare_v2(db, "INSERT INTO vec_faces(face_id, embedding) VALUES (?, ?)", -1, &s,
                           nullptr) != SQLITE_OK)
        die("prep vec", db);
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_blob(s, 2, e.data(), 512, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_DONE) die("insert vec", db);
    sqlite3_finalize(s);
    return id;
}

void delete_face(sqlite3* db, int64_t id) {
    char sql[128];
    std::snprintf(sql, sizeof(sql), "DELETE FROM vec_faces WHERE face_id = %lld", (long long)id);
    ex(db, sql);
    std::snprintf(sql, sizeof(sql), "DELETE FROM faces WHERE id = %lld", (long long)id);
    ex(db, sql);
}

void populate(sqlite3* db, int64_t n, bool with_vec) {
    ex(db, "BEGIN");
    for (int64_t i = 0; i < n; i++) insert_face(db, emb(static_cast<float>(i) * 1e-6f), with_vec);
    ex(db, "COMMIT");
}

// ---------------------------------------------------------------- phases

void phase_reuse() {
    std::printf("\n== 1. rowid reuse ==\n");
    for (bool autoinc : {false, true}) {
        sqlite3* db = open_fresh("/tmp/phig045/reuse.db");
        create_schema(db, autoinc);
        int64_t doomed = insert_face(db, emb(5.0f));
        delete_face(db, doomed);
        int64_t next = insert_face(db, emb(99.0f));
        std::printf("  %-14s deleted id=%lld, next insert got id=%lld  -> %s\n",
                    autoinc ? "AUTOINCREMENT" : "plain rowid", (long long)doomed, (long long)next,
                    next == doomed ? "REUSED" : "not reused");
        sqlite3_close(db);
    }
}

void phase_insert_rate(int64_t n, int reps) {
    std::printf("\n== 2. insert cost, %lld faces into an empty table, %d reps ==\n", (long long)n,
                reps);
    for (bool with_vec : {false, true}) {
        // Alternate the two configurations rep by rep so a drift in machine
        // load spreads across both instead of landing on one. Report the min:
        // noise only ever adds time, so the fastest run is the one least
        // contaminated by it.
        double best[2] = {1e18, 1e18};
        for (int r = 0; r < reps; r++) {
            for (int autoinc = 0; autoinc < 2; autoinc++) {
                sqlite3* db = open_fresh("/tmp/phig045/rate.db");
                create_schema(db, autoinc != 0);
                auto t0 = Clock::now();
                populate(db, n, with_vec);
                double dt = since(t0);
                if (dt < best[autoinc]) best[autoinc] = dt;
                sqlite3_close(db);
            }
        }
        std::printf("  %-18s plain %.3fs   AUTOINCREMENT %.3fs   delta %+.1f%%\n",
                    with_vec ? "faces + vec_faces" : "faces only", best[0], best[1],
                    100.0 * (best[1] - best[0]) / best[0]);
    }
}

void phase_migration(int64_t n) {
    std::printf("\n== 3. v8 table-copy migration, %lld faces (+ %lld vec rows) ==\n", (long long)n,
                (long long)n);
    sqlite3* db = open_fresh("/tmp/phig045/mig.db");
    create_schema(db, /*autoinc=*/false);
    auto t0 = Clock::now();
    populate(db, n, /*with_vec=*/true);
    std::printf("  setup: populated in %.1fs\n", since(t0));

    int64_t before_faces = scalar(db, "SELECT COUNT(*) FROM faces");
    int64_t before_vec = scalar(db, "SELECT COUNT(*) FROM vec_faces");
    int64_t before_max = scalar(db, "SELECT MAX(id) FROM faces");
    ex(db, "UPDATE faces SET person_id = 1 WHERE id = 1");

    // The migration, run the way core/database.cpp:463 runs one: inside
    // BEGIN IMMEDIATE, on a connection that already set foreign_keys=ON
    // (so the pragma cannot be toggled off here -- it is a no-op in a txn).
    std::printf("  foreign_keys at start: %lld\n", scalar(db, "PRAGMA foreign_keys"));
    t0 = Clock::now();
    ex(db, "BEGIN IMMEDIATE");
    std::printf("  foreign_keys inside txn after trying to disable: ");
    ex(db, "PRAGMA foreign_keys=OFF");
    std::printf("%lld\n", scalar(db, "PRAGMA foreign_keys"));
    ex(db,
       "CREATE TABLE faces_new (id INTEGER PRIMARY KEY AUTOINCREMENT,"
       " image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
       " embedding BLOB NOT NULL, x INTEGER, y INTEGER, width INTEGER, height INTEGER,"
       " person_id INTEGER REFERENCES people(id))");
    ex(db,
       "INSERT INTO faces_new(id, image_id, embedding, x, y, width, height, person_id)"
       " SELECT id, image_id, embedding, x, y, width, height, person_id FROM faces");
    ex(db, "DROP TABLE faces");
    ex(db, "ALTER TABLE faces_new RENAME TO faces");
    ex(db, "CREATE INDEX idx_faces_image ON faces(image_id)");
    ex(db, "COMMIT");
    double dt = since(t0);
    std::printf("  MIGRATION: %.2fs for %lld faces (%.1f us/row)\n", dt, (long long)n,
                1e6 * dt / static_cast<double>(n));

    // Invariants.
    std::printf("  rows      %lld -> %lld %s\n", (long long)before_faces,
                (long long)scalar(db, "SELECT COUNT(*) FROM faces"),
                scalar(db, "SELECT COUNT(*) FROM faces") == before_faces ? "ok" : "LOST ROWS");
    std::printf("  max(id)   %lld -> %lld %s\n", (long long)before_max,
                (long long)scalar(db, "SELECT MAX(id) FROM faces"),
                scalar(db, "SELECT MAX(id) FROM faces") == before_max ? "ok (ids preserved)"
                                                                     : "IDS CHANGED");
    std::printf("  vec_faces %lld -> %lld %s\n", (long long)before_vec,
                (long long)scalar(db, "SELECT COUNT(*) FROM vec_faces"),
                scalar(db, "SELECT COUNT(*) FROM vec_faces") == before_vec ? "ok (untouched)"
                                                                          : "DIVERGED");
    std::printf("  person_id survived: %s\n",
                scalar(db, "SELECT person_id FROM faces WHERE id = 1") == 1 ? "yes" : "NO");
    std::printf("  sqlite_sequence seeded to %lld (max id %lld) %s\n",
                (long long)scalar(db, "SELECT seq FROM sqlite_sequence WHERE name = 'faces'"),
                (long long)before_max,
                scalar(db, "SELECT seq FROM sqlite_sequence WHERE name='faces'") == before_max
                    ? "ok"
                    : "MISMATCH");
    std::printf("  orphan check (vec rows with no face): %lld\n",
                scalar(db, "SELECT COUNT(*) FROM vec_faces v LEFT JOIN faces f"
                           " ON f.id = v.face_id WHERE f.id IS NULL"));

    // KNN still works, and reuse is gone on the migrated table.
    {
        auto q = emb(0.0f);
        sqlite3_stmt* s;
        if (sqlite3_prepare_v2(db, "SELECT face_id, distance FROM vec_faces"
                                   " WHERE embedding MATCH ? AND k = 1",
                               -1, &s, nullptr) != SQLITE_OK)
            die("prep knn", db);
        sqlite3_bind_blob(s, 1, q.data(), 512, SQLITE_STATIC);
        std::printf("  KNN after migration: %s\n",
                    sqlite3_step(s) == SQLITE_ROW ? "returns a row" : "NO ROW");
        sqlite3_finalize(s);
    }
    int64_t top = scalar(db, "SELECT MAX(id) FROM faces");
    delete_face(db, top);
    int64_t next = insert_face(db, emb(99.0f));
    std::printf("  post-migration: deleted top id=%lld, next insert got %lld -> %s\n",
                (long long)top, (long long)next, next == top ? "STILL REUSED" : "no reuse");
    sqlite3_close(db);
}

void phase_orphan_collision() {
    std::printf("\n== 4. reused id meets an orphaned vec_faces row ==\n");
    // What happens today if a faces row is deleted while its vector survives
    // (the failure mode plan 046 describes) and the id is then handed out again.
    sqlite3* db = open_fresh("/tmp/phig045/orphan.db");
    create_schema(db, /*autoinc=*/false);
    int64_t doomed = insert_face(db, emb(5.0f));
    ex(db, "DELETE FROM faces WHERE id = 1");  // vec_faces row deliberately left behind
    std::printf("  faces rows %lld, vec_faces rows %lld (orphan left)\n",
                (long long)scalar(db, "SELECT COUNT(*) FROM faces"),
                (long long)scalar(db, "SELECT COUNT(*) FROM vec_faces"));

    // A KNN query now returns a face id that no longer exists.
    {
        auto q = emb(5.0f);
        sqlite3_stmt* s;
        sqlite3_prepare_v2(db, "SELECT face_id FROM vec_faces WHERE embedding MATCH ? AND k = 1",
                           -1, &s, nullptr);
        sqlite3_bind_blob(s, 1, q.data(), 512, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW)
            std::printf("  find_closest_face would return id=%lld (deleted face)\n",
                        (long long)sqlite3_column_int64(s, 0));
        sqlite3_finalize(s);
    }

    // Now reuse the id. The faces insert succeeds; the vec insert collides.
    sqlite3_stmt* s;
    sqlite3_prepare_v2(db,
                       "INSERT INTO faces(image_id, embedding, x, y, width, height)"
                       " VALUES (1, ?, 1, 2, 3, 4)",
                       -1, &s, nullptr);
    auto e = emb(99.0f);
    sqlite3_bind_blob(s, 1, e.data(), 512, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
    int64_t reused = sqlite3_last_insert_rowid(db);
    std::printf("  reinserted face got id=%lld (%s)\n", (long long)reused,
                reused == doomed ? "reused the orphan's id" : "fresh id");
    sqlite3_prepare_v2(db, "INSERT INTO vec_faces(face_id, embedding) VALUES (?, ?)", -1, &s,
                       nullptr);
    sqlite3_bind_int64(s, 1, reused);
    sqlite3_bind_blob(s, 2, e.data(), 512, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    std::printf("  vec_faces insert on the reused id: rc=%d %s (%s)\n", rc,
                rc == SQLITE_DONE ? "SUCCEEDED" : "FAILED", sqlite3_errmsg(db));
    sqlite3_finalize(s);
    std::printf("  vec_faces rows now %lld\n",
                (long long)scalar(db, "SELECT COUNT(*) FROM vec_faces"));
    sqlite3_close(db);
}

// Does the same migration recipe work on a table that is an FK *parent*?
// `faces` is a pure child -- nothing references it. `images`, `people`, `tags`
// and `operations` are all referenced by something, and the standard 12-step
// ALTER procedure requires foreign_keys=OFF, which phase 3 showed cannot be
// turned off inside the transaction the migration loop opens.
void phase_parent_tables() {
    std::printf("\n== 5. same recipe on an FK parent ==\n");

    // --- images: children reference it with ON DELETE CASCADE ---
    {
        sqlite3* db = open_fresh("/tmp/phig045/parent.db");
        create_schema(db, /*autoinc=*/false);
        ex(db,
           "CREATE TABLE image_tags (image_id INTEGER NOT NULL REFERENCES images(id)"
           " ON DELETE CASCADE, tag_id INTEGER NOT NULL, PRIMARY KEY (image_id, tag_id))");
        ex(db, "INSERT INTO image_tags(image_id, tag_id) VALUES (1, 1)");
        for (int i = 0; i < 5; i++) insert_face(db, emb(static_cast<float>(i)));
        std::printf("  images: before -> faces=%lld image_tags=%lld vec_faces=%lld\n",
                    (long long)scalar(db, "SELECT COUNT(*) FROM faces"),
                    (long long)scalar(db, "SELECT COUNT(*) FROM image_tags"),
                    (long long)scalar(db, "SELECT COUNT(*) FROM vec_faces"));

        ex(db, "BEGIN IMMEDIATE");
        ex(db, "PRAGMA foreign_keys=OFF");  // no-op inside a txn, as phase 3 showed
        ex(db,
           "CREATE TABLE images_new (id INTEGER PRIMARY KEY AUTOINCREMENT,"
           " path TEXT NOT NULL UNIQUE, sha256 TEXT NOT NULL, phash INTEGER NOT NULL)");
        ex(db,
           "INSERT INTO images_new(id, path, sha256, phash)"
           " SELECT id, path, sha256, phash FROM images");
        char* err = nullptr;
        int rc = sqlite3_exec(db, "DROP TABLE images", nullptr, nullptr, &err);
        std::printf("  images: DROP TABLE rc=%d (%s)\n", rc, rc == SQLITE_OK ? "ok" : err);
        if (rc == SQLITE_OK) {
            ex(db, "ALTER TABLE images_new RENAME TO images");
            std::printf("  images: after DROP -> faces=%lld image_tags=%lld vec_faces=%lld\n",
                        (long long)scalar(db, "SELECT COUNT(*) FROM faces"),
                        (long long)scalar(db, "SELECT COUNT(*) FROM image_tags"),
                        (long long)scalar(db, "SELECT COUNT(*) FROM vec_faces"));
            std::printf("  images: faces still pointing at a live image: %lld\n",
                        (long long)scalar(db, "SELECT COUNT(*) FROM faces f JOIN images i"
                                              " ON i.id = f.image_id"));
        }
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    // --- people: faces.person_id references it with no ON DELETE action ---
    {
        sqlite3* db = open_fresh("/tmp/phig045/parent2.db");
        create_schema(db, /*autoinc=*/false);
        int64_t f = insert_face(db, emb(1.0f));
        char sql[128];
        std::snprintf(sql, sizeof(sql), "UPDATE faces SET person_id = 1 WHERE id = %lld",
                      (long long)f);
        ex(db, sql);
        ex(db, "BEGIN IMMEDIATE");
        ex(db, "CREATE TABLE people_new (id INTEGER PRIMARY KEY AUTOINCREMENT,"
               " name TEXT NOT NULL UNIQUE)");
        ex(db, "INSERT INTO people_new(id, name) SELECT id, name FROM people");
        char* err = nullptr;
        int rc = sqlite3_exec(db, "DROP TABLE people", nullptr, nullptr, &err);
        std::printf("  people: DROP TABLE rc=%d (%s)\n", rc, rc == SQLITE_OK ? "ok" : err);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int64_t rate_n = argc > 1 ? std::atoll(argv[1]) : 100000;
    int64_t mig_n = argc > 2 ? std::atoll(argv[2]) : 1000000;
    int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    std::filesystem::create_directories("/tmp/phig045");
    std::printf("sqlite %s, sqlite-vec linked\n", sqlite3_libversion());
    phase_reuse();
    phase_insert_rate(rate_n, reps);
    phase_migration(mig_n);
    phase_orphan_collision();
    phase_parent_tables();
    return 0;
}
