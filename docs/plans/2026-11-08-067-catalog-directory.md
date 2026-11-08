# Catalog Directory and Test Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a catalog one directory holding a collection's database, lock, trash, thumbnails and log, selected by `--catalog <dir>`, `PHIG_CATALOG`, the config file, or the default `~/.local/share/phig/`; make reads never create or migrate a catalog; and keep the test suite out of the developer's real files.

**Architecture:** `core/paths` gains `CatalogPaths`, a process-wide installed catalog, and `lock_path(db)`, and the existing getters (`default_db_path`, `thumbs_dir`, `trash_dir`, `default_log_path`) read the installed catalog so engine call sites do not change. A new `core/catalog` holds the open policy (`open_catalog_db` with `Read`/`Write`/`Create`), catalog resolution (flag > env > config > default), and the one-time move of legacy thumbnails and log. The CLI, libphig and gtk each resolve a catalog directory and install it.

**Tech Stack:** C++20, CMake, SQLite, GoogleTest, GLib test framework (gtk), Python 3 (`scripts/rules.py`).

**Spec:** `plans/067-catalog-directory.md`. Read it before starting. Where this document and the spec disagree, the spec wins unless this document's "Decisions beyond 067" section says otherwise.

## Global Constraints

- **Never write to the developer's real phig files**: `~/.local/share/phig/`, `~/.cache/phig/`, `~/.local/state/phig/`, `~/.config/phig/`, or `/mnt/data/Pictures`. After Task 1 the test binaries redirect themselves. Every manual command you run must first export a scratch environment:
  ```bash
  export S=$(mktemp -d /tmp/phig-067-XXXX)
  export HOME=$S/home XDG_CONFIG_HOME=$S/config XDG_DATA_HOME=$S/data XDG_CACHE_HOME=$S/cache XDG_STATE_HOME=$S/state
  unset PHIG_CATALOG
  ```
  The gtk test binaries are GLib tests without the gtest environment, so never let a gtk test resolve a default path without its own redirect.
- **Build and test:**
  - Main: `cmake -B build && cmake --build build -j12`, then `ctest --test-dir build --parallel 6`.
  - gtk: `cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON -DPHIG_WERROR=ON && cmake --build build/with-gtk -j12 --target gtk-all`, then `ctest --test-dir build/with-gtk -L gtk`.
- **Warnings are errors.** Configure with `-DPHIG_WERROR=ON`, as CI does.
- **Formatting:** clang-format, Google style, 4-space indent, 100 columns. Run `clang-format -i` on every C/C++ file you touch, then `clang-format --dry-run --Werror` on the same files.
- **Rules of record:** a guard changed in this plan updates its `rules/*.toml` record in the same commit. Run `python3 scripts/rules.py --check --emit` and commit the regenerated `rules/by-*.md` views with it. Never hand-edit `rules/by-*.md`.
- **Comments describe the present:** what the code does and why, never what it used to do. No plan numbers as justification in comments.
- **Prose is plain:** no flourishes or padding, in comments, commit messages and docs.
- **Commit messages** carry no AI attribution and no `Co-Authored-By` trailer.
- **Data-safety changes ship with real-behavior tests**, not mocks. Subprocess tests drive the built `phig` binary through `run_phig` (`tests/phig_binary.h`).
- **Catalog precedence**, highest first: `--catalog <dir>`, then `PHIG_CATALOG`, then `[paths] catalog` in the config file, then `default_catalog_dir()` = `data_dir()` = `$XDG_DATA_HOME/phig`. An empty `PHIG_CATALOG` counts as unset.
- **A catalog exists when `<dir>/phig.db` exists.** A directory holding only a lock file or a log is not a catalog.
- **Known failures that are not yours:**
  - `OrganizeEngineTest.CancellationPartwayMarksTheOperationInterrupted` can flake under heavy `--parallel`. Re-run it alone.
  - If exactly the three plan-046 `DatabaseTest` vec_faces cases fail, record `sqlite3_libversion()` in plan 046 and continue.
  - `DatabaseConcurrency.ConcurrentMigrationIsSafe`: see root `CLAUDE.md`.

## Decisions beyond 067

Rulings made while writing this plan. Each follows from the spec but is not spelled out in it.

1. **Dry runs only read.** R70 says a command that only reads neither creates nor migrates. A `--dry-run` of `rm`, `cp` or `mv` only reads, so it opens the catalog with `CatalogAccess::Read`. R20 and R21 currently record the opposite ("it still creates the database file and applies any pending schema migration"), and their statements, tests and gaps are rewritten in Task 7.
2. **The log file sink is enabled only when the catalog exists, or the command is `scan`.** spdlog's rotating sink creates its file at construction, so an unconditional sink would create `<catalog>/phig.log`, and with it the catalog directory, on a read against a missing catalog. Without the sink, stderr warnings still print.
3. **The writer lock checks the catalog first.** `acquire_writer_lock(no_wait, may_create)` calls `require_catalog` unless `may_create` is set, because creating the lock file creates the catalog directory. `scan` and `models download` pass `may_create = true`; every other lock-taking command passes nothing.
4. **The removed config keys fail loudly.** `[paths] db` and `[paths] thumbs_dir` fail with a message naming `[paths] catalog`, rather than taking the unknown-key path, which warns and ignores the key. Ignoring `db` would silently send every command to the default catalog.
5. **libphig's read-write context still creates a catalog.** `PHIG_READ_WRITE` opens with `CatalogAccess::Create`, as it creates the database today, because a client indexes into a new catalog through a read-write context. `PHIG_READ_ONLY` opens with `Read`, which already refuses a missing database and a mismatched schema.
6. **A transitional `set_path_overrides`.** Task 2 reimplements `set_path_overrides` on top of the installed catalog so its callers keep working. Task 5 removes the CLI's call, and Task 10 removes the function with its last callers.
7. **Not fixed here: libphig ignores `phig_config.models_dir`.** `phig_init` never reads it, although `phig.h` documents it. It is recorded in the Outcome as a finding, not changed.

## File map

| File | Change |
|---|---|
| `tests/scratch_home.cpp` | Create: gtest global environment redirecting `HOME`, the XDG directories and `PHIG_CATALOG`. |
| `CMakeLists.txt` | Add `tests/scratch_home.cpp` and `tests/test_catalog.cpp` to the test targets, and `core/catalog.cpp` to `phig-core`'s sources. Check where `core/` sources are listed: `core/CMakeLists.txt`. |
| `core/paths.h`, `core/paths.cpp` | `CatalogPaths`, `catalog_paths`, `default_catalog_dir`, `set_catalog`, `clear_catalog`, `current_catalog`, `lock_path(db)`, `set_models_dir`. The getters read the installed catalog. |
| `core/catalog.h`, `core/catalog.cpp` | Create: `CatalogAccess`, `CatalogError`, `open_catalog_db`, `require_catalog`, `ResolvedCatalog`, `resolve_catalog_dir`, `LegacyMigration`, `migrate_legacy_catalog_files`. |
| `core/thumbs.h`, `core/thumbs.cpp` | `ensure_cachedir_tag`, called after a thumbnail write. |
| `core/config.h`, `core/config.cpp` | `catalog` replaces `db` and `thumbs_dir`. |
| `core/remove_exec.cpp` | `cmd_rm` opens through `open_catalog_db`. |
| `cli/main.cpp` | Global `--catalog`, catalog resolution, removal of `--db`, opens through `open_catalog_db`, lock via catalog, log gating, usage text. |
| `libphig/include/phig/phig.h`, `libphig/context.cpp`, `libphig/internal.h`, `libphig/mutate.cpp`, `libphig/settings.cpp` | `catalog_dir` replaces `db_path` and `thumbs_dir`; `PHIG_KEY_CATALOG` replaces `PHIG_KEY_DB` and `PHIG_KEY_THUMBS_DIR`. |
| `gtk/src/collection.h`, `gtk/src/collection.cpp`, `gtk/src/main.cpp`, `gtk/tests/test-collection.cpp` | Open by catalog directory; default from `resolve_catalog_dir`. |
| `tests/test_paths.cpp`, `tests/test_config.cpp`, `tests/test_thumbs.cpp`, `tests/test_thumbs_engine.cpp`, `tests/test_libphig_*.cpp`, `tests/test_faces_engine.cpp`, `tests/test_database.cpp`, `tests/test_organize_engine.cpp`, `tests/test_remove_exec.cpp`, `tests/test_lockfile.cpp`, `tests/phig_binary.h` | Updated as each task says. |
| `tests/test_catalog.cpp` | Create: in-process tests of `core/catalog`, plus the CLI subprocess tests for R70, trash and thumbnails. |
| `rules/concurrency.toml`, `rules/boundary.toml`, `rules/authorization.toml`, `rules/recoverability.toml`, `rules/operations.toml` | R50, R70, R20, R21, trash rationales, `--catalog`. |
| `plans/039-*.md` | Drop R50 from `closes_rules` and its paragraph. |
| Root `CLAUDE.md`, `README.md`, `cli/CLAUDE.md`, `libphig/CLAUDE.md`, `gtk/CLAUDE.md`, `gtk/README.md`, `plans/067-catalog-directory.md`, `plans/000-INDEX.md` | Docs; 067 marked completed. |

## Pre-flight

- [ ] **Step 1: Branch and base.** This needs the user's approval before you run it.
  ```bash
  git checkout master && git checkout -b 067-catalog-directory
  git rev-parse HEAD > /tmp/phig-067-base.txt
  ```
  Use `$(cat /tmp/phig-067-base.txt)` wherever this plan says BASE.

- [ ] **Step 2: Record the real files' timestamps.** This is read-only: it only stats them.
  ```bash
  stat -c '%Y %n' ~/.local/state/phig/phig.log ~/.local/share/phig/phig.db.lock > /tmp/phig-067-real-mtimes.txt; cat /tmp/phig-067-real-mtimes.txt
  ```
  Task 1 and the final verification compare against this.

- [ ] **Step 3: A clean baseline.**
  ```bash
  cmake -B build -DPHIG_WERROR=ON && cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON -DPHIG_WERROR=ON && cmake --build build/with-gtk -j12 --target gtk-all && ctest --test-dir build/with-gtk -L gtk 2>&1 | tail -3
  python3 scripts/rules.py --check
  ```
  Expected: all main tests pass apart from the known flakes, all 3 gtk tests pass, and the rules check prints `OK`. This baseline run still rewrites the real lock file and log; Task 1 stops that.

---

### Task 1: Keep the test binaries out of the developer's files

**Files:**
- Create: `tests/scratch_home.cpp`
- Modify: `CMakeLists.txt`, in the `add_executable(phig-tests` and `add_executable(phig-perf-tests` source lists

**Interfaces:**
- Consumes: `make_temp_dir` (`tests/temp_dir.h`), `models_dir()`, `data_dir()`, `default_log_path()` (`core/paths.h`).
- Produces: every test process starts with `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` and `XDG_STATE_HOME` inside a per-process scratch directory, and `PHIG_CATALOG` unset. `phig` children started by tests inherit this.

- [ ] **Step 1: Write the failing test.** Create `tests/scratch_home.cpp` containing only this test for now:

```cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "paths.h"

namespace fs = std::filesystem;

// A test process must never resolve a path inside the developer's real home:
// a test that reaches a default path, directly or through a `phig` child,
// would otherwise write the real catalog's lock, log, trash or thumbnails.
TEST(ScratchHome, KeepsEveryDefaultPathInScratch) {
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_NE(std::string(home).find("phig-scratch-home-"), std::string::npos) << home;
    for (const char* var :
         {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        const char* value = std::getenv(var);
        ASSERT_NE(value, nullptr) << var;
        EXPECT_EQ(std::string(value).rfind(home, 0), 0u) << var << "=" << value;
    }
    EXPECT_EQ(std::getenv("PHIG_CATALOG"), nullptr);
    EXPECT_EQ(data_dir().rfind(home, 0), 0u) << data_dir();
    EXPECT_EQ(default_log_path().rfind(home, 0), 0u) << default_log_path();
}
```

  In `CMakeLists.txt`, add `tests/scratch_home.cpp` as the first source after `add_executable(phig-tests`, and after `tests/perf_duplicates.cpp` in `add_executable(phig-perf-tests`.

- [ ] **Step 2: Run it to see it fail.**
  ```bash
  cmake --build build -j12 --target phig-tests && ./build/phig-tests --gtest_filter='ScratchHome.*'
  ```
  Expected: FAIL. `HOME` is the real home directory.

- [ ] **Step 3: Add the environment.** Insert this above the test in `tests/scratch_home.cpp`, and add `#include "temp_dir.h"` to the includes:

```cpp
namespace {

// Points HOME and the four XDG base directories into one scratch directory for
// the whole test process, before any test runs, and clears PHIG_CATALOG, which
// a developer's .envrc may set. `phig` children started by a test inherit this.
//
// The face models are the one thing kept: <scratch>/home/.cache/phig/models
// links to the real models directory, so the face tests still find them. A
// test that needs them missing redirects XDG_CACHE_HOME itself.
class ScratchHome : public ::testing::Environment {
   public:
    void SetUp() override {
        std::error_code ec;
        fs::path real_models;
        try {
            real_models = models_dir();
        } catch (const std::exception&) {
            // No HOME and no XDG_CACHE_HOME: there are no real models to keep.
        }
        root_ = make_temp_dir("scratch-home");
        const fs::path home = root_ / "home";
        set("HOME", home);
        set("XDG_CONFIG_HOME", home / ".config");
        set("XDG_DATA_HOME", home / ".local" / "share");
        set("XDG_CACHE_HOME", home / ".cache");
        set("XDG_STATE_HOME", home / ".local" / "state");
        ::unsetenv("PHIG_CATALOG");
        if (!real_models.empty() && fs::is_directory(real_models, ec)) {
            fs::create_directories(home / ".cache" / "phig", ec);
            fs::create_directory_symlink(real_models, home / ".cache" / "phig" / "models", ec);
        }
    }

    // remove_all removes the models symlink, not what it points at.
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

   private:
    static void set(const char* name, const fs::path& value) {
        fs::create_directories(value);
        ::setenv(name, value.c_str(), 1);
    }

    fs::path root_;
};

const ::testing::Environment* const kScratchHome =
    ::testing::AddGlobalTestEnvironment(new ScratchHome);

}  // namespace
```

  `make_temp_dir("scratch-home")` names the directory `phig-scratch-home-<pid>-<n>`, which is what the test looks for.

- [ ] **Step 4: Run the test, then the whole suite.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='ScratchHome.*'
  ctest --test-dir build --parallel 6 2>&1 | tail -3
  ```
  Expected: the test passes, and the suite passes apart from the known flakes. The face tests still run, rather than skipping on missing models, on a machine that has the models.

- [ ] **Step 5: Confirm the real files were not touched.**
  ```bash
  stat -c '%Y %n' ~/.local/state/phig/phig.log ~/.local/share/phig/phig.db.lock | diff - /tmp/phig-067-real-mtimes.txt && echo UNTOUCHED
  ```
  Expected: `UNTOUCHED`. If either timestamp moved, find the test responsible by bisecting with `--gtest_filter` against the file's mtime, and fix it before continuing.

- [ ] **Step 6: Commit.**
  ```bash
  clang-format -i tests/scratch_home.cpp
  git add tests/scratch_home.cpp CMakeLists.txt
  git commit -m "Run every test with HOME and the XDG directories in scratch"
  ```

---

### Task 2: Catalog paths in core/

**Files:**
- Modify: `core/paths.h`, `core/paths.cpp`
- Modify: `cli/main.cpp:90` (`acquire_writer_lock`), `libphig/mutate.cpp:14`
- Modify: `tests/test_paths.cpp`, `tests/test_faces_engine.cpp:699-710`
- Modify: the thumbnail fixtures that redirect `XDG_CACHE_HOME` to capture thumbnails: `tests/test_thumbs.cpp:20-37`, `tests/test_thumbs_engine.cpp` (three fixtures, at about lines 36-50, 105-145 and 395-410), `tests/test_libphig_operations.cpp:30-62`, `gtk/tests/test-collection.cpp:22-48`

**Interfaces:**
- Produces, in `core/paths.h`:
  ```cpp
  struct CatalogPaths { std::string dir, db, lock, trash, thumbs, log; };
  std::string lock_path(const std::string& db_path);
  CatalogPaths catalog_paths(const std::string& dir);
  std::string default_catalog_dir();
  void set_catalog(const CatalogPaths& paths);
  void clear_catalog();
  CatalogPaths current_catalog();
  void set_models_dir(const std::optional<std::string>& dir);
  ```
  `default_db_path()`, `thumbs_dir()`, `trash_dir()` and `default_log_path()` return `current_catalog().db`, `.thumbs`, `.trash` and `.log`. The zero-argument `lock_path()` is removed. `set_path_overrides` stays for now, reimplemented on these functions (Decision 6).

- [ ] **Step 1: Write the failing tests.** In `tests/test_paths.cpp`:
  - In the fixture's teardown at line 49, replace `set_path_overrides(std::nullopt, std::nullopt, std::nullopt);` with:
    ```cpp
    clear_catalog();
    set_models_dir(std::nullopt);
    ```
  - Replace `ThumbsDirUnderCache` (line 109) and `DefaultLogPathUnderStateDir` (line 126) with:
    ```cpp
    TEST_F(PathsTest, ThumbsDirIsInsideTheDefaultCatalog) {
        EXPECT_EQ(thumbs_dir(), "/home/tester/.local/share/phig/thumbs");
    }

    TEST_F(PathsTest, DefaultLogPathIsInsideTheDefaultCatalog) {
        EXPECT_EQ(default_log_path(), "/home/tester/.local/share/phig/phig.log");
    }
    ```
  - Replace the three tests `PathOverridesTakePrecedence`, `UnsetOverridesFallBackToComputed` and `PartialOverrideOnlyAffectsSetPaths` (lines 152-170) with:
    ```cpp
    TEST_F(PathsTest, CatalogPathsLayOutOneDirectory) {
        CatalogPaths p = catalog_paths("/srv/catalog");
        EXPECT_EQ(p.dir, "/srv/catalog");
        EXPECT_EQ(p.db, "/srv/catalog/phig.db");
        EXPECT_EQ(p.lock, "/srv/catalog/phig.db.lock");
        EXPECT_EQ(p.trash, "/srv/catalog/trash");
        EXPECT_EQ(p.thumbs, "/srv/catalog/thumbs");
        EXPECT_EQ(p.log, "/srv/catalog/phig.log");
    }

    TEST_F(PathsTest, LockPathBelongsToTheDatabase) {
        EXPECT_EQ(lock_path("/a/b/other.db"), "/a/b/other.db.lock");
    }

    TEST_F(PathsTest, DefaultCatalogIsTheDataDirectory) {
        setenv("XDG_DATA_HOME", "/custom/data", 1);
        EXPECT_EQ(default_catalog_dir(), "/custom/data/phig");
        EXPECT_EQ(current_catalog().db, "/custom/data/phig/phig.db");
        EXPECT_EQ(trash_dir(), "/custom/data/phig/trash");
    }

    TEST_F(PathsTest, AnInstalledCatalogRedirectsEveryCatalogPath) {
        set_catalog(catalog_paths("/srv/catalog"));
        EXPECT_EQ(default_db_path(), "/srv/catalog/phig.db");
        EXPECT_EQ(thumbs_dir(), "/srv/catalog/thumbs");
        EXPECT_EQ(trash_dir(), "/srv/catalog/trash");
        EXPECT_EQ(default_log_path(), "/srv/catalog/phig.log");
        EXPECT_EQ(current_catalog().lock, "/srv/catalog/phig.db.lock");
        EXPECT_EQ(models_dir(), "/home/tester/.cache/phig/models")
            << "models are user-level, not part of a catalog";

        clear_catalog();
        EXPECT_EQ(default_db_path(), "/home/tester/.local/share/phig/phig.db");
    }

    TEST_F(PathsTest, ModelsDirCanBeSetAndCleared) {
        set_models_dir(std::string("/opt/models"));
        EXPECT_EQ(models_dir(), "/opt/models");
        set_models_dir(std::nullopt);
        EXPECT_EQ(models_dir(), "/home/tester/.cache/phig/models");
    }
    ```

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 --target phig-tests 2>&1 | grep -m3 error
  ```
  Expected: compile errors such as `'CatalogPaths' was not declared`.

- [ ] **Step 3: Implement it in `core/paths.h`.**
  - Replace the "Derived paths." block and the `trash_dir()` declaration with:
    ```cpp
    // A catalog is the directory holding what phig keeps about one collection:
    // its database, the database's writer lock, the trash, the thumbnail cache
    // and the diagnostic log. The images themselves live wherever they were
    // scanned. Face models and the config file are user-level and not part of it.
    struct CatalogPaths {
        std::string dir;
        std::string db;      // dir + "/phig.db"
        std::string lock;    // lock_path(db)
        std::string trash;   // dir + "/trash"
        std::string thumbs;  // dir + "/thumbs"
        std::string log;     // dir + "/phig.log", rotating to phig.1.log
    };

    // The advisory writer lock for the database at db_path: db_path + ".lock".
    std::string lock_path(const std::string& db_path);

    CatalogPaths catalog_paths(const std::string& dir);

    // The catalog used when none is named: data_dir().
    std::string default_catalog_dir();

    // The process's catalog. Installed once at startup, by the CLI from its
    // flags, environment and config, by phig_init and by the gtk client; one
    // process works on one catalog. Until one is installed, current_catalog()
    // is catalog_paths(default_catalog_dir()), computed on each call so a
    // test that changes XDG_DATA_HOME sees the change.
    void set_catalog(const CatalogPaths& paths);
    void clear_catalog();
    CatalogPaths current_catalog();

    // Paths inside current_catalog(), for code that needs only one of them.
    std::string default_db_path();
    std::string thumbs_dir();
    std::string trash_dir();
    std::string default_log_path();

    std::string models_dir();   // the set_models_dir() value, else cache_dir() + /models
    std::string config_path();  // config_dir() + /config.ini

    // Overrides models_dir(); std::nullopt restores the default.
    void set_models_dir(const std::optional<std::string>& dir);
    ```
  - Replace the `set_path_overrides` comment with:
    ```cpp
    // Installs a catalog whose database and thumbnails may sit outside its
    // directory, and the models directory. Kept for the callers that still name
    // paths one at a time; prefer set_catalog() and set_models_dir().
    ```

- [ ] **Step 4: Implement it in `core/paths.cpp`.**
  - Replace the three `g_*_override` globals with:
    ```cpp
    std::optional<CatalogPaths> g_catalog;
    std::optional<std::string> g_models_override;
    ```
  - Replace the definitions from `default_db_path()` through `trash_dir()` with:
    ```cpp
    std::string lock_path(const std::string& db_path) { return db_path + ".lock"; }

    CatalogPaths catalog_paths(const std::string& dir) {
        CatalogPaths p;
        p.dir = dir;
        p.db = dir + "/phig.db";
        p.lock = lock_path(p.db);
        p.trash = dir + "/trash";
        p.thumbs = dir + "/thumbs";
        p.log = dir + "/phig.log";
        return p;
    }

    std::string default_catalog_dir() { return data_dir(); }

    void set_catalog(const CatalogPaths& paths) { g_catalog = paths; }
    void clear_catalog() { g_catalog.reset(); }
    CatalogPaths current_catalog() {
        return g_catalog ? *g_catalog : catalog_paths(default_catalog_dir());
    }

    std::string default_db_path() { return current_catalog().db; }
    std::string thumbs_dir() { return current_catalog().thumbs; }
    std::string trash_dir() { return current_catalog().trash; }
    std::string default_log_path() { return current_catalog().log; }

    std::string models_dir() {
        return g_models_override ? *g_models_override : cache_dir() + "/models";
    }
    std::string config_path() { return config_dir() + "/config.ini"; }

    void set_models_dir(const std::optional<std::string>& dir) { g_models_override = dir; }
    ```
  - Replace the body of `set_path_overrides` with:
    ```cpp
    if (!db && !thumbs) {
        clear_catalog();
    } else {
        CatalogPaths p = catalog_paths(default_catalog_dir());
        if (db) {
            p.db = *db;
            p.lock = lock_path(*db);
        }
        if (thumbs) p.thumbs = *thumbs;
        set_catalog(p);
    }
    set_models_dir(models);
    ```

- [ ] **Step 5: Move the zero-argument `lock_path()` callers.**
  - `cli/main.cpp:90`: `std::string path = lock_path();` becomes `std::string path = current_catalog().lock;`
  - `libphig/mutate.cpp:14`: `ctx->db_path + ".lock"` becomes `lock_path(ctx->db_path)`. Add `#include "paths.h"` if `mutate.cpp` does not already include it.

- [ ] **Step 6: Move the fixtures that capture thumbnails.** Thumbnails now live in the default catalog, under `XDG_DATA_HOME`.
  - In each fixture listed under **Files**, change the variable it saves, sets and restores from `XDG_CACHE_HOME` to `XDG_DATA_HOME`. Keep the same value and the same restore logic. `XDG_DATA_HOME/phig/thumbs` has the same shape as the old `XDG_CACHE_HOME/phig/thumbs`, so assertions such as `ThumbsTest.CacheDirectoryXDG`'s `temp_dir + "/phig/thumbs"` stay correct.
  - Rename member variables such as `orig_cache` to `orig_data`.
  - In `gtk/tests/test-collection.cpp`, also update the fixture comment "XDG_CACHE_HOME points into the temp dir" to "XDG_DATA_HOME points into the temp dir".
  - This step matters most for the gtk test, which has no scratch environment. Without it, the next gtk test run would write a thumbnail into the real `~/.local/share/phig/thumbs`.
  - In `tests/test_faces_engine.cpp`'s `MissingModels` (lines 699-710), replace `set_path_overrides(std::nullopt, empty_dir.string(), std::nullopt);` with `set_models_dir(empty_dir.string());`, and the destructor's call with `set_models_dir(std::nullopt);`.

- [ ] **Step 7: Run both suites.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  cmake --build build/with-gtk -j12 --target gtk-all && ctest --test-dir build/with-gtk -L gtk 2>&1 | tail -3
  ```
  Expected: both pass. If a test still asserts a thumbnail under `XDG_CACHE_HOME`, it is a fixture this step missed. Find it with `grep -rn XDG_CACHE_HOME tests gtk/tests` and move it the same way, unless it is about models.

- [ ] **Step 8: Commit.**
  ```bash
  clang-format -i core/paths.h core/paths.cpp cli/main.cpp libphig/mutate.cpp tests/test_paths.cpp tests/test_thumbs.cpp tests/test_thumbs_engine.cpp tests/test_libphig_operations.cpp tests/test_faces_engine.cpp gtk/tests/test-collection.cpp
  git add -A core/paths.h core/paths.cpp cli/main.cpp libphig/mutate.cpp tests gtk/tests
  git commit -m "Resolve a collection's files from one catalog directory"
  ```

---

### Task 3: Thumbnails are marked as a cache, and legacy files move into the default catalog

**Files:**
- Modify: `core/thumbs.h`, `core/thumbs.cpp`
- Create: `core/catalog.h`, `core/catalog.cpp` (the migration half; Task 4 adds the open policy)
- Modify: `core/CMakeLists.txt` (add `catalog.cpp` to `phig-core`)
- Modify: `cli/main.cpp`, in `thumbs clean`'s loop
- Create: `tests/test_catalog.cpp`; add it to `phig-tests` in `CMakeLists.txt`
- Test: `tests/test_thumbs.cpp`

**Interfaces:**
- Consumes: `CatalogPaths`, `current_catalog()`, `default_catalog_dir()`, `cache_dir()`, `state_dir()`, `temp_sibling()`, `atomic_write_file()` (`core/paths.h`).
- Produces:
  ```cpp
  // core/thumbs.h
  inline constexpr const char* PHIG_CACHEDIR_TAG = "CACHEDIR.TAG";
  void ensure_cachedir_tag(const std::string& dir);

  // core/catalog.h
  struct LegacyMigration {
      std::vector<std::string> moved;      // legacy paths now inside the catalog
      std::vector<std::string> kept_both;  // legacy paths left alone: the catalog already has one
      std::vector<std::string> failed;     // legacy paths whose move failed, left in place
  };
  LegacyMigration migrate_legacy_catalog_files(const CatalogPaths& catalog);
  ```

- [ ] **Step 1: Write the failing tests.**
  - Append to `tests/test_thumbs.cpp`:
    ```cpp
    TEST_F(ThumbsTest, WritingAThumbnailTagsTheCacheDirectory) {
        cv::Mat img(64, 64, CV_8UC3, cv::Scalar(10, 20, 30));
        const std::string sha(64, 'a');
        ASSERT_TRUE(generate_thumb(img, sha, 128));
        const fs::path tag = fs::path(thumbs_dir()) / PHIG_CACHEDIR_TAG;
        ASSERT_TRUE(fs::exists(tag));
        std::ifstream in(tag);
        std::string first;
        std::getline(in, first);
        EXPECT_EQ(first, "Signature: 8a477f597d28d172789f06886806bc55");
    }
    ```
    Add `#include <fstream>` if the file lacks it.
  - Create `tests/test_catalog.cpp`:
    ```cpp
    #include <gtest/gtest.h>
    #include <sys/stat.h>

    #include <cstdlib>
    #include <filesystem>
    #include <fstream>
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
        struct stat a {}, b {};
        if (!fs::exists("/dev/shm") || ::stat("/dev/shm", &a) != 0 ||
            ::stat(root.c_str(), &b) != 0 || a.st_dev == b.st_dev) {
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
    ```
    Add `tests/test_catalog.cpp` to `phig-tests`' source list in `CMakeLists.txt`, after `tests/test_lockfile.cpp`.

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 --target phig-tests 2>&1 | grep -m3 error
  ```
  Expected: `catalog.h: No such file or directory`, and `PHIG_CACHEDIR_TAG` undeclared.

- [ ] **Step 3: Implement the cache tag.**
  - In `core/thumbs.h`, after `is_thumbnail_filename`'s declaration:
    ```cpp
    // The Cache Directory Tagging marker (https://bford.info/cachedir/). Backup
    // tools that honour it, such as borg, restic and GNU tar with --exclude-caches,
    // skip the directory holding it. Thumbnails can be regenerated, so a catalog
    // backup need not carry them.
    inline constexpr const char* PHIG_CACHEDIR_TAG = "CACHEDIR.TAG";

    // Writes PHIG_CACHEDIR_TAG into dir if it is not already there.
    void ensure_cachedir_tag(const std::string& dir);
    ```
  - In `core/thumbs.cpp`:
    ```cpp
    void ensure_cachedir_tag(const std::string& dir) {
        const fs::path tag = fs::path(dir) / PHIG_CACHEDIR_TAG;
        std::error_code ec;
        if (fs::exists(tag, ec)) return;
        static const std::string body =
            "Signature: 8a477f597d28d172789f06886806bc55\n"
            "# This file is a cache directory tag created by phig.\n"
            "# For information about cache directory tags, see https://bford.info/cachedir/\n";
        atomic_write_file(tag.string(), body.data(), body.size());
    }
    ```
    In `generate_thumb(const cv::Mat&, ...)`, replace `return atomic_write_file(path, buf.data(), buf.size());` with:
    ```cpp
    if (!atomic_write_file(path, buf.data(), buf.size())) return false;
    ensure_cachedir_tag(thumbs_dir());
    return true;
    ```
  - In `cli/main.cpp`'s `thumbs clean` loop, before the `is_thumbnail_filename` check, add:
    ```cpp
    // The cache tag belongs to the cache; it is neither a thumbnail nor foreign.
    if (entry.path() == fs::path(thumb_cache) / PHIG_CACHEDIR_TAG) continue;
    ```

- [ ] **Step 4: Implement the migration.**
  - Create `core/catalog.h`:
    ```cpp
    #pragma once

    #include <string>
    #include <vector>

    #include "paths.h"

    // What migrate_legacy_catalog_files did. Paths are the legacy locations, so a
    // caller can log them once its logger is up.
    struct LegacyMigration {
        std::vector<std::string> moved;      // now inside the catalog
        std::vector<std::string> kept_both;  // left alone: the catalog already has one
        std::vector<std::string> failed;     // the move failed; left in place
    };

    // Moves the thumbnails and log that the default catalog keeps inside its
    // directory but phig once kept elsewhere: $XDG_CACHE_HOME/phig/thumbs to
    // <catalog>/thumbs, and $XDG_STATE_HOME/phig/phig.log and phig.1.log to
    // <catalog>/. Only for the default catalog, and only once its database
    // exists, so a command against a missing catalog creates nothing.
    // Idempotent: once the legacy paths are gone there is nothing to move.
    // Never overwrites: a destination that already exists is left, and so is
    // the legacy path.
    LegacyMigration migrate_legacy_catalog_files(const CatalogPaths& catalog);
    ```
  - Create `core/catalog.cpp`:
    ```cpp
    #include "catalog.h"

    #include <filesystem>
    #include <system_error>

    #include "thumbs.h"

    namespace fs = std::filesystem;

    namespace {

    // Renames src to dst. Across filesystems, copies into a temporary sibling of
    // dst, renames that into place, then removes src, so a concurrent run sees
    // src or a complete dst and never a partial one. False leaves src in place.
    bool move_into_place(const fs::path& src, const fs::path& dst) {
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::rename(src, dst, ec);
        if (!ec) return true;
        if (ec != std::errc::cross_device_link) return false;

        const fs::path tmp = temp_sibling(dst);
        std::error_code cleanup;
        fs::copy(src, tmp, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            fs::remove_all(tmp, cleanup);
            return false;
        }
        fs::rename(tmp, dst, ec);
        if (ec) {
            fs::remove_all(tmp, cleanup);
            return false;
        }
        // A failure here leaves a second copy at src, never a lost one.
        fs::remove_all(src, cleanup);
        return true;
    }

    bool same_path(const std::string& a, const std::string& b) {
        return fs::absolute(a).lexically_normal() == fs::absolute(b).lexically_normal();
    }

    }  // namespace

    LegacyMigration migrate_legacy_catalog_files(const CatalogPaths& catalog) {
        LegacyMigration result;
        std::error_code ec;
        try {
            if (!same_path(catalog.dir, default_catalog_dir())) return result;
            if (!fs::exists(catalog.db, ec)) return result;

            const std::pair<fs::path, fs::path> moves[] = {
                {fs::path(cache_dir()) / "thumbs", catalog.thumbs},
                {fs::path(state_dir()) / "phig.log", catalog.log},
                {fs::path(state_dir()) / "phig.1.log", fs::path(catalog.dir) / "phig.1.log"},
            };
            for (const auto& [src, dst] : moves) {
                if (!fs::exists(src, ec) || same_path(src.string(), dst.string())) continue;
                if (fs::exists(dst, ec)) {
                    result.kept_both.push_back(src.string());
                } else if (move_into_place(src, dst)) {
                    result.moved.push_back(src.string());
                } else {
                    result.failed.push_back(src.string());
                }
            }
            if (fs::exists(catalog.thumbs, ec)) ensure_cachedir_tag(catalog.thumbs);
        } catch (const std::exception&) {
            // No HOME and no XDG variables: there is no legacy location to read.
        }
        return result;
    }
    ```
  - Add `catalog.cpp` to `phig-core`'s sources in `core/CMakeLists.txt`, next to `paths.cpp`.

- [ ] **Step 5: Run the tests.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='LegacyMigrationTest.*:ThumbsTest.*'
  ctest --test-dir build --parallel 6 2>&1 | tail -3
  ```
  Expected: all pass. `MovesThumbnailsAcrossFilesystems` skips if `/dev/shm` is on the temp directory's filesystem.

- [ ] **Step 6: Commit.**
  ```bash
  clang-format -i core/thumbs.h core/thumbs.cpp core/catalog.h core/catalog.cpp cli/main.cpp tests/test_thumbs.cpp tests/test_catalog.cpp
  git add core/thumbs.h core/thumbs.cpp core/catalog.h core/catalog.cpp core/CMakeLists.txt cli/main.cpp tests/test_thumbs.cpp tests/test_catalog.cpp CMakeLists.txt
  git commit -m "Tag the thumbnail cache and move legacy files into the default catalog"
  ```

---

### Task 4: The catalog open policy

**Files:**
- Modify: `core/catalog.h`, `core/catalog.cpp`
- Test: `tests/test_catalog.cpp`

**Interfaces:**
- Consumes: `Database(path, read_only)`, `Database::init_schema()`, `Database::schema_version()`, `Database::expected_schema_version()` (`core/database.h`).
- Produces:
  ```cpp
  enum class CatalogAccess { Read, Write, Create };
  class CatalogError : public std::runtime_error {
     public:
      enum class Kind { Missing, Outdated, Newer };
      CatalogError(Kind kind, const std::string& message);
      Kind kind() const;
  };
  std::unique_ptr<Database> open_catalog_db(const std::string& db_path, CatalogAccess access);
  void require_catalog(const std::string& db_path);
  ```

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_catalog.cpp`:

```cpp
namespace {

// Sets the version stamp in meta, leaving the tables as they are. Enough to
// make a read see an older or newer catalog without building one.
void stamp_schema_version(const std::string& db_path, int version) {
    Database db(db_path);
    const std::string sql = "UPDATE meta SET value = '" + std::to_string(version) +
                            "' WHERE key = 'schema_version'";
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
```

  `Database::exec` is private, so the tests go through the public `db_ptr()` with `sqlite3_exec`. Add `#include <sqlite3.h>` and `#include <functional>` to the file's includes.

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 --target phig-tests 2>&1 | grep -m3 error
  ```
  Expected: `'CatalogAccess' has not been declared`.

- [ ] **Step 3: Implement it.**
  - In `core/catalog.h`, add `#include <memory>`, `#include <stdexcept>` and `#include "database.h"`, then:
    ```cpp
    // How a command opens its catalog's database.
    enum class CatalogAccess {
        Read,    // read-only; the catalog must exist at this build's schema (R70)
        Write,   // the catalog must exist; migrated to this build's schema
        Create,  // created if missing, then migrated; only scan and libphig's
                 // read-write context create a catalog
    };

    // Why a catalog could not be opened.
    class CatalogError : public std::runtime_error {
       public:
        enum class Kind {
            Missing,   // no database in the catalog directory
            Outdated,  // older schema; a read cannot migrate it
            Newer,     // schema newer than this build reads
        };
        CatalogError(Kind kind, const std::string& message)
            : std::runtime_error(message), kind_(kind) {}
        Kind kind() const { return kind_; }

       private:
        Kind kind_;
    };

    // Opens the catalog database at db_path. Read never creates or migrates
    // anything: a missing database, or one at another schema version, throws
    // CatalogError. Write throws on a missing database and migrates an older
    // one. Create makes a missing database and migrates. All three refuse a
    // database newer than this build.
    std::unique_ptr<Database> open_catalog_db(const std::string& db_path, CatalogAccess access);

    // Throws CatalogError(Missing) unless the database at db_path exists. For
    // commands that touch a catalog without opening its database, and before
    // taking the writer lock, whose file would otherwise create the directory.
    void require_catalog(const std::string& db_path);
    ```
  - In `core/catalog.cpp`:
    ```cpp
    namespace {

    std::string catalog_dir_of(const std::string& db_path) {
        return fs::path(db_path).parent_path().string();
    }

    }  // namespace

    void require_catalog(const std::string& db_path) {
        std::error_code ec;
        if (fs::exists(db_path, ec)) return;
        throw CatalogError(CatalogError::Kind::Missing,
                           "no phig catalog at " + catalog_dir_of(db_path) +
                               ": it has no phig.db. 'phig scan <directory>' creates one.");
    }

    std::unique_ptr<Database> open_catalog_db(const std::string& db_path, CatalogAccess access) {
        if (access != CatalogAccess::Create) require_catalog(db_path);

        auto db = std::make_unique<Database>(db_path, access == CatalogAccess::Read);
        const int have = db->schema_version();
        const int want = Database::expected_schema_version();
        const std::string dir = catalog_dir_of(db_path);
        if (have > want) {
            throw CatalogError(CatalogError::Kind::Newer,
                               "the catalog at " + dir + " is at schema version " +
                                   std::to_string(have) + ", newer than this build's " +
                                   std::to_string(want) + "; use a newer phig");
        }
        if (access == CatalogAccess::Read) {
            if (have != want) {
                throw CatalogError(CatalogError::Kind::Outdated,
                                   "the catalog at " + dir + " is at schema version " +
                                       std::to_string(have) + " and this build reads version " +
                                       std::to_string(want) +
                                       "; a command that writes to it, such as 'phig scan', "
                                       "upgrades it");
            }
            return db;
        }
        db->init_schema();
        return db;
    }
    ```

- [ ] **Step 4: Run the tests.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='OpenCatalogTest.*'
  ```
  Expected: all pass. A read-only `Database` on a missing file already throws, but `require_catalog` comes first so the error is a `CatalogError` with a message naming the directory.

- [ ] **Step 5: Commit.**
  ```bash
  clang-format -i core/catalog.h core/catalog.cpp tests/test_catalog.cpp
  git add core/catalog.h core/catalog.cpp tests/test_catalog.cpp
  git commit -m "Add the catalog open policy: read, write or create"
  ```

---

### Task 5: The config file and the environment name the catalog

**Files:**
- Modify: `core/config.h`, `core/config.cpp`
- Modify: `core/catalog.h`, `core/catalog.cpp`
- Modify: `cli/main.cpp`: `print_active_config` (about lines 1776-1806), and `main()` from `set_path_overrides(...)` through `log_init(...)` (about lines 1848-1863)
- Test: `tests/test_config.cpp`, `tests/test_catalog.cpp`

**Interfaces:**
- Consumes: `Config` (`core/config.h`), `expand_tilde`, `default_catalog_dir`, `set_catalog`, `set_models_dir`, `migrate_legacy_catalog_files`, `migrate_flat_thumb_cache`.
- Produces:
  ```cpp
  // core/config.h: Config::catalog replaces Config::db and Config::thumbs_dir
  std::optional<std::string> catalog;

  // core/catalog.h
  enum class CatalogSource { Flag, Environment, Config, Default };
  struct ResolvedCatalog { std::string dir; CatalogSource source; };
  ResolvedCatalog resolve_catalog_dir(const std::optional<std::string>& flag, const Config& cfg);
  const char* catalog_source_name(CatalogSource source);
  ```

- [ ] **Step 1: Write the failing tests.**
  - In `tests/test_config.cpp`:
    - `AbsentFileReturnsDefaults`: `c.db.has_value()` becomes `c.catalog.has_value()`.
    - `ParsesAllRecognizedKeys`: replace the ini's `db = ~/photos/phig.db\n` and `thumbs_dir = /var/thumbs\n` lines with `catalog = ~/photos/catalog\n`. Replace the `c.db` and `c.thumbs_dir` expectations with `EXPECT_EQ(c.catalog.value(), "/home/tester/photos/catalog");`.
    - Add:
      ```cpp
      TEST_F(ConfigTest, RemovedPathKeysFailNamingTheirReplacement) {
          for (const char* key : {"db = /x/phig.db\n", "thumbs_dir = /x/thumbs\n"}) {
              std::string p = write_ini(std::string("[paths]\n") + key);
              try {
                  load_config(p);
                  ADD_FAILURE() << "accepted " << key;
              } catch (const std::runtime_error& e) {
                  EXPECT_NE(std::string(e.what()).find("[paths] catalog"), std::string::npos)
                      << e.what();
              }
          }
      }
      ```
  - Append to `tests/test_catalog.cpp`:
    ```cpp
    namespace {

    // PHIG_CATALOG and XDG_DATA_HOME, restored afterwards.
    class ResolveCatalogTest : public ::testing::Test {
       protected:
        std::string saved_data;
        void SetUp() override {
            saved_data = std::getenv("XDG_DATA_HOME");
            setenv("XDG_DATA_HOME", "/xdg/data", 1);
            unsetenv("PHIG_CATALOG");
        }
        void TearDown() override {
            setenv("XDG_DATA_HOME", saved_data.c_str(), 1);
            unsetenv("PHIG_CATALOG");
        }
    };

    }  // namespace

    TEST_F(ResolveCatalogTest, DefaultIsTheDataDirectory) {
        ResolvedCatalog r = resolve_catalog_dir(std::nullopt, Config{});
        EXPECT_EQ(r.dir, "/xdg/data/phig");
        EXPECT_EQ(r.source, CatalogSource::Default);
    }

    TEST_F(ResolveCatalogTest, ConfigBeatsTheDefault) {
        Config cfg;
        cfg.catalog = "/from/config";
        EXPECT_EQ(resolve_catalog_dir(std::nullopt, cfg).dir, "/from/config");
        EXPECT_EQ(resolve_catalog_dir(std::nullopt, cfg).source, CatalogSource::Config);
    }

    TEST_F(ResolveCatalogTest, EnvironmentBeatsConfig) {
        Config cfg;
        cfg.catalog = "/from/config";
        setenv("PHIG_CATALOG", "/from/env", 1);
        ResolvedCatalog r = resolve_catalog_dir(std::nullopt, cfg);
        EXPECT_EQ(r.dir, "/from/env");
        EXPECT_EQ(r.source, CatalogSource::Environment);
    }

    TEST_F(ResolveCatalogTest, FlagBeatsEverything) {
        Config cfg;
        cfg.catalog = "/from/config";
        setenv("PHIG_CATALOG", "/from/env", 1);
        ResolvedCatalog r = resolve_catalog_dir(std::string("/from/flag"), cfg);
        EXPECT_EQ(r.dir, "/from/flag");
        EXPECT_EQ(r.source, CatalogSource::Flag);
    }

    TEST_F(ResolveCatalogTest, EmptyEnvironmentCountsAsUnset) {
        setenv("PHIG_CATALOG", "", 1);
        EXPECT_EQ(resolve_catalog_dir(std::nullopt, Config{}).source, CatalogSource::Default);
    }

    TEST_F(ResolveCatalogTest, RelativeAndTildePathsBecomeAbsolute) {
        const fs::path cwd = fs::current_path();
        EXPECT_EQ(resolve_catalog_dir(std::string("rel/cat"), Config{}).dir,
                  (cwd / "rel" / "cat").lexically_normal().string());
        const std::string home = std::getenv("HOME");
        EXPECT_EQ(resolve_catalog_dir(std::string("~/cat"), Config{}).dir, home + "/cat");
    }
    ```
    Add `#include "config.h"` to the file's includes.

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 --target phig-tests 2>&1 | grep -m3 error
  ```
  Expected: `'struct Config' has no member named 'catalog'`.

- [ ] **Step 3: Implement the config change.**
  - In `core/config.h`, replace the `db` and `thumbs_dir` members with:
    ```cpp
    std::optional<std::string> catalog;  // [paths] catalog — the default catalog directory
    ```
  - In `core/config.cpp`, replace the `paths.db` and `paths.thumbs_dir` branches with:
    ```cpp
    if (sec == "paths" && k == "catalog") {
        cfg.catalog = expand_tilde(val);
    } else if (sec == "paths" && (k == "db" || k == "thumbs_dir")) {
        fail(path, section, key,
             "was replaced by [paths] catalog, a directory holding the database, "
             "thumbnails, trash and log");
    } else if (sec == "paths" && k == "models_dir") {
    ```
    Keep the `models_dir` branch's body as it is. Mind the `else if` chain: the `catalog` branch is now its first link.

- [ ] **Step 4: Implement resolution.** In `core/catalog.h`, add `#include <optional>` and `#include "config.h"`, then:
  ```cpp
  // Where a command's catalog came from.
  enum class CatalogSource { Flag, Environment, Config, Default };

  struct ResolvedCatalog {
      std::string dir;  // absolute, lexically normal
      CatalogSource source;
  };

  // Which catalog a command uses: the --catalog flag, then PHIG_CATALOG, then
  // the config file's [paths] catalog, then default_catalog_dir(). An empty
  // PHIG_CATALOG counts as unset. A leading ~ is expanded, and a relative path
  // is resolved against the working directory.
  ResolvedCatalog resolve_catalog_dir(const std::optional<std::string>& flag, const Config& cfg);

  // "--catalog", "PHIG_CATALOG", "config" or "default", for messages.
  const char* catalog_source_name(CatalogSource source);
  ```
  In `core/catalog.cpp`, add `#include <cstdlib>`, then:
  ```cpp
  namespace {

  std::string absolute_dir(const std::string& path) {
      return fs::absolute(expand_tilde(path)).lexically_normal().string();
  }

  }  // namespace

  ResolvedCatalog resolve_catalog_dir(const std::optional<std::string>& flag, const Config& cfg) {
      if (flag) return {absolute_dir(*flag), CatalogSource::Flag};
      const char* env = std::getenv("PHIG_CATALOG");
      if (env != nullptr && env[0] != '\0') return {absolute_dir(env), CatalogSource::Environment};
      if (cfg.catalog) return {absolute_dir(*cfg.catalog), CatalogSource::Config};
      return {default_catalog_dir(), CatalogSource::Default};
  }

  const char* catalog_source_name(CatalogSource source) {
      switch (source) {
          case CatalogSource::Flag:
              return "--catalog";
          case CatalogSource::Environment:
              return "PHIG_CATALOG";
          case CatalogSource::Config:
              return "config";
          case CatalogSource::Default:
              break;
      }
      return "default";
  }
  ```

- [ ] **Step 5: Install the resolved catalog in the CLI.** In `cli/main.cpp`, add `#include "catalog.h"`.
  - Replace `print_active_config`'s `db:` and `thumbs_dir:` lines with:
    ```cpp
    const CatalogPaths catalog = current_catalog();
    std::cout << "  catalog:      " << catalog.dir << "  [" << catalog_source << "]\n";
    ```
    Add a `const char* catalog_source` parameter to `print_active_config`, and pass `catalog_source_name(resolved.source)` at both call sites.
  - Replace `set_path_overrides(cfg.db, cfg.models_dir, cfg.thumbs_dir);` and everything from there through the `log_init(...)` call with:
    ```cpp
    const ResolvedCatalog resolved = resolve_catalog_dir(std::nullopt, cfg);
    const CatalogPaths catalog = catalog_paths(resolved.dir);
    set_catalog(catalog);
    set_models_dir(cfg.models_dir);

    const LegacyMigration legacy = migrate_legacy_catalog_files(catalog);

    // Thumbnails once lived at <thumbs>/ab/<sha>.jpg, before the size became
    // part of the path. Move any such files under <thumbs>/512/ so an existing
    // cache stays usable. A rename, not a re-encode; a no-op once done.
    const int flat_moved = migrate_flat_thumb_cache();

    // Diagnostic logger: stderr sink for warnings and errors, plus a rotating
    // file sink at <catalog>/phig.log. The file sink is created only when the
    // catalog exists or this command will create it, because opening the sink
    // creates the file and with it the catalog directory.
    const std::string command_name = argc >= 2 ? argv[1] : "";
    const bool log_to_file = (!cfg.log_file || *cfg.log_file) &&
                             (fs::exists(catalog.db) || command_name == "scan");
    log_init(catalog.log, log_to_file,
             cfg.log_file_level ? log_level_from_string(*cfg.log_file_level) : LogLevel::Info);
    for (const std::string& p : legacy.moved) log_info("moved " + p + " into " + catalog.dir);
    for (const std::string& p : legacy.kept_both)
        log_warn("left " + p + " in place: " + catalog.dir + " already has one");
    for (const std::string& p : legacy.failed)
        log_warn("could not move " + p + " into " + catalog.dir);
    if (flat_moved > 0) {
        log_info("migrated " + std::to_string(flat_moved) +
                 " thumbnails into the size-partitioned cache layout");
    }
    ```

- [ ] **Step 6: Run both suites.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  ```
  Expected: all pass.

- [ ] **Step 7: Commit.**
  ```bash
  clang-format -i core/config.h core/config.cpp core/catalog.h core/catalog.cpp cli/main.cpp tests/test_config.cpp tests/test_catalog.cpp
  git add core/config.h core/config.cpp core/catalog.h core/catalog.cpp cli/main.cpp tests/test_config.cpp tests/test_catalog.cpp
  git commit -m "Name the catalog from PHIG_CATALOG or the config file's [paths] catalog"
  ```

---

### Task 6: `--catalog` replaces `--db`

**Files:**
- Modify: `cli/main.cpp`
- Modify: `rules/operations.toml`
- Modify: `tests/phig_binary.h`, `tests/test_database.cpp`, `tests/test_organize_engine.cpp`, `tests/test_faces_engine.cpp`, `tests/test_remove_exec.cpp`, `tests/test_lockfile.cpp` (comment only)

**Interfaces:**
- Consumes: `resolve_catalog_dir(flag, cfg)` from Task 5.
- Produces: a global `--catalog <dir>` recognized anywhere in argv. No `"--db"` literal remains in `cli/main.cpp`; `--db-only` is a different flag and stays.

- [ ] **Step 1: Convert the CLI-driving tests first.** These are the failing tests: each passes `--catalog`, which the binary does not know yet.
  - Every test that runs the binary builds its database as `<dir>/phig.db` and passes `--catalog <dir>`. Go through each site from `grep -n -- "--db '" tests/*.cpp; grep -n -- '--db " ' tests/*.cpp`:
    - `tests/test_database.cpp`, the `DatabaseTest` fixture (line 34): change `db_path` from `dir / "test.db"` to `dir / "phig.db"`, so the fixture's directory is a catalog. No test depends on the name `test.db` for the fixture's database; lines 585 and 1728 build their own. The tests around lines 1901 and 1948 then pass `--catalog '" + dir.string() + "'` in place of `--db '" + db_path + "'`.
    - `tests/test_database.cpp:2275-2310` (`CliMetadataWritesRefuseAnUnnamedSubject`):
      - set `cli_db` to `(root / "catalog" / "phig.db").string()` and `fs::create_directories(root / "catalog")` before building it;
      - replace the hand-built `std::system` call with `PhigRun run = run_phig(phig_bin, root, args + " --catalog '" + (root / "catalog").string() + "'");`, then use `run.exit_code` and `run.output`;
      - add `#include "phig_binary.h"` if missing.
    - `tests/test_organize_engine.cpp:674`: `absent` becomes `dir / "absent"`, a directory that does not exist, passed as `--catalog '<absent>'`. Its expectations change in Task 7; for now, keep the `run_phig` call and change only the argument.
    - `tests/test_organize_engine.cpp:704` and `:723`: the fixture's database is already `dir / "phig.db"`, so pass `--catalog '" + dir.string() + "'` in place of `--db '" + db_path + "'`.
    - `tests/test_faces_engine.cpp:590` and `:638`: replace the bare `std::system` command strings with `run_phig(phig_bin, coll.dir, "face rebuild --mtach '...' --catalog '" + coll.dir.string() + "'")`. `coll.db_path` is already `coll.dir / "phig.db"`. Do the same at `:755` and `:777`, which already use `run_phig`.
    - `tests/test_remove_exec.cpp:618-650`: make `cli_db` `(cli_root / "catalog" / "phig.db")`, and replace the `echo yes | ...` system call with `run_phig(phig_bin, cli_root, "rm --match '*' --hard --catalog '" + (cli_root / "catalog").string() + "'")`. The test asserts the hard delete is refused without a terminal, and `run_phig` gives the child no terminal on stdin, just as the pipe did.
  - In `tests/phig_binary.h`, update the comments at lines 51-60 and 90-99. The lock comment becomes: "Where a child of run_phig() takes its advisory writer lock when it is given no --catalog: the default catalog is data_dir(), inside the redirected XDG_DATA_HOME." `phig_scratch_db_path`'s comment becomes: "The database a child of run_phig() uses when it is given no --catalog."
  - In `tests/test_lockfile.cpp:86-89`, replace "Passing --db instead would put them on different files (R50) and the refusal below would prove nothing." with "Passing --catalog with another directory would put them on different files, and the refusal below would prove nothing."

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='*Cli*:FacesEngine*Mtach*:RemoveExecTest.*'
  ```
  Expected: failures with `Unknown option: --catalog`.

- [ ] **Step 3: Parse `--catalog` globally.** In `main()`'s first loop, beside `--config` and `--no-wait`:
  ```cpp
  } else if (a == "--catalog" && i + 1 < argc) {
      catalog_flag = argv[++i];
  ```
  Declare `std::optional<std::string> catalog_flag;` with the other globals above the loop. Pass `catalog_flag` to `resolve_catalog_dir` in place of `std::nullopt`.

- [ ] **Step 4: Remove `--db`.**
  - Delete every branch matching `arg == "--db"` in `cli/main.cpp`: the inline parsers for `scan`, `duplicates`, `cp`/`mv` and `rm`, the "already handled" and "resolved by caller" skips inside `cmd_thumbs`, `cmd_face`, `cmd_tag`, `cmd_gallery` and `cmd_fav`, and the three pre-scan loops in `main()` for `thumbs`, `tag`/`gallery`/`fav` and `face`. Those pre-scans become plain assignments, for example `std::string thumbs_db = default_db_path();`.
  - Delete the six `"  --db <path>              Database path\n"` lines in `print_usage`.
  - In `print_usage`, after the `phig models download` line, add:
    ```cpp
    << "\nGlobal flags (anywhere on the command line):\n"
    << "  --catalog <dir>          Catalog directory: database, thumbnails, trash and log\n"
    << "                           (default: $PHIG_CATALOG, then [paths] catalog in the\n"
    << "                           config file, then ~/.local/share/phig)\n"
    << "  --config <path>          Config file (default: ~/.config/phig/config.ini)\n"
    << "  --no-wait                Fail instead of waiting for another phig's writer lock\n"
    ```
  - Verify with `grep -n '"--db"' cli/main.cpp`. Expected: no output.

- [ ] **Step 5: Classify the flag.** In `rules/operations.toml`, replace the `"--db",` entry in `mutating_cli_flags` and its three-line comment with:
  ```toml
  # Global. Names the catalog whose database is created, migrated and written,
  # whose lock is taken, and whose trash and thumbnails are written and deleted.
  "--catalog",
  ```

- [ ] **Step 6: Run everything.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  python3 scripts/rules.py --check --emit && git status --short rules/
  ```
  Expected: the suite passes except `OrganizeEngineTest.CliDryRunOverAnAbsentDatabaseStillCreatesAndMigratesIt`, which asserts creation and is rewritten in Task 7. If it passes here, `cp --dry-run` still creates the catalog, which Task 7 fixes. The rules check prints `OK`, and the regenerated views may change.

- [ ] **Step 7: Commit.** If that one organize test fails, commit anyway, and record the fact in the commit body: "OrganizeEngineTest.CliDryRunOverAnAbsentDatabaseStillCreatesAndMigratesIt is rewritten with R21 in the next commit."
  ```bash
  clang-format -i cli/main.cpp tests/test_database.cpp tests/test_organize_engine.cpp tests/test_faces_engine.cpp tests/test_remove_exec.cpp tests/test_lockfile.cpp tests/phig_binary.h
  git add cli/main.cpp rules/ tests/
  git commit -m "Replace --db with a global --catalog <dir>"
  ```

---

### Task 7: A read never creates or migrates a catalog (R70, R20, R21)

**Files:**
- Modify: `cli/main.cpp`, at all twelve `Database db(...); db.init_schema();` sites, and `acquire_writer_lock`
- Modify: `core/remove_exec.cpp:73`
- Modify: `rules/boundary.toml` (R70), `rules/authorization.toml` (R20, R21)
- Test: `tests/test_catalog.cpp`, `tests/test_remove_exec.cpp`, `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: `open_catalog_db`, `CatalogAccess`, `require_catalog`, `CatalogError` (Task 4).
- Produces: `static std::unique_ptr<WriterLock> acquire_writer_lock(bool no_wait, bool may_create = false);`

**Access per command:**

| Command | Access |
|---|---|
| `scan` | `Create` |
| `duplicates`, `search`, `fav list` (through `cmd_search`), `tag list`, `gallery list`, `face list` | `Read` |
| `cp`/`mv`, `rm` | `Read` with `--dry-run`, otherwise `Write` |
| `tag`/`gallery`/`fav` writes, `face name`/`identify`/`rebuild`, `thumbs rebuild`, `thumbs clean` | `Write` |

`trash empty` opens no database. It is guarded by the lock's `require_catalog`.

- [ ] **Step 1: Write the failing tests.**
  - Append to `tests/test_catalog.cpp`. Add `#include "phig_binary.h"` to its includes.
    ```cpp
    namespace {

    class CatalogCliTest : public ::testing::Test {
       protected:
        fs::path phig_bin;
        fs::path dir;
        void SetUp() override {
            phig_bin = phig_binary_path();
            if (phig_bin.empty()) GTEST_SKIP() << "phig binary not built";
            dir = make_temp_dir("catalog-cli");
        }
        void TearDown() override {
            std::error_code ec;
            if (!dir.empty()) fs::remove_all(dir, ec);
        }
        PhigRun run(const fs::path& catalog, const std::string& args) {
            return run_phig(phig_bin, dir, "--catalog '" + catalog.string() + "' " + args);
        }
        // A catalog at this build's schema with one row whose file exists.
        fs::path make_catalog(const std::string& name) {
            const fs::path c = dir / name;
            fs::create_directories(c / "photos");
            const fs::path photo = c / "photos" / "a.jpg";
            std::ofstream(photo) << "x";
            Database db((c / "phig.db").string());
            db.init_schema();
            ImageInfo info;
            info.path = photo.string();
            info.filename = "a.jpg";
            info.extension = "jpg";
            info.file_size = 1;
            info.sha256 = std::string(64, 'a');
            info.metadata_json = "{}";
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db.insert(info);
            return c;
        }
    };

    }  // namespace

    TEST_F(CatalogCliTest, ReadsRefuseAMissingCatalogAndCreateNothing) {
        const fs::path missing = dir / "missing";
        for (const char* args : {"search --count", "duplicates", "tag list", "gallery list",
                                 "fav list", "face list"}) {
            PhigRun r = run(missing, args);
            EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
            EXPECT_NE(r.output.find("no phig catalog at " + missing.string()), std::string::npos)
                << args << ": " << r.output;
            EXPECT_FALSE(fs::exists(missing)) << args << " created the catalog";
        }
    }

    TEST_F(CatalogCliTest, ReadsRefuseAnOutdatedCatalogAndLeaveItsVersion) {
        const fs::path c = make_catalog("old");
        const std::string db_path = (c / "phig.db").string();
        const int older = Database::expected_schema_version() - 1;
        stamp_schema_version(db_path, older);
        for (const char* args : {"search --count", "duplicates", "cp '/tmp/never' --dry-run",
                                 "rm --match '*' --dry-run"}) {
            PhigRun r = run(c, args);
            EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
            Database after(db_path, /*read_only=*/true);
            EXPECT_EQ(after.schema_version(), older) << args << " migrated the catalog";
        }
    }

    TEST_F(CatalogCliTest, WritesRefuseAMissingCatalogAndCreateNothing) {
        const fs::path missing = dir / "missing";
        for (const char* args :
             {"tag add holiday --match '*'", "gallery set trip --match '*'", "fav add --match '*'",
              "rm --match '*' --yes", "cp '/tmp/never'", "trash empty --yes", "thumbs rebuild",
              "thumbs clean", "face rebuild", "face identify Ada"}) {
            PhigRun r = run(missing, args);
            EXPECT_EQ(r.exit_code, 1) << args << ": " << r.output;
            EXPECT_FALSE(fs::exists(missing)) << args << " created the catalog";
        }
    }

    TEST_F(CatalogCliTest, ScanCreatesAMissingCatalog) {
        const fs::path photos = dir / "photos";
        fs::create_directories(photos);
        fs::copy_file("tests/data/faces_small.jpg", photos / "f.jpg");
        const fs::path fresh = dir / "fresh";
        PhigRun r = run(fresh, "scan '" + photos.string() + "'");
        EXPECT_EQ(r.exit_code, 0) << r.output;
        EXPECT_TRUE(fs::exists(fresh / "phig.db"));
        PhigRun count = run(fresh, "search --count");
        EXPECT_EQ(count.exit_code, 0) << count.output;
        EXPECT_NE(count.output.find('1'), std::string::npos) << count.output;
    }
    ```
    `stamp_schema_version` is the helper Task 4 added in the anonymous namespace near the top of this file. Move it above both test groups if it is declared after this point. Add `#include "types.h"` for `ImageInfo` if `database.h` does not pull it in.
  - In `tests/test_remove_exec.cpp`, replace `RmDryRunCreatesAndMigratesADatabaseThatWasNotThere` (line 500) with:
    ```cpp
    TEST_F(RemoveExecTest, RmDryRunOnAMissingCatalogCreatesNothing) {
        const fs::path missing = root / "missing";
        RemoveOptions opts;
        opts.db_path = (missing / "phig.db").string();
        opts.matches = {"*c.jpg"};
        opts.mode = RemoveMode::Trash;
        opts.dry_run = true;
        opts.assume_yes = true;

        EXPECT_THROW(cmd_rm(opts), CatalogError);
        EXPECT_FALSE(fs::exists(missing));
    }
    ```
    Add `#include "catalog.h"`.
  - In `tests/test_organize_engine.cpp`, rename `CliDryRunOverAnAbsentDatabaseStillCreatesAndMigratesIt` to `CliDryRunOverAMissingCatalogRefusesAndCreatesNothing`. Its body runs `cp '<dest>' --dry-run --catalog '<dir>/absent'`, then expects exit code 1, `!fs::exists(dir / "absent")`, and the output to contain `no phig catalog at`. Delete its "measured: the preview leaves a database behind" assertion and the schema check after it.

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='CatalogCliTest.*:RemoveExecTest.RmDryRun*:OrganizeEngineTest.CliDryRun*'
  ```
  Expected: `ReadsRefuseAMissingCatalogAndCreateNothing` fails, because `search --count` creates the database; the outdated-catalog test finds its version migrated; the dry-run tests find a created database.

- [ ] **Step 3: Guard the lock.** Replace `acquire_writer_lock` (line 89) with:
  ```cpp
  // Acquire the process-wide writer lock for a mutating command, on the catalog's
  // lock file. Prints a clear message and exits nonzero if --no-wait was given
  // and another op holds it. Unless may_create is set, the catalog must already
  // exist: creating the lock file would otherwise create the catalog directory.
  static std::unique_ptr<WriterLock> acquire_writer_lock(bool no_wait, bool may_create = false) {
      const CatalogPaths catalog = current_catalog();
      if (!may_create) require_catalog(catalog.db);
      fs::create_directories(fs::path(catalog.lock).parent_path());
      try {
          return std::make_unique<WriterLock>(catalog.lock, no_wait);
      } catch (const LockBusy& e) {
          std::cerr << "Error: " << e.what() << " (use without --no-wait to wait)\n";
          std::exit(1);
      }
  }
  ```
  Pass `true` at the `scan` call site and in `models download`.

- [ ] **Step 4: Route every open through `open_catalog_db`.** At each `Database db(X); db.init_schema();` pair in `cli/main.cpp`, write:
  ```cpp
  auto db_owner = open_catalog_db(X, <access from the table>);
  Database& db = *db_owner;
  ```
  - `cmd_face` opens once before dispatching. Choose the access there with `subcmd == "list" ? CatalogAccess::Read : CatalogAccess::Write`.
  - For `cp`/`mv`, use `dry_run ? CatalogAccess::Read : CatalogAccess::Write`.
  - In `core/remove_exec.cpp:73`, replace the pair with:
    ```cpp
    auto db_owner =
        open_catalog_db(opts.db_path, opts.dry_run ? CatalogAccess::Read : CatalogAccess::Write);
    Database& db = *db_owner;
    ```
    Add `#include "catalog.h"`.
  - Verify with `grep -n 'init_schema' cli/main.cpp core/remove_exec.cpp`. Expected: no output.

- [ ] **Step 5: Run the tests.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  ```
  Expected: all pass. If a dry-run test fails with "attempt to write a readonly database", the dry-run path writes before its `dry_run` check. Find the write in `cmd_rm` or `cmd_organize` and stop the dry run before it, rather than reopening read-write.

- [ ] **Step 6: Update the rules.**
  - R70 in `rules/boundary.toml`:
    ```toml
    enforced_at = [
        "core/catalog.cpp:open_catalog_db",
        "libphig/context.cpp:phig_init",
    ]
    tests = [
        "OpenCatalogTest.ReadAndWriteRefuseAMissingCatalogAndCreateNothing",
        "OpenCatalogTest.ReadRefusesAnOutdatedCatalogAndLeavesItsVersion",
        "CatalogCliTest.ReadsRefuseAMissingCatalogAndCreateNothing",
        "CatalogCliTest.ReadsRefuseAnOutdatedCatalogAndLeaveItsVersion",
    ]
    status = "holds"
    test_gap = "The subprocess tests cover search, duplicates, the four list forms and the two dry runs; which access each remaining command passes is read off cli/main.cpp's call sites rather than asserted one by one."
    ```
    Rationale:
    > open_catalog_db is the one open policy. Read opens read-only, so it can neither create the file nor migrate it, and it refuses a missing database and any schema version other than this build's with a CatalogError naming the catalog. Write and Create migrate; only Create makes a missing database. Which access a command passes is decided at its call site: every CLI command that only reads, including the dry runs of rm, cp and mv, passes Read, and libphig's read-only context does the same. A migration is forward-only and init_schema refuses a database newer than the build, so a read that migrated would make the catalog unopenable by an older phig on another machine. A read that created would turn a mistyped catalog path into an empty catalog reported as empty.
  - R20 in `rules/authorization.toml`:
    ```toml
    statement = "A dry-run removal takes no disk action, deletes no row, opens no logged operation, and opens the catalog read-only, so it neither creates the database nor migrates its schema."
    tests = [
        "RemoveExecTest.RmDryRunTouchesNothing",
        "RemoveExecTest.RmDryRunOnAMissingCatalogCreatesNothing",
        "CatalogCliTest.ReadsRefuseAnOutdatedCatalogAndLeaveItsVersion",
    ]
    test_gap = "The migration half is pinned through the binary against a catalog stamped one version back, not against a database built at an older schema; the suite cannot build one."
    ```
    Rationale:
    > The preview returns before the confirmation guard and before log_begin_operation, so nothing on disk moves and nothing about the selection is recorded. cmd_rm opens the catalog with CatalogAccess::Read when dry_run is set, so the preview cannot create a missing database or migrate an older one; it fails with a CatalogError instead (R70).
  - R21 in `rules/authorization.toml`:
    ```toml
    statement = "The copy and move preview is OrganizeEngine::plan, which writes nothing to disk or to the database, and the command that calls it opens the catalog read-only, so the preview neither creates nor migrates it."
    tests = [
        "OrganizeEngineTest.PlanWritesNothingToDiskOrDatabase",
        "OrganizeEngineTest.CliDryRunOverAMissingCatalogRefusesAndCreatesNothing",
        "OrganizeEngineTest.CliDryRunPreviewsEveryCopyAndWritesNothingElse",
        "CatalogCliTest.ReadsRefuseAnOutdatedCatalogAndLeaveItsVersion",
    ]
    test_gap = "Both clauses are driven through the binary. The migration clause uses a catalog stamped one version back, not one built at an older schema."
    ```
    Rationale:
    > Keeping the preview in core/ rather than in a CLI branch is what lets every surface offer it: organize.plan is an operation with an ABI entry point, so a GUI showing a user what a move would do runs the same code the CLI's dry run runs. The CLI opens the catalog with CatalogAccess::Read for a dry run, so the preview is side-effect-free end to end (R70).
  - In each record, keep every field not named here as it is.
  - Run:
    ```bash
    python3 scripts/rules.py --check --emit
    ./build/phig-tests --gtest_output=xml:/tmp/phig-067-tests.xml >/dev/null; python3 scripts/rules.py --check --test-results /tmp/phig-067-tests.xml
    ```
    Expected: `OK` from both.

- [ ] **Step 7: Commit.**
  ```bash
  clang-format -i cli/main.cpp core/remove_exec.cpp tests/test_catalog.cpp tests/test_remove_exec.cpp tests/test_organize_engine.cpp
  git add cli/main.cpp core/remove_exec.cpp tests/ rules/
  git commit -m "Open a catalog read-only for every command that only reads (R70)"
  ```

---

### Task 8: The lock belongs to the catalog's database (R50)

**Files:**
- Test: `tests/test_lockfile.cpp`
- Modify: `rules/concurrency.toml` (R50), and the `plans/039-*.md` frontmatter and paragraph

**Interfaces:**
- Consumes: `run_phig`, `phig_scratch_lock_path` (`tests/phig_binary.h`), `catalog_paths`.

- [ ] **Step 1: Write the test.** Append to `tests/test_lockfile.cpp`:

```cpp
// Which lock file a command takes is the catalog's: holding catalog A's lock
// refuses a write to A, and leaves a write to B, or a write to A while only the
// default catalog's lock is held, free to run.
TEST_F(LockfileTest, CliLocksTheCatalogItWrites) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) GTEST_SKIP() << "phig binary not built";

    auto make = [&](const std::string& name) {
        const fs::path c = dir / name;
        fs::create_directories(c);
        Database db((c / "phig.db").string());
        db.init_schema();
        ImageInfo info;
        info.path = (c / "a.jpg").string();
        info.filename = "a.jpg";
        info.extension = "jpg";
        info.file_size = 1;
        info.sha256 = "sha-" + name;
        info.metadata_json = "{}";
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        db.insert(info);
        return c;
    };
    const fs::path a = make("a");
    const fs::path b = make("b");
    auto tag = [&](const fs::path& c) {
        return run_phig(phig_bin, dir,
                        "--no-wait --catalog '" + c.string() + "' tag add t --match '*.jpg'");
    };
    auto hold = [](const fs::path& lock) {
        fs::create_directories(lock.parent_path());
        int fd = ::open(lock.c_str(), O_RDWR | O_CREAT, 0644);
        EXPECT_GE(fd, 0);
        EXPECT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
        return fd;
    };

    int fd = hold(catalog_paths(a.string()).lock);
    PhigRun refused = tag(a);
    EXPECT_EQ(refused.exit_code, 1) << refused.output;
    EXPECT_NE(refused.output.find("Another phig operation is running"), std::string::npos)
        << refused.output;
    PhigRun other = tag(b);
    EXPECT_EQ(other.exit_code, 0) << "B was excluded by A's lock: " << other.output;
    ::flock(fd, LOCK_UN);
    ::close(fd);

    fd = hold(phig_scratch_lock_path(dir));
    PhigRun free_a = tag(a);
    EXPECT_EQ(free_a.exit_code, 0) << "A was excluded by the default catalog's lock: "
                                   << free_a.output;
    ::flock(fd, LOCK_UN);
    ::close(fd);
}
```

  Add `#include "paths.h"` if the file lacks it.

- [ ] **Step 2: Run it.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='LockfileTest.*'
  ```
  Expected: PASS, since Tasks 2 and 6 already route the lock through the catalog. To confirm the test can fail, temporarily change `acquire_writer_lock` to lock `catalog_paths(default_catalog_dir()).lock`, rebuild, and watch `CliLocksTheCatalogItWrites` fail. Then revert the change.

- [ ] **Step 3: Update R50 and 039.**
  - R50 in `rules/concurrency.toml`:
    ```toml
    tests = ["LockfileTest.CliLocksTheCatalogItWrites"]
    status = "holds"
    test_gap = "The test drives tag add, one lock-taking command; the others reach the same acquire_writer_lock, which reads current_catalog().lock, and that is read off the code."
    ```
    Rationale:
    > lock_path(db) is db + ".lock", and a catalog's lock is lock_path of its own database. The CLI installs the catalog it resolved, from --catalog, PHIG_CATALOG, the config file or the default, before any command runs, and acquire_writer_lock takes that catalog's lock. libphig's phig_acquire_write_lock calls the same lock_path on the context's database. So two processes writing one catalog exclude each other whichever surface they come through, and commands on different catalogs do not.

    Keep `enforced_at`, `binds` and `protects` as they are.
  - In `plans/039-*.md`, remove `R50` from `closes_rules`; if the list becomes empty, remove the line. Replace the paragraph beginning "**This plan closes the `core/` half only.**" with:
    > R50, the lock following the database it protects, was closed by plan 067. This plan still owns making `lock_path` and `WriterLock` portable.
  - Run `python3 scripts/rules.py --check --emit`. Expected: `OK`.

- [ ] **Step 4: Commit.**
  ```bash
  clang-format -i tests/test_lockfile.cpp
  git add tests/test_lockfile.cpp rules/ plans/039-*.md
  git commit -m "Pin that a command locks the catalog it writes (R50)"
  ```

---

### Task 9: Trash and thumbnails follow the catalog

**Files:**
- Test: `tests/test_catalog.cpp`
- Modify: `rules/authorization.toml`, `rules/recoverability.toml` (trash rationales)

- [ ] **Step 1: Write the tests.** Append to `tests/test_catalog.cpp`:

```cpp
TEST_F(CatalogCliTest, RmTrashesIntoItsOwnCatalog) {
    const fs::path a = make_catalog("a");
    const fs::path b = make_catalog("b");
    PhigRun r = run(a, "rm --match '*' --yes");
    ASSERT_EQ(r.exit_code, 0) << r.output;
    EXPECT_FALSE(fs::exists(a / "photos" / "a.jpg"));
    ASSERT_TRUE(fs::exists(a / "trash"));
    EXPECT_FALSE(fs::is_empty(a / "trash"));
    EXPECT_FALSE(fs::exists(b / "trash"));
    EXPECT_FALSE(fs::exists(phig_scratch_db_path(dir).parent_path() / "trash"))
        << "trash went to the default catalog";
}

TEST_F(CatalogCliTest, TrashEmptyLeavesAnotherCatalogsTrash) {
    const fs::path a = make_catalog("a");
    const fs::path b = make_catalog("b");
    fs::create_directories(a / "trash");
    std::ofstream(a / "trash" / "kept.jpg") << "x";
    PhigRun r = run(b, "trash empty --yes");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_TRUE(fs::exists(a / "trash" / "kept.jpg"));
}

TEST_F(CatalogCliTest, ThumbsCleanLeavesAnotherCatalogsThumbnails) {
    const fs::path a = make_catalog("a");
    const fs::path b = make_catalog("b");
    const std::string orphan = std::string(64, 'f') + ".jpg";
    const fs::path a_orphan = a / "thumbs" / "512" / "ff" / orphan;
    const fs::path b_thumb = b / "thumbs" / "512" / "ff" / orphan;
    for (const fs::path& p : {a_orphan, b_thumb}) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }
    PhigRun r = run(a, "thumbs clean");
    EXPECT_EQ(r.exit_code, 0) << r.output;
    EXPECT_FALSE(fs::exists(a_orphan)) << "the control: A's own orphan is removed";
    EXPECT_TRUE(fs::exists(b_thumb));
}
```

- [ ] **Step 2: Run them.**
  ```bash
  cmake --build build -j12 && ./build/phig-tests --gtest_filter='CatalogCliTest.*'
  ```
  Expected: PASS; Tasks 2 and 6 already put trash and thumbnails inside the catalog. To confirm they can fail, temporarily make `catalog_paths` set `p.trash = data_dir() + "/trash"` and watch `RmTrashesIntoItsOwnCatalog` and `TrashEmptyLeavesAnotherCatalogsTrash` fail. Then revert.

- [ ] **Step 3: Update the trash rationales.** In `rules/authorization.toml` (the rule near line 125) and `rules/recoverability.toml` (the rule near line 72), replace `<data_dir>/trash` with `<catalog>/trash`. Add `"CatalogCliTest.RmTrashesIntoItsOwnCatalog"` to the recoverability rule's `tests`, and `"CatalogCliTest.TrashEmptyLeavesAnotherCatalogsTrash"` to the authorization rule's. Run `python3 scripts/rules.py --check --emit`. Expected: `OK`.

- [ ] **Step 4: Commit.**
  ```bash
  clang-format -i tests/test_catalog.cpp
  git add tests/test_catalog.cpp rules/
  git commit -m "Pin that trash and thumbnails stay inside their own catalog"
  ```

---

### Task 10: libphig names a catalog

**Files:**
- Modify: `libphig/include/phig/phig.h` (the `phig_config` struct and its comments, lines 15 and 245, and the `PHIG_KEY_*` macros at 314-316), `libphig/context.cpp`, `libphig/internal.h:31`, `libphig/settings.cpp:190-198`
- Modify: `core/paths.h`, `core/paths.cpp` (remove `set_path_overrides`)
- Test: `tests/test_libphig_abi.cpp`, `tests/test_libphig_mutate.cpp`, `tests/test_libphig_query.cpp`, `tests/test_libphig_organize.cpp`, `tests/test_libphig_operations.cpp`, `tests/test_libphig_settings.cpp`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
      const char* catalog_dir; /* required */
      phig_access_mode mode;
      const char* models_dir; /* NULL = phig's default (~/.cache/phig/models) */
  } phig_config;
  #define PHIG_KEY_CATALOG "paths.catalog"
  ```
  `PHIG_KEY_DB` and `PHIG_KEY_THUMBS_DIR` are removed.

- [ ] **Step 1: Convert the tests.** These are the failing tests: `catalog_dir` does not exist yet.
  - **`TempDbPath`** in `tests/test_libphig_abi.cpp` and `tests/test_libphig_mutate.cpp` becomes a catalog directory:
    ```cpp
    // A scratch catalog directory; `path` is its database.
    struct TempDbPath {
        std::string dir;
        std::string path;
        TempDbPath() {
            dir = (std::filesystem::temp_directory_path() /
                   ("phig_abi_" + std::to_string(::getpid()) + "_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this))))
                      .string();
            std::filesystem::create_directories(dir);
            path = dir + "/phig.db";
        }
        ~TempDbPath() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };
    ```
    Use the prefix `phig_mut_` in the mutate file.
  - **`cfg.db_path`:** every `cfg.db_path = tmp.path.c_str();` becomes `cfg.catalog_dir = tmp.dir.c_str();`. At the other sites, `cfg.catalog_dir` gets the directory containing a database named `phig.db`. Where a fixture names its database anything else, such as `fx.db_path` in organize or `db_path` in operations, rename that database to `phig.db` in its own directory. `cfg.db_path = "/tmp/whatever.db"` becomes `cfg.catalog_dir = "/tmp/whatever"`, and `cfg.db_path = nullptr` becomes `cfg.catalog_dir = nullptr`.
  - **`PathOverrideGuard`** becomes:
    ```cpp
    struct CatalogGuard {
        ~CatalogGuard() { clear_catalog(); }
    };
    ```
    Rename it at every use.
  - **`AbiThumb.HonorsConfiguredThumbsDir`** becomes:
    ```cpp
    TEST(AbiThumb, ThumbnailsLiveInTheCatalog) {
        CatalogGuard guard;
        TempDbPath tmp;
        make_db(tmp.path);

        phig_config cfg{};
        cfg.catalog_dir = tmp.dir.c_str();
        cfg.mode = PHIG_READ_ONLY;
        phig_ctx* ctx = nullptr;
        ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

        std::string sha(64, 'a');
        char buf[4096];
        bool exists = true;
        ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), 512, buf, sizeof(buf), &exists), PHIG_OK);
        EXPECT_EQ(std::string(buf).rfind(tmp.dir + "/thumbs/", 0), 0u) << buf;
        EXPECT_FALSE(exists);

        phig_shutdown(ctx);
    }
    ```
  - Delete `AbiThumb.NullThumbsDirLeavesDefaultInPlace`; there is no separate thumbnail directory left to leave in place.
  - Delete `ScratchThumbsDir`, the `ScratchThumbsDir thumbs;` lines and the `cfg.thumbs_dir = ...` lines in the `AbiThumbGenerate` tests. Their thumbnails now go to `tmp.dir/thumbs`, which `TempDbPath` removes.
  - **`OpFixture`** in `tests/test_libphig_operations.cpp`: thumbnails now go to `<catalog>/thumbs`. Point any assertion that expected them under the fixture's data directory at `<fixture dir>/thumbs`.
  - **`tests/test_libphig_settings.cpp`:**
    - Replace every `PHIG_KEY_DB` with `PHIG_KEY_CATALOG`.
    - In `KeyLookupIsCaseAndSeparatorInsensitive`, use `models-dir`: the ini becomes `"[General]\nparallel = 2\n[Paths]\nmodels-dir = /ssd\n"`, with `PHIG_KEY_MODELS_DIR`, `"PATHS.Models-Dir"` and `"paths.models_dir"`.
    - `ToConfigFillsPathsAndLeavesModeAlone`: the ini is `"[paths]\ncatalog = /photos/catalog\nmodels_dir = /ssd/models\n"`. Expect `cfg.catalog_dir` to be `"/photos/catalog"` and `cfg.models_dir` to be `"/ssd/models"`, and delete the `models_dir == nullptr` expectation.
    - `ToConfigDoesNotClobberPreSeededFields`: `cfg.catalog_dir = "/caller/seeded"`, expecting the same value back.
    - `ToConfigPointersSurviveALaterSetOfTheSameKey`: the ini is `"[paths]\ncatalog = /photos/catalog\nmodels_dir = /ssd/models\n"`. `db` becomes `cfg.catalog_dir` and `thumbs` becomes `cfg.models_dir`. The set is `phig_settings_set(s, PHIG_KEY_CATALOG, "/new/place")` with the expectation updated to match, and the unset is `phig_settings_unset(s, PHIG_KEY_MODELS_DIR)`.
    - Leave alone the other tests whose ini text merely contains `db =` or `thumbs_dir =` as arbitrary keys. `phig_settings` stores any key.

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build -j12 --target phig-tests 2>&1 | grep -m3 error
  ```
  Expected: `'phig_config' has no member named 'catalog_dir'`.

- [ ] **Step 3: Implement it.**
  - In `phig.h`, replace the `phig_config` struct with:
    ```c
    typedef struct {
        /* The catalog directory: phig.db, its lock, the trash, thumbs/ and
           phig.log live inside it. Required.

           PROCESS-WIDE: phig resolves the catalog's thumbnail and trash
           locations through a global, so the last context initialized decides
           them for every context in the process. Open one catalog per process.
           The catalog is installed before the database is opened, so it takes
           effect even if phig_init then fails.

           PHIG_READ_ONLY requires the catalog to exist at this build's schema.
           PHIG_READ_WRITE creates a missing catalog and migrates an older one. */
        const char* catalog_dir;
        phig_access_mode mode;

        const char* models_dir; /* NULL = phig's default (~/.cache/phig/models) */
    } phig_config;
    ```
    Update the comments at lines 15 and 245 that mention `phig_config.thumbs_dir` to say "the catalog's thumbs/ directory (process-wide; see phig_config.catalog_dir)". Replace the `PHIG_KEY_DB` and `PHIG_KEY_THUMBS_DIR` defines with `#define PHIG_KEY_CATALOG "paths.catalog"`.
  - In `libphig/internal.h`, delete the `std::string thumbs_dir;` member.
  - In `libphig/context.cpp`, add `#include "catalog.h"`, and replace `phig_init`'s body after the `out` check with:
    ```cpp
    if (cfg == nullptr || cfg->catalog_dir == nullptr || cfg->catalog_dir[0] == '\0') {
        return PHIG_ERR_INVALID_ARG;
    }
    try {
        const CatalogPaths paths = catalog_paths(cfg->catalog_dir);
        set_catalog(paths);
        migrate_legacy_catalog_files(paths);
        auto ctx = std::make_unique<phig_ctx>();
        ctx->mode = cfg->mode;
        ctx->db_path = paths.db;
        // A read-only context neither creates nor migrates (R70); a read-write
        // one creates a missing catalog, because a client indexes into a new
        // catalog through one.
        ctx->db = open_catalog_db(paths.db, cfg->mode == PHIG_READ_ONLY ? CatalogAccess::Read
                                                                        : CatalogAccess::Create);
        *out = ctx.release();
        return PHIG_OK;
    } catch (const CatalogError& e) {
        return e.kind() == CatalogError::Kind::Missing ? PHIG_ERR_DB : PHIG_ERR_SCHEMA;
    } catch (...) {
        return PHIG_ERR_DB;
    }
    ```
  - In `libphig/settings.cpp`, the `map[]` becomes:
    ```cpp
    {PHIG_KEY_CATALOG, &cfg->catalog_dir},
    {PHIG_KEY_MODELS_DIR, &cfg->models_dir},
    ```
  - Remove `set_path_overrides` from `core/paths.h` and `core/paths.cpp`. Verify with `grep -rn set_path_overrides core cli libphig gtk tests`. Expected: no output.

- [ ] **Step 4: Run everything, including the C compile check.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  python3 scripts/rules.py --check
  ```
  Expected: all pass. `tests/test_libphig_c_compile.c` still compiles as C99. `Abi.InitFailsOnUnopenableDatabase` and the schema tests keep their status codes: a missing read-only catalog is `PHIG_ERR_DB` and a mismatched schema is `PHIG_ERR_SCHEMA`.

- [ ] **Step 5: Commit.**
  ```bash
  clang-format -i libphig/context.cpp libphig/settings.cpp libphig/include/phig/phig.h libphig/internal.h core/paths.h core/paths.cpp tests/test_libphig_*.cpp
  git add libphig core/paths.h core/paths.cpp tests/
  git commit -m "Open a libphig context on a catalog directory"
  ```

---

### Task 11: The gtk client opens a catalog

**Files:**
- Modify: `gtk/src/collection.h:36-42`, `gtk/src/collection.cpp:48-80`, `gtk/src/main.cpp` (the `db_path` member at line 97, `startup_thread_func` at line 310, `default_db_path` at lines 898-904, `main` at line 908)
- Test: `gtk/tests/test-collection.cpp`

**Interfaces:**
- Produces:
  ```c
  PhigCollection* phig_collection_open(const char* catalog_dir, char** error);
  char* phig_collection_default_catalog(void);  /* replaces phig_collection_default_path */
  ```

- [ ] **Step 1: Convert the tests.**
  - **Fixture:** `dir` is the catalog, and its database is already `dir / "phig.db"`. Delete the `XDG_DATA_HOME` save, set and restore that Task 2 introduced, and the `saved_*` member: thumbnails now go to `dir / "thumbs"`. Make `open()` call `phig_collection_open(dir.c_str(), &error)`, and update the fixture comment to "XDG_DATA_HOME needs no redirect: the catalog is the temp dir, so generated thumbnails land inside it."
  - **`test_open_reports_a_missing_file_without_creating_it`:** `absent` becomes `f.dir / "absent"`, a directory, passed as the catalog. Add `g_assert_false(fs::exists(absent));` at the end.
  - **`test_a_thumbnail_is_generated_on_demand`:** after the second successful lookup, add:
    ```cpp
    g_assert_true(std::string(path).rfind((f.dir / "thumbs").string(), 0) == 0);
    ```
  - **`test_default_path_follows_xdg_data_home`** becomes:
    ```cpp
    void test_default_catalog_follows_the_environment() {
        // A config directory with no config file, so a developer's real
        // config.ini cannot decide the answer.
        const std::string config_home =
            "/tmp/phig-gtk-xdg-config-" + std::to_string(static_cast<long>(getpid()));
        g_setenv("XDG_CONFIG_HOME", config_home.c_str(), TRUE);
        g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
        g_unsetenv("PHIG_CATALOG");

        char* path = phig_collection_default_catalog();
        g_assert_cmpstr(path, ==, "/tmp/phig-gtk-xdg-data/phig");
        g_free(path);

        g_setenv("PHIG_CATALOG", "/tmp/phig-gtk-elsewhere", TRUE);
        path = phig_collection_default_catalog();
        g_assert_cmpstr(path, ==, "/tmp/phig-gtk-elsewhere");
        g_free(path);

        g_unsetenv("PHIG_CATALOG");
        g_unsetenv("XDG_DATA_HOME");
        g_unsetenv("XDG_CONFIG_HOME");
    }
    ```
    Register it as `/collection/default-catalog-follows-the-environment`, in place of the old registration. Add `#include <unistd.h>` for `getpid`.

- [ ] **Step 2: Run them to see them fail.**
  ```bash
  cmake --build build/with-gtk -j12 --target gtk-all 2>&1 | grep -m3 error
  ```
  Expected: `'phig_collection_default_catalog' was not declared`.

- [ ] **Step 3: Implement it.**
  - In `collection.h`, replace the open and default-path declarations and their comments with:
    ```c
    /* Opens the catalog in catalog_dir read-only, and installs it as the
     * process's catalog, so thumbnail lookups resolve inside it. Returns NULL
     * and sets *error (free with g_free) if the catalog has no database, or its
     * schema is not the version this build reads. Never creates a file. */
    PhigCollection* phig_collection_open(const char* catalog_dir, char** error);
    void phig_collection_close(PhigCollection* coll);

    /* The catalog phig uses when none is named: PHIG_CATALOG, then the config
     * file's [paths] catalog, then $XDG_DATA_HOME/phig. Free with g_free. NULL,
     * with a warning, if the config file cannot be read. */
    char* phig_collection_default_catalog(void);
    ```
  - In `collection.cpp`, add `#include "catalog.h"` and `#include "config.h"`, and replace `phig_collection_open` and `phig_collection_default_path` with:
    ```cpp
    PhigCollection* phig_collection_open(const char* catalog_dir, char** error) {
        *error = nullptr;
        try {
            const CatalogPaths paths = catalog_paths(catalog_dir);
            auto coll = std::make_unique<PhigCollection>();
            coll->db = open_catalog_db(paths.db, CatalogAccess::Read);
            set_catalog(paths);
            migrate_legacy_catalog_files(paths);
            return coll.release();
        } catch (const std::exception& e) {
            *error = g_strdup(e.what());
        } catch (...) {
            *error = g_strdup("Could not open the phig catalog.");
        }
        return nullptr;
    }

    char* phig_collection_default_catalog(void) {
        try {
            const Config cfg = load_config(config_path());
            return g_strdup(resolve_catalog_dir(std::nullopt, cfg).dir.c_str());
        } catch (...) {
            warn_caught("default catalog");
        }
        return nullptr;
    }
    ```
  - In `main.cpp`:
    - rename the `PhigApp` member `db_path` to `catalog_dir`, and its use in `startup_thread_func`;
    - rename `default_db_path()` to `resolve_default_catalog()`, calling `phig_collection_default_catalog()`;
    - in `main`, use `app->catalog_dir = argc > 1 ? argv[1] : resolve_default_catalog();`.

- [ ] **Step 4: Run the gtk tests, then a smoke run with a display.**
  ```bash
  cmake --build build/with-gtk -j12 --target gtk-all && ctest --test-dir build/with-gtk -L gtk 2>&1 | tail -3
  ```
  Expected: 3/3 pass.

  Then, in the scratch environment from Global Constraints:
  ```bash
  ./build/phig --catalog $S/cat scan tests/data
  PHIG_MEASURE=1 ./build/with-gtk/gtk/phig-gtk $S/cat
  ```
  Expected: the window shows the scanned rows, and `$S/cat/thumbs` fills as you scroll. `PHIG_MEASURE` quits on its own.

- [ ] **Step 5: Commit.**
  ```bash
  clang-format -i gtk/src/collection.h gtk/src/collection.cpp gtk/src/main.cpp gtk/tests/test-collection.cpp
  git add gtk/
  git commit -m "Open the gtk client on a catalog directory"
  ```

---

### Task 12: Docs, plan status, and final verification

**Files:**
- Modify: root `CLAUDE.md`, `README.md`, `cli/CLAUDE.md`, `libphig/CLAUDE.md`, `gtk/CLAUDE.md`, `gtk/README.md`, `plans/067-catalog-directory.md`, `plans/000-INDEX.md`

- [ ] **Step 1: Root `CLAUDE.md`.**
  - **CLI command reference:** the global line becomes `phig [--config <path>] [--catalog <dir>] [--no-wait] <command> ...`. Delete every ` [--db <path>]` in the block. Check with `grep -n -- '--db' CLAUDE.md`; only `--db-only` should remain.
  - **The paragraph beginning "Paths follow XDG conventions"** becomes:
    > A catalog is one directory holding a collection's database (`phig.db`), its writer lock (`phig.db.lock`), the trash (`trash/`), the thumbnail cache (`thumbs/<size>/ab/<sha>.jpg`, tagged with `CACHEDIR.TAG`) and the diagnostic log (`phig.log`). Which catalog a command uses: `--catalog <dir>`, then `PHIG_CATALOG`, then `[paths] catalog` in the config file, then `~/.local/share/phig/`. All of it is resolved in `core/paths.cpp` and `core/catalog.cpp`. User-level files stay in XDG locations: models at `~/.cache/phig/models`, config at `~/.config/phig/config.ini`. The first run against the default catalog moves thumbnails from `~/.cache/phig/thumbs` and the log from `~/.local/state/phig/` into it. A read never creates or migrates a catalog (R70): a read against a missing or outdated catalog fails, and only `scan` creates one. An optional INI config file (parsed in `core/config.cpp` via vendored inih) sets defaults; a global `--config <path>` flag overrides its location. Precedence: CLI flag > config > default.
  - **Concurrency:** "take a process-wide advisory `flock` on `<db>.lock`" becomes "take a process-wide advisory `flock` on the catalog's `phig.db.lock`".
  - **Thumbnails:** "JPEGs cached in `~/.cache/phig/thumbs/<size>/ab/<sha>.jpg`" becomes "JPEGs cached in `<catalog>/thumbs/<size>/ab/<sha>.jpg`".
  - **Real-run checks:** add "A read against a catalog that does not exist now fails with `no phig catalog at <dir>` and creates nothing (R70), so `phig --catalog <dir> search --count` is a sound check against a catalog you have scanned."
  - **Building and testing:** add, after the `--parallel` paragraph: "The test binaries point `HOME` and the four XDG directories at a per-process scratch directory, and clear `PHIG_CATALOG`, before any test runs (`tests/scratch_home.cpp`). The face models are linked in from the real `~/.cache/phig/models`. A test therefore never writes the developer's catalog, log or lock."

- [ ] **Step 2: The other docs.**
  - `cli/CLAUDE.md`: "Flag order convention: `phig <subcommand> <args> --db <path>`" becomes "Global flags (`--catalog`, `--config`, `--no-wait`) are recognized anywhere in argv; by convention they go first: `phig --catalog <dir> <subcommand> <args>`."
  - `libphig/CLAUDE.md`: replace mentions of `db_path` and `thumbs_dir` in `phig_config` with `catalog_dir`. Note that `PHIG_READ_WRITE` creates a missing catalog and `PHIG_READ_ONLY` refuses one.
  - `gtk/CLAUDE.md` and `gtk/README.md`: the client's argument is a catalog directory, and the default comes from `phig_collection_default_catalog`, so `PHIG_CATALOG` and the config file apply.
  - `README.md`: wherever it shows `--db`, show `--catalog <dir>`. Add one sentence on `PHIG_CATALOG` for development, for example in a `.envrc`.

- [ ] **Step 3: Close the plan.**
  - In `plans/067-catalog-directory.md`, set `status: completed` and add `completed: <today's date>` after `created`.
  - Append an `## Outcome` section that lists the commits from BASE to HEAD (`git log --oneline $(cat /tmp/phig-067-base.txt)..HEAD`), the final test counts, and these findings:
    - libphig's `phig_init` ignores `phig_config.models_dir` (Decision 7);
    - the "Decisions beyond 067" rulings, R20 and R21 among them.
  - In `plans/000-INDEX.md`, set row 067's status to `completed`. Re-read both files and confirm they agree.

- [ ] **Step 4: Final verification.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  cmake --build build/with-gtk -j12 --target gtk-all && ctest --test-dir build/with-gtk -L gtk 2>&1 | tail -3
  python3 scripts/rules.py --check --emit && git status --short rules/
  ./build/phig-tests --gtest_output=xml:/tmp/phig-067-tests.xml >/dev/null; python3 scripts/rules.py --check --test-results /tmp/phig-067-tests.xml
  git diff --name-only $(cat /tmp/phig-067-base.txt)..HEAD -- '*.cpp' '*.h' '*.c' | xargs clang-format --dry-run --Werror
  grep -rn '"--db"' cli/ ; grep -rn 'set_path_overrides\|PHIG_KEY_DB\|PHIG_KEY_THUMBS_DIR' core cli libphig gtk tests
  stat -c '%Y %n' ~/.local/state/phig/phig.log ~/.local/share/phig/phig.db.lock | diff - /tmp/phig-067-real-mtimes.txt && echo UNTOUCHED
  ```
  Expected:
  - both suites pass;
  - the rules check prints `OK` with no diff under `rules/`, and the test-results check prints `OK`;
  - clang-format is clean;
  - both greps print nothing;
  - `UNTOUCHED`.

- [ ] **Step 5: Smoke test the CLI**, in the scratch environment from Global Constraints:
  ```bash
  ./build/phig --catalog $S/none search --count; echo "exit=$?"; ls $S/none 2>&1
  ./build/phig --catalog $S/cat scan tests/data && ./build/phig --catalog $S/cat search --count
  PHIG_CATALOG=$S/cat ./build/phig search --count
  ./build/phig --catalog $S/cat thumbs rebuild && ls $S/cat/thumbs | head
  mkdir -p $XDG_CACHE_HOME/phig/thumbs/512/ab && touch $XDG_CACHE_HOME/phig/thumbs/512/ab/$(printf 'a%.0s' {1..64}).jpg
  ./build/phig scan tests/data >/dev/null && ./build/phig search --count >/dev/null && ls $XDG_DATA_HOME/phig/thumbs/512/ab
  ```
  Expected:
  - the first command prints `no phig catalog at $S/none`, gives `exit=1`, and `ls` reports that `$S/none` does not exist;
  - the scan and both counts agree;
  - `$S/cat/thumbs` holds size directories and `CACHEDIR.TAG`;
  - after the default-catalog scan, the legacy thumbnail appears under `$XDG_DATA_HOME/phig/thumbs/512/ab`.

- [ ] **Step 6: Commit.**
  ```bash
  git add CLAUDE.md README.md cli/CLAUDE.md libphig/CLAUDE.md gtk/CLAUDE.md gtk/README.md plans/067-catalog-directory.md plans/000-INDEX.md
  git commit -m "Document the catalog directory and mark plan 067 completed"
  ```
