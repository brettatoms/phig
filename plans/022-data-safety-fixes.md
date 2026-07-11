---
name: Data-Safety Fixes (2026-07-11 review)
status: completed
created: 2026-07-11
description: >
  Fix the eight mechanical findings from the 2026-07-11 data-safety review:
  scan re-insert destroying tags/favorites/gallery/faces, mv silently
  overwriting files that appear at the destination after planning, unlogged
  scan removals, purge skipping the schema guard, LIKE wildcard over-matching
  in --path, missing fsync in the copy paths, unchecked short reads in
  compute_sha256, vec_faces orphans, and non-transactional first schema init.
---

# Data-Safety Fixes

## Background

A follow-up data-safety review on 2026-07-11 (after the plan 020 activity-log
work) found nine issues. This plan fixes the eight that change existing
behavior mechanically. The ninth — re-linking user metadata across external
file moves by sha256 — is new behavior and lives in plan
[023](023-scan-relink.md).

Findings fixed here, by review number:

| # | Severity | Problem |
|---|----------|---------|
| 1 | HIGH | `Database::insert` uses `INSERT OR REPLACE`; re-scanning a changed file (or any `scan --force`) deletes the row and cascades away its tags, favorites, and faces, and resets gallery to NULL. |
| 2 | HIGH | `move_file` uses plain `fs::rename`, which silently overwrites a destination file that appeared after the planning phase (concurrent phig runs, sync apps). cp re-checks at execution time; mv does not. |
| 3a | MED | scan's automatic removal of missing rows uses unlogged `delete_paths` — no operation record, no restore snapshot — while purge gets both. |
| 4 | MED | `cmd_purge` never calls `init_schema`, bypassing the "DB newer than this phig" guard and pending migrations. |
| 5 | MED | `get_all_images(path_prefix)` uses `LIKE` with an unescaped prefix; `_`/`%` in directory names over-match, so `tag`/`gallery`/`fav --path` write metadata to unintended rows. |
| 6 | MED-LOW | `atomic_copy_file` never fsyncs; on cross-device mv the source is unlinked before the copied bytes are durable, so a power loss can destroy the only copy. |
| 7 | LOW | `compute_sha256` ignores a failed/short read and silently hashes a zero-padded buffer. |
| 8 | LOW | Deletes that bypass `delete_faces_for_image` (`update_path_in_txn` stale-dest DELETE, `insert_copy` REPLACE) leave orphaned `vec_faces` embeddings. |
| 9 | LOW | First-ever schema creation is not transactional; a crash partway leaves a versionless partial schema that the next run misdetects as a v1 pre-migration DB, wedging it on the v2 migration. |

## Design decisions

**#1 — what survives a re-scan.** Row data splits into user-authored (tags,
favorites, gallery, face person labels) and pixel-derived (sha256, phash,
dimensions, blur score, face boxes/embeddings). The fix replaces `INSERT OR
REPLACE` with `INSERT ... ON CONFLICT(path) DO UPDATE`, which preserves the row
id (so tags survive) and never touches `gallery`. Faces are conditional: if the
new sha256 equals the stored one (touched file, `--force` on unchanged bytes),
faces and their person labels are kept; if the content actually changed, the
old faces describe dead pixels and are deleted via `delete_faces_for_image`
(which also cleans `vec_faces`). The scan's face-insertion path then only
inserts detections for rows that currently have no faces, so a `--force` pass
cannot duplicate face rows. `phig face rebuild` remains the way to force
re-detection.

**#2 — no-clobber rename.** A new `rename_noreplace` helper uses
`renameat2(RENAME_NOREPLACE)` on Linux and `renamex_np(RENAME_EXCL)` on macOS,
falling back to `link`+`unlink` where the filesystem doesn't support the flag,
and finally to a check-then-rename (tiny TOCTOU window, still far safer than a
blind rename). `move_file` uses it for both the same-device rename and the
final rename of the cross-device copy path. An existing destination becomes a
per-file error surfaced by `cmd_organize`, never an overwrite.

**#6 — fsync scope.** fsync is added where user data is being placed:
`atomic_copy_file` (cp, and the cross-device mv copy) fsyncs the temp file
before the rename and best-effort fsyncs the parent directory after. The
same-filesystem mv path is a pure rename and gains no fsync (no perf change for
the common case). `atomic_write_file` (thumbnails — a regenerable cache) gets
no fsync, so `scan --thumbs` throughput is unaffected.

**#8 — insert_copy keeps REPLACE semantics.** When cp lands on a
stale tracked dest row (file gone, row remains), the old row described a dead
file; replacing it (and cascading its tags) is correct. Only the vec_faces
cleanup is added. `update_path_in_txn` likewise cleans faces before dropping
the stale dest row.

Out of scope: `Database::purge(prefix, glob)` also uses LIKE but is only called
from tests (production purge matches in C++ via `passes_filters`); left
untouched. `get_exact_duplicates(path_prefix)` gets the same range fix as
`get_all_images` since it shares the pattern and has tests.

## Testing

Per the established pattern: every fix ships with tests exercising the real
implementation against real files and real SQLite databases (temp dirs), not
mocks. Fixes without an observable unit-level behavior (fsync durability, the
purge schema guard wiring in `main.cpp`) get an end-to-end verification step
using the built binary instead of a fake test.

## Checklist

- [ ] #1: `insert` upsert preserving id/tags/gallery; conditional face clear; scan face-insert guard
- [ ] #8: vec_faces cleanup in `update_path_in_txn` + `insert_copy`
- [ ] #2: `rename_noreplace` + `move_file` no-clobber (same-device and cross-device)
- [ ] #6: fsync temp + parent dir in `atomic_copy_file`
- [ ] #3a: scan removals logged as a `scan` operation with restore snapshots
- [ ] #4: `cmd_purge` calls `init_schema`
- [ ] #5: range comparison instead of LIKE in `get_all_images` / `get_exact_duplicates`
- [ ] #7: `compute_sha256` throws on short/failed read
- [ ] #9: fresh schema creation and pre-migration re-establishment each run in one transaction

---

# Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the eight mechanical data-safety holes from the 2026-07-11 review so phig cannot destroy user files or user-authored metadata through normal operation.

**Architecture:** Point fixes to existing functions in `phig-lib` (`database.cpp`, `paths.cpp`, `hasher.cpp`) plus small wiring changes in `main.cpp`. No new files, no schema change.

**Tech Stack:** C++20, SQLite, GoogleTest. Build via `direnv exec . cmake --build build`; a bare shell has a glibc/include mismatch.

## Global Constraints

- Build/test command: `direnv exec . cmake --build build && ./build/phig-tests`
- Formatting: clang-format (Google style, 4-space, 100-col) on `src/` and `tests/`
- Commit messages: plain, no AI attribution of any kind
- Every task ends with the FULL test suite passing, not just the new tests

---

### Task 1: Upsert in `Database::insert` — re-scan must not destroy user metadata (#1)

**Files:**
- Modify: `src/database.cpp` (function `Database::insert`, ~line 356)
- Modify: `src/main.cpp` (lambda `flush_batch` inside `cmd_scan`, ~line 382)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: existing `Database::delete_faces_for_image(int64_t)`, `get_image_id`, `has_faces`
- Produces: `Database::insert(const ImageInfo&)` with new semantics — preserves row id and `gallery` on path conflict; deletes the row's faces (and vec_faces) if and only if the incoming `sha256` differs from the stored one. Signature unchanged.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_database.cpp` (uses the existing `DatabaseTest` fixture and `make_image` helper). Add `#include "faces.h"` if not already present, and this file-scope helper above the tests:

```cpp
static int64_t count_rows(sqlite3* h, const char* sql) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(h, sql, -1, &s, nullptr) != SQLITE_OK) return -1;
    int64_t n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}
```

```cpp
TEST_F(DatabaseTest, ReinsertPreservesRowIdAndTags) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    int64_t id = db->get_image_id("/test/a.jpg");
    db->set_tag({"/test/a.jpg"}, "favorite", true);

    auto changed = make_image("/test/a.jpg", "sha2");  // content changed
    changed.file_size = 2000;
    db->insert(changed);

    EXPECT_EQ(db->get_image_id("/test/a.jpg"), id);
    auto tags = db->get_tags_for_image("/test/a.jpg");
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
    // Scalar columns still updated
    for (const auto& img : db->get_all_images()) {
        if (img.path == "/test/a.jpg") {
            EXPECT_EQ(img.sha256, "sha2");
            EXPECT_EQ(img.file_size, 2000);
        }
    }
}

TEST_F(DatabaseTest, ReinsertPreservesGallery) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->set_gallery({"/test/a.jpg"}, "summer", false);
    db->insert(make_image("/test/a.jpg", "sha2"));
    bool found = false;
    for (const auto& img : db->get_all_images()) {
        if (img.path == "/test/a.jpg") {
            found = true;
            EXPECT_EQ(img.gallery, "summer");
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(DatabaseTest, ReinsertSameContentKeepsFaces) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    int64_t id = db->get_image_id("/test/a.jpg");
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.x = 1; face.y = 2; face.width = 10; face.height = 10;
    db->insert_face(id, face);

    db->insert(make_image("/test/a.jpg", "sha1"));  // same bytes (e.g. --force)
    EXPECT_TRUE(db->has_faces("/test/a.jpg"));
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM vec_faces"), 1);
}

TEST_F(DatabaseTest, ReinsertChangedContentClearsFacesAndVecFaces) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    int64_t id = db->get_image_id("/test/a.jpg");
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.x = 1; face.y = 2; face.width = 10; face.height = 10;
    db->insert_face(id, face);

    db->insert(make_image("/test/a.jpg", "sha2"));  // content changed
    EXPECT_FALSE(db->has_faces("/test/a.jpg"));
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM faces"), 0);
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM vec_faces"), 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.Reinsert*'`
Expected: all four FAIL (id changes, tags/gallery gone, vec_faces count wrong).

- [ ] **Step 3: Rewrite `Database::insert`**

Replace the SQL and add the pre-insert face check. The whole function becomes:

```cpp
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
            const char* old_sha =
                reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
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
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, metadata, media_type, duration_ms, blur_score)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
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
            blur_score = excluded.blur_score
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
```

Note: the existing test `InsertReplaceOnDuplicatePath` asserts scalar columns
update on re-insert; the upsert keeps it passing. If it asserts a *new* row id,
update the test — id preservation is now the required behavior.

- [ ] **Step 4: Guard the scan's face insertion against duplicates**

In `src/main.cpp`, inside `cmd_scan`'s `flush_batch` lambda, the face-insert
loop currently inserts `r.faces` unconditionally. Change it to skip rows that
still have faces (kept because content was unchanged; `insert()` already
cleared faces when content changed):

```cpp
            // Insert faces (need image_id from DB) and auto-label known people
            for (auto& r : batch) {
                if (!r.faces.empty()) {
                    int64_t image_id = db.get_image_id(r.info.path);
                    // Rows whose bytes didn't change kept their existing faces
                    // (and person labels) through insert(); re-adding this
                    // run's detections would duplicate them. insert() cleared
                    // faces when the content changed, so has_faces == false
                    // means these detections are needed. `face rebuild` is the
                    // path for forced re-detection.
                    if (image_id >= 0 && !db.has_faces(r.info.path)) {
                        for (const auto& face : r.faces) {
                            db.insert_face(image_id, face);
                            int64_t face_id = sqlite3_last_insert_rowid(db.db_ptr());
                            db.auto_label_face(face_id, face.embedding);
                        }
                        face_count.fetch_add(r.faces.size());
                    }
                }
            }
```

- [ ] **Step 5: Run the full suite**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS, including the four new tests.

- [ ] **Step 6: Commit**

```bash
git add src/database.cpp src/main.cpp tests/test_database.cpp
git commit -m "Preserve tags/gallery/faces across re-scan: upsert instead of REPLACE"
```

---

### Task 2: Clean vec_faces on stale-dest delete and insert_copy replace (#8)

**Files:**
- Modify: `src/database.cpp` (`update_path_in_txn` ~line 607, `insert_copy` ~line 413)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: `delete_faces_for_image`, `get_image_id` (both existing)
- Produces: same signatures; `update_path_in_txn` and `insert_copy` now clear the stale dest row's faces + vec_faces before deleting/replacing it.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(DatabaseTest, UpdatePathOntoStaleDestClearsItsFaces) {
    // A tracked dest row with a face; its file is about to be replaced by mv.
    db->insert(make_image("/dst/b.jpg", "shaB"));
    int64_t dest_id = db->get_image_id("/dst/b.jpg");
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.x = 0; face.y = 0; face.width = 5; face.height = 5;
    db->insert_face(dest_id, face);

    db->insert(make_image("/src/a.jpg", "shaA"));
    db->update_path("/src/a.jpg", "/dst/b.jpg");

    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM faces"), 0);
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM vec_faces"), 0);
    EXPECT_EQ(db->count(), 1);
}

TEST_F(DatabaseTest, InsertCopyOntoStaleDestClearsItsFaces) {
    db->insert(make_image("/dst/b.jpg", "shaB"));
    int64_t dest_id = db->get_image_id("/dst/b.jpg");
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.x = 0; face.y = 0; face.width = 5; face.height = 5;
    db->insert_face(dest_id, face);

    db->insert(make_image("/src/a.jpg", "shaA"));
    db->insert_copy("/src/a.jpg", "/dst/b.jpg");

    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM faces"), 0);
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM vec_faces"), 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='*StaleDest*'`
Expected: FAIL — vec_faces count is 1 in both (faces cascade via FK, vec_faces does not).

- [ ] **Step 3: Implement**

In `update_path_in_txn`, immediately before the `DELETE FROM images WHERE path = ?` block:

```cpp
    // vec_faces is a vec0 virtual table and unreachable by FK cascade; clear
    // the stale dest row's face data explicitly before dropping the row.
    {
        int64_t stale_id = get_image_id(new_path);
        if (stale_id >= 0) delete_faces_for_image(stale_id);
    }
```

In `insert_copy`, immediately before preparing the `INSERT OR REPLACE`:

```cpp
    // Same vec_faces cleanup as update_path_in_txn: the REPLACE below deletes
    // any stale row tracking dest_path, and FK cascade can't reach vec_faces.
    {
        int64_t stale_id = get_image_id(dest_path);
        if (stale_id >= 0) delete_faces_for_image(stale_id);
    }
```

- [ ] **Step 4: Run the full suite**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS. (`insert_copy` is exercised inside `execute_organize_action`'s transaction by `test_organize_exec.cpp` — those must stay green.)

- [ ] **Step 5: Commit**

```bash
git add src/database.cpp tests/test_database.cpp
git commit -m "Clear vec_faces when a stale dest row is dropped by mv/cp"
```

---

### Task 3: No-clobber `move_file` (#2)

**Files:**
- Modify: `src/paths.cpp` (`move_file` ~line 65, plus new static helpers)
- Test: `tests/test_paths.cpp`

**Interfaces:**
- Consumes: existing static `temp_sibling(dest)` helper in `paths.cpp`
- Produces: `move_file(source, dest)` — same signature; now throws `fs::filesystem_error` (with `std::errc::file_exists`) if dest exists at execution time, on both the same-device and cross-device paths. Also produces static helpers `rename_noreplace(const char*, const char*) -> int` (0 or errno) and `fsync_file(const fs::path&) -> bool` / `fsync_dir(const fs::path&)` used by Task 4.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_paths.cpp` next to the existing `move_file` tests (~line 155). Mirror the existing temp-dir setup used there:

```cpp
TEST(MoveFileTest, RefusesToOverwriteExistingDest) {
    auto dir = fs::temp_directory_path() / "phig-move-noclobber-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto src = dir / "src.jpg";
    auto dst = dir / "dst.jpg";
    std::ofstream(src) << "SOURCE";
    std::ofstream(dst) << "PRECIOUS";

    EXPECT_THROW(move_file(src.string(), dst.string()), fs::filesystem_error);

    // Both files intact, contents untouched
    std::ifstream s(src), d(dst);
    std::string sc((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
    std::string dc((std::istreambuf_iterator<char>(d)), std::istreambuf_iterator<char>());
    EXPECT_EQ(sc, "SOURCE");
    EXPECT_EQ(dc, "PRECIOUS");
    fs::remove_all(dir);
}

TEST(MoveFileTest, CrossDeviceRefusesToOverwriteExistingDest) {
    // Same /dev/shm guard as the existing cross-device move test: skip when
    // /dev/shm is missing or on the same device as the temp dir.
    if (!fs::exists("/dev/shm")) GTEST_SKIP() << "/dev/shm not available";
    auto dir = fs::temp_directory_path() / "phig-move-noclobber-xdev";
    auto shm = fs::path("/dev/shm") / "phig-move-noclobber-xdev";
    fs::remove_all(dir);
    fs::remove_all(shm);
    fs::create_directories(dir);
    fs::create_directories(shm);
    struct stat a{}, b{};
    stat(dir.c_str(), &a);
    stat(shm.c_str(), &b);
    if (a.st_dev == b.st_dev) GTEST_SKIP() << "same device";

    auto src = dir / "src.jpg";
    auto dst = shm / "dst.jpg";
    std::ofstream(src) << "SOURCE";
    std::ofstream(dst) << "PRECIOUS";

    EXPECT_THROW(move_file(src.string(), dst.string()), fs::filesystem_error);
    std::ifstream s(src), d(dst);
    std::string sc((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
    std::string dc((std::istreambuf_iterator<char>(d)), std::istreambuf_iterator<char>());
    EXPECT_EQ(sc, "SOURCE");
    EXPECT_EQ(dc, "PRECIOUS");
    fs::remove_all(dir);
    fs::remove_all(shm);
}
```

If the file doesn't already include `<sys/stat.h>` and `<fstream>`, add them.

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='MoveFileTest.*Refuses*'`
Expected: FAIL — no throw; dst contains "SOURCE" (the clobber this task removes).

- [ ] **Step 3: Implement `rename_noreplace`, fsync helpers, and the new `move_file`**

In `src/paths.cpp`, add includes `<cerrno>`, `<fcntl.h>` (AT_FDCWD / O_DIRECTORY). Add to the anonymous namespace (near `temp_sibling`):

```cpp
// Flush a file's bytes to stable storage. Returns false on failure.
bool fsync_file(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

// Best-effort: make a completed rename in `dir` durable. Failure is ignored —
// the data itself was already fsynced.
void fsync_dir(const std::filesystem::path& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

// Rename that NEVER overwrites dest. Returns 0 on success or an errno:
// EEXIST = dest exists, EXDEV = cross-device (caller falls back), other =
// real failure. Uses the kernel's atomic no-replace rename where available,
// then a hard-link emulation, then (for filesystems with neither, e.g. FAT)
// a check-then-rename with a small TOCTOU window — still far safer than a
// blind rename.
int rename_noreplace(const char* src, const char* dst) {
#if defined(__linux__)
    if (::renameat2(AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0) return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) return errno;
#elif defined(__APPLE__)
    if (::renamex_np(src, dst, RENAME_EXCL) == 0) return 0;
    if (errno != ENOTSUP) return errno;
#endif
    if (::link(src, dst) == 0) {
        if (::unlink(src) != 0) return errno;
        return 0;
    }
    if (errno == EEXIST) return EEXIST;
    std::error_code ec;
    if (std::filesystem::exists(dst, ec)) return EEXIST;
    if (::rename(src, dst) == 0) return 0;
    return errno;
}
```

Replace `move_file`:

```cpp
void move_file(const std::string& source, const std::string& dest) {
    namespace fs = std::filesystem;
    fs::path src(source), dst(dest);
    fs::create_directories(dst.parent_path());

    int err = rename_noreplace(source.c_str(), dest.c_str());
    if (err == 0) return;
    if (err == EEXIST || err == ENOTEMPTY) {
        // A file appeared at dest after conflict planning (concurrent phig
        // run, sync app, ...). Never overwrite it — surface a per-file error.
        throw fs::filesystem_error("move_file: destination already exists", src, dst,
                                   std::make_error_code(std::errc::file_exists));
    }
    if (err != EXDEV) {
        throw fs::filesystem_error("cannot rename", src, dst,
                                   std::error_code(err, std::generic_category()));
    }

    // Cross-device: copy to a temp sibling of dest, fsync so the bytes are
    // durable BEFORE the source is unlinked (a power loss must never leave
    // the only copy truncated), then no-replace rename into place.
    fs::path tmp = temp_sibling(dst);
    std::error_code ec;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("move_file: copy failed", src, dst, ec);
    }
    if (!fsync_file(tmp)) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("move_file: fsync failed", tmp, dst,
                                   std::error_code(errno, std::generic_category()));
    }
    err = rename_noreplace(tmp.string().c_str(), dest.c_str());
    if (err != 0) {
        fs::remove(tmp, ec);
        if (err == EEXIST) {
            throw fs::filesystem_error("move_file: destination already exists", src, dst,
                                       std::make_error_code(std::errc::file_exists));
        }
        throw fs::filesystem_error("move_file: rename failed", tmp, dst,
                                   std::error_code(err, std::generic_category()));
    }
    fsync_dir(dst.parent_path());
    fs::remove(src);
}
```

Platform notes for the implementer: `renameat2` needs glibc ≥ 2.28 and
`_GNU_SOURCE` (g++ defines it by default); `renamex_np`/`RENAME_EXCL` come from
macOS `<stdio.h>`. If the macOS CI build can't find `renamex_np`, keep the
`__APPLE__` branch as just the link+unlink emulation — correctness is the same,
only atomicity of the primary path differs.

- [ ] **Step 4: Run the full suite (existing move/EXDEV tests must still pass)**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS — including the pre-existing `move_file` cross-device test and `test_organize_exec.cpp` compensator tests (they move to non-existent dests and move *back* to a vacated source, both still allowed).

- [ ] **Step 5: Commit**

```bash
git add src/paths.cpp tests/test_paths.cpp
git commit -m "Refuse to overwrite an existing destination in move_file"
```

---

### Task 4: fsync in `atomic_copy_file` (#6)

**Files:**
- Modify: `src/paths.cpp` (`atomic_copy_file` ~line 109)

**Interfaces:**
- Consumes: `fsync_file`, `fsync_dir` from Task 3
- Produces: `atomic_copy_file(source, dest)` — same signature; the temp file is fsynced before the rename and the parent dir best-effort after. `atomic_write_file` (thumbnails) is deliberately NOT changed.

- [ ] **Step 1: Implement**

In `atomic_copy_file`, between the successful `fs::copy_file` and the `fs::rename`:

```cpp
    // User data is being placed: make the bytes durable before the rename
    // publishes the path, so a power loss can't leave a valid-looking
    // truncated dest. (Thumbnails skip this — regenerable cache.)
    if (!fsync_file(tmp)) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("atomic_copy_file: fsync failed", tmp, dst,
                                   std::error_code(errno, std::generic_category()));
    }
```

And after the successful rename:

```cpp
    fsync_dir(dst.parent_path());
```

- [ ] **Step 2: Run the full suite**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS. Durability itself isn't unit-testable without power loss; the
existing `atomic_copy_file` tests are the regression net.

- [ ] **Step 3: Commit**

```bash
git add src/paths.cpp
git commit -m "fsync copied data before publishing it in atomic_copy_file"
```

---

### Task 5: Log scan removals with restore snapshots (#3a)

**Files:**
- Modify: `src/main.cpp` (`ScanOptions` ~line 215, removal block in `cmd_scan` ~line 605, scan arg wiring in `main` ~line 2574)
- Modify: `src/database.h` (comment on `OperationEntry::kind` — add `"scan"` to the command list is not needed; kind stays `purge_row`)

**Interfaces:**
- Consumes: `log_begin_operation` / `delete_paths_logged` / `log_finish_operation` / `log_prune` (plan 020), `iso_utc_now()`, `join_argv()` (existing statics in main.cpp)
- Produces: scan removals recorded as an operation with `command = "scan"` and one `purge_row` entry (with `restore_json` snapshot) per removed row. `ScanOptions` gains `bool log_enabled = true; int log_max_entries = 100000; std::string argv_str;`.

- [ ] **Step 1: Add the option fields**

In `struct ScanOptions`:

```cpp
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
```

- [ ] **Step 2: Replace the removal call in `cmd_scan`**

The current block ends with `db.delete_paths(removed_paths); removed_count = removed_paths.size();`. Replace those two lines with:

```cpp
            // Removing a row destroys user-authored metadata (tags, gallery,
            // faces) that a rescan cannot rebuild — record it like purge does,
            // with a restorable row snapshot per entry (review #3a).
            if (opts.log_enabled) {
                int64_t op_id =
                    db.log_begin_operation("scan", opts.argv_str, iso_utc_now());
                removed_count = db.delete_paths_logged(removed_paths, op_id);
                db.log_finish_operation(op_id, "completed");
                db.log_prune(opts.log_max_entries);
            } else {
                db.delete_paths(removed_paths);
                removed_count = removed_paths.size();
            }
```

- [ ] **Step 3: Wire config + argv in `main`**

In the `scan` command branch, alongside the existing config seeding:

```cpp
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);
```

- [ ] **Step 4: Build and verify end-to-end**

The removal path itself is covered by the existing `delete_paths_logged` tests;
this task is wiring in `cmd_scan`, so verify with the real binary:

```bash
direnv exec . cmake --build build
D=$(mktemp -d)
cp tests/data/*.jpg "$D"/ 2>/dev/null || convert -size 8x8 xc:red "$D/a.jpg"  # any small jpg
./build/phig scan "$D" --db /tmp/scanlog-test.db
rm "$D"/*.jpg
./build/phig scan "$D" --db /tmp/scanlog-test.db
sqlite3 /tmp/scanlog-test.db "SELECT command, status, entry_count FROM operations; SELECT kind, src FROM operation_entries;"
```

Expected: a `scan|completed` operation whose `entry_count` equals the removed
file count, with `purge_row` entries carrying `restore_json` snapshots. Clean
up `/tmp/scanlog-test.db*` and `$D` afterwards.

- [ ] **Step 5: Run the full suite, then commit**

Run: `./build/phig-tests` — Expected: PASS.

```bash
git add src/main.cpp
git commit -m "Log scan's automatic row removals with restore snapshots"
```

---

### Task 6: `cmd_purge` must run `init_schema` (#4)

**Files:**
- Modify: `src/main.cpp` (`cmd_purge` ~line 1221)

**Interfaces:**
- Consumes: `Database::init_schema()` (existing)
- Produces: purge honors the schema-version guard and pending migrations like every other command.

- [ ] **Step 1: Implement**

In `cmd_purge`, directly after `Database db(opts.db_path);`:

```cpp
    db.init_schema();
```

- [ ] **Step 2: Build and verify the guard end-to-end**

```bash
direnv exec . cmake --build build
sqlite3 /tmp/newer-schema.db "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT); \
  INSERT INTO meta VALUES('schema_version','99'); \
  CREATE TABLE images (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE);"
./build/phig purge --match '*' --db /tmp/newer-schema.db
```

Expected: exit code 1 with `Error: Database schema (v99) is newer than this
version of phig (v5). Please update phig.` and no rows touched. Clean up
`/tmp/newer-schema.db*`.

- [ ] **Step 3: Run the full suite, then commit**

Run: `./build/phig-tests` — Expected: PASS.

```bash
git add src/main.cpp
git commit -m "Run init_schema in purge so the schema-version guard applies"
```

---

### Task 7: Range comparison instead of LIKE for path prefixes (#5)

**Files:**
- Modify: `src/database.cpp` (`get_all_images` ~line 922, `get_exact_duplicates` ~line 972)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: existing static `prefix_upper_bound(prefix)` in `database.cpp` (already defined above both functions)
- Produces: same signatures; `_`/`%` in a path prefix now match literally.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(DatabaseTest, GetAllImagesPrefixTreatsUnderscoreLiterally) {
    db->insert(make_image("/photos/my_dir/a.jpg", "s1"));
    db->insert(make_image("/photos/myxdir/b.jpg", "s2"));
    auto rows = db->get_all_images("/photos/my_dir/");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, "/photos/my_dir/a.jpg");
}

TEST_F(DatabaseTest, ExactDuplicatesPrefixTreatsUnderscoreLiterally) {
    db->insert(make_image("/photos/my_dir/a.jpg", "same"));
    db->insert(make_image("/photos/my_dir/b.jpg", "same"));
    db->insert(make_image("/photos/myxdir/c.jpg", "same"));
    db->insert(make_image("/photos/myxdir/d.jpg", "same"));
    auto groups = db->get_exact_duplicates("/photos/my_dir/");
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].size(), 2u);  // only the two files actually under my_dir/
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='*Underscore*'`
Expected: FAIL — `myxdir` rows leak in (`_` matched as a wildcard).

- [ ] **Step 3: Implement**

In `get_all_images`, replace the LIKE clause with the same range comparison
`get_paths_with_prefix` uses:

```cpp
    std::optional<std::string> upper;
    if (!path_prefix.empty()) {
        upper = prefix_upper_bound(path_prefix);
        sql += " WHERE path >= ?";
        if (upper) sql += " AND path < ?";
    }
    ...
    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, 1, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (upper) sqlite3_bind_text(stmt, 2, upper->c_str(), -1, SQLITE_TRANSIENT);
    }
```

In `get_exact_duplicates`, apply the same substitution to both `LIKE ? || '%'`
occurrences in the prefixed query (outer filter and the sha256 subquery); bind
`prefix, upper, prefix, upper` in order (or `prefix, prefix` when `upper` is
nullopt, adjusting the SQL accordingly):

```cpp
        auto upper = prefix_upper_bound(path_prefix);
        std::string bound = upper ? " AND path < ?" : "";
        sql = "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, "
              "phash, width, height, metadata, media_type, duration_ms FROM images "
              "WHERE path >= ?" + bound +
              " AND sha256 IN (SELECT sha256 FROM images WHERE path >= ?" + bound +
              " GROUP BY sha256 HAVING COUNT(*) > 1) ORDER BY sha256, path";
```

with binds:

```cpp
        int i = 1;
        sqlite3_bind_text(stmt, i++, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (upper) sqlite3_bind_text(stmt, i++, upper->c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, i++, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        if (upper) sqlite3_bind_text(stmt, i++, upper->c_str(), -1, SQLITE_TRANSIENT);
```

- [ ] **Step 4: Run the full suite (existing prefix tests at test_database.cpp:282,300 must still pass)**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/database.cpp tests/test_database.cpp
git commit -m "Match path prefixes exactly instead of via LIKE wildcards"
```

---

### Task 8: Throw on short read in `compute_sha256` (#7)

**Files:**
- Modify: `src/hasher.cpp` (`compute_sha256` ~line 135)
- Test: `tests/test_hasher.cpp`

**Interfaces:**
- Produces: `compute_sha256(path)` — same signature; now throws `std::runtime_error` when the file size can't be determined or the read returns fewer bytes than expected, instead of silently hashing a zero-padded buffer.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(HasherTest, Sha256ThrowsOnUnreadableInput) {
    // A directory opens as a stream on Linux/macOS but reads fail — the old
    // code silently hashed a zero-filled buffer of the directory's st_size.
    auto dir = fs::temp_directory_path() / "phig-hash-unreadable-test";
    fs::create_directories(dir);
    EXPECT_THROW(compute_sha256(dir.string()), std::runtime_error);
    fs::remove_all(dir);
}
```

(Match the include/namespace conventions already used in `tests/test_hasher.cpp`.)

- [ ] **Step 2: Run test to verify it fails**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='HasherTest.Sha256ThrowsOnUnreadableInput'`
Expected: FAIL — no throw; a bogus hash is returned.

- [ ] **Step 3: Implement**

Replace the body after the open check:

```cpp
    file.seekg(0, std::ios::end);
    std::streampos end = file.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot determine file size for hashing: " + path);
    }
    size_t size = static_cast<size_t>(end);
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    // A short read (I/O error, file shrank mid-scan) must not produce a
    // silently wrong hash — duplicates output drives user deletions.
    if (static_cast<size_t>(file.gcount()) != size) {
        throw std::runtime_error("Short read while hashing (file changed or I/O error): " + path);
    }

    return sha256_bytes(buffer.data(), buffer.size());
```

- [ ] **Step 4: Run the full suite (known-vector hash tests must still pass)**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/hasher.cpp tests/test_hasher.cpp
git commit -m "Fail hashing on short reads instead of returning a wrong hash"
```

---

### Task 9: Transactional first-time schema creation (#9)

**Files:**
- Modify: `src/database.cpp` (`init_schema`, the fresh-create `else` branch ~line 236 and the pre-migration branch ~line 191)

**Interfaces:**
- Produces: `init_schema()` — same signature; the fresh-create statement batch and the pre-migration re-establishment batch each run inside one transaction ending with their `set_schema_version` call, so a crash can never leave a versionless partial schema.

- [ ] **Step 1: Implement**

Wrap the entire fresh-create branch (all `exec(...)` calls through
`set_schema_version(CURRENT_SCHEMA_VERSION)`):

```cpp
        } else {
            // Truly fresh database — create the full schema at the latest
            // version in ONE transaction: a crash partway must not leave a
            // versionless partial schema that a later run misreads as a
            // pre-migration v1 DB (whose v2 migration would then fail).
            exec("BEGIN TRANSACTION");
            try {
                // ... existing exec(...) statements, unchanged ...
                set_schema_version(CURRENT_SCHEMA_VERSION);
                exec("COMMIT");
            } catch (...) {
                exec("ROLLBACK");
                throw;
            }
        }
```

Apply the same wrap to the `has_images_table` pre-migration branch, ending with
`set_schema_version(1)` inside the transaction (the `db_version = 1;`
assignment stays after the COMMIT).

- [ ] **Step 2: Run the full suite**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS. Every test fixture creates a fresh DB in `SetUp`, so this
exercises the transactional path hundreds of times — in particular it proves
`CREATE VIRTUAL TABLE ... vec0` works inside an explicit transaction. If any
fixture fails on the vec0 create, stop and report; do not work around it by
moving the virtual-table create outside the transaction without discussion.

- [ ] **Step 3: Commit**

```bash
git add src/database.cpp
git commit -m "Create fresh schema in one transaction"
```

---

## Final verification

- [x] Run the full suite: `direnv exec . cmake --build build && ./build/phig-tests` — all green (281 tests).
- [x] Lint: `clang-format --dry-run --Werror` clean on changed files.
- [x] Update `plans/000-INDEX.md` status for 022 to `completed` and record the outcome at the bottom of this file.

## Outcome (2026-07-11)

All nine tasks implemented via subagent-driven development, each with real-behavior
tests and an individual spec+quality review; a final whole-branch review returned
READY TO MERGE. Full suite: 281 tests passing.

Commits (on `master`):

| Fix | Commit(s) |
|-----|-----------|
| #1 upsert preserves tags/gallery/faces; conditional face clear; scan face-insert guard | `405f29a`; `147f0b8` (guard the `needs_faces_only` scan path too — a sibling face-insert loop the first pass missed) |
| #8 vec_faces cleanup on stale-dest drop (`update_path_in_txn`, `insert_copy`) | `8680018` |
| #2 no-clobber `move_file` (renameat2 RENAME_NOREPLACE, cross-device fsync) | `5cf122e`; `d83fdd5` (fix copy-failure error reporting; correct docs/header) |
| #6 fsync copied data before publishing in `atomic_copy_file` | `4af9d75` |
| #3a logged scan removals with restore snapshots | `a3ecad1`; `ec0008e` (mark op `interrupted` on failure, matching purge — whole-branch review finding) |
| #4 `cmd_purge` runs `init_schema` (schema-version guard) | `2381523` |
| #5 range comparison instead of LIKE in `get_all_images` / `get_exact_duplicates` | `20f53aa` |
| #7 `compute_sha256` throws on short/failed read | `705b55f` |
| #9 transactional fresh + pre-migration schema creation | `49263f6` |

Notes:
- Task 3 removed the obsolete `MoveFileTest.OverwritesExistingDestination` test,
  which asserted the pre-#2 overwrite behavior this plan intentionally reverses
  (design decision #2). Coverage is preserved by the new no-clobber tests.
- `Database::purge(prefix, glob)` still uses `LIKE` but is confirmed test-only
  (production purge filters in C++ via `passes_filters`); left untouched as scoped.
- A pre-existing clang-tidy finding (`bugprone-throwing-static-initialization` on
  the `migrations` static array) predates this plan and is out of scope.
