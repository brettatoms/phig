# phig rm Command Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `phig rm` command that removes selected images from disk and the database in one step, with modes trash (default) / `--hard` / `--db-only` / `--orphans`, replacing the `purge` command.

**Architecture:** A new `remove_exec.{h,cpp}` module owns the per-file disk action (trash move with cross-mount fallback and collision suffixing, or hard delete) and its activity-log entry, mirroring how `organize_exec` owns per-file cp/mv. A new `cmd_rm` in `main.cpp` owns selection, the confirmation guard, and the operation lifecycle (`log_begin_operation` … `log_finish_operation`), then calls `remove_exec` per file and `delete_paths_logged()` for the rows. A `trash_dir()` path helper and a `cmd_trash_empty` round it out. The `purge` command and `cmd_purge` are deleted; `rm --db-only` replaces them.

**Tech Stack:** C++20, CMake, GoogleTest, SQLite3, `std::filesystem`.

**Note on scope:** The `--ignored` mode is specified in plan 028 (`.phigignore`) and is **out of scope here**. This plan builds trash / `--hard` / `--db-only` / `--orphans` only. Where the spec lists `--ignored`, treat it as a future addition; do not add it.

## Global Constraints

- C++20; match existing file style (4-space indent, `namespace fs = std::filesystem;`, no exceptions for control flow across module boundaries beyond what the codebase already does).
- Every data-safety change ships with real-behavior tests (project rule): tests exercise real files and a real SQLite DB in a temp dir, not mocks.
- Trash location: `data_dir() + "/trash"` (i.e. `~/.local/share/phig/trash`; honors `XDG_DATA_HOME`). Created on first use.
- Activity-log operation name is `"rm"` for all modes. Two new `OperationEntry` kinds: `"trash"` (records `src` = origin, `dst` = trash path) and `"delete_file"` (records `src` = origin, `dst` = ""). Row deletion continues to use `delete_paths_logged()` which writes `"purge_row"` entries.
- Ordering per file: disk action first (append `trash`/`delete_file` entry), then row deletion via `delete_paths_logged()`, in the same operation.
- Broad-selection guard (verbatim from `cmd_purge`): broad when `matched*2 >= total && matched >= 20`; prompt on a tty, refuse when stdin is not a tty, unless `--yes`. `--hard` always requires `--yes` regardless of size.
- Modes are mutually exclusive; two mode flags is an error. Trash/`--hard`/`--db-only` require at least one `--match`; `--orphans` makes `--match` optional (whole-DB sweep when absent).
- Sidecars: use `find_sidecars()`; each sidecar gets the same disk action and its own log entry. Never touch same-stem RAW/JPEG siblings.
- `--dry-run`: no disk, DB, or log writes.
- `purge` is removed outright (no alias); `phig purge` errors as an unknown command.

---

## File Structure

- **Create `src/remove_exec.h` / `src/remove_exec.cpp`** — per-file disk action + its log entry. Interface: `execute_trash_action`, `execute_hard_delete_action`, and a `remove_with_sidecars` helper. Depends on `database.h`, `paths.h`, `sidecar.h`.
- **Modify `src/paths.h` / `src/paths.cpp`** — add `trash_dir()` and `trash_path_for()` (collision-suffixing helper).
- **Modify `src/database.h`** — extend the `OperationEntry` kind comment to list `trash` / `delete_file`.
- **Modify `src/main.cpp`** — add `cmd_rm`, `cmd_trash_empty`, and dispatch for `rm` / `trash`; delete `cmd_purge` and the `purge` dispatch branch; update `print_usage()`.
- **Modify `CMakeLists.txt`** — add `src/remove_exec.cpp` to `phig-lib`; add `tests/test_remove_exec.cpp` to `phig-tests`.
- **Create `tests/test_remove_exec.cpp`** — module-level tests for trash/hard-delete/collision/sidecars.
- **Modify `tests/test_paths.cpp`** — tests for `trash_dir()` / `trash_path_for()`.
- **Modify `tests/test_database.cpp`** — update any `"purge"` command/operation-name references (the DB-level `Database::purge()` method and its unit tests stay).
- **Modify `README.md` / `AGENTS.md`** — replace `phig purge` docs with `phig rm`.

---

## Task 1: `trash_dir()` and `trash_path_for()` path helpers

**Files:**
- Modify: `src/paths.h` (add two declarations near `thumbs_dir()`)
- Modify: `src/paths.cpp`
- Test: `tests/test_paths.cpp`

**Interfaces:**
- Produces:
  - `std::string trash_dir();` — returns `data_dir() + "/trash"`.
  - `std::string trash_path_for(const std::string& filename);` — returns a non-colliding absolute path inside `trash_dir()` for a file named `filename`: `trash_dir()/filename` if free, else `trash_dir()/filename.1`, `.2`, … The returned path does not yet exist on disk. Does **not** create `trash_dir()`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_paths.cpp` (this file already manipulates `HOME`/`XDG_*`; follow its existing env-save/restore fixture pattern — set `XDG_DATA_HOME` to a temp dir in the test body):

```cpp
TEST(TrashPathTest, TrashDirUnderDataHome) {
    fs::path tmp = fs::temp_directory_path() / "phig-trashdir-test";
    fs::remove_all(tmp);
    setenv("XDG_DATA_HOME", tmp.c_str(), 1);
    EXPECT_EQ(trash_dir(), (tmp / "phig" / "trash").string());
    unsetenv("XDG_DATA_HOME");
    fs::remove_all(tmp);
}

TEST(TrashPathTest, TrashPathForIsFreeSlotAndSuffixesOnCollision) {
    fs::path tmp = fs::temp_directory_path() / "phig-trashpath-test";
    fs::remove_all(tmp);
    setenv("XDG_DATA_HOME", tmp.c_str(), 1);
    fs::create_directories(trash_dir());

    std::string p0 = trash_path_for("IMG_1234.jpg");
    EXPECT_EQ(p0, (fs::path(trash_dir()) / "IMG_1234.jpg").string());

    // Occupy the free slot; next call must suffix.
    std::ofstream(p0) << "x";
    std::string p1 = trash_path_for("IMG_1234.jpg");
    EXPECT_EQ(p1, (fs::path(trash_dir()) / "IMG_1234.jpg.1").string());

    std::ofstream(p1) << "x";
    std::string p2 = trash_path_for("IMG_1234.jpg");
    EXPECT_EQ(p2, (fs::path(trash_dir()) / "IMG_1234.jpg.2").string());

    unsetenv("XDG_DATA_HOME");
    fs::remove_all(tmp);
}
```

Ensure `tests/test_paths.cpp` includes `<fstream>` and `<filesystem>` (add if missing).

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='TrashPathTest.*'`
Expected: FAIL to compile/link with "trash_dir"/"trash_path_for" not declared.

- [ ] **Step 3: Add declarations to `src/paths.h`**

After the `std::string thumbs_dir();` line (near line 21), add:

```cpp
std::string trash_dir();    // data_dir() + /trash  (phig-managed trash)

// A non-colliding absolute path inside trash_dir() for a file named `filename`:
// trash_dir()/filename if free, else trash_dir()/filename.1, .2, … The returned
// path does not exist yet. Does NOT create trash_dir().
std::string trash_path_for(const std::string& filename);
```

- [ ] **Step 4: Implement in `src/paths.cpp`**

Near the other derived-path definitions (after `thumbs_dir()`), add:

```cpp
std::string trash_dir() { return data_dir() + "/trash"; }

std::string trash_path_for(const std::string& filename) {
    namespace fs = std::filesystem;
    fs::path base = fs::path(trash_dir()) / filename;
    if (!fs::exists(base)) return base.string();
    for (int i = 1;; i++) {
        fs::path candidate = fs::path(trash_dir()) / (filename + "." + std::to_string(i));
        if (!fs::exists(candidate)) return candidate.string();
    }
}
```

Confirm `src/paths.cpp` already includes `<filesystem>` (it uses `fs::` throughout) — if it uses a `namespace fs` alias at file scope, drop the local alias line above and rely on it.

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='TrashPathTest.*'`
Expected: PASS (2 tests).

- [ ] **Step 6: Commit**

```bash
git add src/paths.h src/paths.cpp tests/test_paths.cpp
git commit -m "Add trash_dir() and trash_path_for() path helpers"
```

---

## Task 2: `remove_exec` module — trash and hard-delete a single file

**Files:**
- Create: `src/remove_exec.h`
- Create: `src/remove_exec.cpp`
- Modify: `CMakeLists.txt` (add `src/remove_exec.cpp` to the `phig-lib` source list near `src/organize_exec.cpp`; add `tests/test_remove_exec.cpp` to the `phig-tests` list)
- Test: `tests/test_remove_exec.cpp` (create)

**Interfaces:**
- Consumes: `trash_dir()`, `trash_path_for()` (Task 1); `Database::log_add_entry` with `OperationEntry{kind, src, dst, restore_json}`; `find_sidecars()` from `sidecar.h`.
- Produces:
  - `std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id);`
    Moves the single file `path` into the trash (same-fs `fs::rename`, else copy-then-`fs::remove`), creating `trash_dir()` first. Appends a `{"trash", path, <trash_dst>, ""}` log entry when `op_id > 0`. Returns the trash destination path. Throws `std::filesystem_error`/`std::runtime_error` on failure (nothing logged if the move failed).
  - `void execute_hard_delete_action(Database& db, const std::string& path, int64_t op_id);`
    `fs::remove(path)`. Appends `{"delete_file", path, "", ""}` when `op_id > 0`. A file already absent is not an error (removes nothing, still logs the `delete_file` entry so the audit records the intent). Throws on a real removal error.
  - `std::vector<std::string> remove_media_with_sidecars(Database& db, const std::string& media_path, bool hard, int64_t op_id);`
    Applies the chosen disk action to `media_path` and each of its `find_sidecars()`, in that order. Returns the list of paths acted on (media + sidecars) for the caller to pass to `delete_paths_logged()`. On a per-file disk error it throws (the caller decides skip-and-continue).

- [ ] **Step 1: Write `src/remove_exec.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "database.h"

// Per-file removal executor for `phig rm` (plan 027). Mirrors organize_exec:
// the disk action plus its activity-log entry live here; the command function
// owns the operation lifecycle (log_begin/finish) and DB-row deletion.

// Move `path` into trash_dir() (created on first use). Same-filesystem moves
// use fs::rename; cross-mount (EXDEV) falls back to copy-then-remove. Collision
// names are suffixed via trash_path_for(). Logs {"trash", path, <dst>, ""} when
// op_id > 0. Returns the trash destination. Throws on failure (unlogged).
std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id);

// Permanently remove `path` (fs::remove). A file already gone is not an error.
// Logs {"delete_file", path, "", ""} when op_id > 0. Throws on a real error.
void execute_hard_delete_action(Database& db, const std::string& path, int64_t op_id);

// Apply the disk action (hard ? hard-delete : trash) to `media_path` and each
// of its find_sidecars(), each logged as its own entry. Returns every path
// acted on (media first, then sidecars) for the caller to delete rows for.
// Throws on the first disk error.
std::vector<std::string> remove_media_with_sidecars(Database& db, const std::string& media_path,
                                                    bool hard, int64_t op_id);
```

- [ ] **Step 2: Write the failing tests in `tests/test_remove_exec.cpp`**

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "database.h"
#include "paths.h"
#include "remove_exec.h"
#include "types.h"

namespace fs = std::filesystem;

class RemoveExecTest : public ::testing::Test {
   protected:
    fs::path root;   // XDG_DATA_HOME
    fs::path photos; // where media lives
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        root = fs::temp_directory_path() / "phig-remove-exec-test";
        fs::remove_all(root);
        fs::create_directories(root);
        setenv("XDG_DATA_HOME", root.c_str(), 1);
        photos = root / "photos";
        fs::create_directories(photos);
        db_path = (root / "t.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        unsetenv("XDG_DATA_HOME");
        fs::remove_all(root);
    }

    void write_file(const fs::path& p, const std::string& c) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << c;
    }

    // Count operation_entries rows of a given kind for a src path.
    int count_entries(const std::string& kind, const std::string& src) {
        std::string sql = "SELECT COUNT(*) FROM operation_entries WHERE kind='" + kind +
                          "' AND src='" + src + "'";
        int n = 0;
        sqlite3_exec(
            db->db_ptr(), sql.c_str(),
            [](void* out, int, char** v, char**) {
                *static_cast<int*>(out) = std::atoi(v[0]);
                return 0;
            },
            &n, nullptr);
        return n;
    }
};

TEST_F(RemoveExecTest, TrashMovesFileAndLogs) {
    std::string src = (photos / "IMG_1.jpg").string();
    write_file(src, "data");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    std::string dst = execute_trash_action(*db, src, op);

    EXPECT_FALSE(fs::exists(src));
    EXPECT_TRUE(fs::exists(dst));
    EXPECT_EQ(fs::path(dst).parent_path(), fs::path(trash_dir()));
    EXPECT_EQ(count_entries("trash", src), 1);
}

TEST_F(RemoveExecTest, TrashSuffixesOnCollisionAndLogsSuffixedPath) {
    std::string a = (photos / "IMG_1.jpg").string();
    std::string b = (photos / "sub" / "IMG_1.jpg").string();  // same filename
    write_file(a, "a");
    write_file(b, "b");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    std::string da = execute_trash_action(*db, a, op);
    std::string db2 = execute_trash_action(*db, b, op);

    EXPECT_EQ(fs::path(da).filename().string(), "IMG_1.jpg");
    EXPECT_EQ(fs::path(db2).filename().string(), "IMG_1.jpg.1");
    EXPECT_TRUE(fs::exists(da));
    EXPECT_TRUE(fs::exists(db2));

    // The log records the actual (suffixed) trash path in dst, so a future undo
    // restores from the right place. Verify dst for the second file.
    std::string dst_logged;
    std::string sql = "SELECT dst FROM operation_entries WHERE kind='trash' AND src='" + b + "'";
    sqlite3_exec(
        db->db_ptr(), sql.c_str(),
        [](void* out, int, char** v, char**) {
            *static_cast<std::string*>(out) = v[0] ? v[0] : "";
            return 0;
        },
        &dst_logged, nullptr);
    EXPECT_EQ(dst_logged, db2);
}

TEST_F(RemoveExecTest, HardDeleteRemovesAndLogs) {
    std::string src = (photos / "IMG_2.jpg").string();
    write_file(src, "data");
    int64_t op = db->log_begin_operation("rm", "phig rm --hard", "2026-07-12T00:00:00Z");

    execute_hard_delete_action(*db, src, op);

    EXPECT_FALSE(fs::exists(src));
    EXPECT_EQ(count_entries("delete_file", src), 1);
    EXPECT_FALSE(fs::exists(trash_dir() + "/IMG_2.jpg"));  // not trashed
}

TEST_F(RemoveExecTest, HardDeleteMissingFileStillLogs) {
    std::string src = (photos / "gone.jpg").string();  // never created
    int64_t op = db->log_begin_operation("rm", "phig rm --hard", "2026-07-12T00:00:00Z");

    EXPECT_NO_THROW(execute_hard_delete_action(*db, src, op));
    EXPECT_EQ(count_entries("delete_file", src), 1);
}

TEST_F(RemoveExecTest, RemoveMediaCarriesSidecarsNotSiblings) {
    std::string raw = (photos / "IMG_9.CR2").string();
    std::string jpg = (photos / "IMG_9.JPG").string();   // same-stem sibling
    std::string xmp = (photos / "IMG_9.xmp").string();    // sidecar
    write_file(raw, "raw");
    write_file(jpg, "jpg");
    write_file(xmp, "xmp");
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");

    auto acted = remove_media_with_sidecars(*db, raw, /*hard=*/false, op);

    // RAW and its sidecar are trashed; the JPEG sibling is left alone.
    EXPECT_FALSE(fs::exists(raw));
    EXPECT_FALSE(fs::exists(xmp));
    EXPECT_TRUE(fs::exists(jpg));
    // acted contains raw + xmp (order: media first).
    ASSERT_EQ(acted.size(), 2u);
    EXPECT_EQ(acted[0], raw);
    EXPECT_EQ(count_entries("trash", raw), 1);
    EXPECT_EQ(count_entries("trash", xmp), 1);
}
```

Note: the sibling test relies on `find_sidecars()` returning `IMG_9.xmp` for `IMG_9.CR2` and NOT returning `IMG_9.JPG` (a media file, not a sidecar) — this is existing `find_sidecars` behavior (see `sidecar.h`). If `find_sidecars` cedes the stem-style `.xmp` to the JPEG sibling, adjust the expectation to append-style `IMG_9.CR2.xmp`; write the sidecar as `IMG_9.CR2.xmp` instead to avoid the ceding rule. Prefer the append-style sidecar in the test to keep it unambiguous.

- [ ] **Step 3: Add `src/remove_exec.cpp` and CMake wiring, run to verify tests fail**

Add to `CMakeLists.txt`: `src/remove_exec.cpp` in the `phig-lib` `add_library(...)` list (next to `src/organize_exec.cpp`), and `tests/test_remove_exec.cpp` in the `phig-tests` `add_executable(...)` list.

Run: `cmake --build build && ./build/phig-tests --gtest_filter='RemoveExecTest.*'`
Expected: FAIL — link error (functions undefined) until Step 4.

- [ ] **Step 4: Implement `src/remove_exec.cpp`**

```cpp
#include "remove_exec.h"

#include <filesystem>
#include <system_error>

#include "paths.h"
#include "sidecar.h"

namespace fs = std::filesystem;

std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id) {
    fs::create_directories(trash_dir());
    std::string dst = trash_path_for(fs::path(path).filename().string());

    std::error_code ec;
    fs::rename(path, dst, ec);
    if (ec) {
        // Cross-mount (EXDEV) or other rename failure: copy then remove.
        fs::copy_file(path, dst, fs::copy_options::overwrite_existing);
        fs::remove(path);  // throws on failure — leaves the copy in trash
    }

    if (op_id > 0) db.log_add_entry(op_id, {"trash", path, dst, ""});
    return dst;
}

void execute_hard_delete_action(Database& db, const std::string& path, int64_t op_id) {
    std::error_code ec;
    fs::remove(path, ec);  // false+no-ec if already gone; that's fine
    if (ec) throw fs::filesystem_error("hard delete failed", path, ec);
    if (op_id > 0) db.log_add_entry(op_id, {"delete_file", path, "", ""});
}

std::vector<std::string> remove_media_with_sidecars(Database& db, const std::string& media_path,
                                                    bool hard, int64_t op_id) {
    std::vector<std::string> acted;
    auto act = [&](const std::string& p) {
        if (hard)
            execute_hard_delete_action(db, p, op_id);
        else
            execute_trash_action(db, p, op_id);
        acted.push_back(p);
    };

    act(media_path);
    for (const auto& side : find_sidecars(fs::path(media_path))) {
        act(side.string());
    }
    return acted;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='RemoveExecTest.*'`
Expected: PASS (5 tests).

- [ ] **Step 6: Commit**

```bash
git add src/remove_exec.h src/remove_exec.cpp tests/test_remove_exec.cpp CMakeLists.txt
git commit -m "Add remove_exec: trash/hard-delete a file and its sidecars"
```

---

## Task 3: Extend the OperationEntry kind comment

**Files:**
- Modify: `src/database.h` (the `OperationEntry::kind` comment, around lines 31-32)

**Interfaces:** none (comment-only; documents the two kinds Task 2 introduced).

- [ ] **Step 1: Update the comment**

Change the `kind` comment to include the new kinds:

```cpp
        std::string kind;  // "move" | "copy" | "purge_row" | "relink" | "move_sidecar" |
                           // "copy_sidecar" | "trash" | "delete_file"
```

- [ ] **Step 2: Build to confirm no breakage**

Run: `cmake --build build`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add src/database.h
git commit -m "Document trash/delete_file activity-log kinds"
```

---

## Task 4: `cmd_rm` — selection, guard, modes, operation lifecycle

**Files:**
- Modify: `src/main.cpp` (add `RemoveOptions` struct, `cmd_rm`, and — later, Task 6 — dispatch)
- Test: `tests/test_remove_exec.cpp` is module-level; command-level behavior is exercised end-to-end via the built binary in Task 7. This task's verification is a build + a focused manual run.

**Interfaces:**
- Consumes: `passes_selection()` (selection.h), `paths_safe_to_delete()` (paths.h), `remove_media_with_sidecars()` (Task 2), `delete_paths_logged()`, `log_begin_operation`/`log_finish_operation`/`log_prune`, `iso_utc_now()`/`join_argv()` (static in main.cpp — `cmd_rm` lives in main.cpp so it can call them), `db.get_all_images()`.
- Produces: `int cmd_rm(const RemoveOptions& opts);` returning a process exit code.

- [ ] **Step 1: Add the `RemoveOptions` struct**

Place near `PurgeOptions` (which this task will delete in Task 5; for now add alongside). In `src/main.cpp`:

```cpp
enum class RemoveMode { Trash, Hard, DbOnly, Orphans };

struct RemoveOptions {
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    RemoveMode mode = RemoveMode::Trash;
    bool dry_run = false;
    bool assume_yes = false;
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;
};
```

- [ ] **Step 2: Implement `cmd_rm`**

```cpp
int cmd_rm(const RemoveOptions& opts) {
    const bool needs_match = opts.mode == RemoveMode::Trash || opts.mode == RemoveMode::Hard ||
                             opts.mode == RemoveMode::DbOnly;
    if (needs_match && opts.matches.empty()) {
        std::cerr << "Error: rm requires at least one --match pattern (except with --orphans)\n";
        return 1;
    }

    Database db(opts.db_path);
    db.init_schema();

    auto all_images = db.get_all_images();
    const size_t total = all_images.size();

    // Selection: match/exclude over the DB.
    std::vector<std::string> matched;
    for (const auto& img : all_images) {
        if (passes_selection(img.path, opts.matches, opts.excludes)) matched.push_back(img.path);
    }

    // --orphans narrows to rows whose files are missing (mount-safe).
    if (opts.mode == RemoveMode::Orphans) {
        matched = paths_safe_to_delete(matched);
    }

    if (matched.empty()) {
        std::cout << "No matching entries found.\n";
        return 0;
    }

    const char* mode_name = opts.mode == RemoveMode::Trash    ? "trash"
                            : opts.mode == RemoveMode::Hard   ? "hard-delete"
                            : opts.mode == RemoveMode::DbOnly ? "db-only"
                                                              : "orphans";
    std::cout << "Found " << matched.size() << " matching entries (mode: " << mode_name << ")\n";

    if (opts.dry_run) {
        std::cout << "\n[DRY RUN]\n";
        for (const auto& p : matched) std::cout << "  " << p << "\n";
        return 0;
    }

    // Guard: --hard always needs confirmation; other modes only when broad.
    const bool broad = total > 0 && matched.size() * 2 >= total && matched.size() >= 20;
    const bool needs_confirm = opts.mode == RemoveMode::Hard || broad;
    if (needs_confirm && !opts.assume_yes) {
        if (opts.mode == RemoveMode::Hard)
            std::cout << "\nThis will PERMANENTLY delete " << matched.size() << " file(s).\n";
        else
            std::cout << "\nThis will remove " << matched.size() << " of " << total
                      << " database entries (" << (matched.size() * 100 / total) << "%).\n";
        if (!::isatty(fileno(stdin))) {
            std::cerr << "Refusing without confirmation.\n"
                         "Re-run with --yes to proceed, or --dry-run to preview.\n";
            return 1;
        }
        std::cout << "Type 'yes' to proceed: " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "yes") {
            std::cout << "Aborted.\n";
            return 1;
        }
    }

    const bool does_disk = opts.mode == RemoveMode::Trash || opts.mode == RemoveMode::Hard;
    const bool hard = opts.mode == RemoveMode::Hard;

    int64_t op_id = 0;
    const bool logging = opts.log_enabled;
    if (logging) op_id = db.log_begin_operation("rm", opts.argv_str, iso_utc_now());

    std::vector<std::string> rows_to_delete;  // media + sidecars whose rows to drop
    size_t failed = 0;
    try {
        if (does_disk) {
            // Per-file disk action first; collect the paths whose rows to drop.
            for (const auto& media : matched) {
                try {
                    auto acted = remove_media_with_sidecars(db, media, hard, op_id);
                    // Only the media path has a DB row; sidecars are not indexed.
                    rows_to_delete.push_back(media);
                } catch (const std::exception& e) {
                    log_error(std::string("rm: ") + media + ": " + e.what());
                    failed++;
                }
            }
        } else {
            // db-only / orphans: no disk action; every matched path is a row.
            rows_to_delete = matched;
        }
        // Drop rows (writes purge_row entries) in the same operation.
        db.delete_paths_logged(rows_to_delete, op_id);
    } catch (...) {
        if (logging) {
            try {
                db.log_finish_operation(op_id, "interrupted");
            } catch (...) {
            }
        }
        throw;
    }

    if (logging) {
        db.log_finish_operation(op_id, failed ? "interrupted" : "completed");
        db.log_prune(opts.log_max_entries);
    }

    std::cout << "Removed " << rows_to_delete.size() << " entries";
    if (failed) std::cout << " (" << failed << " file(s) failed and were skipped)";
    std::cout << ".\nTotal DB: " << db.count() << " images\n";
    return failed ? 1 : 0;
}
```

Note: only the media path has a DB row (sidecars and demoted JPEGs are not indexed), so `rows_to_delete` gets the media path, not the sidecar paths. The sidecar files were still trashed/deleted and logged by `remove_media_with_sidecars`.

- [ ] **Step 3: Build to confirm it compiles (dispatch added in Task 6)**

Add a temporary reference so the compiler doesn't warn "unused" if needed, or simply confirm it compiles as a free function. Run: `cmake --build build`
Expected: compiles (the function is defined but not yet dispatched — that's fine; C++ tolerates an unused non-static free function).

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "Add cmd_rm: selection, mode guard, and operation lifecycle"
```

---

## Task 5: Remove the `purge` command and `cmd_purge`

**Files:**
- Modify: `src/main.cpp` (delete `PurgeOptions`, `cmd_purge`, and the `command == "purge"` dispatch branch)
- Modify: `tests/test_database.cpp` (only if any test invokes the `purge` *command* or asserts a `"purge"` operation name; the DB-level `Database::purge()` method and its unit tests stay)

**Interfaces:**
- Removes: the `purge` command. `rm --db-only` is its replacement.

- [ ] **Step 1: Confirm what references the purge command vs. the DB method**

Run: `grep -n 'cmd_purge\|PurgeOptions\|command == "purge"\|"purge"' src/main.cpp tests/test_database.cpp`
Expected: `cmd_purge`, `PurgeOptions`, the dispatch branch in main.cpp; in tests, any `log_begin_operation("purge", …)` or command-level purge use. The `Database::purge(...)` method itself is separate — leave it.

- [ ] **Step 2: Delete `PurgeOptions` and `cmd_purge` from `src/main.cpp`**

Remove the `struct PurgeOptions { … };` and the entire `int cmd_purge(const PurgeOptions& opts) { … }` function (the block read at spec-writing time spanned the struct at ~1225 and the function through its `return 0;`).

- [ ] **Step 3: Delete the dispatch branch**

Remove the `} else if (command == "purge") { … }` block in `main()` (the arg-parsing + `cmd_purge` call).

- [ ] **Step 4: Update any purge-command test references**

If `tests/test_database.cpp` (or another test) logs an operation named `"purge"` at the command level, retarget it to `"rm"` or leave DB-method tests untouched (they call `Database::purge()` directly, which is fine). Do not delete `Database::purge()` unit tests.

- [ ] **Step 5: Build and run the full suite**

Run: `cmake --build build && (cd build && ctest --output-on-failure)`
Expected: builds; all tests pass. (`cmd_rm` is defined but not yet dispatched — added next.)

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp tests/test_database.cpp
git commit -m "Remove purge command (replaced by rm --db-only)"
```

---

## Task 6: Dispatch `rm` and `trash`; `cmd_trash_empty`; usage

**Files:**
- Modify: `src/main.cpp` (arg parsing + dispatch for `rm` and `trash`; `cmd_trash_empty`; `print_usage()`)

**Interfaces:**
- Consumes: `cmd_rm` (Task 4), `trash_dir()` (Task 1), config seeding (`cfg.log_enabled`/`cfg.log_max_entries`) exactly as the `mv`/`purge` branches did.
- Produces: `int cmd_trash_empty(bool assume_yes);` and the `rm` / `trash` dispatch branches.

- [ ] **Step 1: Implement `cmd_trash_empty`**

```cpp
int cmd_trash_empty(bool assume_yes) {
    std::string dir = trash_dir();
    if (!fs::exists(dir)) {
        std::cout << "Trash is empty.\n";
        return 0;
    }
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        n++;
    }
    if (n == 0) {
        std::cout << "Trash is empty.\n";
        return 0;
    }
    if (!assume_yes) {
        std::cout << "This will PERMANENTLY delete " << n << " item(s) from " << dir << ".\n";
        if (!::isatty(fileno(stdin))) {
            std::cerr << "Refusing without confirmation. Re-run with --yes.\n";
            return 1;
        }
        std::cout << "Type 'yes' to proceed: " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "yes") {
            std::cout << "Aborted.\n";
            return 1;
        }
    }
    for (const auto& e : fs::directory_iterator(dir)) fs::remove_all(e.path());
    std::cout << "Emptied trash (" << n << " item(s)).\n";
    return 0;
}
```

Ensure `main.cpp` has `namespace fs = std::filesystem;` in scope (it does — used elsewhere).

- [ ] **Step 2: Add the `rm` dispatch branch**

Insert alongside the other command branches in `main()` (model the arg loop on the deleted `purge` branch). `--hard`, `--db-only`, `--orphans` set `opts.mode`; two of them is an error:

```cpp
    } else if (command == "rm") {
        RemoveOptions opts;
        opts.db_path = default_db_path();
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        int mode_flags = 0;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--hard") {
                opts.mode = RemoveMode::Hard;
                mode_flags++;
            } else if (arg == "--db-only") {
                opts.mode = RemoveMode::DbOnly;
                mode_flags++;
            } else if (arg == "--orphans") {
                opts.mode = RemoveMode::Orphans;
                mode_flags++;
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--yes" || arg == "-y") {
                opts.assume_yes = true;
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }
        if (mode_flags > 1) {
            std::cerr << "Error: --hard, --db-only, and --orphans are mutually exclusive\n";
            return 1;
        }
        try {
            return cmd_rm(opts);
        } catch (const std::exception& e) {
            log_error(std::string("rm: ") + e.what());
            return 1;
        }
    } else if (command == "trash") {
        // Only subcommand: `trash empty`.
        if (argc < 3 || std::string(argv[2]) != "empty") {
            std::cerr << "Usage: phig trash empty [--yes]\n";
            return 1;
        }
        bool yes = false;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--yes" || arg == "-y")
                yes = true;
            else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        try {
            return cmd_trash_empty(yes);
        } catch (const std::exception& e) {
            log_error(std::string("trash: ") + e.what());
            return 1;
        }
    }
```

Match the exact `} else if` chaining style already in `main()`; insert where the `purge` branch was.

- [ ] **Step 3: Update `print_usage()`**

Remove the `phig purge …` line. Add (near the other file commands):

```cpp
        << "  phig rm [--match|--exclude] [--hard|--db-only|--orphans] [--dry-run] [--yes]\n"
        << "                               Remove images: trash (default), --hard (permanent),\n"
        << "                               --db-only (keep file), --orphans (missing files)\n"
        << "  phig trash empty [--yes]     Permanently delete everything in the trash\n"
```

And in the flags section, keep the existing `--match`/`--exclude` help; add a short note that `rm` uses them.

- [ ] **Step 4: Build**

Run: `cmake --build build`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Dispatch rm and trash empty; update usage"
```

---

## Task 7: End-to-end command tests via the built binary

**Files:**
- Test: `tests/test_remove_exec.cpp` (add a `RmCommandE2E` section) OR extend `tests/test_porcelain.cpp` if that's where CLI-level tests live. Check first: `grep -l 'system(\|popen\|/phig ' tests/*.cpp`. If the repo has no binary-invoking test harness, add these as `cmd_rm`-level tests calling `cmd_rm(opts)` directly (it's a free function in main.cpp — expose it via a small `main_commands.h` only if main.cpp symbols aren't linkable into tests; otherwise test the observable effects through the module + DB as below).

**Interfaces:**
- Consumes: everything above. These tests assert observable end-state: files trashed/deleted, rows gone, log entries present, guards enforced.

**Decision:** `cmd_rm` lives in `main.cpp`, which is NOT part of `phig-lib` (tests link `phig-lib`, not `main.cpp`). So command-level logic is not directly callable from tests. Two options — pick during implementation:
  - (a) Keep `cmd_rm` in `main.cpp` and cover command behavior with a shell-level test that runs `./build/phig rm …` against a temp DB (if a CLI test harness exists).
  - (b) Move `cmd_rm` + `RemoveOptions` into the `remove_exec` module (part of `phig-lib`) so it's unit-testable, leaving only arg-parsing in `main.cpp`. **Recommended** — it makes the guard, mode selection, and lifecycle directly testable and keeps `main.cpp` thin, consistent with how `cmd_organize` logic sits in `organize_exec`.

If (b): move `RemoveMode`, `RemoveOptions`, and `cmd_rm` into `remove_exec.{h,cpp}`. `cmd_rm` needs `iso_utc_now()`/`join_argv()` — pass `argv_str` (already in opts) and a timestamp string into `cmd_rm`, or add small public helpers. Simplest: `cmd_rm` takes `opts` (which already carries `argv_str`); replace the internal `iso_utc_now()` call with a `now_iso` parameter defaulting via a public `iso_utc_now()` moved to `paths`/a util, OR have `cmd_rm` accept the timestamp. Keep it minimal: add `std::string iso_utc_now();` to a shared header (e.g. `paths.h`) and define once, removing the static in main.cpp.

- [ ] **Step 1: Adopt option (b) — move cmd_rm into the library**

Move `RemoveMode`, `RemoveOptions`, `cmd_rm` declarations to `remove_exec.h` and definitions to `remove_exec.cpp`. In `main.cpp`, keep only the `rm`/`trash` arg-parsing that builds `RemoveOptions` and calls `cmd_rm`. Promote `iso_utc_now()` to a shared header if `cmd_rm` needs it (declare `std::string iso_utc_now();` in `paths.h`, define in `paths.cpp`, delete the `static` copy in `main.cpp`), or pass the timestamp in via `opts`. Rebuild.

Run: `cmake --build build`
Expected: builds clean.

- [ ] **Step 2: Write command-level tests (now linkable)**

Add to `tests/test_remove_exec.cpp` (fixture already sets `XDG_DATA_HOME` and a temp DB). Insert real image rows with `insert_row`-style helpers (see `test_organize_exec.cpp`) whose `path` points at real files under `photos`.

```cpp
TEST_F(RemoveExecTest, RmTrashRemovesFileAndRow) {
    std::string p = (photos / "a.jpg").string();
    write_file(p, "x");
    ImageInfo info; info.path = p; info.filename = "a.jpg"; info.extension = "jpg";
    info.file_size = 1; info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z"; info.sha256 = "s"; info.phash = 0;
    info.width = 1; info.height = 1; info.metadata_json = "{}";
    db->insert(info);

    RemoveOptions opts;
    opts.db_path = db_path;
    opts.matches = {"*a.jpg"};
    opts.mode = RemoveMode::Trash;
    opts.assume_yes = true;
    opts.argv_str = "phig rm --match *a.jpg";

    int rc = cmd_rm(opts);
    EXPECT_EQ(rc, 0);
    EXPECT_FALSE(fs::exists(p));                    // file trashed
    EXPECT_EQ(db->get_all_images().size(), 0u);     // row gone
}

TEST_F(RemoveExecTest, RmDbOnlyKeepsFile) {
    std::string p = (photos / "b.jpg").string();
    write_file(p, "x");
    ImageInfo info; info.path = p; info.filename = "b.jpg"; info.extension = "jpg";
    info.file_size = 1; info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z"; info.sha256 = "s"; info.phash = 0;
    info.width = 1; info.height = 1; info.metadata_json = "{}";
    db->insert(info);

    RemoveOptions opts;
    opts.db_path = db_path; opts.matches = {"*b.jpg"};
    opts.mode = RemoveMode::DbOnly; opts.assume_yes = true;
    opts.argv_str = "phig rm --db-only --match *b.jpg";

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(fs::exists(p));                      // file kept
    EXPECT_EQ(db->get_all_images().size(), 0u);      // row gone
}

TEST_F(RemoveExecTest, RmRequiresMatchExceptOrphans) {
    RemoveOptions opts;
    opts.db_path = db_path; opts.mode = RemoveMode::Trash; opts.assume_yes = true;
    EXPECT_EQ(cmd_rm(opts), 1);                      // no --match, non-orphans → error

    opts.mode = RemoveMode::Orphans;
    EXPECT_EQ(cmd_rm(opts), 0);                      // orphans with no rows → "none found", ok
}

TEST_F(RemoveExecTest, RmDryRunTouchesNothing) {
    std::string p = (photos / "c.jpg").string();
    write_file(p, "x");
    ImageInfo info; info.path = p; info.filename = "c.jpg"; info.extension = "jpg";
    info.file_size = 1; info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z"; info.sha256 = "s"; info.phash = 0;
    info.width = 1; info.height = 1; info.metadata_json = "{}";
    db->insert(info);

    RemoveOptions opts;
    opts.db_path = db_path; opts.matches = {"*c.jpg"};
    opts.mode = RemoveMode::Trash; opts.dry_run = true; opts.assume_yes = true;

    EXPECT_EQ(cmd_rm(opts), 0);
    EXPECT_TRUE(fs::exists(p));                      // untouched
    EXPECT_EQ(db->get_all_images().size(), 1u);      // row kept
}

TEST_F(RemoveExecTest, RmOrphansDropsMissingKeepsPresent) {
    std::string present = (photos / "here.jpg").string();
    std::string missing = (photos / "gone.jpg").string();
    write_file(present, "x");
    write_file(missing, "x");
    for (auto* p : {&present, &missing}) {
        ImageInfo info; info.path = *p; info.filename = fs::path(*p).filename().string();
        info.extension = "jpg"; info.file_size = 1;
        info.created_at = "2024-01-01T00:00:00Z"; info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = "s"; info.phash = 0; info.width = 1; info.height = 1;
        info.metadata_json = "{}";
        db->insert(info);
    }
    fs::remove(missing);  // now orphaned; parent dir still exists (mount-safe keeps present)

    RemoveOptions opts;
    opts.db_path = db_path; opts.mode = RemoveMode::Orphans; opts.assume_yes = true;
    opts.argv_str = "phig rm --orphans";

    EXPECT_EQ(cmd_rm(opts), 0);
    auto rows = db->get_all_images();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, present);                // present kept, missing dropped
}
```

- [ ] **Step 3: Run the new tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='RemoveExecTest.Rm*'`
Expected: PASS (5 tests).

- [ ] **Step 4: Run the full suite**

Run: `cd build && ctest --output-on-failure`
Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp src/remove_exec.h src/remove_exec.cpp src/paths.h src/paths.cpp tests/test_remove_exec.cpp
git commit -m "Move cmd_rm into remove_exec and cover it with command-level tests"
```

---

## Task 8: Docs and final verification

**Files:**
- Modify: `README.md`, `AGENTS.md`

- [ ] **Step 1: Update README.md**

Replace the `phig purge` usage/examples with `phig rm`. Show the modes:

```
phig rm --match "*/old-folder/*"            # trash (recoverable)
phig rm --match "*.tmp" --hard --yes        # permanent delete
phig rm --match "*/Trash/*" --db-only       # forget rows, keep files
phig rm --orphans                            # drop rows for missing files
phig trash empty --yes                       # empty the trash
```

Update any "Match/exclude" prose that referenced purge.

- [ ] **Step 2: Update AGENTS.md**

Replace the `phig purge [...]` command line with:

```
phig rm [--match <glob>] [--exclude <glob>] [--hard|--db-only|--orphans] [--dry-run] [--yes] [--db <path>]
phig trash empty [--yes] [--db <path>]
```

Remove the incorrect `--path` from the old purge line (it was never parsed). Add `remove_exec.h/cpp` to the architecture file list if that section enumerates modules.

- [ ] **Step 3: Verify the binary end-to-end (real run)**

```bash
# Fresh temp home so we don't touch the real DB/trash.
export XDG_DATA_HOME=$(mktemp -d)
D="$XDG_DATA_HOME/lib"; mkdir -p "$D"
printf x > "$D/one.jpg"; printf x > "$D/two.jpg"
./build/phig scan "$D"
./build/phig rm --match "*one.jpg" --dry-run          # lists, changes nothing
./build/phig rm --match "*one.jpg" --yes              # trashes one.jpg
ls "$XDG_DATA_HOME/phig/trash"                          # -> one.jpg
./build/phig rm --match "*two.jpg" --hard              # refuses: --hard needs --yes
./build/phig rm --match "*two.jpg" --hard --yes       # permanently deletes two.jpg
./build/phig trash empty --yes                          # empties trash
./build/phig purge --match '*'                          # -> unknown command
```
Expected: dry-run touches nothing; trash move lands in `$XDG_DATA_HOME/phig/trash`; `--hard` without `--yes` refuses; `purge` is unknown. Clean up: `rm -rf "$XDG_DATA_HOME"; unset XDG_DATA_HOME`.

- [ ] **Step 4: Commit**

```bash
git add README.md AGENTS.md
git commit -m "Document phig rm and trash empty; drop purge from docs"
```

---

## Self-Review Notes (for the implementer)

- **`--ignored` is deliberately absent** — it belongs to plan 028. `RemoveMode` has four values here; plan 028 adds the fifth.
- **Row deletion covers media rows only.** Sidecars and demoted JPEGs are not indexed, so `delete_paths_logged` is called with the media paths; the sidecar *files* are still trashed/deleted and logged by `remove_media_with_sidecars`. Do not pass sidecar paths to `delete_paths_logged`.
- **Task 7 option (b)** (move `cmd_rm` into `remove_exec`) is the recommended path and is assumed by the command-level tests. If you keep `cmd_rm` in `main.cpp` instead, those tests must become shell-level runs of the built binary.
- **Guard parity:** the broad test (`matched*2 >= total && matched >= 20`) and the tty/`--yes` handling are copied verbatim from the old `cmd_purge`; `--hard` adds an always-confirm on top.
- **Cross-mount (EXDEV) trash** is in the spec's test list but is hard to exercise in a unit test (needs two real filesystems). The code path (`fs::rename` fails → `fs::copy_file` + `fs::remove`) is covered by the same-fs `TrashMovesFileAndLogs` for the logging/return contract; the EXDEV branch itself is exercised only if a tmpfs/second mount is available in CI. If you can bind-mount a second filesystem under the temp dir, add a test that trashes a file living on it; otherwise note it as manually verified and move on. Do not fake EXDEV by mocking `fs::rename`.
