# phig-core Extraction, Part 2a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move all three parallel CLI commands onto the shared `parallel_for_each` + `CancelToken` primitives, delete the `g_interrupted` global, and close two data-safety defects — so Part 2b can extract engines onto a foundation that already works.

**Architecture:** Purely mechanical migration, no engines yet. Each of `cmd_scan`, `cmd_thumbs`, and `cmd_face` currently hand-rolls the same thread pool around a file-scope `SIGINT` flag. They move onto the primitives Part 1 built, the prerequisites that block Part 2b get resolved, and two defects found during Part 2b research are fixed in the code being touched anyway.

**Tech Stack:** C++20, CMake, GoogleTest. No new third-party dependencies.

**Spec:** `../specs/2026-08-01-phig-core-extraction-design.md` — see its "Found later, during Part 2 research (2026-08-03)" section, which records the verified findings this plan acts on.

## Global Constraints

- **C++20** (`CMAKE_CXX_STANDARD 20`, `CMakeLists.txt:7`). No new third-party dependencies.
- **Behavior visible to a CLI user must not change**, with exactly one deliberate exception: `phig scan --on-error fail` currently calls `std::terminate` (SIGABRT, exit 134, zero rows committed, WAL left uncheckpointed) and will instead exit cleanly with an error message and a consistent database. That change is the point of Task 3; every other observable behavior — output text, exit codes, progress format, summary lines — must be identical.
- **All 535 tests in `phig-tests` and 4 in `phig-perf-tests` must pass.** If a test encoded broken behavior, fix the test and say so explicitly rather than preserving the bug.
- **The core performs no I/O and holds no global mutable state.** Nothing moved into `src/*.cpp` may use `std::cout`, `std::cerr`, or `std::cin`. `src/log.h`'s `log_warn`/`log_error`/`log_info` go through spdlog, are thread-safe, and ARE permitted in the core.
- **`.clang-format`** at the repo root is authoritative. CI does **not** lint `src/libphig/` or `include/` (its glob is `find src tests -maxdepth 1`), so check those two manually with `--dry-run --Werror`.
- **The C ABI is frozen.** `src/libphig/` and `include/phig/phig.h` gain no surface and must keep compiling and passing. `libphig.so` must still export exactly 41 `phig_` symbols.
- **Build/test commands must be prefixed with `direnv exec .`** — dependencies come from a Nix devenv and are not on the bare PATH; a bare `cmake` fails on missing opencv4:
  - `direnv exec . cmake -B build && direnv exec . cmake --build build`
  - `direnv exec . ./build/phig-tests` (fast suite, ~1 s)
  - `direnv exec . ./build/phig-perf-tests` (~3 s)
  - `direnv exec . ctest --test-dir build` (both; what CI runs)
  - `direnv exec . clang-format -i <files>`
- **Commits are authorized on the working branch. Do NOT push to any remote and do NOT merge to master.**

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `tests/test_dates_format.cpp` | Tests for `format_time` once it moves into the core. |

**Modified files:**

| Path | Change |
|---|---|
| `src/dates.h`, `src/dates.cpp` | Gain `format_time`, moved out of `main.cpp`. |
| `src/database.h`, `src/database.cpp` | `insert_face` returns the new rowid; `flush_batch`'s transaction gap needs no `Database` change but the rowid one removes a hazard. |
| `src/main.cpp` | Three commands migrated to `parallel_for_each`; `g_interrupted` and `sigint_handler` deleted; `format_time` removed; `flush_batch` wrapped in a transaction. |
| `tests/test_database.cpp` | Coverage for `insert_face`'s return value and the batch transaction. |
| `CMakeLists.txt` | Register the new test file. |

## Interface summary

What this plan produces, for Part 2b to build on.

```cpp
// src/dates.h — moved from main.cpp:80
// Formats a filesystem timestamp as "YYYY-MM-DDTHH:MM:SSZ" (UTC).
// This is the form stored in images.modified_at.
std::string format_time(std::filesystem::file_time_type ftime);

// src/database.h — signature change
// Inserts a face row and returns its rowid. Returns the id directly so callers
// need not follow with sqlite3_last_insert_rowid(), which is only correct while
// no other statement runs on the connection in between.
int64_t insert_face(int64_t image_id, const FaceInfo& face);
```

Existing primitives, unchanged, that all three commands adopt:

```cpp
// src/cancel.h
struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel();
    bool stop() const;
};

// src/parallel.h
// Returns false if cancellation stopped the run before every item ran.
bool parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t item, int worker_id)>& fn);
```

---

### Task 1: Move `format_time` into the core

`ScanEngine` cannot compile outside `main.cpp` while it depends on a function
defined there. Smallest possible first task, and it establishes the pattern.

**Files:**
- Modify: `src/dates.h`, `src/dates.cpp`, `src/main.cpp`
- Create: `tests/test_dates_format.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `std::string format_time(std::filesystem::file_time_type)` declared in `src/dates.h`.

- [ ] **Step 1: Read the current definition**

`src/main.cpp:80-92`. It converts a `fs::file_time_type` to a UTC string via a
`file_clock`/`system_clock` offset, then `strftime` with `"%Y-%m-%dT%H:%M:%SZ"`.

This format matters beyond display: it is what gets stored in
`images.modified_at`, and `cmd_scan`'s mtime-skip check at `main.cpp:470`
compares a freshly formatted value against the stored one. A formatting change
would make every file look modified and trigger a full rescan of every
collection. Do not alter the format string or the conversion.

- [ ] **Step 2: Write the failing test**

Create `tests/test_dates_format.cpp`:

```cpp
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include "dates.h"

namespace {
// Creates a file and returns its path; removed by the caller.
std::string make_temp_file() {
    auto p = std::filesystem::temp_directory_path() /
             ("phig_fmttime_" + std::to_string(::getpid()) + ".txt");
    std::ofstream(p) << "x";
    return p.string();
}
}  // namespace

TEST(FormatTime, ProducesIso8601UtcWithZSuffix) {
    auto path = make_temp_file();
    auto ftime = std::filesystem::last_write_time(path);
    std::string s = format_time(ftime);

    // "YYYY-MM-DDTHH:MM:SSZ" — exactly 20 characters.
    EXPECT_EQ(s.size(), 20u) << s;
    EXPECT_TRUE(std::regex_match(
        s, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)")))
        << s;

    std::filesystem::remove(path);
}

TEST(FormatTime, IsStableAcrossRepeatedCalls) {
    auto path = make_temp_file();
    auto ftime = std::filesystem::last_write_time(path);
    // The mtime-skip check in scan compares a fresh format against a stored
    // one, so the same input must always produce the same string.
    EXPECT_EQ(format_time(ftime), format_time(ftime));
    std::filesystem::remove(path);
}

TEST(FormatTime, LaterFileSortsAfterEarlierFile) {
    auto a = make_temp_file();
    auto ta = std::filesystem::last_write_time(a);
    // Move one file's mtime a day forward; the formatted strings must order
    // the same way, since modified_at is compared lexicographically.
    auto tb = ta + std::chrono::hours(24);
    EXPECT_LT(format_time(ta), format_time(tb));
    std::filesystem::remove(a);
}
```

Add `#include <unistd.h>` for `::getpid()`, matching the convention in
`tests/test_database.cpp`.

- [ ] **Step 3: Register the test and run it to verify failure**

Add `tests/test_dates_format.cpp` to the `add_executable(phig-tests ...)` list in
`CMakeLists.txt`.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `format_time` is not declared in `dates.h`.

- [ ] **Step 4: Move the function**

Add to `src/dates.h`:

```cpp
#include <filesystem>

// Formats a filesystem timestamp as "YYYY-MM-DDTHH:MM:SSZ" (UTC). This is the
// form stored in images.modified_at, and scan's mtime-skip check compares a
// freshly formatted value against the stored one — changing the format would
// make every tracked file look modified.
std::string format_time(std::filesystem::file_time_type ftime);
```

Move the body from `main.cpp:80-92` into `src/dates.cpp` **verbatim** — same
clock-offset arithmetic, same `strftime` format string. Add whatever includes it
needs (`<chrono>`, `<ctime>`). Then delete it from `main.cpp` and confirm
`main.cpp` still includes `dates.h` (it should already; check).

- [ ] **Step 5: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='FormatTime.*'`
Expected: 3 tests PASS.

- [ ] **Step 6: Verify no rescan was introduced**

This is the risk that matters. Build a scratch collection, scan it, scan it
again, and confirm the second scan skips everything:

```bash
rm -rf /tmp/p2a-fmt && mkdir -p /tmp/p2a-fmt
cp tests/data/*.mp4 /tmp/p2a-fmt/ 2>/dev/null || true
direnv exec . ./build/phig scan /tmp/p2a-fmt --db /tmp/p2a-fmt/db
direnv exec . ./build/phig scan /tmp/p2a-fmt --db /tmp/p2a-fmt/db
```

Expected: the second run reports the files as skipped, not rescanned. Include the
output in your report. If the second scan reprocesses anything, `format_time`'s
output changed — stop and report BLOCKED.

- [ ] **Step 7: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . clang-format -i src/dates.h src/dates.cpp src/main.cpp tests/test_dates_format.cpp
git add src/dates.h src/dates.cpp src/main.cpp tests/test_dates_format.cpp CMakeLists.txt
git commit -m "Move format_time into the core"
```

---

### Task 2: `insert_face` returns its rowid

Removes a correctness hazard before any code moves across an abstraction
boundary.

**Files:**
- Modify: `src/database.h:195`, `src/database.cpp`, `src/main.cpp`
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `int64_t Database::insert_face(int64_t image_id, const FaceInfo& face)`.

- [ ] **Step 1: Understand the hazard**

Three call sites follow `db.insert_face(...)` with
`sqlite3_last_insert_rowid(db.db_ptr())` to get the new face's id:
`main.cpp:426`, `main.cpp:529-530`, and `main.cpp:2049`.

That is correct **only** because `db_mutex` serializes access and no other
statement runs on the connection in between. It reaches through `db_ptr()`,
bypassing the `Database` abstraction, and any future reordering or intervening
query would silently read the wrong rowid — attaching a face embedding to the
wrong person.

Having `insert_face` return the id removes the hazard structurally rather than
relying on call-ordering discipline the compiler cannot check.

- [ ] **Step 2: Write the failing test**

Add to `tests/test_database.cpp`. Match the file's existing fixture style — it
uses `TEST_F(DatabaseTest, ...)`; check how `db` and a seeded image are obtained
there and follow it rather than inventing a new setup.

```cpp
TEST_F(DatabaseTest, InsertFaceReturnsTheNewRowid) {
    ImageInfo info;
    info.path = "/photos/face1.jpg";
    info.filename = "face1.jpg";
    info.extension = "jpg";
    info.sha256 = std::string(64, 'a');
    info.modified_at = "2024-01-01T00:00:00Z";
    db.insert(info);
    int64_t image_id = db.get_image_id(info.path);
    ASSERT_GT(image_id, 0);

    FaceInfo face;
    face.x = 10;
    face.y = 20;
    face.width = 30;
    face.height = 40;
    face.embedding.fill(0.5f);

    int64_t id1 = db.insert_face(image_id, face);
    int64_t id2 = db.insert_face(image_id, face);
    EXPECT_GT(id1, 0);
    EXPECT_GT(id2, id1) << "each insert must return its own distinct rowid";
}

TEST_F(DatabaseTest, InsertFaceReturnValueMatchesLastInsertRowid) {
    ImageInfo info;
    info.path = "/photos/face2.jpg";
    info.filename = "face2.jpg";
    info.extension = "jpg";
    info.sha256 = std::string(64, 'b');
    info.modified_at = "2024-01-01T00:00:00Z";
    db.insert(info);
    int64_t image_id = db.get_image_id(info.path);

    FaceInfo face;
    face.x = 1;
    face.y = 2;
    face.width = 3;
    face.height = 4;
    face.embedding.fill(0.25f);

    // The returned id must equal what the old call-site pattern would have
    // produced, so replacing that pattern cannot change behavior.
    int64_t returned = db.insert_face(image_id, face);
    EXPECT_EQ(returned, sqlite3_last_insert_rowid(db.db_ptr()));
}
```

Check `FaceInfo`'s actual definition in `src/faces.h` and adjust the field names
if they differ — do not guess. If `embedding` is not a `std::array<float,128>`,
initialize it however that type requires.

- [ ] **Step 3: Run to verify failure**

Run: `direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `insert_face` returns `void`, so the assignments do not compile.

- [ ] **Step 4: Change the signature**

In `src/database.h:195`:

```cpp
    // Inserts a face row and returns its rowid. Returning the id directly means
    // callers need not follow with sqlite3_last_insert_rowid(), which is only
    // correct while no other statement runs on this connection in between.
    int64_t insert_face(int64_t image_id, const FaceInfo& face);
```

In `src/database.cpp`, have the implementation return
`sqlite3_last_insert_rowid(db_)` after the successful step. Keep every existing
error path and message exactly as it is.

- [ ] **Step 5: Update the three call sites**

`main.cpp:426`, `main.cpp:529-530`, `main.cpp:2049` become:

```cpp
int64_t face_id = db.insert_face(image_id, face);
db.auto_label_face(face_id, face.embedding);
```

Delete the now-unused `sqlite3_last_insert_rowid(db.db_ptr())` calls. Then check
whether `db_ptr()` still has any caller; if `main.cpp` no longer uses it, say so
in your report — do **not** remove `db_ptr()` itself, `cmd_face identify` still
needs it and that is Part 2b's concern.

- [ ] **Step 6: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='DatabaseTest.*'`
Expected: all PASS, including the two new cases.

- [ ] **Step 7: Verify faces still work end to end**

Unit tests do not prove the CLI path. If face models are available
(`~/.cache/phig/models/`), scan a directory of photos with `--faces` and confirm
`phig face list` reports detections. If models are absent, say so in your report
and note that this check was skipped — do not download models.

- [ ] **Step 8: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . clang-format -i src/database.h src/database.cpp src/main.cpp tests/test_database.cpp
git add src/database.h src/database.cpp src/main.cpp tests/test_database.cpp
git commit -m "Return the new rowid from insert_face"
```

---

### Task 3: Migrate `cmd_scan` onto `parallel_for_each`

The largest task, and the one that fixes the `std::terminate` abort.

**Files:**
- Modify: `src/main.cpp` (`cmd_scan`, lines ~324-857; `g_interrupted` at :52-53)

**Interfaces:**
- Consumes: `CancelToken` (`src/cancel.h`), `parallel_for_each` (`src/parallel.h`), `format_time` (Task 1), `insert_face` returning a rowid (Task 2).
- Produces: no new symbols. `cmd_scan` no longer references `g_interrupted`.

- [ ] **Step 1: Read what you are replacing**

`main.cpp:387-388` resets `g_interrupted` and installs a `SIGINT` handler,
saving the previous one. `:436` is `auto worker = [&](int thread_id)`, whose loop
condition at `:439` is `while (!g_interrupted)` with a
`next_file.fetch_add(1)` work cursor. `:629` restores the handler. `:631` reads
the flag to decide whether to print the interrupted message.

`parallel_for_each(count, num_threads, cancel, fn)` replaces the cursor, the
threads, and the join. It returns `false` if cancellation stopped the run early,
which is what `:631` needs. Its callback signature is
`fn(size_t item, int worker_id)` — the `worker_id` replaces `thread_id`, and
`cmd_scan` uses it at `:437` to index `face_detectors`.

- [ ] **Step 2: Keep the signal handler in the CLI, not the core**

A `CancelToken` cannot install a signal handler — it is a flag. The CLI still
owns SIGINT; it just sets a token instead of a global.

Replace the file-scope `g_interrupted` and `sigint_handler` (`:52-53`) with a
CLI-local mechanism. A handler must be a plain function with C linkage, so it
needs a file-scope pointer to the active token:

```cpp
namespace {
// The CLI installs a SIGINT handler that cancels whichever operation is
// running. A signal handler cannot take arguments, so the active token is
// reachable through this pointer. Only main.cpp touches it; the core takes a
// CancelToken by reference and knows nothing about signals.
std::atomic<CancelToken*> g_active_cancel{nullptr};

extern "C" void cli_sigint_handler(int) {
    CancelToken* t = g_active_cancel.load(std::memory_order_relaxed);
    if (t != nullptr) t->cancel();
}

// Installs the handler for the duration of the scope and restores it after.
class ScopedInterruptHandler {
   public:
    explicit ScopedInterruptHandler(CancelToken& token) {
        g_active_cancel.store(&token, std::memory_order_relaxed);
        prev_ = std::signal(SIGINT, cli_sigint_handler);
    }
    ~ScopedInterruptHandler() {
        std::signal(SIGINT, prev_);
        g_active_cancel.store(nullptr, std::memory_order_relaxed);
    }
    ScopedInterruptHandler(const ScopedInterruptHandler&) = delete;
    ScopedInterruptHandler& operator=(const ScopedInterruptHandler&) = delete;

   private:
    void (*prev_)(int) = nullptr;
};
}  // namespace
```

Note `CancelToken::cancel()` only stores to an `atomic<bool>`, which is
async-signal-safe. Do not add anything else to the handler.

- [ ] **Step 3: Convert the worker**

The existing `worker` lambda's body becomes `parallel_for_each`'s callback nearly
unchanged. Replace:

```cpp
    auto worker = [&](int thread_id) {
        while (!g_interrupted) {
            size_t idx = next_file.fetch_add(1);
            if (idx >= files.size()) break;
            // ... body ...
        }
    };
    // ... thread spawn/join ...
```

with:

```cpp
    CancelToken cancel;
    ScopedInterruptHandler interrupt_guard(cancel);

    bool completed = parallel_for_each(
        files.size(), num_threads, cancel, [&](size_t idx, int worker_id) {
            // ... the same body, with thread_id renamed worker_id ...
        });
```

Delete `next_file` — `parallel_for_each` owns the cursor. Keep every line of the
body otherwise identical, including the `db_mutex` locking, the three-way
skip/`needs_faces_only`/`needs_thumbs_only` branching, and the progress printing.

Where the body previously did `if (g_interrupted) return;` or similar
mid-iteration checks, use `cancel.stop()`.

- [ ] **Step 4: Handle the interrupted summary and the fail-on-error path**

`:631`'s `if (g_interrupted)` becomes `if (!completed)`.

The `fail_on_error` branch at `:601-603` currently rethrows from inside the
worker. Keep the `throw;` — `parallel_for_each` now captures it, abandons
remaining work, joins every thread, and rethrows on the calling thread. Wrap the
`parallel_for_each` call so the exception surfaces as a clean CLI error:

```cpp
    bool completed = false;
    try {
        completed = parallel_for_each(...);
    } catch (const std::exception& e) {
        // --on-error fail: flush what completed, then report and exit non-zero.
        // Previously this exception escaped a std::thread lambda and called
        // std::terminate, aborting mid-write with an uncheckpointed WAL.
        {
            std::lock_guard<std::mutex> lock(db_mutex);
            flush_batch();
        }
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
```

**This is the one intended behavior change in the plan.** Before: SIGABRT, exit
134, zero rows committed. After: the completed batch is flushed, a message is
printed, exit 1.

- [ ] **Step 5: Fix `flush_batch`'s transaction gap**

`flush_batch` (`:405-434`) calls `db.insert_batch(infos)` then loops inserting
faces per row. If any insert throws mid-loop, some rows are committed and others
are not, `batch.clear()` at `:433` never runs, and nothing rolls back. The
`needs_faces_only` path at `:523-539` wraps the same kind of work in an explicit
transaction — this makes the two consistent.

```cpp
        auto flush_batch = [&]() {
            if (batch.empty()) return;
            std::vector<ImageInfo> infos;
            infos.reserve(batch.size());
            for (auto& r : batch) infos.push_back(r.info);

            // One transaction for the images and their faces: a throw partway
            // through the face loop would otherwise leave some rows committed
            // and others not, with batch never cleared.
            db.begin_transaction();
            try {
                db.insert_batch(infos);
                for (auto& r : batch) {
                    if (r.faces.empty()) continue;
                    int64_t image_id = db.get_image_id(r.info.path);
                    // Rows whose bytes didn't change kept their existing faces
                    // (and person labels) through insert(); re-adding this
                    // run's detections would duplicate them. insert() cleared
                    // faces when the content changed, so has_faces == false
                    // means these detections are needed. `face rebuild` is the
                    // path for forced re-detection.
                    if (image_id >= 0 && !db.has_faces(r.info.path)) {
                        for (const auto& face : r.faces) {
                            int64_t face_id = db.insert_face(image_id, face);
                            db.auto_label_face(face_id, face.embedding);
                        }
                        face_count.fetch_add(r.faces.size());
                    }
                }
                db.commit_transaction();
            } catch (...) {
                db.rollback_transaction();
                throw;
            }
            batch.clear();
        };
```

Note the `needs_faces_only` path at `:519` calls `flush_batch()` while already
holding `db_mutex` — verify that path does not now open a nested transaction. If
`Database::begin_transaction` cannot nest, and `:523` opens one after calling
`flush_batch` at `:519`, the ordering is fine (flush commits before `:523`
begins) — but confirm it by reading, and say what you found.

- [ ] **Step 6: Write the tests**

Two behaviors need coverage. Add to `tests/test_database.cpp`:

```cpp
TEST_F(DatabaseTest, RollbackLeavesNoPartialBatch) {
    // A transaction that inserts images then throws must leave the DB as it
    // was — this is the guarantee flush_batch's transaction provides.
    ImageInfo a;
    a.path = "/photos/batch_a.jpg";
    a.filename = "batch_a.jpg";
    a.extension = "jpg";
    a.sha256 = std::string(64, 'c');
    a.modified_at = "2024-01-01T00:00:00Z";

    int64_t before = db.count();
    db.begin_transaction();
    try {
        db.insert(a);
        throw std::runtime_error("simulated mid-batch failure");
    } catch (const std::runtime_error&) {
        db.rollback_transaction();
    }
    EXPECT_EQ(db.count(), before) << "rolled-back insert must not persist";
}
```

The `--on-error fail` behavior is a CLI-level concern and cannot be unit-tested
against `Database`. Verify it end to end in Step 8 instead.

- [ ] **Step 7: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests`
Expected: all PASS. If a scan-related test fails, the migration changed behavior
— investigate rather than adjusting the test.

- [ ] **Step 8: Verify the CLI end to end**

Three checks, all with recorded output in your report.

**(a) A normal scan is unchanged.** Build a scratch collection, scan it with the
pre-change binary (build `HEAD~1` in a git worktree) and the new one, and diff
the output. Expect identical text.

**(b) Ctrl-C still works.** Start a scan over enough files to take a second,
send `SIGINT`, and confirm it stops at a file boundary, prints the interrupted
message, and leaves a readable database (`phig search --count` succeeds).

**(c) `--on-error fail` no longer aborts.** This is the deliberate change.
Create a directory containing a deliberately corrupt image file (a text file
renamed `.jpg` will do), scan it with `--on-error fail`, and confirm:
- exit code is 1, **not** 134
- an error message names the file
- the database is readable afterward and contains any files processed before the
  failure

Record the before and after exit codes. If you can build the pre-change binary,
show that it exits 134 and the new one exits 1.

- [ ] **Step 9: Run everything and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . ctest --test-dir build
direnv exec . clang-format -i src/main.cpp tests/test_database.cpp
git add src/main.cpp tests/test_database.cpp
git commit -m "Migrate cmd_scan to parallel_for_each and fix the flush_batch transaction gap"
```

---

### Task 4: Migrate `cmd_thumbs` and `cmd_face`, delete `g_interrupted`

**Files:**
- Modify: `src/main.cpp` (`cmd_thumbs` ~1589-1815, `cmd_face` ~1815-2103)

**Interfaces:**
- Consumes: `CancelToken`, `parallel_for_each`, `ScopedInterruptHandler` (Task 3).
- Produces: no new symbols. `g_interrupted` and `sigint_handler` no longer exist.

- [ ] **Step 1: Migrate `cmd_thumbs rebuild`**

Its worker (`main.cpp:1662`) is the simplest of the three — a flat skip check and
a `generate_thumb` call, with no DB writes at all. Its handler install/restore is
at `:1654-1655` and `:1715`.

Replace the same way Task 3 did: a local `CancelToken`, a
`ScopedInterruptHandler`, and `parallel_for_each`. Its worker takes `[&](int)`
today — it does not use the thread id, so the new callback ignores `worker_id`.

Keep the progress format exactly (`\r\033[K[done/total] parent/filename`), the
counters, and the summary.

- [ ] **Step 2: Migrate `cmd_face rebuild`**

Its worker (`main.cpp:2026`) takes `int thread_id` and indexes `detectors[thread_id]`,
so it needs `parallel_for_each`'s `worker_id`. Handler install/restore is at
`:2068-2069` and `:2078`.

Keep its per-item transaction (`begin_transaction`, delete-then-insert faces,
`commit_transaction`, rollback on exception) exactly as it is. Unlike scan, this
command has no batching and no skip logic — it unconditionally reprocesses every
selected image.

- [ ] **Step 3: Delete the global**

`g_interrupted` and `sigint_handler` at `main.cpp:52-53` now have no readers.
Delete both. Grep to confirm:

```bash
grep -n "g_interrupted\|sigint_handler" src/ tests/ || echo "clean"
```

Expected: no matches outside `cli_sigint_handler` from Task 3.

- [ ] **Step 4: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests`
Expected: all PASS.

- [ ] **Step 5: Verify both commands end to end**

Build a scratch collection with a few images. For each of `phig thumbs rebuild`
and `phig face rebuild` (the latter only if face models are present — if absent,
say so and skip):
- run it and confirm the output format and summary match the pre-change binary
- interrupt it with `SIGINT` mid-run and confirm it stops cleanly, prints the
  interrupted message, and leaves a readable database

Record the output comparisons in your report.

- [ ] **Step 6: Confirm all three commands now share one mechanism**

```bash
grep -c "parallel_for_each" src/main.cpp
```

Expected: 3. Report the number. Also confirm `std::thread` no longer appears in
`main.cpp`:

```bash
grep -n "std::thread" src/main.cpp || echo "no raw threads left"
```

- [ ] **Step 7: Run everything and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . ctest --test-dir build
direnv exec . clang-format -i src/main.cpp
git add src/main.cpp
git commit -m "Migrate cmd_thumbs and cmd_face to parallel_for_each; delete g_interrupted"
```

---

## Plan self-review

**Spec coverage.** This plan implements the prerequisites and defect fixes the
spec's Part 2 research section records: `format_time` into the core (Task 1),
`insert_face` returning a rowid (Task 2), the `flush_batch` transaction gap and
the `--on-error fail` abort (Task 3), and migrating all three commands off
`g_interrupted` (Tasks 3-4).

**Deferred to Part 2b**, matching the spec's sequencing: `ScanEngine`,
`ThumbsEngine`, `FacesEngine`; `cmd_face identify`'s inline SQL through
`db_ptr()`; the unchecked 128-float blob reads at `database.cpp:1811` and
`main.cpp:2043/2064`; `remove_exec.cpp`'s `std::cin` prompt; and moving progress
output behind a callback. Part 2a deliberately leaves progress printing in
`main.cpp` — the engines are what need a pluggable sink, and inventing one before
they exist would be speculative.

**Three things an implementer should know:**
- **Task 1 carries a rescan risk out of proportion to its size.** `format_time`'s
  output is stored in `images.modified_at` and compared against a fresh value on
  every scan. A formatting change makes every file look modified. Step 6 exists
  to catch that and is not optional.
- **Task 3 contains the plan's only intended behavior change.** `--on-error fail`
  goes from SIGABRT/exit 134 to a clean error and exit 1. Every other observable
  behavior must be identical, and Step 8 verifies that by diffing against a
  pre-change binary.
- **A `CancelToken` cannot install a signal handler.** The CLI keeps owning
  SIGINT; Task 3's `ScopedInterruptHandler` is the bridge. Only
  `CancelToken::cancel()` — a single atomic store — runs in the handler, because
  that is what is async-signal-safe.

**Type consistency check.** `format_time`'s signature (Task 1) matches its use at
`main.cpp:464`. `insert_face`'s new `int64_t` return (Task 2) matches the three
call sites updated in the same task and the pattern used again in Task 3's
`flush_batch` rewrite. `parallel_for_each`'s callback is
`fn(size_t item, int worker_id)` throughout — the worker-id parameter added at the
end of Part 1 specifically so `cmd_scan` and `cmd_face` could index their
per-worker detector arrays, which Tasks 3 and 4 both rely on. `CancelToken` and
`ScopedInterruptHandler` are used identically in Tasks 3 and 4.
