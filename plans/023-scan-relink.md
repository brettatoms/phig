---
name: Scan Re-link (metadata follows externally moved files)
status: completed
created: 2026-07-11
description: >
  When a scan finds that a tracked file is gone but another tracked file with
  the identical sha256 exists on disk, treat it as an external move and carry
  the user-authored metadata (tags, favorites, gallery, faces) to the surviving
  row before removing the stale one. Closes the "user moved files without phig"
  metadata-loss gap from the 2026-07-11 review (finding #3b).
---

# Scan Re-link

## Goal

Users move and rename files outside phig. Today a rescan handles that as
"delete the old row, insert a new one": the file's tags, favorite status,
gallery, and face data — the metadata a rescan cannot rebuild — are lost even
though a byte-identical file was scanned into the DB moments earlier. This plan
makes scan recognize that situation by content hash and move the metadata to
the surviving row before deleting the stale one.

Depends on plan [022](022-data-safety-fixes.md): the upsert semantics of
`insert` (task 1) and the logged-removal block in `cmd_scan` (task 5).

## Behavior

During scan cleanup, before the stale rows are deleted:

- For each removed path, look up its stored sha256 and find **all other DB
  rows with the same sha256 whose file currently exists on disk**. The lookup
  is DB-global, not scoped to the scan directory — a file moved *out* of the
  scanned tree into another tracked tree still re-links.
- **Exactly one such candidate** → treat as a move and transfer metadata:
  - tags: unioned onto the target (`INSERT OR IGNORE`).
  - gallery: copied only if the target has none — a gallery the user already
    set on the target wins.
  - faces: repointed (`faces.image_id` updated) only if the target has no
    faces of its own; `vec_faces` rows are keyed by `face_id` and need no
    change. If the target was scanned with `--faces` it already has fresh
    detections, which win; the old rows are then dropped with the stale row
    (their count is preserved in the purge snapshot's `_faces_omitted`).
- **Zero candidates** (file deleted, or moved somewhere untracked) or **two or
  more candidates** (the content exists at several paths — ambiguous) → no
  transfer. The row is still deleted with a full `purge_row` snapshot (plan
  022 task 5), so nothing is lost silently.
- Each transfer is logged as a `relink` entry (`src` = old path, `dst` =
  target path) in the same scan operation, committed in the same transaction
  as the transfer itself.
- The scan summary reports `Relinked: N`.

Known limitation, accepted: if the moved file's content *also* changed (edited
elsewhere before rescanning), the sha256 no longer matches and no re-link
happens. Content-changed moves are indistinguishable from delete+new-file.

## Structure

Matching needs both DB state and disk checks, so the orchestration lives in a
small phig-lib unit (testable against real files + real SQLite, per the
established pattern):

- `src/relink.h` / `src/relink.cpp` — `relink_moved_rows(db, removed_paths,
  op_id)`.
- `Database` gains two primitives: `get_paths_for_sha256(sha, exclude_path)`
  and `transfer_user_metadata(from_id, to_id)`.
- `cmd_scan` calls `relink_moved_rows` just before deleting the removed rows.

## Checklist

- [ ] `Database::get_paths_for_sha256` + `Database::transfer_user_metadata`
- [ ] `relink_moved_rows` in new `src/relink.{h,cpp}` (+ CMake)
- [ ] `cmd_scan` integration + `Relinked:` summary line
- [ ] `relink` entry kind documented in `database.h`
- [ ] Tests: transfer, zero/ambiguous candidates, missing-on-disk candidate, target-wins gallery/faces, log entry
- [ ] End-to-end verify with the binary: favorite survives an external `mv` + rescan

---

# Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Scan carries user-authored metadata across external file moves detected by identical sha256.

**Architecture:** Two new `Database` primitives (candidate lookup, metadata transfer), a `relink_moved_rows` orchestrator in a new `src/relink.{h,cpp}` phig-lib unit (it needs `fs::exists`, so it sits above the DB layer), and a three-line call site in `cmd_scan`'s removal block.

**Tech Stack:** C++20, SQLite, GoogleTest. Build via `direnv exec . cmake --build build`.

## Global Constraints

- Requires plan 022 to be implemented first (upsert `insert`, logged scan removals).
- Build/test command: `direnv exec . cmake --build build && ./build/phig-tests`
- Formatting: clang-format (Google style, 4-space, 100-col)
- Commit messages: plain, no AI attribution of any kind
- Every task ends with the FULL test suite passing

---

### Task 1: Database primitives — candidate lookup and metadata transfer

**Files:**
- Modify: `src/database.h` (declare both methods; extend the `OperationEntry::kind` comment to `"move" | "copy" | "purge_row" | "relink"`)
- Modify: `src/database.cpp`
- Test: `tests/test_database.cpp`

**Interfaces:**
- Produces:
  - `std::vector<std::string> Database::get_paths_for_sha256(const std::string& sha256, const std::string& exclude_path);` — paths of all rows with this sha256, excluding `exclude_path`, ordered by path.
  - `void Database::transfer_user_metadata(int64_t from_id, int64_t to_id);` — unions tags onto `to_id`; copies gallery only when `to_id`'s is NULL; repoints faces only when `to_id` has none. Statement-level writes checked via `step_write_or_throw`; runs in the caller's transaction (no BEGIN/COMMIT of its own).

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_database.cpp` (uses the `DatabaseTest` fixture, `make_image`, `count_rows` from plan 022, and `FaceInfo`):

```cpp
TEST_F(DatabaseTest, GetPathsForSha256ExcludesGivenPath) {
    db->insert(make_image("/a/x.jpg", "same"));
    db->insert(make_image("/b/y.jpg", "same"));
    db->insert(make_image("/c/z.jpg", "other"));
    auto paths = db->get_paths_for_sha256("same", "/a/x.jpg");
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], "/b/y.jpg");
}

TEST_F(DatabaseTest, TransferUserMetadataMovesTagsGalleryFaces) {
    db->insert(make_image("/old/a.jpg", "same"));
    db->insert(make_image("/new/a.jpg", "same"));
    int64_t from_id = db->get_image_id("/old/a.jpg");
    int64_t to_id = db->get_image_id("/new/a.jpg");
    db->set_tag({"/old/a.jpg"}, "favorite", true);
    db->set_tag({"/old/a.jpg"}, "vacation", true);
    db->set_gallery({"/old/a.jpg"}, "summer", false);
    FaceInfo face{};
    face.embedding.fill(0.5f);
    face.x = 1; face.y = 1; face.width = 5; face.height = 5;
    db->insert_face(from_id, face);

    db->begin_transaction();
    db->transfer_user_metadata(from_id, to_id);
    db->commit_transaction();

    auto tags = db->get_tags_for_image("/new/a.jpg");
    ASSERT_EQ(tags.size(), 2u);  // favorite + vacation (ordered by name)
    EXPECT_TRUE(db->has_faces("/new/a.jpg"));
    EXPECT_FALSE(db->has_faces("/old/a.jpg"));
    // vec_faces untouched: still exactly one embedding row
    EXPECT_EQ(count_rows(db->db_ptr(), "SELECT COUNT(*) FROM vec_faces"), 1);
    for (const auto& img : db->get_all_images()) {
        if (img.path == "/new/a.jpg") EXPECT_EQ(img.gallery, "summer");
    }
}

TEST_F(DatabaseTest, TransferUserMetadataTargetGalleryAndFacesWin) {
    db->insert(make_image("/old/a.jpg", "same"));
    db->insert(make_image("/new/a.jpg", "same"));
    int64_t from_id = db->get_image_id("/old/a.jpg");
    int64_t to_id = db->get_image_id("/new/a.jpg");
    db->set_gallery({"/old/a.jpg"}, "old-gallery", false);
    db->set_gallery({"/new/a.jpg"}, "new-gallery", false);
    FaceInfo old_face{};
    old_face.embedding.fill(0.1f);
    old_face.x = 1; old_face.y = 1; old_face.width = 5; old_face.height = 5;
    db->insert_face(from_id, old_face);
    FaceInfo new_face{};
    new_face.embedding.fill(0.9f);
    new_face.x = 2; new_face.y = 2; new_face.width = 6; new_face.height = 6;
    db->insert_face(to_id, new_face);

    db->begin_transaction();
    db->transfer_user_metadata(from_id, to_id);
    db->commit_transaction();

    for (const auto& img : db->get_all_images()) {
        if (img.path == "/new/a.jpg") EXPECT_EQ(img.gallery, "new-gallery");
    }
    // Old face NOT moved: target already had fresh detections.
    EXPECT_TRUE(db->has_faces("/old/a.jpg"));
    std::string q = "SELECT COUNT(*) FROM faces WHERE image_id = " + std::to_string(to_id);
    EXPECT_EQ(count_rows(db->db_ptr(), q.c_str()), 1);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='*TransferUserMetadata*:*GetPathsForSha256*'`
Expected: compile failure (methods don't exist) — declare stubs if needed to see red tests, or accept the compile error as the failure signal.

- [ ] **Step 3: Declare in `database.h`**

Next to `get_all_sha256s()`:

```cpp
    // Paths of all rows with this sha256, excluding `exclude_path` (used by
    // scan re-link to find where an externally moved file's content lives now).
    std::vector<std::string> get_paths_for_sha256(const std::string& sha256,
                                                  const std::string& exclude_path);

    // Move user-authored metadata from one image row to another (scan re-link):
    // tags are unioned onto `to_id`; gallery copies only when `to_id`'s is
    // NULL; faces repoint only when `to_id` has none (vec_faces rows are keyed
    // by face_id and are unaffected). Runs in the caller's transaction.
    void transfer_user_metadata(int64_t from_id, int64_t to_id);
```

Update the `OperationEntry::kind` comment to `// "move" | "copy" | "purge_row" | "relink"`.

- [ ] **Step 4: Implement in `database.cpp`**

```cpp
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
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='*TransferUserMetadata*:*GetPathsForSha256*'`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/database.h src/database.cpp tests/test_database.cpp
git commit -m "Add sha256 row lookup and user-metadata transfer primitives"
```

---

### Task 2: `relink_moved_rows` orchestrator

**Files:**
- Create: `src/relink.h`, `src/relink.cpp`
- Modify: `CMakeLists.txt` (add `src/relink.cpp` to `phig-lib` after `src/organize_exec.cpp`; add `tests/test_relink.cpp` to the test sources after `tests/test_organize_exec.cpp`)
- Test: `tests/test_relink.cpp` (new)

**Interfaces:**
- Consumes: `Database::get_sha256`, `get_paths_for_sha256`, `get_image_id`, `transfer_user_metadata`, `log_add_entry`, `begin/commit/rollback_transaction` (Task 1 + existing)
- Produces: `int relink_moved_rows(Database& db, const std::vector<std::string>& removed_paths, int64_t op_id);` — returns the number of rows whose metadata was transferred. `op_id <= 0` skips logging. Rows are NOT deleted; the caller deletes them afterwards.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_relink.cpp` (fixture modeled on `test_organize_exec.cpp`):

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "database.h"
#include "faces.h"
#include "relink.h"
#include "types.h"

namespace fs = std::filesystem;

class RelinkTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-relink-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        db_path = (dir / "t.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove_all(dir);
    }

    // Insert a row; when on_disk, also create the real file it points at.
    std::string add_row(const std::string& rel, const std::string& sha, bool on_disk) {
        fs::path p = dir / rel;
        if (on_disk) {
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "bytes-" << sha;
        }
        ImageInfo info;
        info.path = p.string();
        info.filename = p.filename().string();
        info.extension = "jpg";
        info.file_size = 10;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = sha;
        info.phash = 0;
        info.width = 8;
        info.height = 8;
        info.metadata_json = "{}";
        db->insert(info);
        return p.string();
    }

    static int64_t count_rows(sqlite3* h, const std::string& sql) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(h, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return -1;
        int64_t n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int64(s, 0) : -1;
        sqlite3_finalize(s);
        return n;
    }
};

TEST_F(RelinkTest, SingleCandidateTransfersMetadata) {
    std::string old_path = add_row("old/a.jpg", "sameSha", /*on_disk=*/false);
    std::string new_path = add_row("new/a.jpg", "sameSha", /*on_disk=*/true);
    db->set_tag({old_path}, "favorite", true);
    db->set_gallery({old_path}, "summer", false);

    int n = relink_moved_rows(*db, {old_path}, 0);

    EXPECT_EQ(n, 1);
    auto tags = db->get_tags_for_image(new_path);
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "favorite");
    for (const auto& img : db->get_all_images()) {
        if (img.path == new_path) EXPECT_EQ(img.gallery, "summer");
    }
}

TEST_F(RelinkTest, NoCandidateDoesNothing) {
    std::string old_path = add_row("old/a.jpg", "uniqueSha", false);
    db->set_tag({old_path}, "favorite", true);
    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 0);
}

TEST_F(RelinkTest, AmbiguousCandidatesSkipped) {
    std::string old_path = add_row("old/a.jpg", "dupSha", false);
    std::string c1 = add_row("new/a.jpg", "dupSha", true);
    std::string c2 = add_row("new/b.jpg", "dupSha", true);
    db->set_tag({old_path}, "favorite", true);

    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 0);
    EXPECT_TRUE(db->get_tags_for_image(c1).empty());
    EXPECT_TRUE(db->get_tags_for_image(c2).empty());
}

TEST_F(RelinkTest, CandidateMissingOnDiskSkipped) {
    std::string old_path = add_row("old/a.jpg", "sameSha", false);
    add_row("new/a.jpg", "sameSha", /*on_disk=*/false);  // row exists, file doesn't
    db->set_tag({old_path}, "favorite", true);
    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 0);
}

TEST_F(RelinkTest, MissingCandidateFileDoesNotCountTowardAmbiguity) {
    // One on-disk candidate + one stale row: still an unambiguous move.
    std::string old_path = add_row("old/a.jpg", "sameSha", false);
    std::string target = add_row("new/a.jpg", "sameSha", true);
    add_row("stale/a.jpg", "sameSha", false);
    db->set_tag({old_path}, "favorite", true);

    EXPECT_EQ(relink_moved_rows(*db, {old_path}, 0), 1);
    EXPECT_EQ(db->get_tags_for_image(target).size(), 1u);
}

TEST_F(RelinkTest, LogsRelinkEntryUnderOperation) {
    std::string old_path = add_row("old/a.jpg", "sameSha", false);
    std::string new_path = add_row("new/a.jpg", "sameSha", true);
    db->set_tag({old_path}, "favorite", true);

    int64_t op_id = db->log_begin_operation("scan", "phig scan test", "2026-07-11T00:00:00Z");
    EXPECT_EQ(relink_moved_rows(*db, {old_path}, op_id), 1);
    db->log_finish_operation(op_id, "completed");

    EXPECT_EQ(count_rows(db->db_ptr(),
                         "SELECT COUNT(*) FROM operation_entries WHERE kind = 'relink'"),
              1);
    EXPECT_EQ(count_rows(db->db_ptr(),
                         "SELECT COUNT(*) FROM operations WHERE entry_count = 1"),
              1);
}
```

- [ ] **Step 2: Add the new files to CMake and run tests to verify they fail**

Add `src/relink.cpp` to the `phig-lib` sources and `tests/test_relink.cpp` to
the `phig-tests` sources in `CMakeLists.txt`.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build`
Expected: compile failure — `relink.h` doesn't exist yet.

- [ ] **Step 3: Implement**

`src/relink.h`:

```cpp
#pragma once

#include <string>
#include <vector>

#include "database.h"

// Scan re-link (plan 023): for each removed path whose stored sha256 matches
// EXACTLY ONE other DB row whose file exists on disk, transfer user-authored
// metadata (tags; gallery if the target has none; faces if the target has
// none) to that row, logging a 'relink' entry when op_id > 0. Zero or multiple
// on-disk candidates: do nothing (ambiguous). Rows are NOT deleted here — the
// caller deletes them afterwards. Returns the number of rows re-linked.
int relink_moved_rows(Database& db, const std::vector<std::string>& removed_paths,
                      int64_t op_id);
```

`src/relink.cpp`:

```cpp
#include "relink.h"

#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

int relink_moved_rows(Database& db, const std::vector<std::string>& removed_paths,
                      int64_t op_id) {
    int relinked = 0;
    for (const auto& old_path : removed_paths) {
        auto sha = db.get_sha256(old_path);
        if (!sha || sha->empty()) continue;

        // Candidates: other rows with identical content whose file is real.
        std::string target;
        int on_disk = 0;
        for (const auto& candidate : db.get_paths_for_sha256(*sha, old_path)) {
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                on_disk++;
                target = candidate;
                if (on_disk > 1) break;  // ambiguous — no need to keep looking
            }
        }
        if (on_disk != 1) continue;

        int64_t from_id = db.get_image_id(old_path);
        int64_t to_id = db.get_image_id(target);
        if (from_id < 0 || to_id < 0) continue;

        // Transfer + log entry commit together: the audit trail must never
        // claim a re-link that didn't stick (plan 020 invariant).
        db.begin_transaction();
        try {
            db.transfer_user_metadata(from_id, to_id);
            if (op_id > 0) db.log_add_entry(op_id, {"relink", old_path, target, ""});
            db.commit_transaction();
        } catch (...) {
            db.rollback_transaction();
            throw;
        }
        relinked++;
    }
    return relinked;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='RelinkTest.*'`
Expected: PASS (all six).

- [ ] **Step 5: Run the full suite, then commit**

Run: `./build/phig-tests` — Expected: PASS.

```bash
git add src/relink.h src/relink.cpp tests/test_relink.cpp CMakeLists.txt
git commit -m "Add relink_moved_rows: metadata follows externally moved files"
```

---

### Task 3: Wire re-link into `cmd_scan`

**Files:**
- Modify: `src/main.cpp` (include `relink.h`; the removal block in `cmd_scan` from plan 022 task 5; the summary output)

**Interfaces:**
- Consumes: `relink_moved_rows` (Task 2), the logged-removal block (plan 022 task 5)
- Produces: scan output gains a `Relinked:` line when > 0.

- [ ] **Step 1: Implement**

Add `#include "relink.h"` to `main.cpp`. In `cmd_scan`, extend the removal
block (re-link runs BEFORE the delete, inside the same operation, so the
deleted rows carry no faces and the snapshots reflect the post-transfer state):

```cpp
    size_t removed_count = 0;
    int relinked_count = 0;
    if (!removed_paths.empty()) {
        if (!db_paths.empty() && removed_paths.size() == db_paths.size() &&
            !opts.ignore_mount_warning) {
            std::cerr << "\nWarning: All " << removed_paths.size()
                      << " previously scanned files in this directory are missing.\n"
                      << "This may indicate an unmounted drive or incorrect path.\n"
                      << "Use --ignore-mount-warning to remove these entries from the database.\n";
        } else if (opts.log_enabled) {
            int64_t op_id = db.log_begin_operation("scan", opts.argv_str, iso_utc_now());
            relinked_count = relink_moved_rows(db, removed_paths, op_id);
            removed_count = db.delete_paths_logged(removed_paths, op_id);
            db.log_finish_operation(op_id, "completed");
            db.log_prune(opts.log_max_entries);
        } else {
            relinked_count = relink_moved_rows(db, removed_paths, 0);
            db.delete_paths(removed_paths);
            removed_count = removed_paths.size();
        }
    }
```

In the summary block, after the `Removed:` line:

```cpp
    if (relinked_count > 0) {
        std::cout << "  Relinked:  " << relinked_count
                  << " (metadata moved to identical files at new paths)\n";
    }
```

- [ ] **Step 2: Build and run the full suite**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: PASS.

- [ ] **Step 3: End-to-end verify with the binary**

```bash
D=$(mktemp -d)
convert -size 8x8 xc:red "$D/photo.jpg" 2>/dev/null || \
  cp "$(find tests -name '*.jpg' | head -1)" "$D/photo.jpg"
./build/phig scan "$D" --db /tmp/relink-e2e.db
./build/phig fav add --match "$D/photo.jpg" --db /tmp/relink-e2e.db
mkdir "$D/sub" && mv "$D/photo.jpg" "$D/sub/renamed.jpg"   # external move
./build/phig scan "$D" --recursive --db /tmp/relink-e2e.db
./build/phig fav list --db /tmp/relink-e2e.db
sqlite3 /tmp/relink-e2e.db "SELECT kind, src, dst FROM operation_entries ORDER BY id;"
```

Expected: scan reports `Relinked: 1`; `fav list` shows `$D/sub/renamed.jpg`;
the log shows a `relink` entry followed by a `purge_row` entry for the old
path. Clean up `/tmp/relink-e2e.db*` and `$D`.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Re-link user metadata across external moves during scan"
```

---

## Final verification

- [x] Full suite green: `direnv exec . cmake --build build && ./build/phig-tests` (290 tests).
- [x] Lint: `clang-format --dry-run --Werror` clean on changed files.
- [x] Update `AGENTS.md`/`README.md` scan documentation: scan now re-links metadata across external moves and logs its removals.
- [x] Update `plans/000-INDEX.md` status for 023 to `completed` and record the outcome at the bottom of this file.

## Outcome (2026-07-11)

Implemented via subagent-driven development (3 tasks, each with real-behavior
tests + spec/quality review); final whole-branch review returned READY TO MERGE.
Full suite: 290 tests passing. Built on plan 022 (upsert `insert`, logged scan
removals).

Commits (on `master`):

| Task | Commit |
|------|--------|
| 1 — `Database::get_paths_for_sha256` + `transfer_user_metadata` primitives | `3604fb6` |
| 2 — `relink_moved_rows` orchestrator in new `src/relink.{h,cpp}` + tests + CMake | `22651aa` |
| 3 — wire into `cmd_scan` (relink before delete, same op) + `Relinked:` summary | `0c76509` |

Notes:
- Task 3 merged the relink call into the existing scan-removal `try/catch` guard
  (022 commit `ec0008e`) rather than the plan's pre-guard snippet, so a relink
  failure also marks the operation `interrupted`. Relink runs before
  `delete_paths_logged` within the same `scan` operation.
- Whole-branch review confirmed: a transfer target can never be another stale
  row (the `fs::exists` gate excludes gone files, and `removed_paths` are files
  already gone from disk); vec_faces stays coherent (repoint touches only
  `faces.image_id`, never `face_id`); no nested transactions.
- Forward-looking (non-blocking): a future `undo` implementation should decide
  how to treat committed `relink` entries under an operation later marked
  `interrupted` (no undo consumer exists yet). See [020](020-activity-log.md).
