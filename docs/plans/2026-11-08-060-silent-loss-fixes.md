# Silent Loss Fixes (R27, R48, R69, R81, R83) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close five of plan 060's violated rules:
- R27: a copy or move that did not do everything is logged `interrupted` and exits 1;
- R48: a half-done item is counted once;
- R69: `--output` never destroys an existing file;
- R81: staging stdin never touches an existing path;
- R83: a failed delete reports its cause and never leaves a transaction open.

**Architecture:** Every guard lives in `core/`, so each rule can hold without a `cli/` anchor:
- `carry_sidecars` carries each attachment independently and reports failures;
- `OrganizeEngine::apply` counts by photo and derives `completed` from what happened;
- `core/paths` gains `write_output_file`, a temp-and-rename that refuses or replaces, and `write_private_temp_file`, which uses `mkstemp`;
- `core/database.cpp` reads `sqlite3_errmsg` before rolling back and rolls back on any throw.

The CLI calls these; libphig's organize result gains two fields.

**Tech Stack:** C++20, CMake, SQLite, GoogleTest, Python 3 (`scripts/rules.py`).

**Spec:** `plans/060-silent-loss-on-success.md`, especially its "Decisions for R27, R48, R69, R81 and R83" section and the R27, R48, R69, R81 and R83 sections. Read those before starting. The spec wins over this plan where they disagree.

## Global Constraints

- **Never touch the developer's real phig files:** `~/.local/share/phig/`, `~/.cache/phig/`, `~/.local/state/phig/`, `~/.config/phig/`, `/mnt/data/Pictures`. Not even a read-only `sqlite3` query, which creates sidecar files.
  - The gtest binaries redirect HOME and XDG themselves (`tests/scratch_home.cpp`), so run them in your normal environment. Never export a scratch XDG environment around `ctest` or `./build/phig-tests`, or the face tests lose their models.
  - Before any hand-run `./build/phig`, export a scratch environment:
    ```bash
    export S=$(mktemp -d /tmp/phig-060-XXXX)
    export HOME=$S/home XDG_CONFIG_HOME=$S/config XDG_DATA_HOME=$S/data XDG_CACHE_HOME=$S/cache XDG_STATE_HOME=$S/state
    unset PHIG_CATALOG
    ```
- **Real-file check:** at pre-flight, `stat -c '%Y %n' ~/.local/state/phig/phig.log ~/.local/share/phig/phig.db.lock > /tmp/phig-060-real-mtimes.txt`. Every task ends with `stat -c '%Y %n' ~/.local/state/phig/phig.log ~/.local/share/phig/phig.db.lock | diff - /tmp/phig-060-real-mtimes.txt && echo UNTOUCHED`.
- **Build and test:**
  - `cmake --build build -j12`, then `ctest --test-dir build --parallel 6`.
  - Also run `./build/phig-tests` once unfiltered as a single process, writing `--gtest_output=xml:/tmp/phig-060-tests.xml`.
  - Then `python3 scripts/rules.py --check --test-results /tmp/phig-060-tests.xml`.
- **Warnings are errors** (`-DPHIG_WERROR=ON`). Format with clang-format (Google style, 4-space indent, 100 columns): run `clang-format -i` only on touched files under `core/`, `libphig/`, `cli/` and `tests/`, then `--dry-run --Werror`. Do not reformat lines you did not change.
- **Rules of record:** a guard changed here updates its `rules/*.toml` record in the same commit, then `python3 scripts/rules.py --check --emit`, with the regenerated `rules/by-*.md` committed. Never hand-edit those views.
  - An anchor under `cli/` is legal only on a `violated` rule. Every rule this plan sets to `holds` anchors in `core/` or `libphig/`.
- **Data-safety changes ship with real-behaviour tests.** Subprocess tests drive the built binary through `run_phig` (`tests/phig_binary.h`) against a catalog under the test's temp directory.
- **Comments describe the present; prose is plain.** No AI attribution in commits.
- **Known flaky tests:** `OrganizeEngineTest.CancellationPartwayMarksTheOperationInterrupted` under heavy `--parallel`. Re-run it alone.

## Decisions beyond the spec

1. **`--overwrite` without `--output` is an error, as `--count` with `--output` is.** The spec retires flags that do nothing; an `--overwrite` with no file to overwrite does nothing.
2. **An existing path is also refused before the search runs.** This saves the work of a search whose results cannot be written. The authoritative refusal is still the no-replace rename in `write_output_file`.
3. **An attachment failure's progress message names the parent and each attachment,** in one line joined by `; `, because the CLI logs one line per failed item.
4. **The face-search `--output` site has no subprocess test.** A face search needs the models and a detectable face; it goes through the same `ResultSink` as the three tested sites.

## File map

| File | Change |
|---|---|
| `core/database.h`, `core/database.cpp` | R83: message before rollback; one quiet rollback on any throw in `delete_paths`, `delete_paths_logged`, `log_prune`. |
| `core/organize_exec.h`, `core/organize_exec.cpp` | `CarryResult`; `carry_sidecars` carries each attachment independently. |
| `core/organize_engine.h`, `core/organize_engine.cpp` | `OrganizeResult::attachment_errors`, `cancelled`; count once; derive `completed`. |
| `libphig/include/phig/phig.h`, `libphig/organize.cpp` | `phig_organize_result` gains `attachment_errors`, `cancelled`. |
| `core/paths.h`, `core/paths.cpp` | `OutputExists`, `write_output_file`, `write_private_temp_file`. |
| `cli/main.cpp` | `cmd_organize` summary and exit code; `ResultSink` for the four `--output` sites; `--overwrite`; `--count`/`--overwrite` checks; stdin staging. |
| `tests/phig_binary.h` | `run_phig` gains an optional stdin file. |
| `tests/test_database.cpp`, `tests/test_organize_exec.cpp`, `tests/test_organize_engine.cpp`, `tests/test_libphig_organize.cpp`, `tests/test_paths.cpp` | Tests per task. |
| `tests/test_cli_output.cpp` | Create: subprocess tests for `--output` and stdin staging. Add to `phig-tests` in `CMakeLists.txt`. |
| `rules/recoverability.toml` (R27, R83), `rules/atomicity.toml` (R48), `rules/boundary.toml` (R69, R81), `rules/operations.toml` (`--overwrite`) | Records. |
| Root `CLAUDE.md`, `README.md`, `libphig/CLAUDE.md`, `plans/060-silent-loss-on-success.md`, `plans/000-INDEX.md` | Docs and status. |

## Pre-flight

- [ ] Branch (needs the user's approval): `git checkout master && git checkout -b 060-silent-loss-fixes && git rev-parse HEAD > /tmp/phig-060-base.txt`.
- [ ] Record the real files' mtimes (Global Constraints).
- [ ] Baseline: build and `ctest --test-dir build --parallel 6`. Expected: all pass. Run `python3 scripts/rules.py --check`. Expected: `OK`.

---

### Task 1: A failed delete reports its cause and closes its transaction (R83)

**Files:** `core/database.h`, `core/database.cpp` (`log_prune` at ~841, `delete_paths` at ~1052, `delete_paths_logged` at ~1085); test `tests/test_database.cpp`; `rules/recoverability.toml` (R83).

**Interfaces:**
- Produces a private `void Database::rollback_if_open() noexcept;`. It runs `ROLLBACK` only when `sqlite3_get_autocommit(db_) == 0`, and swallows any error, so it can never replace the exception being thrown.

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_database.cpp`, using the `DatabaseTest` fixture (`db`, `dir`). These need `#include <sqlite3.h>` if the file lacks it.

```cpp
namespace {
// A trigger that makes one statement fail with a recognizable message, the way
// a full disk or a constraint would, without needing either.
void add_fault(Database& db, const std::string& trigger_sql) {
    char* err = nullptr;
    ASSERT_EQ(sqlite3_exec(db.db_ptr(), trigger_sql.c_str(), nullptr, nullptr, &err), SQLITE_OK)
        << (err ? err : "");
    sqlite3_free(err);
}

std::string thrown_message(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}
}  // namespace

TEST_F(DatabaseTest, DeletePathsReportsTheCauseItRolledBack) {
    ImageInfo info;
    info.path = (dir / "a.jpg").string();
    info.filename = "a.jpg";
    info.extension = "jpg";
    info.metadata_json = "{}";
    db->insert(info);
    add_fault(*db,
              "CREATE TRIGGER fault BEFORE DELETE ON images BEGIN "
              "SELECT RAISE(ABORT, 'phig-test-fault: delete blocked'); END;");

    std::string msg = thrown_message([&] { db->delete_paths({info.path}); });
    EXPECT_NE(msg.find("phig-test-fault: delete blocked"), std::string::npos) << msg;
    EXPECT_EQ(sqlite3_get_autocommit(db->db_ptr()), 1) << "the transaction was left open";
    EXPECT_GT(db->get_image_id(info.path), 0) << "the rollback must keep the row";
}

TEST_F(DatabaseTest, DeletePathsLoggedReportsTheCauseItRolledBack) {
    ImageInfo info;
    info.path = (dir / "a.jpg").string();
    info.filename = "a.jpg";
    info.extension = "jpg";
    info.metadata_json = "{}";
    db->insert(info);
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-11-08T00:00:00Z");
    add_fault(*db,
              "CREATE TRIGGER fault BEFORE DELETE ON images BEGIN "
              "SELECT RAISE(ABORT, 'phig-test-fault: delete blocked'); END;");

    std::string msg = thrown_message([&] { db->delete_paths_logged({info.path}, op); });
    EXPECT_NE(msg.find("phig-test-fault: delete blocked"), std::string::npos) << msg;
    EXPECT_EQ(sqlite3_get_autocommit(db->db_ptr()), 1);
}

TEST_F(DatabaseTest, DeletePathsLoggedClosesItsTransactionWhenAnEntryFails) {
    ImageInfo info;
    info.path = (dir / "a.jpg").string();
    info.filename = "a.jpg";
    info.extension = "jpg";
    info.metadata_json = "{}";
    db->insert(info);
    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-11-08T00:00:00Z");
    add_fault(*db,
              "CREATE TRIGGER fault BEFORE INSERT ON operation_entries BEGIN "
              "SELECT RAISE(ABORT, 'phig-test-fault: entry blocked'); END;");

    std::string msg = thrown_message([&] { db->delete_paths_logged({info.path}, op); });
    EXPECT_NE(msg.find("phig-test-fault: entry blocked"), std::string::npos) << msg;
    EXPECT_EQ(sqlite3_get_autocommit(db->db_ptr()), 1)
        << "a throw from inside the loop must roll back, or the caller's terminal status "
           "is written into a transaction that never commits";
    EXPECT_GT(db->get_image_id(info.path), 0);
}

TEST_F(DatabaseTest, LogPruneReportsTheCauseItRolledBack) {
    for (int i = 0; i < 3; i++) {
        int64_t op = db->log_begin_operation("cp", "phig cp", "2026-11-08T00:00:00Z");
        db->log_add_entry(op, {"copy", "/s" + std::to_string(i), "/d" + std::to_string(i), ""});
        db->log_finish_operation(op, "completed");
    }
    add_fault(*db,
              "CREATE TRIGGER fault BEFORE DELETE ON operations BEGIN "
              "SELECT RAISE(ABORT, 'phig-test-fault: prune blocked'); END;");

    std::string msg = thrown_message([&] { db->log_prune(1); });
    EXPECT_NE(msg.find("phig-test-fault: prune blocked"), std::string::npos) << msg;
    EXPECT_EQ(sqlite3_get_autocommit(db->db_ptr()), 1);
}
```

  Check `log_add_entry`'s argument type in `core/database.h` and adapt the brace initializer if it differs. Add `#include <functional>` if needed.

- [ ] **Step 2: See them fail.**
  ```bash
  cmake --build build -j12 --target phig-tests && ./build/phig-tests --gtest_filter='DatabaseTest.*Cause*:DatabaseTest.*Closes*'
  ```
  Expected:
  - the three "Cause" tests fail with "not an error" in the message;
  - the "Closes" test fails on `sqlite3_get_autocommit == 0`.

- [ ] **Step 3: Implement it.**
  - In `core/database.h`, declare `void rollback_if_open() noexcept;` in the private section.
  - In `core/database.cpp`:
    ```cpp
    // Rolls back the open transaction, if there is one. A failed statement can
    // make SQLite roll back on its own, and a second ROLLBACK would then throw
    // "no transaction is active" over the error being reported, so this checks
    // first and never throws.
    void Database::rollback_if_open() noexcept {
        if (sqlite3_get_autocommit(db_) != 0) return;
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    ```
  - **`log_prune` and `delete_paths_logged`:** each `fail` lambda becomes
    ```cpp
    auto fail = [&](const std::string& what) {
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };
    ```
    It reads the message first and no longer rolls back itself. Wrap the body from just after `BEGIN IMMEDIATE TRANSACTION` up to the final `COMMIT` in:
    ```cpp
    try {
        ...
    } catch (...) {
        rollback_if_open();
        throw;
    }
    ```
    The rollback then runs once for every throw, including a throw from `delete_faces_for_image` or `log_add_entry`.
  - **`delete_paths`:** both inline sites become
    ```cpp
    const std::string err = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);   // second site only, as today
    rollback_if_open();
    throw std::runtime_error("Failed to delete path '" + path + "': " + err);
    ```
    Use the matching message text at the prepare site. Wrap the loop the same way, so a throw from `delete_faces_for_image` rolls back too. Do not roll back twice: the inline sites throw out of the `try`, and the `catch` then sees `autocommit != 0` and does nothing.

- [ ] **Step 4: Run the tests and the full suite.** Expected: all pass. Plan 046's three vec_faces tests must still pass. If exactly those three fail, record `sqlite3_libversion()` in plan 046 and report it.

- [ ] **Step 5: Update R83.** In `rules/recoverability.toml`:
  ```toml
  tests = [
      "DatabaseTest.DeletePathsReportsTheCauseItRolledBack",
      "DatabaseTest.DeletePathsLoggedReportsTheCauseItRolledBack",
      "DatabaseTest.LogPruneReportsTheCauseItRolledBack",
      "DatabaseTest.DeletePathsLoggedClosesItsTransactionWhenAnEntryFails",
  ]
  status = "holds"
  test_gap = "The faults are raised by triggers, not by a full disk or an expired busy timeout; the message path is the same."
  ```
  Rewrite the rationale to the present: each site reads `sqlite3_errmsg` before rolling back, and any throw inside the transaction rolls back once through `rollback_if_open`, which never replaces the original error. Remove the sentence saying the open-transaction case is not claimed by this rule. Run `python3 scripts/rules.py --check --emit`.

- [ ] **Step 6: Commit** `"Report why a delete failed and close its transaction on any throw (R83)"`.

---

### Task 2: Attachments are carried independently

**Files:** `core/organize_exec.h`, `core/organize_exec.cpp` (`carry_sidecars` at ~161); `core/organize_engine.cpp:174`, which is its only caller; test `tests/test_organize_exec.cpp` (the three `OrganizeCarryNoClobber` tests at ~710-770).

**Interfaces:**
- Produces:
  ```cpp
  // What carrying a parent's attachments did. One failure never stops the rest.
  struct CarryResult {
      size_t carried = 0;
      std::vector<std::string> failures;  // "<attachment path>: <reason>"
  };
  CarryResult carry_sidecars(Database& db, const std::string& parent_source,
                             const std::string& parent_dest, bool is_move, int64_t op_id);
  ```

- [ ] **Step 1: Update the tests first.**
  - The two refusal tests in `OrganizeCarryNoClobber` currently expect a throw. Make them assert that the call returns:
    - a `CarryResult` with one entry in `failures`, naming the attachment's source path;
    - `carried == 0`;
    - the occupant's bytes unchanged, as today.
  - `StillCarriesWhenTheDestinationIsFree` asserts `carried` and empty `failures`.
  - Add `OrganizeCarryNoClobber.OneRefusedAttachmentDoesNotStopTheOthers`: a parent with two sidecars, `IMG_1.xmp` and `IMG_1.pp3`, where only the `.xmp` destination is occupied. Expect `carried == 1`, `failures.size() == 1`, the `.pp3` at its destination, and the `.xmp` occupant unchanged.
  - Build and see the tests fail to compile, or fail.

- [ ] **Step 2: Implement it.** In `carry_sidecars`:
  - Wrap each loop iteration's body, from `refuse_occupied_carry_destination` through the log entry, in `try { ... } catch (const std::exception& e) { result.failures.push_back(side.string() + ": " + e.what()); }`.
  - Return the `CarryResult`.
  - Update the header's comment on the function to say a failed attachment is reported, not thrown, and the others are still carried.
  - In `core/organize_engine.cpp:174`, keep the build compiling for now with `result.files_written += carry_sidecars(...).carried;`. Task 3 replaces this.

- [ ] **Step 3: Run** `./build/phig-tests --gtest_filter='Organize*'`, then the full suite. Expected: pass.

- [ ] **Step 4: Commit** `"Carry each attachment independently and report the ones refused"`.

---

### Task 3: A copy or move counts each item once and says when it did not finish (R27, R48)

**Files:**
- `core/organize_engine.h` (`OrganizeResult` at ~101), `core/organize_engine.cpp` (`apply`, ~138-205)
- `libphig/include/phig/phig.h` (`phig_organize_result` at ~674), `libphig/organize.cpp` (~200)
- `cli/main.cpp` (`cmd_organize`'s summary, ~690)
- tests: `tests/test_organize_engine.cpp`, `tests/test_libphig_organize.cpp`, `tests/test_catalog.cpp` (the CLI exit test)
- rules: `rules/recoverability.toml` (R27), `rules/atomicity.toml` (R48)

**Interfaces:**
- Produces:
  ```cpp
  struct OrganizeResult {
      size_t applied = 0, skipped = 0, errors = 0;
      size_t attachment_errors = 0;  // attachments not carried; their parents count in applied
      size_t files_written = 0;
      bool cancelled = false;        // stopped by the cancel token
      bool completed = true;         // did everything asked: not cancelled, no error of either kind
  };
  ```
  ```c
  typedef struct {
      uint64_t applied;           /* parents moved or copied */
      uint64_t skipped;           /* conflicts left alone, or destinations occupied since planning */
      uint64_t errors;            /* parents that failed */
      uint64_t attachment_errors; /* attachments not carried; their parents count in applied */
      uint64_t files_written;     /* including sidecars and attached JPEGs */
      bool cancelled;             /* stopped by the caller partway */
      bool completed;             /* false when cancelled, or when anything failed */
  } phig_organize_result;
  ```

- [ ] **Step 1: Write the failing tests.**
  - Append to `tests/test_organize_engine.cpp`, using the `OrganizeEngineTest` fixture (`dir`, `db_path`, `add_image`). Read the fixture and `CancellationPartwayMarksTheOperationInterrupted` first for the exact helpers.
    - **`AnAttachmentNotCarriedCountsItsItemOnceAndInterruptsTheRun`:**
      - Set up one image with a sidecar `<stem>.xmp` beside it.
      - Plan a copy to `dir/dest`, then occupy the sidecar's computed destination (`fs::path(plan.at(0).dest).parent_path() / "<stem>.xmp"`) with a file reading `OCCUPANT`.
      - Apply with a progress queue, and collect every `FileDone` tick.
      - Expect:
        - `applied == 1`, `errors == 0`, `attachment_errors == 1`;
        - `completed == false`, `cancelled == false`;
        - the occupant still reads `OCCUPANT`;
        - every tick has `done <= total`;
        - the newest `operations.status` is `interrupted`.
    - **`AFileThatFailsInterruptsTheRun`:**
      - Set up two images and plan a copy.
      - Delete the first image's source file, then apply.
      - Expect:
        - `applied == 1`, `errors == 1`;
        - `completed == false`, `cancelled == false`;
        - status `interrupted`.
    - **`ACleanRunIsCompleted`:** a two-image copy. Expect `completed == true`, `cancelled == false`, `attachment_errors == 0`, and status `completed`.
  - In `CancellationPartwayMarksTheOperationInterrupted`, also assert `result.cancelled`.
  - In `tests/test_libphig_organize.cpp`, find the test that reads `phig_op_organize_result` after a clean run and also assert `attachment_errors == 0` and `cancelled == false`.
  - Append to `tests/test_catalog.cpp`'s `CatalogCliTest`:
    ```cpp
    TEST_F(CatalogCliTest, CopyExitsOneWhenAFileFails) {
        const fs::path c = make_catalog("c");
        fs::remove(c / "photos" / "a.jpg");  // the row's file is gone, so its copy throws
        PhigRun r = run(c, "cp '" + (dir / "dest").string() + "'");
        EXPECT_EQ(r.exit_code, 1) << r.output;
        EXPECT_NE(r.output.find("Errors: 1"), std::string::npos) << r.output;
    }
    ```
  - Build and see them fail.

- [ ] **Step 2: Implement the engine.** In `OrganizeEngine::apply`:
  - The cancel branch sets `result.cancelled = true;` and breaks; it no longer sets `completed`.
  - Replace the `try` block with:
    ```cpp
    bool parent_done = false;
    try {
        // Disk op, DB update and log entry commit together, with on-failure
        // compensation so disk and database never disagree.
        execute_organize_action(db_, act.source, act.dest, is_move, op_id, authorized_overwrite);
        parent_done = true;
    } catch (const std::exception& e) {
        result.errors++;
        p.error = "Failed to organize " + act.source + ": " + e.what();
    }
    if (parent_done) {
        // The photo landed, so the item counts as applied whatever happens to
        // its attachments; an attachment not carried is counted on its own.
        result.applied++;
        result.files_written++;
        std::vector<std::string> failures;
        try {
            CarryResult carry = carry_sidecars(db_, act.source, act.dest, is_move, op_id);
            result.files_written += carry.carried;
            failures = std::move(carry.failures);
        } catch (const std::exception& e) {
            failures.push_back(e.what());
        }
        if (!failures.empty()) {
            result.attachment_errors += failures.size();
            std::string joined;
            for (const auto& f : failures) joined += (joined.empty() ? "" : "; ") + f;
            p.error = std::string(is_move ? "Moved " : "Copied ") + act.source +
                      ", but did not carry: " + joined;
        }
    }
    ```
  - After the loop, before the log is finished:
    `result.completed = !result.cancelled && result.errors == 0 && result.attachment_errors == 0;`
    The log line already writes `completed ? "completed" : "interrupted"`.
  - Update `OrganizeResult` as in Interfaces, with the comments shown.

- [ ] **Step 3: libphig.** Replace the `phig_organize_result` struct with the Interfaces version, and map both new fields in `libphig/organize.cpp`. `tests/test_libphig_c_compile.c` must still compile as C99.

- [ ] **Step 4: CLI.** In `cmd_organize`:
  - Print `Interrupted. Stopped at a file boundary.` only when `result.cancelled`.
  - After the `Errors:` line, print `"  Attachments not carried: " << result.attachment_errors` when it is non-zero.
  - Return `result.completed ? 0 : 1`.

- [ ] **Step 5: Run** the focused tests, the full suite and the single-process run. Expected: pass.

- [ ] **Step 6: Update the rules.**
  - **R27:** tests become
    ```toml
    tests = [
        "OrganizeEngineTest.CancellationPartwayMarksTheOperationInterrupted",
        "OrganizeEngineTest.AFileThatFailsInterruptsTheRun",
        "OrganizeEngineTest.AnAttachmentNotCarriedCountsItsItemOnceAndInterruptsTheRun",
        "CatalogCliTest.CopyExitsOneWhenAFileFails",
    ]
    ```
    Set `status = "holds"`. The rationale now says organize derives `completed` from cancellation, file errors and attachment errors, and writes `interrupted` exactly when `rm` would. The test_gap names what is still unpinned: `rm`'s own exit code on a per-file failure, if no test covers it. Check `tests/test_remove_exec.cpp` first.
  - **R48:**
    ```toml
    tests = ["OrganizeEngineTest.AnAttachmentNotCarriedCountsItsItemOnceAndInterruptsTheRun"]
    status = "holds"
    ```
    The rationale now says an item counts once, in `applied` or `errors`, by its photo's outcome, and attachments not carried count in `attachment_errors`. Keep the R41 and R33 context.
  - Run `rules.py --check --emit`.

- [ ] **Step 7: Commit** `"Count a copy or move by its photo and mark an unfinished run interrupted (R27, R48)"`.

---

### Task 4: Core writes output files and stages temporary files safely

**Files:** `core/paths.h`, `core/paths.cpp`; test `tests/test_paths.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  // Thrown by write_output_file when something is already at the path and the
  // caller did not ask to replace it.
  class OutputExists : public std::runtime_error {
     public:
      explicit OutputExists(const std::string& path)
          : std::runtime_error(path + " already exists"), path_(path) {}
      const std::string& path() const { return path_; }

     private:
      std::string path_;
  };

  // Writes `contents` to `path` through a temporary sibling and a rename, so the
  // path never holds a partial file. Without `replace`, the rename refuses to
  // replace anything at `path`, a symlink included, and throws OutputExists.
  // With it, the rename replaces the directory entry: a symlink is replaced
  // itself, never the file it points at. Throws on any other failure.
  void write_output_file(const std::string& path, std::string_view contents, bool replace);

  // Creates a new file in the temporary directory with a unique name starting
  // with `prefix`, exclusively and readable only by the owner, writes
  // `contents`, and returns its path. Never opens an existing path, so a file or
  // symlink planted in the temporary directory is never written through.
  // The caller removes the file.
  std::string write_private_temp_file(const std::string& prefix, std::string_view contents);
  ```

- [ ] **Step 1: Write the failing tests.** Append to `tests/test_paths.cpp`; use a `make_temp_dir` directory and remove it at the end of each test.
  - **`WriteOutputFileCreatesAMissingFile`:** the contents round-trip.
  - **`WriteOutputFileRefusesAnExistingFile`:** a file reading `IRREPLACEABLE`. `write_output_file(p, "new", false)` throws `OutputExists`; the file still reads `IRREPLACEABLE`; no `.phig-tmp` sibling is left (list the directory).
  - **`WriteOutputFileRefusesADanglingSymlink`:** a symlink to a missing target. It throws `OutputExists`, and the symlink still exists (`fs::is_symlink`).
  - **`WriteOutputFileReplacesASymlinkNotItsTarget`:** a symlink to `victim` (`IRREPLACEABLE`), written with `replace = true`. Afterwards the path is a regular file reading `new`, and `victim` still reads `IRREPLACEABLE`.
  - **`WriteOutputFileReplacesAnExistingFileWhenAsked`.**
  - **`PrivateTempFilesAreUniqueAndOwnerOnly`:** two calls with the same prefix return different paths, both inside `fs::temp_directory_path()` and both starting with the prefix. Both hold their contents and both have permissions `fs::perms::owner_read | fs::perms::owner_write` only. Remove both.
  - **`PrivateTempFileNeverWritesThroughAPlantedSymlink`:**
    - Set `TMPDIR` to a scratch directory and restore it afterwards; `fs::temp_directory_path` honours `TMPDIR`.
    - Plant a symlink there named `<prefix>` plus every name you can predict, such as `<prefix>XXXXXX`. The real guarantee is `O_EXCL`; this test pins that the function creates new names, not that a guess fails.
    - Assert the returned path is not a symlink and the planted target is unchanged.

- [ ] **Step 2: See them fail** (compile errors).

- [ ] **Step 3: Implement them** in `core/paths.cpp`. Add `#include <string_view>`, and `#include <cerrno>` and `<system_error>` if missing.
  ```cpp
  void write_output_file(const std::string& path, std::string_view contents, bool replace) {
      namespace fs = std::filesystem;
      if (replace) {
          if (!atomic_write_file(path, contents.data(), contents.size())) {
              throw std::runtime_error("Cannot write " + path);
          }
          return;
      }
      const fs::path tmp = temp_sibling(fs::path(path));
      if (!atomic_write_file(tmp.string(), contents.data(), contents.size())) {
          throw std::runtime_error("Cannot write " + path);
      }
      try {
          move_file(tmp.string(), path);  // never replaces what is there
      } catch (const fs::filesystem_error& e) {
          std::error_code ec;
          fs::remove(tmp, ec);
          if (e.code() == std::errc::file_exists) throw OutputExists(path);
          throw;
      }
  }

  std::string write_private_temp_file(const std::string& prefix, std::string_view contents) {
      namespace fs = std::filesystem;
      std::string name = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
      std::vector<char> buf(name.begin(), name.end());
      buf.push_back('\0');
      // mkstemp creates with O_CREAT | O_EXCL and mode 0600: it never opens a
      // path that already exists, so a planted file or symlink is never used.
      int fd = ::mkstemp(buf.data());
      if (fd < 0) {
          throw std::system_error(errno, std::generic_category(),
                                  "Cannot create a temporary file in " +
                                      fs::temp_directory_path().string());
      }
      const std::string path(buf.data());
      size_t written = 0;
      while (written < contents.size()) {
          ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
          if (n < 0) {
              if (errno == EINTR) continue;
              int err = errno;
              ::close(fd);
              ::unlink(path.c_str());
              throw std::system_error(err, std::generic_category(), "Cannot write " + path);
          }
          written += static_cast<size_t>(n);
      }
      ::close(fd);
      return path;
  }
  ```
  Check `move_file`'s documented behaviour (`core/paths.h`) and confirm it throws a `filesystem_error` with `errc::file_exists` on an occupied destination. If it signals it differently, catch accordingly.

- [ ] **Step 4: Run** `./build/phig-tests --gtest_filter='PathsTest.*'` and the full suite. Expected: pass.

- [ ] **Step 5: Commit** `"Add core writers for output files and private temporary files"`.

---

### Task 5: `--output` never destroys a file (R69)

**Files:**
- `cli/main.cpp`:
  - `DuplicatesOptions` (~387) and `SearchOptions` (~751) gain `bool overwrite = false;`;
  - the four output sites (~587, ~956, ~985, ~1034);
  - the `--output` parsers for `duplicates` (~2014), `search` (~2353) and `fav list` (~1760).
- Create `tests/test_cli_output.cpp` and add it to `phig-tests`.
- `rules/boundary.toml` (R69), `rules/operations.toml`.

**Interfaces:**
- Consumes `write_output_file` and `OutputExists` from Task 4.

- [ ] **Step 1: Write the failing tests.** Create `tests/test_cli_output.cpp`:
  - **Fixture `CliOutputTest`:**
    - `phig_bin = phig_binary_path()`, skipping if it is empty; `dir = make_temp_dir("cli-output")`.
    - A catalog at `dir / "cat"` with two rows sharing one `sha256`, so `duplicates` has an exact group. Each row's file exists under `dir / "cat" / "photos"`.
    - One face on the first row, labelled `Ada`, so `search --person Ada` finds it. Build it as `tests/test_lockfile.cpp`'s `CliRefusesALockTakingWrite...` does: `insert_face`, `get_or_create_person`, `set_face_person`.
    - A `run(args, stdin_file = nullopt)` helper that calls `run_phig(phig_bin, dir, "--catalog '" + cat + "' " + args, std::nullopt, stdin_file)`. Task 6 adds the stdin parameter; in this task, call `run_phig` with three arguments.
  - **Tests,** with `const std::string kKeep = "IRREPLACEABLE";`:
    - **`OutputRefusesAnOccupiedPath`:** for each of `"search"`, `"duplicates"`, `"search --person Ada"` and `"fav list"`:
      - write `kKeep` to `dir / "out.txt"`;
      - run `<args> --output '<out>'`;
      - expect exit 1, output containing `already exists`, and the file still reading `kKeep`.
    - **`OutputOverwriteReplacesTheFile`:** for the same four, with `--overwrite`, expect exit 0. For `search` and `duplicates`, the file content now contains the first row's path. For `fav list` and `--person`, it differs from `kKeep`.
    - **`OutputCreatesAMissingFile`:** `search --output <new>` gives exit 0, and the file exists and contains a row path.
    - **`OutputOverwriteReplacesASymlinkNotItsTarget`:** `out.txt` is a symlink to `victim.txt` (`kKeep`). `search --output <out> --overwrite` gives exit 0, `out.txt` is now a regular file, and `victim.txt` still reads `kKeep`.
    - **`OutputRefusesADanglingSymlink`:** exit 1, and the symlink is still a symlink.
    - **`CountWithOutputIsAnError`:** `search --count --output <new>` gives exit 1, and the file was not created. The same for `fav list --count --output <new>`.
    - **`OverwriteWithoutOutputIsAnError`:** `search --overwrite` gives exit 1.
  - Add the file to `phig-tests` in `CMakeLists.txt`, after `tests/test_catalog.cpp`. Build and see the tests fail: today `--output` truncates and `--overwrite` is an unknown option.

- [ ] **Step 2: Implement it** in `cli/main.cpp`, in the anonymous namespace above `cmd_duplicates`:
  ```cpp
  // Where a command's results go: stdout, or a buffer written to --output in one
  // piece when the command finishes, through write_output_file, which refuses
  // an occupied path unless --overwrite was given.
  class ResultSink {
     public:
      ResultSink(const std::string& path, bool overwrite) : path_(path), overwrite_(overwrite) {}
      std::ostream& out() { return path_.empty() ? std::cout : buffer_; }
      // Writes the buffer to --output, if one was named. Returns the exit code.
      int finish() {
          if (path_.empty()) return 0;
          try {
              write_output_file(path_, buffer_.str(), overwrite_);
              return 0;
          } catch (const OutputExists&) {
              std::cerr << "Error: " << path_ << " already exists. Pass --overwrite to replace it.\n";
          } catch (const std::exception& e) {
              std::cerr << "Error: cannot write " << path_ << ": " << e.what() << "\n";
          }
          return 1;
      }

     private:
      std::string path_;
      bool overwrite_;
      std::ostringstream buffer_;
  };

  // Refuses --output flags that would do nothing or destroy a file, before any
  // work is done. write_output_file refuses an occupied path again at the end.
  int check_output_flags(const std::string& output, bool overwrite, bool count_only) {
      if (count_only && !output.empty()) {
          std::cerr << "Error: --count prints a number and writes no file; drop --output\n";
          return 1;
      }
      if (overwrite && output.empty()) {
          std::cerr << "Error: --overwrite needs --output\n";
          return 1;
      }
      std::error_code ec;
      if (!output.empty() && !overwrite &&
          fs::symlink_status(output, ec).type() != fs::file_type::not_found) {
          std::cerr << "Error: " << output << " already exists. Pass --overwrite to replace it.\n";
          return 1;
      }
      return 0;
  }
  ```
  - Add `#include <sstream>` if missing.
  - Call `check_output_flags` first thing in `cmd_search` with `opts.count_only`, and in `cmd_duplicates` with `false`, returning its code if non-zero.
  - At each of the four sites, replace the `ofstream` block and the `out` reference with:
    ```cpp
    ResultSink sink(opts.output, opts.overwrite);
    std::ostream& out = sink.out();
    ```
    Replace that branch's `return 0;` after writing with `return sink.finish();`.
  - Parse `--overwrite` (`opts.overwrite = true;`) in the `duplicates`, `search` and `fav list` parsers, next to `--output`. Add a usage line under each command's `--output` line: `"  --overwrite              Replace the --output file if it exists\n"`.

- [ ] **Step 3: Classify the flag.** In `rules/operations.toml`, add to `mutating_cli_flags`:
  ```toml
  # search, duplicates, fav list. Lets --output replace an existing file (R69).
  "--overwrite",
  ```

- [ ] **Step 4: Update R69.** In `rules/boundary.toml`:
  - `enforced_at = ["core/paths.cpp:write_output_file"]`;
  - `tests` lists the seven `CliOutputTest` tests and the five `PathsTest.WriteOutputFile*` tests;
  - `status = "holds"`.

  The rationale says results are written to a temporary sibling and renamed into place, so the path never holds a partial file. The rename refuses an occupied path, a symlink included, unless `--overwrite` is given, and with it replaces the link and never its target. All four open sites and `fav list` go through `ResultSink`. The test_gap says the face-search site has no subprocess test because it needs the models; it shares `ResultSink` with the tested sites. Keep `binds_surface` and `protects`. Run `rules.py --check --emit`.

- [ ] **Step 5: Run** the focused tests, the full suite and the single-process run, then the UNTOUCHED check.

- [ ] **Step 6: Commit** `"Refuse to replace an existing --output file without --overwrite (R69)"`.

---

### Task 6: Staging stdin never touches an existing path (R81)

**Files:** `cli/main.cpp` (`read_stdin_to_temp` at ~817); `tests/phig_binary.h`; `tests/test_cli_output.cpp`; `rules/boundary.toml` (R81).

**Interfaces:**
- Consumes `write_private_temp_file` from Task 4.
- `run_phig(binary, scratch, args, stdin_line = nullopt, stdin_file = nullopt)`. When `stdin_file` is set, the child's stdin is that file. When both are unset, stdin is `/dev/null`. Setting both is a test bug, so `assert` on it.

- [ ] **Step 1: Extend `run_phig`.** Add a fifth parameter, `const std::optional<std::filesystem::path>& stdin_file = std::nullopt`. When set, append `< '<file>'` in place of `< /dev/null`. Update its comment.

- [ ] **Step 2: Write the failing tests.** Append to `tests/test_cli_output.cpp`:
  ```cpp
  namespace {
  // TMPDIR pointed at a scratch directory for the scope, so a test can see what
  // the child creates in its temporary directory.
  struct ScopedTmpdir {
      std::optional<std::string> saved;
      explicit ScopedTmpdir(const fs::path& p) {
          if (const char* v = std::getenv("TMPDIR")) saved = v;
          fs::create_directories(p);
          setenv("TMPDIR", p.c_str(), 1);
      }
      ~ScopedTmpdir() {
          if (saved) setenv("TMPDIR", saved->c_str(), 1);
          else unsetenv("TMPDIR");
      }
  };
  }  // namespace

  TEST_F(CliOutputTest, StdinQueryNeverWritesThroughAPlantedPath) {
      const fs::path tmp = dir / "tmp";
      ScopedTmpdir scoped(tmp);
      const fs::path victim = dir / "victim.txt";
      std::ofstream(victim) << "IRREPLACEABLE USER DATA";
      fs::create_symlink(victim, tmp / "phig-stdin-image");

      PhigRun r = run("search --similar -", fs::path("tests/data/faces_small.jpg"));
      EXPECT_EQ(r.exit_code, 0) << r.output;
      std::ifstream in(victim);
      std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(content, "IRREPLACEABLE USER DATA");
      EXPECT_TRUE(fs::is_symlink(tmp / "phig-stdin-image"));
      size_t entries = 0;
      for (const auto& e : fs::directory_iterator(tmp)) {
          (void)e;
          entries++;
      }
      EXPECT_EQ(entries, 1u) << "the staged file was not removed";
  }

  TEST_F(CliOutputTest, EmptyStdinCreatesNothing) {
      const fs::path tmp = dir / "tmp";
      ScopedTmpdir scoped(tmp);
      PhigRun r = run("search --similar -");  // stdin is /dev/null
      EXPECT_EQ(r.exit_code, 1) << r.output;
      EXPECT_NE(r.output.find("No data received on stdin"), std::string::npos) << r.output;
      EXPECT_TRUE(fs::is_empty(tmp));
  }
  ```
  The `run` helper passes `stdin_file` through. Build and see the first test fail: today the symlink is followed, the victim is overwritten, and the link is removed.

- [ ] **Step 3: Implement it.** Replace `read_stdin_to_temp` with:
  ```cpp
  // Reads stdin and stages it in a private temporary file, returning its path.
  // Empty stdin is refused before anything is created. The caller removes the
  // file.
  std::string read_stdin_to_temp() {
      std::string bytes((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
      if (bytes.empty()) throw std::runtime_error("No data received on stdin");
      return write_private_temp_file("phig-stdin-", bytes);
  }
  ```
  If stdin is read in text mode on any platform this targets, note it. On Linux and macOS, `std::cin` passes bytes unchanged.

- [ ] **Step 4: Update R81.** In `rules/boundary.toml`:
  - `enforced_at = ["core/paths.cpp:write_private_temp_file"]`;
  - `tests` lists the two `CliOutputTest` stdin tests and the two `PathsTest.PrivateTemp*` tests;
  - `status = "holds"`.

  The rationale says stdin is read into memory first, empty stdin is refused before anything is created, and the file is created by `mkstemp`: a unique name, `O_EXCL`, mode 0600. No existing path or symlink is opened, and concurrent stdin searches get different files. Keep `binds_surface` and `protects`. Run `rules.py --check --emit`.

- [ ] **Step 5: Run** the focused tests, the full suite, the single-process run, and UNTOUCHED.

- [ ] **Step 6: Commit** `"Stage stdin in a private temporary file (R81)"`.

---

### Task 7: Docs, plan status, final verification

**Files:** root `CLAUDE.md`, `README.md`, `libphig/CLAUDE.md`, `plans/060-silent-loss-on-success.md`, `plans/000-INDEX.md`.

- [ ] **Step 1: Docs.**
  - In root `CLAUDE.md`'s command reference, add `[--overwrite]` after `[--output <path>]` on `search`, `duplicates` and `fav list`. Under the block, add one sentence: `--output` refuses an existing file unless `--overwrite` is given, and `--count` with `--output` is an error.
  - In `README.md`, wherever `--output` is shown, mention `--overwrite` the same way, and state that `cp` and `mv` exit 1 when any file or attachment was not carried.
  - In `libphig/CLAUDE.md`, if it describes `phig_organize_result`, add `attachment_errors` and `cancelled`.

- [ ] **Step 2: Plan 060.**
  - Set `status: in-progress` in its frontmatter and in its index row, and re-read both to confirm they agree.
  - Add a `## Progress` section after the decisions:
    - R60 is `partial`, as its own section records;
    - R27, R48, R69, R81 and R83 now hold, by this plan's commits; list them from `git log --oneline $(cat /tmp/phig-060-base.txt)..HEAD`;
    - left: R73, the layer move; R66, faces with no plan phase, which also stays on 053; and open question 7.
  - Update the index row's description only if it no longer says what the plan is.

- [ ] **Step 3: Final verification.**
  ```bash
  cmake --build build -j12 && ctest --test-dir build --parallel 6 2>&1 | tail -3
  ./build/phig-tests --gtest_output=xml:/tmp/phig-060-tests.xml >/dev/null; python3 scripts/rules.py --check --test-results /tmp/phig-060-tests.xml
  python3 scripts/rules.py --check --emit && git status --short rules/
  cmake --build build/with-gtk -j12 --target gtk-all && ctest --test-dir build/with-gtk -L gtk 2>&1 | tail -3
  git diff --name-only $(cat /tmp/phig-060-base.txt)..HEAD -- 'core/*' 'libphig/*' 'cli/*' 'tests/*' | grep -E '\.(cpp|h|c)$' | xargs clang-format --dry-run --Werror
  stat -c '%Y %n' ~/.local/state/phig/phig.log ~/.local/share/phig/phig.db.lock | diff - /tmp/phig-060-real-mtimes.txt && echo UNTOUCHED
  ```
  Expected:
  - all suites pass;
  - both rules checks print `OK`, with no diff under `rules/`;
  - clang-format is clean;
  - `UNTOUCHED`.

- [ ] **Step 4: Smoke test** in the scratch environment from Global Constraints:
  ```bash
  ./build/phig --catalog $S/cat scan tests/data >/dev/null
  echo IRREPLACEABLE > $S/out.txt
  ./build/phig --catalog $S/cat search --output $S/out.txt; echo "exit=$?"; cat $S/out.txt
  ./build/phig --catalog $S/cat search --output $S/out.txt --overwrite; echo "exit=$?"; head -2 $S/out.txt
  cat tests/data/faces_small.jpg | ./build/phig --catalog $S/cat search --similar - | tail -1
  ```
  Expected:
  - the first search gives `exit=1`, and the file still reads `IRREPLACEABLE`;
  - the second gives `exit=0`, and the file holds results;
  - the similar search prints a result count.

- [ ] **Step 5: Commit** `"Document --overwrite and mark plan 060's remaining fixes in progress"`.
