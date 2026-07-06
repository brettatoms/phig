---
name: Activity Log & Rollback
status: completed
created: 2026-07-06
description: >
  Record file-changing operations (mv, cp, purge) as an append-only log in the
  main database so changes are visible and reversible. This slice implements the
  schema, the transactional write path, a config toggle, and entry-count
  retention. The `phig log` viewer and `phig undo` command are a deferred slice
  the schema is designed to support.
---

# Activity Log & Rollback

## Goal

Give phig a durable, human-readable record of the operations that change files
or database state — starting with `mv`, `cp`, and `purge` — captured well enough
that a later `phig undo` can reverse a whole operation. Two goals weighted
equally: a real audit trail and reliable rollback.

This plan (020) is the first slice: the log's data model, write path, config,
and retention. Viewing (`phig log`) and undo (`phig undo`) are deferred to a
follow-up plan, but the schema here is designed so they need no schema change.

## Scope

In scope for the log: `phig mv`, `phig cp`, `phig purge`. These are the
data-moving / destructive operations. `scan` (high-volume, additive) and
`thumbs`/tags/gallery (lower-stakes cache/DB edits) are intentionally excluded.

Out of scope for this slice (deferred): the `phig log` viewer command, the
`phig undo` command, and any manual `log prune` command. The schema supports
them; this slice only writes and auto-prunes the log.

## Data model

Two new tables in `phig.db` via a schema migration (v5). The
operation → entries split is what lets whole-operation undo now and per-file
undo later without a schema change.

```sql
CREATE TABLE operations (
  id          INTEGER PRIMARY KEY,
  ts          TEXT NOT NULL,          -- ISO-8601 UTC, when the op started
  command     TEXT NOT NULL,          -- 'mv' | 'cp' | 'purge' | 'undo'
  argv        TEXT,                   -- full invocation, for the audit trail
  status      TEXT NOT NULL,          -- 'completed' | 'interrupted'
  undoes_id   INTEGER REFERENCES operations(id),  -- set only when command='undo'
  entry_count INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE operation_entries (
  id           INTEGER PRIMARY KEY,
  op_id        INTEGER NOT NULL REFERENCES operations(id) ON DELETE CASCADE,
  kind         TEXT NOT NULL,          -- 'move' | 'copy' | 'purge_row'
  src          TEXT,                   -- original path (move/copy); path (purge)
  dst          TEXT,                   -- new path (move/copy); NULL for purge
  restore_json TEXT                    -- purge_row: deleted row's columns, to re-insert
);
CREATE INDEX idx_op_entries ON operation_entries(op_id);
```

Notes:

- `kind` is per entry (not per operation) so a future per-file undo can invert
  each row independently.
- **Undo is itself a logged operation** (Option A). Undoing op #5 writes a new
  operation with `command='undo'`, `undoes_id=5`, and entries that are the
  inverses. The original op is never mutated. This makes the log append-only
  (the audit-trail half) and gives redo for free — redo is "undo the undo,"
  reusing the same write path. "Is op N still in effect?" = "does any later op
  have `undoes_id = N`?"
- For **purge**, the only way to undo is to re-insert the deleted row, so the
  row's columns are snapshotted into `restore_json` at delete time. mv/cp
  entries don't need it — their inverse is derivable from `src`/`dst`.

## Write path & atomicity

The log must not reintroduce the non-atomic move+update bug that the data-safety
audit closed (finding #7). Because the log lives in `phig.db`, each entry write
joins the **same SQLite transaction** as the DB change it records.

Each `cmd_organize` / `cmd_purge` invocation is one operation: open the
`operations` row up front, append one `operation_entries` row per file as it
succeeds, close the operation at the end.

**mv (move):** unchanged disk-first ordering, with the log entry folded into the
existing DB transaction:

```
move_file(src, dst)                    // disk — already SIGINT-safe atomic rename
BEGIN
  update_path(src, dst)                // existing
  insert entry(op, 'move', src, dst)   // new — same txn
COMMIT                                 // both or neither
  └─ on throw: move_file(dst, src)     // existing compensator, unchanged
```

No new failure window: the log row is written in the transaction that already
had to succeed.

**cp (copy):** `atomic_copy_file` on disk, then `insert_copy` + log entry in one
transaction. On a throw after the copy, a compensator undoes the disk change so
no orphan file is left and disk/DB stay in sync. (Today an `insert_copy` failure
after the copy leaves an orphan file; the second DB write this plan adds widens
that window, so closing it is worthwhile — matching the audit approach.)

The compensator must stay non-destructive on the `--on-conflict overwrite` path,
where cp copies onto a pre-existing file. Deleting `dst` there would destroy a
file cp didn't originate. So cp uses **backup-then-copy on overwrite** — the only
option that keeps an overwrite fully reversible:

```
if dst existed:  rename(dst, dst.bak)   // O(1) same-dir metadata op — no byte copy
atomic_copy_file(src, dst)              // the one real copy
BEGIN; insert_copy(src, dst); insert entry(op, 'copy', src, dst); COMMIT
  success →  unlink(dst.bak)            // if a backup was made
  failure →  unlink(dst); if backup: rename(dst.bak, dst)   // restore prior file
             throw                       // still report the original DB failure
```

Cost is one byte-for-byte copy (unchanged from today) plus, only on the overwrite
path, two renames and an unlink — all metadata ops. The non-overwrite common case
(dest absent) does no extra work. The backup is in the same directory as `dst`,
so the rename is always same-filesystem and cheap; peak disk use is briefly ~2×
that one file until the op commits.

**purge:** the per-row delete becomes, in one transaction: snapshot the row to
`restore_json`, insert the `purge_row` entry, delete the row. Atomic per row.

**Interrupted operations:** the SIGINT handler already stops at a file boundary.
The operation row is marked `status='interrupted'` instead of `'completed'`.
Every already-committed entry is valid and undoable — a partial op is just a
shorter op.

**Dry-run writes nothing.** `--dry-run` changes neither disk nor DB, so it logs
nothing. The log records only what actually happened.

## Config

Follows the existing `Config` pattern (every field `optional`; unset = built-in
default; CLI would override config, though these keys have no CLI flag in this
slice).

```ini
[log]
enabled     = true      # default; false disables logging entirely
max_entries = 100000    # default; 0 = unlimited
```

- `std::optional<bool> log_enabled;` — INI `[log] enabled`, `parse_bool`.
  Default **enabled**. Opt out with `enabled = false`.
- `std::optional<int> log_max_entries;` — INI `[log] max_entries`, `parse_int`,
  must be `>= 0`. Default **100000**. `0` = unlimited.

When logging is disabled, `cmd_organize`/`cmd_purge` skip opening the operation
row and skip every entry insert; behavior is otherwise byte-for-byte unchanged.
No empty operation rows are written. The tables still exist (migration runs) but
stay empty, so the cost of disabling is negligible.

## Retention (size cap)

Bounded by **entry count** — the closest proxy to DB growth, since entries are
the rows that multiply.

After a logged operation commits, on the same connection: count total entries;
if over `max_entries`, delete whole operations oldest-first (`id` ascending)
until back under the cap. Entries cascade-delete via the FK.

- Whole operations only — never a partial operation — so history stays coherent.
- A dangling `undoes_id` (its target pruned) is tolerated on read: it just means
  "the undone original is too old to show." It never blocks anything.
- Pruning is skipped when logging is disabled or `max_entries = 0`.
- Piggybacks on the write; no background job, no required command.

## Testing

Per project rule, every data-safety change ships with real-behavior tests.

- **Schema/migration:** v4→v5 creates both tables; fresh DB gets them.
- **Write path:** mv logs `move` entries with correct src/dst; cp logs `copy`
  entries; purge logs `purge_row` entries whose `restore_json` round-trips the
  row. Disabled config writes nothing. `--dry-run` writes nothing.
- **Atomicity:** DB failure after the disk move → entry not committed and file
  moved back (extends existing compensator test). For cp: DB failure after a
  non-overwrite copy removes the orphan; DB failure after an overwrite copy
  restores the original file (both non-destructive and in-sync).
- **Interrupted op:** SIGINT mid-batch → operation `interrupted`, committed
  entries intact.
- **Retention:** with a low `max_entries`, an op exceeding it prunes oldest
  operations to fit; an `undoes_id` pointing at a pruned op doesn't error.

Undo/log-viewing execution tests belong to the deferred slice; the schema and
entries they consume are all tested here.

## Checklist

- [ ] v5 migration: `operations` + `operation_entries` tables + index (migration
      list and fresh-DB path)
- [ ] `Config` fields `log_enabled`, `log_max_entries` + `[log]` parsing in
      `config.cpp`
- [ ] Database methods to open an operation, append an entry, close an
      operation, and prune to the entry cap — all transaction-aware
- [ ] mv: fold log entry into the existing `update_path` transaction
- [ ] cp: fold log entry into `insert_copy`; add backup-then-copy compensator
      (rename-aside on overwrite, restore on failure, unlink backup on success)
- [ ] purge: snapshot row to `restore_json` + log entry + delete, per row, in one
      transaction
- [ ] Mark operation `interrupted` on SIGINT; `completed` otherwise
- [ ] Auto-prune to `max_entries` after each logged op
- [ ] Tests: migration, write path (mv/cp/purge), disabled + dry-run no-write,
      atomicity compensators, interrupted, retention

---

# Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an append-only activity log (operations + entries) to `phig.db` that records `mv`/`cp`/`purge`, written transactionally with the DB changes it describes, with a config toggle and entry-count retention.

**Architecture:** A v5 schema migration adds `operations` and `operation_entries`. New `Database` methods (`log_begin_operation`, `log_add_entry`, `log_finish_operation`, `log_prune`) form the testable core, TDD'd in the GTest harness. `cmd_organize`/`cmd_purge` in `main.cpp` are rewired so each log write joins the existing DB transaction for its file change — preserving the audited disk-first/DB-atomic ordering. cp gains a backup-then-copy compensator on the overwrite path. Two `Config` fields (`log_enabled`, `log_max_entries`) gate logging and retention.

**Tech Stack:** C++20, SQLite (sqlite3 C API), CMake, GoogleTest. Vendored inih for config.

## Global Constraints

- **Language/standard:** C++20. Match existing `src/` style (Google/LLVM clang-format, 4-space, 100-col).
- **Build/test:** reconfigure/build via the devenv env — `direnv exec . cmake --build build` then `./build/phig-tests`. A bare shell has a glibc/include mismatch; wipe `build/` if the CMake cache goes stale.
- **Lint gate:** first-party code must pass `-Wall -Wextra` (CI uses `-DPHIG_WERROR=ON`) and clang-format `--dry-run --Werror`.
- **DB write discipline:** every `sqlite3_step` on a DML statement must be checked (`SQLITE_DONE`) and the statement finalized on all paths — use the existing `step_write_or_throw` helper (`database.cpp:51`). A failed write must throw, never be reported as success (audit #12).
- **Atomicity:** disk change first, then DB change; the log entry commits in the same transaction as the DB change. On DB failure after a disk change, compensate on disk so disk and DB never disagree (audit #7).
- **No new deps.** Timestamps via existing facilities; JSON snapshotting for purge uses hand-built strings consistent with the codebase (no new JSON lib).

## File Structure

- `src/database.cpp` / `src/database.h` — migration v5, the four `log_*` methods, a `LogEntry`/`OperationEntry` struct for round-tripping, and a `purge`-with-snapshot path. Most new code lives here; it is the unit-testable core.
- `src/config.cpp` / `src/config.h` — two new `Config` fields + `[log]` parsing.
- `src/main.cpp` — thread `log_enabled`/`log_max_entries` into `OrganizeOptions`/`PurgeOptions`; rewire `cmd_organize` (mv + cp) and `cmd_purge` to open/append/close an operation and prune; cp overwrite backup compensator; capture `argv`.
- `tests/test_database.cpp` — schema/migration, `log_*` round-trip, retention, purge snapshot round-trip.
- `tests/test_config.cpp` — `[log]` parsing and defaults.
- `CMakeLists.txt` — no change (files already registered).

---

### Task 1: v5 schema migration

**Files:**
- Modify: `src/database.cpp:20` (bump `CURRENT_SCHEMA_VERSION`), `src/database.cpp:26-44` (add migration), `src/database.cpp:196-262` (fresh-DB path)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Produces: two tables usable by later tasks — `operations(id, ts, command, argv, status, undoes_id, entry_count)` and `operation_entries(id, op_id, kind, src, dst, restore_json)`, plus `idx_op_entries`.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_database.cpp`:

```cpp
TEST_F(DatabaseTest, V5TablesExist) {
    auto has_table = [&](const char* name) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db->db_ptr(),
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    };
    EXPECT_TRUE(has_table("operations"));
    EXPECT_TRUE(has_table("operation_entries"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.V5TablesExist'`
Expected: FAIL — `operations` table does not exist.

- [ ] **Step 3: Add the migration and fresh-DB creation**

In `src/database.cpp`, change line 20:

```cpp
static const int CURRENT_SCHEMA_VERSION = 5;
```

Append to the `migrations` vector (after the v4 entry, before the closing `};` at line 44):

```cpp
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
```

In the fresh-DB branch, after the `vec_faces` virtual table block (before `set_schema_version(CURRENT_SCHEMA_VERSION);` at line 264), add the same two `CREATE TABLE IF NOT EXISTS` statements and index so a brand-new DB gets them directly:

```cpp
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.V5TablesExist'`
Expected: PASS.

- [ ] **Step 5: Verify migration from a v4 DB**

Add a second test that opens a DB, forces it to v4 by writing the schema version, closes, reopens, and asserts the tables now exist:

```cpp
TEST_F(DatabaseTest, MigratesV4ToV5) {
    // Simulate an older DB by stamping version 4 and dropping v5 tables.
    db->db_ptr();  // ensure open
    sqlite3_exec(db->db_ptr(), "DROP TABLE IF EXISTS operation_entries;"
                               "DROP TABLE IF EXISTS operations;"
                               "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','4');",
                 nullptr, nullptr, nullptr);
    db.reset();                                   // close
    db = std::make_unique<Database>(db_path);     // reopen
    db->init_schema();                            // should run v5 migration
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db->db_ptr(),
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='operations'", -1, &stmt, nullptr);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    EXPECT_TRUE(found);
}
```

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.Migrates*'`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/database.cpp tests/test_database.cpp
git commit -m "Add v5 schema: activity-log operations + entries tables"
```

---

### Task 2: Database log API — begin / add / finish

**Files:**
- Modify: `src/database.h` (declare methods + structs), `src/database.cpp` (implement, near `insert_copy`)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: v5 tables from Task 1; `step_write_or_throw` (`database.cpp:51`).
- Produces, used by Tasks 4–6:

```cpp
// database.h — public section
struct OperationEntry {
    std::string kind;          // "move" | "copy" | "purge_row"
    std::string src;
    std::string dst;           // "" when N/A (purge)
    std::string restore_json;  // "" unless kind == "purge_row"
};

// Open an operation row; returns its id. `argv` is the full invocation.
// `ts` is an ISO-8601 UTC timestamp supplied by the caller.
int64_t log_begin_operation(const std::string& command, const std::string& argv,
                            const std::string& ts);
// Append one entry to an already-open operation. Bumps operations.entry_count.
// Both writes happen in the CURRENT transaction (caller owns BEGIN/COMMIT).
void log_add_entry(int64_t op_id, const OperationEntry& entry);
// Set the operation's terminal status ("completed" | "interrupted").
void log_finish_operation(int64_t op_id, const std::string& status);
```

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(DatabaseTest, LogOperationRoundTrip) {
    int64_t op = db->log_begin_operation("mv", "phig mv /dest", "2026-07-06T00:00:00Z");
    EXPECT_GT(op, 0);
    db->log_add_entry(op, {"move", "/a.jpg", "/dest/a.jpg", ""});
    db->log_add_entry(op, {"move", "/b.jpg", "/dest/b.jpg", ""});
    db->log_finish_operation(op, "completed");

    // entry_count reflects the two entries
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db->db_ptr(),
        "SELECT command, status, entry_count FROM operations WHERE id=?", -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, op);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)), "mv");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(s, 1)), "completed");
    EXPECT_EQ(sqlite3_column_int(s, 2), 2);
    sqlite3_finalize(s);

    // two entry rows with the right kind/src/dst
    sqlite3_prepare_v2(db->db_ptr(),
        "SELECT COUNT(*) FROM operation_entries WHERE op_id=? AND kind='move'", -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, op);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(s, 0), 2);
    sqlite3_finalize(s);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.LogOperationRoundTrip'`
Expected: FAIL to compile — `log_begin_operation` not declared.

- [ ] **Step 3: Declare in `database.h`**

Add the `OperationEntry` struct and the three method declarations (shown in Interfaces above) to the public section, e.g. right after the `insert_copy` declaration at `database.h:26`.

- [ ] **Step 4: Implement in `database.cpp`**

Add after `insert_copy` (around `database.cpp:372`):

```cpp
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
```

Note: `status` starts `'in_progress'` and is set to `'completed'`/`'interrupted'` by `log_finish_operation`; a row left `'in_progress'` marks a crash between begin and finish — harmless for this slice, meaningful for the deferred viewer.

- [ ] **Step 5: Run test to verify it passes**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.LogOperationRoundTrip'`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/database.h src/database.cpp tests/test_database.cpp
git commit -m "Add Database log API: begin/add/finish operation"
```

---

### Task 3: Retention — prune to entry cap

**Files:**
- Modify: `src/database.h`, `src/database.cpp`
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: `log_*` from Task 2.
- Produces, used by Tasks 4/6:

```cpp
// Delete whole operations oldest-first (by id) until the total number of
// operation_entries rows is <= max_entries. No-op if max_entries <= 0 or
// already under the cap. Runs in its own transaction.
void log_prune(int max_entries);
```

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(DatabaseTest, LogPruneDropsOldestWholeOperations) {
    auto make_op = [&](const char* tag, int n) {
        int64_t op = db->log_begin_operation("mv", tag, "2026-07-06T00:00:00Z");
        for (int i = 0; i < n; i++)
            db->log_add_entry(op, {"move", "/s", "/d", ""});
        db->log_finish_operation(op, "completed");
        return op;
    };
    int64_t op1 = make_op("op1", 3);
    int64_t op2 = make_op("op2", 3);  // total 6 entries

    db->log_prune(4);  // must drop whole op1 (3) -> 3 entries left, under 4

    auto count = [&](const char* sql, int64_t bind) {
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db->db_ptr(), sql, -1, &s, nullptr);
        if (bind) sqlite3_bind_int64(s, 1, bind);
        sqlite3_step(s);
        int c = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return c;
    };
    EXPECT_EQ(count("SELECT COUNT(*) FROM operation_entries", 0), 3);
    EXPECT_EQ(count("SELECT COUNT(*) FROM operations WHERE id=?", op1), 0);  // op1 gone
    EXPECT_EQ(count("SELECT COUNT(*) FROM operations WHERE id=?", op2), 1);  // op2 kept
}

TEST_F(DatabaseTest, LogPruneUnlimitedIsNoOp) {
    int64_t op = db->log_begin_operation("mv", "x", "2026-07-06T00:00:00Z");
    db->log_add_entry(op, {"move", "/s", "/d", ""});
    db->log_finish_operation(op, "completed");
    db->log_prune(0);  // 0 = unlimited
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db->db_ptr(), "SELECT COUNT(*) FROM operations", -1, &s, nullptr);
    sqlite3_step(s);
    EXPECT_EQ(sqlite3_column_int(s, 0), 1);
    sqlite3_finalize(s);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.LogPrune*'`
Expected: FAIL to compile — `log_prune` not declared.

- [ ] **Step 3: Declare + implement**

Declare `void log_prune(int max_entries);` in `database.h`. Implement in `database.cpp`:

```cpp
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

    // Drop whole operations oldest-first until under the cap. entry_count on each
    // operation lets us decide without re-counting entries each loop.
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

        // entries cascade via FK ON DELETE CASCADE (foreign_keys is ON per-conn).
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
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.LogPrune*'`
Expected: PASS (both).

- [ ] **Step 5: Commit**

```bash
git add src/database.h src/database.cpp tests/test_database.cpp
git commit -m "Add log retention: prune oldest operations to entry cap"
```

---

### Task 4: Config — `[log]` enabled + max_entries

**Files:**
- Modify: `src/config.h` (fields), `src/config.cpp` (parse)
- Test: `tests/test_config.cpp`

**Interfaces:**
- Produces: `Config::log_enabled` (`std::optional<bool>`), `Config::log_max_entries` (`std::optional<int>`), consumed by Task 5's wiring.

- [ ] **Step 1: Write the failing test**

The `ConfigTest` fixture (`tests/test_config.cpp:37`) exposes `write_ini(content)`, which writes the INI and returns its path. Add:

```cpp
TEST_F(ConfigTest, ParsesLogSection) {
    std::string p = write_ini("[log]\nenabled = false\nmax_entries = 50\n");
    Config cfg = load_config(p);
    ASSERT_TRUE(cfg.log_enabled.has_value());
    EXPECT_FALSE(*cfg.log_enabled);
    ASSERT_TRUE(cfg.log_max_entries.has_value());
    EXPECT_EQ(*cfg.log_max_entries, 50);
}

TEST_F(ConfigTest, LogDefaultsUnset) {
    std::string p = write_ini("[general]\nparallel = 4\n");
    Config cfg = load_config(p);
    EXPECT_FALSE(cfg.log_enabled.has_value());
    EXPECT_FALSE(cfg.log_max_entries.has_value());
}

TEST_F(ConfigTest, RejectsNegativeMaxEntries) {
    std::string p = write_ini("[log]\nmax_entries = -1\n");
    EXPECT_THROW(load_config(p), std::runtime_error);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='ConfigTest.*Log*'`
Expected: FAIL to compile — `log_enabled` not a member.

- [ ] **Step 3: Add fields + parsing**

In `src/config.h`, add to `struct Config` (after `faces_threshold`):

```cpp
    std::optional<bool> log_enabled;      // [log] enabled  — default true (unset)
    std::optional<int> log_max_entries;   // [log] max_entries — default 100000 (unset); 0 = unlimited
```

In `src/config.cpp`, add branches to the dispatch chain (before the final `else` at line 131):

```cpp
            } else if (sec == "log" && k == "enabled") {
                cfg.log_enabled = parse_bool(path, section, key, val);
            } else if (sec == "log" && k == "max_entries") {
                int n = parse_int(path, section, key, val);
                if (n < 0) fail(path, section, key, "must be >= 0, got " + val);
                cfg.log_max_entries = n;
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='ConfigTest.*Log*'`
Expected: PASS (all three).

- [ ] **Step 5: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "Add [log] config: enabled + max_entries"
```

---

### Task 5: Wire logging into `cmd_organize` (mv + cp)

**Files:**
- Modify: `src/main.cpp` — `OrganizeOptions` (910-919), `cmd_organize` (1028-1179), the `cp`/`mv` dispatch (2684-2718), and add an `iso_utc_now()` helper + an `argv` join helper.
- Test: exercised via `tests/test_database.cpp` log round-trip (Task 2) + a manual end-to-end check here; command functions aren't in the GTest harness.

**Interfaces:**
- Consumes: `log_begin_operation`/`log_add_entry`/`log_finish_operation`/`log_prune` (Tasks 2–3); `Config::log_enabled`/`log_max_entries` (Task 4); existing `move_file`/`atomic_copy_file` (`paths.cpp`), `db.update_path`/`db.insert_copy`.
- Produces: nothing consumed by later tasks except the shared `iso_utc_now()`/`join_argv()` helpers (Task 6 reuses them).

- [ ] **Step 1: Add option fields + helpers, thread config through dispatch**

In `OrganizeOptions` (`main.cpp:918`) add:

```cpp
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
```

Add helpers near the top of `main.cpp` (anonymous namespace, by the other free functions). Ensure `#include <ctime>` is present at the top of `main.cpp` (add it if missing — `gmtime_r`/`strftime`/`std::time` need it):

```cpp
static std::string iso_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static std::string join_argv(int argc, char* argv[]) {
    std::string s;
    for (int i = 0; i < argc; i++) {
        if (i) s += ' ';
        s += argv[i];
    }
    return s;
}
```

In the `cp`/`mv` dispatch block (`main.cpp:2684`), after seeding `opts.format` from config, add:

```cpp
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);
```

- [ ] **Step 2: Open the operation at the top of the execute phase**

In `cmd_organize`, right after the SIGINT handler is installed (`main.cpp:1127`) and before the loop, add:

```cpp
    // Open a log operation for this run unless logging is disabled. Entries are
    // appended per file inside the same transaction as each DB change.
    int64_t op_id = 0;
    const bool logging = opts.log_enabled;
    if (logging) {
        op_id = db.log_begin_operation(opts.is_move ? "mv" : "cp", opts.argv_str, iso_utc_now());
    }
```

- [ ] **Step 3: Fold the log entry into the mv transaction**

The current mv path calls `db.update_path(...)` (which owns its own BEGIN/COMMIT). To put the log entry in the *same* transaction, replace the `db.update_path(a.source, a.dest.string());` call (`main.cpp:1141`) with a transaction that does both:

```cpp
                db.begin_transaction();
                try {
                    db.update_path_in_txn(a.source, a.dest.string());
                    if (logging) db.log_add_entry(op_id, {"move", a.source, a.dest.string(), ""});
                    db.commit_transaction();
                } catch (...) {
                    db.rollback_transaction();
                    throw;  // outer catch runs the move-back compensator
                }
```

This requires splitting `update_path` into a transaction-free core. In `database.cpp`, extract the body of `update_path` (the DELETE + UPDATE between the `BEGIN`/`COMMIT` at lines 436/468) into a new private-or-public method `update_path_in_txn(old, new)` that assumes a transaction is already open, and make the existing `update_path` wrap it:

```cpp
void Database::update_path(const std::string& old_path, const std::string& new_path) {
    if (old_path == new_path) return;
    exec("BEGIN TRANSACTION");
    try {
        update_path_in_txn(old_path, new_path);
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}
```

`update_path_in_txn` keeps the no-op `old==new` guard at its top and the DELETE-then-UPDATE logic, throwing on failure (no BEGIN/COMMIT of its own). Declare it in `database.h`. The existing move-back compensator (`main.cpp:1143-1152`) stays exactly as-is.

- [ ] **Step 4: Fold the log entry into the cp transaction with backup-then-copy**

Replace the cp branch (`main.cpp:1153-1158`) with the backup-then-copy compensator:

```cpp
            } else {
                // Copy to disk atomically. On the overwrite path we may land on a
                // pre-existing file; back it up first so a DB failure can restore
                // it (rename is a same-dir metadata op — no byte copy).
                namespace fs = std::filesystem;
                const std::string& dst = a.dest.string();
                bool had_existing = fs::exists(dst);
                std::string backup;
                if (had_existing) {
                    backup = dst + ".phig-bak";
                    fs::rename(dst, backup);
                }
                try {
                    atomic_copy_file(a.source, dst);
                    db.begin_transaction();
                    try {
                        db.insert_copy(a.source, dst);
                        if (logging) db.log_add_entry(op_id, {"copy", a.source, dst, ""});
                        db.commit_transaction();
                    } catch (...) {
                        db.rollback_transaction();
                        throw;
                    }
                } catch (...) {
                    // Undo the disk change so no orphan is left. Restore the prior
                    // file if we backed one up; otherwise remove the copy we made.
                    std::error_code ec;
                    fs::remove(dst, ec);
                    if (had_existing) fs::rename(backup, dst, ec);
                    throw;
                }
                if (had_existing) {
                    std::error_code ec;
                    fs::remove(backup, ec);  // success — drop the backup
                }
            }
```

Note: `insert_copy` currently owns no transaction, so wrapping it in `begin/commit` is safe. `.phig-bak` sits in the same directory as `dst`, so the rename is same-filesystem and O(1).

- [ ] **Step 5: Close the operation + prune after the loop**

After the loop and `std::signal(SIGINT, prev_handler);` (`main.cpp:1169`), add:

```cpp
    if (logging) {
        db.log_finish_operation(op_id, g_interrupted ? "interrupted" : "completed");
        db.log_prune(opts.log_max_entries);
    }
```

- [ ] **Step 6: Build clean and run the full suite**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: all existing tests still PASS (the `update_path` refactor is behavior-preserving; the DatabaseTest log tests still pass).

- [ ] **Step 7: Manual end-to-end verification**

```bash
cd "$(mktemp -d)" && mkdir src && printf x > src/a.jpg
DB=./t.db
./"$OLDPWD"/build/phig scan src --db "$DB" >/dev/null
./"$OLDPWD"/build/phig cp dest --db "$DB" >/dev/null
# expect one 'cp' operation with one 'copy' entry
sqlite3 "$DB" "SELECT command,status,entry_count FROM operations; SELECT kind,src,dst FROM operation_entries;"
```
Expected: a `cp|completed|1` row and a `copy|.../src/a.jpg|.../dest/...` entry. Then verify a disabled run logs nothing:
```bash
printf "[log]\nenabled = false\n" > cfg.ini
./"$OLDPWD"/build/phig --config cfg.ini mv dest2 --db "$DB" >/dev/null
sqlite3 "$DB" "SELECT COUNT(*) FROM operations WHERE command='mv';"   # expect 0
```

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp src/database.h src/database.cpp
git commit -m "Log mv/cp operations transactionally; cp overwrite backup compensator"
```

---

### Task 6: Wire logging into `cmd_purge` (with row snapshot)

**Files:**
- Modify: `src/database.h`/`src/database.cpp` (a snapshot-aware delete), `src/main.cpp` — `PurgeOptions` (1183-1189), `cmd_purge` (1191-1251), purge dispatch (2648-2676)
- Test: `tests/test_database.cpp` (snapshot round-trip)

**Interfaces:**
- Consumes: `log_*` (Tasks 2–3); `delete_paths`/`get_all_images` (existing).
- Produces:

```cpp
// Delete the given paths AND, if op_id > 0, log a 'purge_row' entry per deleted
// path whose restore_json snapshots enough columns to re-insert the row later.
// All deletes + log writes run in ONE transaction. Returns rows deleted.
int delete_paths_logged(const std::vector<std::string>& paths, int64_t op_id);
```

- [ ] **Step 1: Write the failing test (snapshot round-trip)**

```cpp
TEST_F(DatabaseTest, PurgeLogsRestorableSnapshot) {
    db->insert(make_image("/x/a.jpg", "shaA"));
    int64_t op = db->log_begin_operation("purge", "phig purge --match *", "2026-07-06T00:00:00Z");
    int n = db->delete_paths_logged({"/x/a.jpg"}, op);
    db->log_finish_operation(op, "completed");
    EXPECT_EQ(n, 1);
    EXPECT_EQ(db->count(), 0);  // row gone

    // the entry carries a restore_json mentioning the original path + sha
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db->db_ptr(),
        "SELECT kind, src, restore_json FROM operation_entries WHERE op_id=?", -1, &s, nullptr);
    sqlite3_bind_int64(s, 1, op);
    ASSERT_EQ(sqlite3_step(s), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)), "purge_row");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(s, 1)), "/x/a.jpg");
    std::string restore = reinterpret_cast<const char*>(sqlite3_column_text(s, 2));
    EXPECT_NE(restore.find("shaA"), std::string::npos);
    EXPECT_NE(restore.find("/x/a.jpg"), std::string::npos);
    sqlite3_finalize(s);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.PurgeLogsRestorableSnapshot'`
Expected: FAIL to compile — `delete_paths_logged` not declared.

- [ ] **Step 3: Implement `delete_paths_logged`**

Declare in `database.h`. Implement in `database.cpp` modeled on `delete_paths` (`database.cpp:517`), but per row: read the row's columns, build a `restore_json` string, log a `purge_row` entry, then delete — all in one transaction. Build the JSON with a small local escaper (no new dep):

```cpp
namespace {
std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c;
        }
    }
    return o;
}
}  // namespace

int Database::delete_paths_logged(const std::vector<std::string>& paths, int64_t op_id) {
    if (paths.empty()) return 0;
    exec("BEGIN TRANSACTION");
    auto fail = [&](const std::string& what) {
        exec("ROLLBACK");
        throw std::runtime_error(what + ": " + std::string(sqlite3_errmsg(db_)));
    };

    int deleted = 0;
    for (const auto& path : paths) {
        // Snapshot the row (the columns insert/insert_copy carry) for restore.
        std::string restore;
        {
            const char* sel =
                "SELECT path, filename, extension, file_size, created_at, modified_at, "
                "sha256, phash, width, height, metadata, media_type, duration_ms, gallery, "
                "blur_score FROM images WHERE path = ?";
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(db_, sel, -1, &s, nullptr) != SQLITE_OK)
                fail("Failed to prepare snapshot select");
            sqlite3_bind_text(s, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) != SQLITE_ROW) {
                sqlite3_finalize(s);
                continue;  // not present — nothing to delete/log
            }
            // Build a JSON object of column name -> text value (nulls -> null).
            static const char* cols[] = {"path", "filename", "extension", "file_size",
                "created_at", "modified_at", "sha256", "phash", "width", "height",
                "metadata", "media_type", "duration_ms", "gallery", "blur_score"};
            restore = "{";
            for (int i = 0; i < 15; i++) {
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
            restore += "}";
            sqlite3_finalize(s);
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
```

Note: `log_add_entry` runs inside this already-open transaction, so the snapshot, the entry, and the delete commit together.

- [ ] **Step 4: Run test to verify it passes**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='DatabaseTest.PurgeLogsRestorableSnapshot'`
Expected: PASS.

- [ ] **Step 5: Wire into `cmd_purge`**

In `PurgeOptions` (`main.cpp:1189`) add `bool log_enabled = true; int log_max_entries = 100000; std::string argv_str;`. In the purge dispatch (`main.cpp:2649`), set them from `cfg` and `join_argv(argc, argv)` (same three lines as Task 5). In `cmd_purge`, replace `db.delete_paths(matched);` (`main.cpp:1247`) with:

```cpp
    int64_t op_id = 0;
    const bool logging = opts.log_enabled;
    if (logging) op_id = db.log_begin_operation("purge", opts.argv_str, iso_utc_now());
    int removed = db.delete_paths_logged(matched, op_id);
    if (logging) {
        db.log_finish_operation(op_id, "completed");
        db.log_prune(opts.log_max_entries);
    }
    std::cout << "Removed " << removed << " entries from database.\n"
              << "Total DB: " << db.count() << " images\n";
    return 0;
```

(Purge has no file-boundary interruption to track, so it always finishes `completed`.)

- [ ] **Step 6: Build clean, run full suite, manual check**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: all PASS.

Manual:
```bash
cd "$(mktemp -d)" && mkdir src && printf x > src/a.jpg
DB=./t.db
./"$OLDPWD"/build/phig scan src --db "$DB" >/dev/null
./"$OLDPWD"/build/phig purge --match '*' --yes --db "$DB" >/dev/null
sqlite3 "$DB" "SELECT command,entry_count FROM operations; SELECT kind,src,substr(restore_json,1,40) FROM operation_entries;"
```
Expected: a `purge|1` operation and a `purge_row` entry with a JSON snapshot.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp src/database.h src/database.cpp tests/test_database.cpp
git commit -m "Log purge operations with restorable row snapshots"
```

---

### Task 7: dry-run writes nothing + docs

**Files:**
- Verify: `src/main.cpp` dry-run early-returns (`cmd_organize:1111`, `cmd_purge:1216`)
- Modify: `AGENTS.md` (config keys + Key Design Decisions), `README.md` if it documents config
- Test: manual

**Interfaces:** none new.

- [ ] **Step 1: Confirm dry-run logs nothing**

Both `cmd_organize` and `cmd_purge` return before the execute phase when `opts.dry_run` is set (`main.cpp:1111`, `1216`) — the `log_begin_operation` calls added in Tasks 5–6 are after those returns, so no operation row is written. Confirm by reading; no code change expected.

Manual:
```bash
cd "$(mktemp -d)" && mkdir src && printf x > src/a.jpg
DB=./t.db
./"$OLDPWD"/build/phig scan src --db "$DB" >/dev/null
./"$OLDPWD"/build/phig cp dest --dry-run --db "$DB" >/dev/null
sqlite3 "$DB" "SELECT COUNT(*) FROM operations;"   # expect 0
```
Expected: `0`.

- [ ] **Step 2: Document the config keys**

In `AGENTS.md`, add a `[log]` note to the config description (near the XDG/config paragraph) and a bullet under "Key Design Decisions":

```
- **Activity log** — `mv`/`cp`/`purge` record an operation + per-file entries in
  `phig.db` (append-only, undo is a future logged operation). Disable with
  `[log] enabled = false`; cap size with `[log] max_entries` (default 100000,
  0 = unlimited, oldest operations pruned first).
```

- [ ] **Step 3: Run the full suite once more + lint**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: all PASS.
Run: `clang-format --dry-run --Werror src/database.cpp src/main.cpp src/config.cpp src/config.h src/database.h`
Expected: no output (clean).

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md README.md
git commit -m "Document activity-log config keys"
```

---

## Implementation outcome (2026-07-06)

Implemented as planned, then hardened after a high-effort data-safety code
review. Notable deltas from the plan:

- **Executor extracted.** The per-file disk+DB+compensator logic lives in
  `execute_organize_action` (`src/organize_exec.{h,cpp}`), linked into `phig-lib`
  so its failure paths are unit-testable (`tests/test_organize_exec.cpp` forces
  DB writes to fail via `PRAGMA query_only` after the disk op).
- **Overwrite semantics changed (behavior change).** `is_protected` in
  `cmd_organize` now protects *every* existing on-disk destination, tracked or
  untracked — not just untracked files. `--on-conflict overwrite` therefore
  renames around any existing file instead of clobbering it. This closes a
  data-loss path: mv had no backup (only cp did), and cp's `INSERT OR REPLACE`
  would cascade a clobbered tracked row's faces/tags away with nothing logged to
  restore them. The cp backup-then-copy remains as defense-in-depth for direct
  callers of the executor.
- **`.phig-bak` uniqueness.** The overwrite backup uses a pid+counter sibling
  name (`unique_backup_path`), matching the audited atomic-copy temps, so it can
  never clobber a stale backup, a concurrent process's file, or a real file.
- **`log_prune` always keeps the newest operation** and prunes in a single bulk
  `DELETE` (O(n), not O(n²)), so a run larger than `max_entries` cannot erase its
  own rollback record.
- **No-op self-move/copy** (dest == source) touches nothing and logs nothing.
- **purge snapshot** records a `_faces_omitted` count when a purged image had
  faces, since `restore_json` captures only the images row (faces/embeddings are
  not serialized in this slice — the deferred undo will define that format).

Tests: 266 total (was 246). New coverage: schema/migration, log API round-trip,
retention (incl. keeps-newest), config parsing, purge snapshot + faces-omitted,
and the executor's compensator paths (mv move-back, cp orphan-removal, cp
overwrite restore, no-op).

## Deferred (future plan)

- `phig log` — list operations (id, ts, command, counts) and `phig log <id>`
  for per-file entries.
- `phig undo [<id>]` — reverse a whole operation (default: most recent), with
  `--dry-run` and verify-and-refuse-conflicts safety (check disk state before
  each inverse; skip and report rather than clobber). Writes a new `undo`
  operation with `undoes_id` set.
- Per-file undo — the data model already supports addressing individual entries.
- A manual `phig log prune` command, if auto-retention proves insufficient.
