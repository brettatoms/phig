---
name: Concurrency Safety Fixes
status: completed
created: 2026-07-12
description: >
  Close four concurrency correctness defects (temp-file counter data race,
  trash rename clobber, migration re-apply race, deferred-BEGIN BUSY_SNAPSHOT
  failures) and add a process-wide advisory writer lock so mutating phig
  commands serialize coherently instead of interleaving. Reads are never
  blocked. Fine-grained per-fileset locking investigated and rejected (see
  Rejected Alternatives).
---

# Concurrency Safety Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make phig safe and coherent under concurrent invocation and concurrent scan threads by fixing four correctness defects and adding a global advisory writer lock.

**Architecture:** Four independent, small correctness fixes to existing files (`paths.cpp`, `remove_exec.cpp`, `database.cpp`), plus one new `src/lockfile.{h,cpp}` unit added to `phig-lib` that wraps `flock(2)` on a sidecar file beside the DB. Mutating command branches in `main.cpp` acquire the lock after arg parsing and before DB work; reads never take it. Each fix ships with a real-behavior test (real files, real threads/processes, real SQLite) per the project's data-safety testing rule.

**Tech Stack:** C++20, CMake, SQLite (WAL), GoogleTest, POSIX `flock`/`renameat2`.

## Global Constraints

- Plans live in `plans/` as numbered `NNN-name.md`. This is 029. Add a row to `plans/000-INDEX.md`. (AGENTS.md rule.)
- Every data-safety fix ships with comprehensive real-behavior tests — real files, real SQLite, real concurrency where the bug is concurrency. No mock-only tests.
- First-party code builds with `-Wall -Wextra`; CI sets `-DPHIG_WERROR=ON`. New code must be warning-clean.
- Formatting: clang-format LLVM style, 4-space, 100-col (`.clang-format`). Run `clang-format --dry-run --Werror` on touched `src/`+`tests/` files.
- Reads (`search`, `duplicates`, `face list`, `tag list`, `gallery list`, `fav list`, `log`) MUST NOT acquire the writer lock.
- The lock is per-machine/per-DB advisory only. Document, do not try to solve, the network-share case.
- No AI attribution in commit messages.

---

### Task 1: Fix the `temp_sibling` counter data race

**Files:**
- Modify: `src/paths.cpp:85-91` (the `temp_sibling` function)
- Test: `tests/test_paths.cpp` (new test)

**Interfaces:**
- Consumes: nothing new.
- Produces: no signature change. `temp_sibling(const std::filesystem::path&)` keeps returning `std::filesystem::path`; only its internal counter becomes thread-safe.

**Background:** `temp_sibling` uses `static unsigned long counter = 0; ... counter++` with no synchronization. It's called from concurrent scan/thumbs workers via `generate_thumb` → `atomic_write_file` → `temp_sibling`. Two duplicate-content files (same SHA256) thumbnailed by two workers resolve to the *same* dest path, same pid; a torn/duplicated counter yields the same tmp path, so two writers interleave into one tmp file that is then renamed into place as a corrupt thumbnail `thumb_exists()` trusts forever.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_paths.cpp`. The race is on uniqueness of the returned name under concurrency; assert that N threads calling `temp_sibling` for the same dest never collide.

```cpp
#include <atomic>
#include <set>
#include <thread>
#include <vector>

TEST(TempSiblingConcurrency, NoCollisionUnderThreads) {
    namespace fs = std::filesystem;
    const fs::path dest = fs::temp_directory_path() / "phig-tmpsib" / "IMG_1.jpg";
    const int threads = 8;
    const int per_thread = 2000;

    std::vector<std::vector<std::string>> results(threads);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            results[t].reserve(per_thread);
            for (int i = 0; i < per_thread; ++i)
                results[t].push_back(temp_sibling(dest).string());
        });
    }
    for (auto& th : pool) th.join();

    std::set<std::string> uniq;
    size_t total = 0;
    for (auto& r : results)
        for (auto& s : r) { uniq.insert(s); ++total; }

    // Every generated temp path must be distinct.
    EXPECT_EQ(uniq.size(), total);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='TempSiblingConcurrency.*'`
Expected: FAIL (uniq.size() < total) — or a TSan/UB report if built with sanitizers. On a fast machine it may occasionally pass; run 3x to confirm the race. If it never fails, temporarily raise `per_thread` to 20000.

- [ ] **Step 3: Make the counter atomic**

In `src/paths.cpp`, change `temp_sibling`:

```cpp
std::filesystem::path temp_sibling(const std::filesystem::path& dest) {
    static std::atomic<unsigned long> counter{0};
    std::string name = "." + dest.filename().string() + ".phig-tmp." +
                       std::to_string(static_cast<long>(getpid())) + "." +
                       std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return dest.parent_path() / name;
}
```

Add `#include <atomic>` at the top of `src/paths.cpp` if not already present.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='TempSiblingConcurrency.*'`
Expected: PASS. Run 3x.

- [ ] **Step 5: Commit**

```bash
git add src/paths.cpp tests/test_paths.cpp
git commit -m "Fix data race on temp_sibling counter (atomic)"
```

---

### Task 2: Fix the trash rename clobber in `execute_trash_action`

**Files:**
- Modify: `src/paths.cpp` (expose `rename_noreplace` — add a declaration to `src/paths.h`)
- Modify: `src/paths.h` (declare `rename_noreplace`)
- Modify: `src/remove_exec.cpp:11-25` (`execute_trash_action`)
- Test: `tests/test_remove_exec.cpp` (new test)

**Interfaces:**
- Consumes: `int rename_noreplace(const char* src, const char* dst)` (currently file-local in `paths.cpp:131`), `std::string trash_path_for(const std::string&)`.
- Produces: `execute_trash_action` unchanged signature: `std::string execute_trash_action(Database&, const std::string& path, int64_t op_id)`.

**Background:** `trash_path_for` picks a free name via an exists-check loop, then `execute_trash_action` does a plain `fs::rename`, which **overwrites** on POSIX. Between the check and the rename another process can create that same trash name (two `phig rm` runs trashing two different files that share a basename). The second rename silently destroys the first file's only remaining copy. The EXDEV fallback uses `copy_options::overwrite_existing`, same clobber. Fix: no-replace rename, and on collision re-derive the next free suffix and retry; cross-mount uses an exclusive-create copy.

- [ ] **Step 1: Declare `rename_noreplace` in the header**

In `src/paths.h`, near the other filesystem helpers (after `move_file`, around line 45), add:

```cpp
// Rename src -> dst but NEVER overwrite an existing dst. Returns 0 on success,
// EEXIST if dst already exists, or another errno on failure. Uses the kernel's
// atomic no-replace rename where available, then a hardlink emulation, then a
// check-then-rename fallback (small TOCTOU window on filesystems with neither,
// e.g. FAT). Prefer this over fs::rename for any move into a shared directory.
int rename_noreplace(const char* src, const char* dst);
```

Remove the `static` keyword nothing — `rename_noreplace` is already a non-static free function in `paths.cpp` (`int rename_noreplace(...)` at line 131), just undeclared in the header. Verify with `rg -n 'rename_noreplace' src/paths.cpp` that its definition has external linkage (no `static`). It does.

- [ ] **Step 2: Write the failing test**

Add to `tests/test_remove_exec.cpp`. Simulate the TOCTOU: pre-create the trash file that `trash_path_for` would have picked, so a naive `fs::rename` would clobber it, and assert the trashed file lands at a *different* (suffixed) path and the pre-existing file survives.

```cpp
TEST_F(RemoveExecTest, TrashDoesNotClobberRaceWinner) {
    // Two files share a basename; the second must not overwrite the first even
    // if the free-name it computed gets taken before its rename.
    std::string src = (photos / "IMG_R.jpg").string();
    write_file(src, "mine");

    // Pre-occupy the exact trash path trash_path_for() will hand out first,
    // standing in for a concurrent process that trashed a same-named file in
    // the window between name-selection and rename.
    fs::create_directories(trash_dir());
    std::string taken = trash_path_for("IMG_R.jpg");   // e.g. .../trash/IMG_R.jpg
    write_file(fs::path(taken), "someone-elses");

    int64_t op = db->log_begin_operation("rm", "phig rm", "2026-07-12T00:00:00Z");
    std::string dst = execute_trash_action(*db, src, op);

    EXPECT_FALSE(fs::exists(src));               // ours was trashed
    EXPECT_NE(dst, taken);                        // did NOT reuse the taken path
    EXPECT_TRUE(fs::exists(dst));                 // ours is present at the new path
    // The pre-existing file is intact and unmodified.
    ASSERT_TRUE(fs::exists(taken));
    std::ifstream in(taken); std::string content; std::getline(in, content);
    EXPECT_EQ(content, "someone-elses");
    // The log records the actual destination we used.
    EXPECT_EQ(count_entries("trash", src), 1);
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='RemoveExecTest.TrashDoesNotClobberRaceWinner'`
Expected: FAIL — `dst == taken` and/or the pre-existing file's content becomes "mine" (clobbered) with the current plain `fs::rename`.

- [ ] **Step 4: Implement no-clobber trash**

Replace the body of `execute_trash_action` in `src/remove_exec.cpp`:

```cpp
std::string execute_trash_action(Database& db, const std::string& path, int64_t op_id) {
    fs::create_directories(trash_dir());
    std::string base = fs::path(path).filename().string();

    // No-clobber move into the shared trash dir. trash_path_for() picks a free
    // name, but another process can take it before our rename — so retry on
    // EEXIST, re-deriving the next free name each time. Never overwrite.
    std::string dst;
    for (;;) {
        dst = trash_path_for(base);
        int err = rename_noreplace(path.c_str(), dst.c_str());
        if (err == 0) break;
        if (err == EEXIST) continue;  // someone took it; pick the next free name
        if (err != EXDEV)
            throw fs::filesystem_error("trash: rename failed", path, dst,
                                       std::error_code(err, std::generic_category()));
        // Cross-mount: exclusive-create copy so we never overwrite, then remove
        // the source. Retry on collision the same way.
        std::error_code ec;
        fs::copy_file(path, dst, fs::copy_options::none, ec);
        if (ec) {
            if (ec == std::errc::file_exists) continue;
            throw fs::filesystem_error("trash: cross-mount copy failed", path, dst, ec);
        }
        fs::remove(path);  // throws on failure — leaves the copy in trash
        break;
    }

    if (op_id > 0) db.log_add_entry(op_id, {"trash", path, dst, ""});
    return dst;
}
```

Ensure `#include <cerrno>` is present in `src/remove_exec.cpp` for `EEXIST`/`EXDEV` (add it near the other includes if missing).

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='RemoveExecTest.*'`
Expected: PASS — the new test plus all pre-existing RemoveExec tests (including `TrashSuffixesOnCollisionAndLogsSuffixedPath`, which still holds: sequential trashing of same-named files still suffixes).

- [ ] **Step 6: Commit**

```bash
git add src/paths.h src/remove_exec.cpp tests/test_remove_exec.cpp
git commit -m "Trash files with no-clobber rename; expose rename_noreplace"
```

---

### Task 3: Use `BEGIN IMMEDIATE` for write transactions (BUSY_SNAPSHOT fix)

**Files:**
- Modify: `src/database.cpp:622` (`begin_transaction`) and the inline `exec("BEGIN TRANSACTION")` sites for write paths
- Test: `tests/test_database.cpp` (new test)

**Interfaces:**
- Consumes: nothing new.
- Produces: `Database::begin_transaction()` now issues `BEGIN IMMEDIATE`. No signature change.

**Background:** All write transactions use deferred `BEGIN TRANSACTION`. Under WAL a transaction that reads first and writes later (e.g. `insert_copy` does `get_image_id` before its INSERT; `insert` does a SELECT before UPSERT) can get `SQLITE_BUSY_SNAPSHOT` when another process committed in between — and the `busy_timeout` handler does **not** retry that error; it throws immediately. `BEGIN IMMEDIATE` takes the write lock at transaction start, so the busy handler's 30s wait applies and concurrent writers queue instead of failing.

Scope: change the shared `begin_transaction()` and the write-path inline `BEGIN TRANSACTION` sites. Do NOT change the schema-init/migration `BEGIN`s here — Task 4 handles those specifically. The write-path inline sites are the ones in insert/update/log functions, not the two schema-creation blocks (`database.cpp:199` and `database.cpp:252`) or the migration loop (`database.cpp:360`).

- [ ] **Step 1: Write the failing test**

Add to `tests/test_database.cpp`. Two connections to the same WAL DB; connection A opens a read-then-write transaction, connection B commits a write in the middle, then A writes. With deferred BEGIN + a *short* busy timeout this reproduces the immediate BUSY_SNAPSHOT throw; with IMMEDIATE it serializes.

```cpp
TEST(DatabaseConcurrency, ReadThenWriteDoesNotBusySnapshot) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "phig-db-concurrency";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string dbp = (dir / "c.db").string();

    { Database init(dbp); init.init_schema(); }

    Database a(dbp);
    Database b(dbp);

    // A: read first (opens a read snapshot under deferred BEGIN).
    a.begin_transaction();
    (void)a.count();  // a read inside the txn

    // B: independently commit a write while A's txn is open.
    b.begin_transaction();
    ImageInfo info;
    info.path = (dir / "x.jpg").string();
    info.filename = "x.jpg"; info.extension = "jpg"; info.file_size = 1;
    info.created_at = "2024-01-01T00:00:00Z"; info.modified_at = info.created_at;
    info.sha256 = "abc"; info.phash = 0; info.width = 1; info.height = 1;
    info.metadata_json = "{}";
    b.insert(info);
    b.commit_transaction();

    // A now upgrades to a write. With deferred BEGIN this throws BUSY_SNAPSHOT;
    // with BEGIN IMMEDIATE, A already holds the write lock, so this succeeds.
    ImageInfo info2 = info;
    info2.path = (dir / "y.jpg").string();
    info2.filename = "y.jpg";
    EXPECT_NO_THROW(a.insert(info2));
    EXPECT_NO_THROW(a.commit_transaction());

    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='DatabaseConcurrency.ReadThenWriteDoesNotBusySnapshot'`
Expected: FAIL — A's `insert`/`commit` throws `SQLITE_BUSY_SNAPSHOT` ("database is locked" / "cannot start a write transaction within a read transaction") because A opened a read snapshot with deferred BEGIN and B moved the DB forward.

- [ ] **Step 3: Switch write transactions to IMMEDIATE**

In `src/database.cpp`, change the shared helper:

```cpp
void Database::begin_transaction() { exec("BEGIN IMMEDIATE TRANSACTION"); }
```

Then update the write-path inline transaction sites — the ones inside insert/update/copy/log functions — from `exec("BEGIN TRANSACTION")` to `exec("BEGIN IMMEDIATE TRANSACTION")`. Locate them with:

```bash
rg -n 'BEGIN TRANSACTION' src/database.cpp
```

Change every site EXCEPT the three schema/migration sites (the pre-migration block at ~199, the fresh-schema block at ~252, and the migration loop at ~360) — those are handled in Task 4. The remaining sites (insert_batch ~461, insert_copy ~571, and the log functions ~723/~782/~814/~1247/~1307/~1326) all become `BEGIN IMMEDIATE TRANSACTION`. Verify each site by content, not by these numbers — they drift. A quick way to confirm you caught them all: after editing, `rg -n 'BEGIN TRANSACTION' src/database.cpp` should return only the three schema/migration sites (Task 4 converts those).

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='DatabaseConcurrency.*'`
Expected: PASS. Also run the full suite to confirm no regression from IMMEDIATE locking: `./build/phig-tests`.

- [ ] **Step 5: Commit**

```bash
git add src/database.cpp tests/test_database.cpp
git commit -m "Use BEGIN IMMEDIATE for write transactions to avoid BUSY_SNAPSHOT"
```

---

### Task 4: Make schema migration concurrency-safe

**Files:**
- Modify: `src/database.cpp:168-373` (`init_schema` — migration transaction and version re-check)
- Test: `tests/test_database.cpp` (new test)

**Interfaces:**
- Consumes: `Database::get_schema_version()`, `Database::set_schema_version(int)`.
- Produces: no signature change to `init_schema()`.

**Background:** `get_schema_version()` is read outside the migration transaction, which uses deferred `BEGIN`. Two processes opening an old DB both read version N; the writes serialize, but the loser re-applies the migration SQL. Non-idempotent statements (`ALTER TABLE ADD COLUMN`) then fail with "duplicate column name", aborting that command. Fix: run each migration step in a `BEGIN IMMEDIATE` transaction and re-read the version *inside* it, skipping if another process already applied it.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_database.cpp`. Build a v(N-1) DB, then have two threads each construct a `Database` and call `init_schema()` concurrently. Neither should throw, and the final version must be current.

```cpp
TEST(DatabaseConcurrency, ConcurrentMigrationIsSafe) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "phig-db-migrate";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string dbp = (dir / "m.db").string();

    // Create a DB then roll its recorded schema_version back to force a
    // migration on next init_schema(). (v5 is CURRENT; set to 4 to force the
    // v5 migration to run.)
    {
        Database init(dbp);
        init.init_schema();
        init.set_schema_version(4);
    }

    std::atomic<int> failures{0};
    auto run = [&] {
        try {
            Database d(dbp);
            d.init_schema();
        } catch (...) {
            failures.fetch_add(1);
        }
    };
    std::thread t1(run), t2(run);
    t1.join(); t2.join();

    EXPECT_EQ(failures.load(), 0);
    Database check(dbp);
    EXPECT_EQ(check.get_schema_version(), 5);  // CURRENT_SCHEMA_VERSION

    fs::remove_all(dir);
}
```

Note: two threads in one process still use two independent SQLite connections (one per `Database`), so this exercises the cross-connection race that mirrors two processes. If `CURRENT_SCHEMA_VERSION` has advanced past 5 by the time this runs, update both the rollback target (`set_schema_version(CURRENT-1)`) and the expected value.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='DatabaseConcurrency.ConcurrentMigrationIsSafe'`
Expected: FAIL intermittently — one thread throws "duplicate column name" (or similar) when re-applying the migration. Run 5x to surface it; if flaky-passing, the deferred BEGIN still lets both read the same version.

- [ ] **Step 3: Make the migration loop re-check under IMMEDIATE**

In `src/database.cpp`, replace the migration loop (currently ~356-373):

```cpp
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
                if (!migrations[v].empty()) exec(migrations[v]);
                set_schema_version(v);
                exec("COMMIT");
            } catch (...) {
                exec("ROLLBACK");
                throw;
            }
        }
    }
```

The two schema-creation blocks (pre-migration at ~199, fresh at ~252) should also take `BEGIN IMMEDIATE TRANSACTION` instead of `BEGIN TRANSACTION` so two processes creating a fresh DB serialize cleanly; the `CREATE TABLE IF NOT EXISTS` bodies are already idempotent, so the loser's transaction is a harmless no-op followed by `set_schema_version`. Change those two `exec("BEGIN TRANSACTION")` lines to `exec("BEGIN IMMEDIATE TRANSACTION")`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='DatabaseConcurrency.*'`
Expected: PASS. Run 5x for the migration test. Then full suite: `./build/phig-tests`.

- [ ] **Step 5: Commit**

```bash
git add src/database.cpp tests/test_database.cpp
git commit -m "Make schema migration concurrency-safe (IMMEDIATE + in-txn version recheck)"
```

---

### Task 5: Add the lockfile primitive

**Files:**
- Create: `src/lockfile.h`, `src/lockfile.cpp`
- Modify: `CMakeLists.txt` (add `src/lockfile.cpp` to `phig-lib` sources after `src/dates.cpp`; add `tests/test_lockfile.cpp` to `phig-tests` after `tests/test_dates.cpp`)
- Modify: `src/paths.h` / `src/paths.cpp` (add `lock_path()` returning `default_db_path() + ".lock"`)
- Test: `tests/test_lockfile.cpp` (new)

**Interfaces:**
- Consumes: `std::string lock_path()`.
- Produces:
  - `std::string lock_path();` (in `paths.h`) — the advisory lock file path, `<db>.lock`.
  - A RAII lock class in `lockfile.h`:
    ```cpp
    // Process-wide advisory writer lock (flock on a sidecar file). Held for the
    // lifetime of the object; released on destruction or process death. Blocks
    // waiting for the lock unless no_wait is set. Reads must never construct one.
    class WriterLock {
       public:
        // Acquire the lock on `path`. If the lock is held by another process:
        //  - no_wait=false (default): print a "waiting" message once and block.
        //  - no_wait=true: throw LockBusy immediately (no wait).
        // Throws std::runtime_error on unexpected errors (open/flock failure).
        explicit WriterLock(const std::string& path, bool no_wait = false);
        ~WriterLock();
        WriterLock(const WriterLock&) = delete;
        WriterLock& operator=(const WriterLock&) = delete;
        int holder_pid() const;  // pid recorded in the lock file, or -1
       private:
        int fd_ = -1;
    };
    // Thrown by WriterLock when no_wait is set and the lock is already held.
    struct LockBusy : std::runtime_error { using std::runtime_error::runtime_error; };
    ```

**Design notes:**
- Use `open(path, O_RDWR|O_CREAT, 0644)` then `flock(fd, LOCK_EX|LOCK_NB)`. On `EWOULDBLOCK`: if `no_wait`, read the holder pid from the file and `throw LockBusy(...)`; else print `"Another phig operation is running (pid N); waiting..."` to stderr once, then `flock(fd, LOCK_EX)` (blocking).
- After acquiring, `ftruncate(fd, 0)` and write the current pid as text so `holder_pid()` and the waiting message can report it. Best-effort — a failed write doesn't fail the lock.
- Do NOT unlink the lock file on release; unlinking races with other waiters. Just `close(fd)` (which releases the flock). A stale zero-byte `<db>.lock` sitting on disk is harmless.
- `flock` is released automatically on process death (crash, `kill -9`), so there are no stale locks.

- [ ] **Step 1: Add `lock_path()`**

In `src/paths.h`, after `default_db_path()`:

```cpp
std::string lock_path();  // default_db_path() + ".lock"  (advisory writer lock)
```

In `src/paths.cpp`, near `default_db_path()`:

```cpp
std::string lock_path() { return default_db_path() + ".lock"; }
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_lockfile.cpp`:

```cpp
#include <gtest/gtest.h>

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "lockfile.h"

namespace fs = std::filesystem;

class LockfileTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string lpath;
    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-lockfile-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        lpath = (dir / "phig.db.lock").string();
    }
    void TearDown() override { fs::remove_all(dir); }
};

TEST_F(LockfileTest, AcquiresAndReleases) {
    { WriterLock lk(lpath); EXPECT_EQ(lk.holder_pid(), getpid()); }
    // Released — a second lock in the same process succeeds without blocking.
    WriterLock lk2(lpath, /*no_wait=*/true);
    SUCCEED();
}

TEST_F(LockfileTest, NoWaitThrowsWhenHeldByAnotherFd) {
    // Simulate another process holding the lock by taking a raw flock on the
    // same file from a separate fd.
    int other = ::open(lpath.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(other, 0);
    ASSERT_EQ(::flock(other, LOCK_EX | LOCK_NB), 0);

    EXPECT_THROW({ WriterLock lk(lpath, /*no_wait=*/true); }, LockBusy);

    ::flock(other, LOCK_UN);
    ::close(other);
}

TEST_F(LockfileTest, WaitBlocksThenAcquiresAfterRelease) {
    int other = ::open(lpath.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(other, 0);
    ASSERT_EQ(::flock(other, LOCK_EX | LOCK_NB), 0);

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        WriterLock lk(lpath, /*no_wait=*/false);  // should block until release
        acquired.store(true);
    });

    // Give the waiter time to reach the blocking flock.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(acquired.load());  // still blocked

    ::flock(other, LOCK_UN);
    ::close(other);

    waiter.join();
    EXPECT_TRUE(acquired.load());
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='LockfileTest.*'`
Expected: FAIL to compile — `lockfile.h` / `WriterLock` don't exist yet. (After adding to CMake in Step 4, it will link.)

- [ ] **Step 4: Implement the lockfile and wire CMake**

Create `src/lockfile.h`:

```cpp
#pragma once

#include <stdexcept>
#include <string>

// Thrown by WriterLock when no_wait is set and the lock is already held.
struct LockBusy : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Process-wide advisory writer lock (flock on a sidecar file). Held for the
// lifetime of the object; released on destruction or process death. Blocks
// waiting for the lock unless no_wait is set. Reads must never construct one.
class WriterLock {
   public:
    explicit WriterLock(const std::string& path, bool no_wait = false);
    ~WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    int holder_pid() const { return holder_pid_; }

   private:
    int fd_ = -1;
    int holder_pid_ = -1;
};
```

Create `src/lockfile.cpp`:

```cpp
#include "lockfile.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <system_error>

namespace {
int read_pid(int fd) {
    char buf[32] = {0};
    if (::pread(fd, buf, sizeof(buf) - 1, 0) > 0) {
        try {
            return std::stoi(std::string(buf));
        } catch (...) {
        }
    }
    return -1;
}

void write_pid(int fd) {
    std::string s = std::to_string(::getpid());
    if (::ftruncate(fd, 0) == 0) {
        (void)::pwrite(fd, s.data(), s.size(), 0);  // best-effort
    }
}
}  // namespace

WriterLock::WriterLock(const std::string& path, bool no_wait) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot open lock file " + path + ": " +
                                 std::strerror(errno));
    }

    if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        holder_pid_ = ::getpid();
        write_pid(fd_);
        return;
    }
    if (errno != EWOULDBLOCK) {
        int e = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot lock " + path + ": " + std::strerror(e));
    }

    // Lock is held by another process.
    int other_pid = read_pid(fd_);
    if (no_wait) {
        ::close(fd_);
        fd_ = -1;
        throw LockBusy("Another phig operation is running (pid " +
                       std::to_string(other_pid) + ")");
    }

    std::cerr << "Another phig operation is running (pid " << other_pid
              << "); waiting..." << std::endl;
    if (::flock(fd_, LOCK_EX) != 0) {  // blocking
        int e = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot lock " + path + ": " + std::strerror(e));
    }
    holder_pid_ = ::getpid();
    write_pid(fd_);
}

WriterLock::~WriterLock() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);  // explicit; close() would also release
        ::close(fd_);
    }
}
```

In `CMakeLists.txt`, add `src/lockfile.cpp` to the `phig-lib` source list (after `src/dates.cpp`, line ~64):

```cmake
    src/dates.cpp
    src/raw.cpp
    src/lockfile.cpp
```

And add the test to `phig-tests` (after `tests/test_dates.cpp`, line ~135):

```cmake
    tests/test_raw.cpp
    tests/test_lockfile.cpp
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cmake -B build && cmake --build build && ./build/phig-tests --gtest_filter='LockfileTest.*'`
Expected: PASS (all three).

- [ ] **Step 6: Commit**

```bash
git add src/lockfile.h src/lockfile.cpp src/paths.h src/paths.cpp CMakeLists.txt tests/test_lockfile.cpp
git commit -m "Add WriterLock advisory lockfile primitive"
```

---

### Task 6: Acquire the writer lock in mutating commands

**Files:**
- Modify: `src/main.cpp` — add a `--no-wait` global flag (parsed alongside `--config` at ~2444), and construct a `WriterLock` at the top of each mutating command branch.
- Test: `tests/test_lockfile.cpp` — no new unit needed if Task 5's tests cover WriterLock; add a note. Manual/integration verification below.

**Interfaces:**
- Consumes: `WriterLock`, `LockBusy` (from `lockfile.h`); `std::string lock_path()`.
- Produces: mutating commands hold a `WriterLock` for their duration; `--no-wait` makes contention fail fast.

**Background:** Mutating commands must serialize; reads must not. The dispatch is a flat if/else chain on `command` in `main()` (~2516+). The mutating commands are: `scan`, `cp`, `mv`, `rm`, `trash`, `thumbs` (rebuild/clean), `tag`, `gallery`, `fav`, `face` (name/identify/rebuild), `models` (download). (`purge` was removed by plan 027; do not add it.) The reads are: `duplicates`, `search`, `log`, and the `list` subcommands.

Note on subcommands: `thumbs`, `face`, `tag`, `gallery`, `fav`, `models` have both mutating and read subcommands (e.g. `tag list` reads, `tag add` writes; `face list` reads, `face rebuild` writes). The lock must be taken only for the mutating subcommands. Take the lock *inside* each `cmd_*` function after it determines the subcommand, OR pass a flag. Simplest: take it inside the `cmd_*` function right before the mutating work, since that's where the subcommand is already known.

- [ ] **Step 1: Add the `--no-wait` global flag**

In `src/main.cpp`, extend the `--config` extraction loop (~2450) to also capture `--no-wait`:

```cpp
    std::string config_file;
    bool have_config_file = false;
    bool no_wait = false;
    std::vector<char*> filtered_args;
    filtered_args.push_back(argv[0]);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_file = argv[++i];
            have_config_file = true;
        } else if (a == "--no-wait") {
            no_wait = true;
        } else {
            filtered_args.push_back(argv[i]);
        }
    }
```

Make `no_wait` visible where commands are dispatched (it's already in `main`'s scope). Add `#include "lockfile.h"` near the other project includes at the top of `main.cpp`.

- [ ] **Step 2: Add a lock helper and take it in whole-command mutating branches**

Add a small helper near the top of `main.cpp` (after includes):

```cpp
// Acquire the process-wide writer lock for a mutating command. Prints a clear
// message and exits nonzero if --no-wait was given and another op holds it.
static std::unique_ptr<WriterLock> acquire_writer_lock(bool no_wait) {
    try {
        return std::make_unique<WriterLock>(lock_path(), no_wait);
    } catch (const LockBusy& e) {
        std::cerr << "Error: " << e.what() << " (use without --no-wait to wait)\n";
        std::exit(1);
    }
}
```

For whole-command mutating branches (`scan`, `cp`/`mv`, `rm`, `trash`), construct the lock right after arg parsing and before the `cmd_*` call, e.g. in the `cp`/`mv` branch:

```cpp
    } else if (command == "cp" || command == "mv") {
        // ... existing arg parsing populating opts ...
        auto lock = acquire_writer_lock(no_wait);  // held until end of branch
        return cmd_organize(opts);
```

Do the same in the `scan`, `rm`, and `trash` branches — place `auto lock = acquire_writer_lock(no_wait);` immediately before the `cmd_*` return call so parsing/`--help`/validation errors don't take the lock.

- [ ] **Step 3: Take the lock inside subcommand-dispatching commands**

For `thumbs`, `face`, `tag`, `gallery`, `fav`, `models`, pass `no_wait` into the `cmd_*` function and acquire the lock only on mutating subcommands. Update each signature to accept `bool no_wait` and construct the lock after the subcommand is known. Example for `cmd_tag` (add subcommands only): in the `add`/`remove` branch, before the mutating `db.set_tag(...)`:

```cpp
    if (subcmd == "add" || subcmd == "remove") {
        // ... parse ...
        WriterLock lock(lock_path(), no_wait);  // wrap in try if you want the
                                                // friendly message; or rely on
                                                // acquire_writer_lock-style helper
        int n = db.set_tag(...);
```

To keep the friendly `LockBusy` message consistent, expose `acquire_writer_lock` (drop `static`, declare it in a small internal header or forward-declare at the top of the `cmd_*` block) and use it here too. For `tag list` / `gallery list` / `fav list` / `face list` / `thumbs` read paths: do NOT acquire the lock.

Concretely, the mutating subcommands that take the lock:
- `thumbs rebuild`, `thumbs clean`
- `face name`, `face identify`, `face rebuild`
- `tag add`, `tag remove`
- `gallery set`, `gallery clear`
- `fav add`, `fav remove`
- `models download`

- [ ] **Step 4: Build and manually verify serialization + reads-not-blocked**

Run: `cmake --build build`

Manual integration check (real binary, real lock):

```bash
# Set up an isolated DB/data dir.
export XDG_DATA_HOME=$(mktemp -d)
export XDG_CACHE_HOME=$(mktemp -d)
mkdir -p "$XDG_DATA_HOME/pics"
# Make a couple of files.
head -c 100000 /dev/urandom > "$XDG_DATA_HOME/pics/a.bin"; cp "$XDG_DATA_HOME/pics/a.bin" "$XDG_DATA_HOME/pics/a.jpg"
./build/phig scan "$XDG_DATA_HOME/pics"

# 1. Two mutating commands serialize: start a slow-ish scan in the background,
#    then a second mutating command should print the "waiting..." message.
./build/phig scan "$XDG_DATA_HOME/pics" --force &
./build/phig tag add x --match '*a.jpg' 2>&1 | grep -q "waiting" && echo "SERIALIZED OK"
wait

# 2. --no-wait fails fast under contention.
./build/phig scan "$XDG_DATA_HOME/pics" --force &
sleep 0.05
./build/phig tag add y --match '*a.jpg' --no-wait 2>&1 | grep -q "Another phig operation" && echo "NO-WAIT OK"
wait

# 3. A read is NOT blocked while a mutation holds the lock.
./build/phig scan "$XDG_DATA_HOME/pics" --force &
./build/phig search --ext jpg >/dev/null && echo "READ NOT BLOCKED OK"
wait
```

Expected: prints `SERIALIZED OK`, `NO-WAIT OK`, `READ NOT BLOCKED OK`. (Timing note: if the scan is too fast to still hold the lock, add more files so it runs long enough to observe contention.)

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Acquire writer lock in mutating commands; add --no-wait"
```

---

### Task 7: Documentation

**Files:**
- Modify: `README.md` (document the writer lock + `--no-wait` and the per-machine caveat)
- Modify: `AGENTS.md` (Key Design Decisions: add a concurrency note)
- Modify: `plans/000-INDEX.md` (add the 029 row)

- [ ] **Step 1: README — add a "Concurrency" note**

Add to `README.md` after the Configuration section (before Usage), a short subsection:

```markdown
### Concurrency

Mutating commands (`scan`, `mv`, `cp`, `rm`, `tag`/`gallery`/`fav` writes,
`face` writes, `thumbs rebuild`/`clean`, `models download`) take a process-wide
advisory lock (`<db>.lock`) so two of them never run at once and interleave.
If another mutating command is already running, phig prints
`Another phig operation is running (pid N); waiting...` and waits. Pass
`--no-wait` to fail immediately instead. Read commands (`search`,
`duplicates`, `log`, the `list` subcommands) never take the lock and are never
blocked.

The lock is per-machine (POSIX `flock`) and released automatically if phig
crashes or is killed. It does not coordinate phig instances on different hosts
sharing one database over a network filesystem.
```

- [ ] **Step 2: AGENTS.md — add a Key Design Decision**

Add a bullet under "Key Design Decisions":

```markdown
- **Concurrency** — SQLite is WAL with a 30s busy_timeout; write transactions
  use `BEGIN IMMEDIATE` so read-then-write paths don't hit BUSY_SNAPSHOT, and
  schema migrations re-check the version inside an IMMEDIATE transaction so
  concurrent first-runs don't re-apply DDL. Mutating commands additionally take
  a process-wide advisory `flock` on `<db>.lock` (`src/lockfile.cpp`) so whole
  operations serialize; reads never take it. Per-machine only.
```

- [ ] **Step 3: Add the plan index row**

In `plans/000-INDEX.md`, add after the 028 row:

```markdown
| 029 | Concurrency Safety Fixes | planned | [029-concurrency-fixes.md](029-concurrency-fixes.md) | Fix four concurrency defects (atomic temp_sibling counter, no-clobber trash rename, BEGIN IMMEDIATE write transactions to avoid BUSY_SNAPSHOT, migration re-check under IMMEDIATE) and add a process-wide advisory writer lock (flock on `<db>.lock`) with `--no-wait`, so mutating commands serialize while reads are never blocked. Fine-grained per-fileset locking investigated and rejected. |
```

- [ ] **Step 4: Commit**

```bash
git add README.md AGENTS.md plans/000-INDEX.md
git commit -m "Document concurrency model and add plan 029 to index"
```

---

## Rejected Alternatives

### Fine-grained (per-fileset) locking

Considered locking only the files a command touches, so disjoint operations
(`mv --match '*/2024/*'` vs `tag add x --match '*/2023/*'`) could run
concurrently. Rejected in favor of the global writer lock. The investigation:

**Several commands cannot produce a lock set before acting.**
- `scan` discovers files by walking the disk and *creates* rows for
  previously-untracked paths; its write-set is the whole subtree and is
  unknowable up front. It can conflict with any other command, so it always
  needs the coarse lock — and it is the most common mutating command.
- `rm --orphans` targets rows whose files are *gone*; "lock the file" is
  meaningless.
- `mv`/`cp` know their sources but compute destinations from format strings and
  resolve conflicts against live disk state, *acquiring new destination paths
  mid-operation* (rename-around-conflict). That is hold-and-wait, which breaks
  the one assumption (lock the full set atomically up front) that makes sorted
  acquisition deadlock-free.

**Path locks don't cover the actual shared resources.** phig has four
contended resources, and only one keys on path:
1. the single SQLite writer (WAL permits exactly one; no file-locking scheme
   changes this — disjoint `mv`s still serialize their DB commits),
2. per-file filesystem state (path-keyed — the only thing fine-grained locking
   addresses),
3. the thumbnail cache, which is **SHA256-keyed** — two commands on different
   paths with duplicate content collide (this plan's Task 1); a path lock
   cannot protect it, so a second lock domain keyed on content hash would be
   required,
4. the activity-log operation sequence (global ordering; interleaving makes
   undo semantics murky even for disjoint filesets).

**Deadlock-freedom is achievable but the design fights the codebase.** Sorted,
two-phase (all-or-nothing, non-blocking + backoff) acquisition is provably
deadlock-free, but it requires the full set known up front (violated by `scan`,
`orphans`, and mv/cp's dynamic dests), a lock manager to avoid fd exhaustion on
large filesets, and a second SHA256 lock domain. The interactions among those
accommodations are where subtle bugs live.

**The payoff is narrow.** Because SQLite serializes every DB commit regardless,
the only concurrency actually gained is overlapping *file I/O* of disjoint large
operations, and `scan` (the most-run command) can never participate.

**Cost/benefit and future.** A multi-week concurrency subsystem with its own
test matrix (concurrent disjoint, concurrent overlapping, scan-vs-anything,
duplicate-content thumb races, dest-collision-during-mv) for a narrow win that
SQLite's single writer caps anyway. The question largely dissolves once a
daemon exists (plan 006): a single long-lived writer serializes in-process and
can be as fine-grained as it likes, with no cross-process lock negotiation, no
fd limits, and no stale-lock concerns. If a concrete need for concurrent
disjoint mutations appears, revisit it then, likely inside the daemon.
