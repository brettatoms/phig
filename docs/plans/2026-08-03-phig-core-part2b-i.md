# phig-core Extraction, Part 2b-i Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the progress queue, clear the three remaining prerequisites, and extract `cmd_scan`'s 438 lines into a `ScanEngine` in the core — so scan's orchestration is testable and callable from a library for the first time.

**Architecture:** A small bounded `ProgressQueue<T>` lets an engine publish progress without ever invoking caller code, which is what keeps client callbacks out of the database lock. Three prerequisites land first (`Database` methods for the raw SQL `cmd_face identify` uses, blob length checks, and `remove_exec`'s prompt as a callback), then `ScanOptions`/`ProcessResult`/`process_file` move to the core and `ScanEngine` takes over the orchestration. `main.cpp` keeps argv parsing, output rendering, and signal handling.

**Tech Stack:** C++20, CMake, GoogleTest. No new third-party dependencies.

**Spec:** `../specs/2026-08-01-phig-core-extraction-design.md` — see "Part 2b decisions and research (2026-08-03, after Part 2a merged)".

## Global Constraints

- **C++20** (`CMAKE_CXX_STANDARD 20`). No new third-party dependencies. The progress queue is written rather than taken as a dependency — phig's posture is deliberately austere (plan 018; `inih` vendored at ~400 lines; a TOML library rejected during the settings work for being large relative to its benefit).
- **Behavior visible to a CLI user must not change.** Output text, progress format, summary lines, exit codes, and skip decisions must all be identical. There is no sanctioned behavior change in this plan.
- **All 543 tests in `phig-tests` and 4 in `phig-perf-tests` must pass.** If a test encoded broken behavior, fix the test and say so explicitly rather than preserving the bug.
- **The core performs no I/O.** Nothing in `src/*.cpp` may use `std::cout`, `std::cerr`, or `std::cin`. `src/log.h`'s `log_warn`/`log_error`/`log_info` go through spdlog, are thread-safe, and ARE permitted.
- **Progress must be emitted outside every lock.** `main.cpp:511` currently prints while holding `db_mutex` (the guard spans `:491-515`). With `std::cout` that is poor practice; with a client callback it would mean invoking GUI code under the database lock. The queue makes this structurally impossible, but the emit site still has to move.
- **`.clang-format`** is authoritative. CI does **not** lint `src/libphig/` or `include/` (its glob is `find src tests -maxdepth 1`), so check those manually with `--dry-run --Werror`.
- **The C ABI is frozen.** `src/libphig/` and `include/phig/phig.h` gain no surface. `libphig.so` must still export exactly 41 `phig_` symbols.
- **Build/test commands must be prefixed with `direnv exec .`** — dependencies come from a Nix devenv; a bare `cmake` fails on missing opencv4:
  - `direnv exec . cmake -B build && direnv exec . cmake --build build`
  - `direnv exec . ./build/phig-tests` (fast suite, ~1 s)
  - `direnv exec . ./build/phig-perf-tests` (~3 s)
  - `direnv exec . ctest --test-dir build` (both; what CI runs)
  - `direnv exec . clang-format -i <files>`
- **The CLI's flag order is `phig <subcommand> <args> --db <path>`.** `phig --db <path> scan <dir>` fails with "Unknown command: --db". The fail-fast flag is `--on-error fail`, not `--fail-on-error`.
- **Clean up after yourself.** A previous agent left a scan running at 1041% CPU and 287 MB of fixtures in `/tmp`. Keep scratch collections under ~50 files, remove them when done, and never leave a background process running.
- **Commits are authorized on the working branch. Do NOT push to any remote and do NOT merge to master.**

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `src/progress_queue.h` | `ProgressQueue<T>` — bounded, producer never blocks, closeable. Header-only. |
| `src/scan_engine.h` | `ForceOptions`, `ScanOptions`, `ProcessResult`, `ScanProgress`, `ScanResult`, `ScanEngine`. |
| `src/scan_engine.cpp` | `process_file` and `ScanEngine`'s implementation. |
| `tests/test_progress_queue.cpp` | Queue semantics: ordering, drop policy, close, blocking, threading. |
| `tests/test_scan_engine.cpp` | Skip/rescan decisions, batching, cancellation, error accounting. |

**Modified files:**

| Path | Change |
|---|---|
| `src/database.h`, `src/database.cpp` | Two new methods replacing `cmd_face identify`'s raw SQL; blob length checks. |
| `src/remove_exec.h`, `src/remove_exec.cpp` | Prompt becomes an injected `ConfirmFn`. |
| `src/main.cpp` | `cmd_scan` becomes argv + rendering over `ScanEngine`; supplies the confirm callback; loses the moved types. |
| `tests/test_remove_exec.cpp` | Pass a confirm callback. |
| `CMakeLists.txt` | New sources and test files. |

## Interface summary

Names later tasks and Part 2b-ii depend on.

```cpp
// src/progress_queue.h
template <typename T>
class ProgressQueue {
   public:
    explicit ProgressQueue(size_t capacity = 256);
    void push(T v);        // never blocks; drops the OLDEST when full
    bool pop(T& out);      // blocks until an item is available or the queue closes;
                           // returns false only when closed AND drained
    bool try_pop(T& out);  // non-blocking; false if empty
    void close();          // wakes every waiter
    bool closed() const;
    size_t dropped() const;  // count of items discarded by the drop policy
};

// src/database.h — new methods (replacing cmd_face identify's raw SQL)
struct FaceEmbeddingRow {
    int64_t face_id;
    std::array<float, 128> embedding;
};
std::vector<FaceEmbeddingRow> get_face_embeddings_for_person(int64_t person_id);
std::vector<FaceEmbeddingRow> get_unlabeled_face_embeddings();

// src/remove_exec.h
using ConfirmFn = std::function<bool(const std::string& prompt)>;

// src/scan_engine.h
struct ScanProgress {
    size_t done = 0, total = 0;
    size_t processed = 0, skipped = 0, errors = 0;
    std::string path;      // full path of the file just finished; empty on a skip-only tick
    bool show_path = false;  // false reproduces the CLI's terse skip line
};

struct ScanResult {
    size_t processed = 0, skipped = 0, errors = 0;
    size_t images = 0, videos = 0, faces = 0, thumbs = 0;
    size_t removed = 0, relinked = 0, demoted = 0;
    bool completed = true;       // false if cancellation stopped it early
    bool mount_warning = false;  // every known row vanished; cleanup was skipped
};

class ScanEngine {
   public:
    ScanEngine(const ScanOptions& opts, Database& db);
    ScanResult run(const CancelToken& cancel, ProgressQueue<ScanProgress>* progress);
};
```

---

### Task 1: `ProgressQueue<T>`

The primitive every engine publishes through. Built and proven before anything
uses it, the same sequencing that worked for `parallel_for_each` in Part 1.

**Files:**
- Create: `src/progress_queue.h`, `tests/test_progress_queue.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `ProgressQueue<T>` (signature above).

- [ ] **Step 1: Write the failing tests**

Create `tests/test_progress_queue.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include "progress_queue.h"

TEST(ProgressQueue, PopReturnsItemsInPushOrder) {
    ProgressQueue<int> q(16);
    q.push(1);
    q.push(2);
    q.push(3);
    int v = 0;
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 3);
}

TEST(ProgressQueue, TryPopIsFalseWhenEmpty) {
    ProgressQueue<int> q(4);
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    q.push(7);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 7);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(ProgressQueue, PushNeverBlocksAndDropsOldestWhenFull) {
    ProgressQueue<int> q(3);
    for (int i = 0; i < 10; ++i) q.push(i);  // must not block
    EXPECT_EQ(q.dropped(), 7u);

    // The three most recent survive, in order.
    int v = 0;
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 7);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 8);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 9);
}

TEST(ProgressQueue, CloseDrainsRemainingItemsThenReturnsFalse) {
    ProgressQueue<int> q(8);
    q.push(1);
    q.push(2);
    q.close();
    int v = 0;
    // Closing must not discard what was already queued.
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v)); EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.pop(v));
    EXPECT_TRUE(q.closed());
}

TEST(ProgressQueue, PopOnAnEmptyClosedQueueReturnsFalseImmediately) {
    ProgressQueue<int> q(4);
    q.close();
    int v = 0;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(q.pop(v));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(ms, 500) << "pop() on a closed queue must not block";
}

TEST(ProgressQueue, CloseWakesABlockedConsumer) {
    ProgressQueue<int> q(4);
    std::atomic<bool> returned{false};
    std::thread consumer([&] {
        int v = 0;
        q.pop(v);  // blocks: queue is empty and open
        returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(returned.load()) << "pop() should still be blocked";
    q.close();
    consumer.join();
    EXPECT_TRUE(returned.load());
}

TEST(ProgressQueue, PushAfterCloseIsANoOp) {
    ProgressQueue<int> q(4);
    q.close();
    q.push(42);  // must not throw, must not enqueue
    int v = 0;
    EXPECT_FALSE(q.pop(v));
}

TEST(ProgressQueue, ManyProducersOneConsumerLosesNothingUnderCapacity) {
    // 4 producers x 50 items = 200, capacity 1024, so nothing is dropped and
    // the consumer must observe every value exactly once.
    ProgressQueue<int> q(1024);
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < 50; ++i) q.push(p * 100 + i);
        });
    }
    std::set<int> seen;
    std::thread consumer([&] {
        int v = 0;
        while (q.pop(v)) seen.insert(v);
    });
    for (auto& t : producers) t.join();
    q.close();
    consumer.join();

    EXPECT_EQ(q.dropped(), 0u);
    EXPECT_EQ(seen.size(), 200u);
}

TEST(ProgressQueue, SurvivesAProducerFasterThanItsConsumer) {
    // The real shape: a fast producer and a slow consumer with a small queue.
    // Nothing may block or crash; some items are dropped and that is correct.
    ProgressQueue<int> q(8);
    std::atomic<size_t> consumed{0};
    std::thread consumer([&] {
        int v = 0;
        while (q.pop(v)) {
            consumed.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    for (int i = 0; i < 5000; ++i) q.push(i);
    q.close();
    consumer.join();

    EXPECT_GT(consumed.load(), 0u);
    EXPECT_LE(consumed.load(), 5000u);
    EXPECT_EQ(consumed.load() + q.dropped(), 5000u)
        << "every pushed item was either consumed or counted as dropped";
}
```

- [ ] **Step 2: Register the test and run it to verify failure**

Add `tests/test_progress_queue.cpp` to the `add_executable(phig-tests ...)` list.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `progress_queue.h` does not exist.

- [ ] **Step 3: Write `src/progress_queue.h`**

```cpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// A bounded queue for publishing progress from a worker to a consumer.
//
// The producer never blocks: when the queue is full the OLDEST item is dropped,
// because a progress update nobody has read yet is worthless once a newer one
// exists. `dropped()` reports how many were discarded so a caller can tell the
// difference between "nothing happened" and "I could not keep up".
//
// Why a queue rather than a callback: a callback runs on the producer's thread,
// so an engine holding a database lock would invoke arbitrary caller code while
// holding it. Here the consumer pulls on its own thread and no engine code ever
// runs in caller context.
template <typename T>
class ProgressQueue {
   public:
    explicit ProgressQueue(size_t capacity = 256) : capacity_(capacity ? capacity : 1) {}

    ProgressQueue(const ProgressQueue&) = delete;
    ProgressQueue& operator=(const ProgressQueue&) = delete;

    // Never blocks. Dropped silently (and counted) if the queue is full, and
    // ignored entirely once closed.
    void push(T v) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (closed_) return;
            if (items_.size() >= capacity_) {
                items_.pop_front();
                ++dropped_;
            }
            items_.push_back(std::move(v));
        }
        cv_.notify_one();
    }

    // Blocks until an item is available or the queue is closed and drained.
    // Returns false only in the latter case.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) return false;  // closed and drained
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    // Wakes every waiter. Items already queued stay readable, so a consumer
    // draining after close() still sees them.
    void close() {
        {
            std::lock_guard<std::mutex> lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(m_);
        return closed_;
    }

    size_t dropped() const {
        std::lock_guard<std::mutex> lock(m_);
        return dropped_;
    }

   private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> items_;
    size_t capacity_;
    bool closed_ = false;
    size_t dropped_ = 0;
};
```

- [ ] **Step 4: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='ProgressQueue.*'`
Expected: 9 tests PASS.

- [ ] **Step 5: Verify under ThreadSanitizer**

The queue is a concurrency primitive; the test suite alone is not evidence. Build
a separate TSan binary and run the threaded cases:

```bash
direnv exec . g++ -std=c++20 -fsanitize=thread -g -Isrc -Itests \
  tests/test_progress_queue.cpp -lgtest -lgtest_main -pthread -o /tmp/pq_tsan
/tmp/pq_tsan
rm -f /tmp/pq_tsan
```

Expected: all pass, zero TSan reports. If gtest is not directly linkable that
way, write a small standalone `main()` exercising the two threaded tests instead
and say what you did. **Report the actual TSan output** — "no warnings" without
output is not evidence.

- [ ] **Step 6: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . clang-format -i src/progress_queue.h tests/test_progress_queue.cpp
git add src/progress_queue.h tests/test_progress_queue.cpp CMakeLists.txt
git commit -m "Add a bounded progress queue for engine-to-caller updates"
```

---

### Task 2: `Database` methods for face embeddings, with length-checked blobs

Clears two prerequisites at once: `cmd_face identify`'s raw SQL, and three
unchecked 128-float blob reads.

**Files:**
- Modify: `src/database.h`, `src/database.cpp`, `src/main.cpp`
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `FaceEmbeddingRow`, `Database::get_face_embeddings_for_person`, `Database::get_unlabeled_face_embeddings` (signatures above).

- [ ] **Step 1: Read what you are replacing**

`src/main.cpp:1947-1997` runs two raw prepared statements against
`db.db_ptr()`, bypassing `Database` entirely:
- `:1947-1968` — `SELECT f.id, f.embedding FROM faces f WHERE f.person_id = ?`, collecting reference embeddings for a named person
- `:1972-1997` — `SELECT id, embedding FROM faces WHERE person_id IS NULL`, scanning untagged faces, computing `embedding_distance` against each reference and calling `db.set_face_person` on a match within 0.6

Three sites read 128 floats from a blob with only a null check and no length
check: `src/database.cpp:1813` (in `auto_label_face`), `src/main.cpp:1962`, and
`src/main.cpp:1985`. A truncated `faces.embedding` reads out of bounds.

- [ ] **Step 2: Write the failing tests**

Add to `tests/test_database.cpp`. Follow the file's existing `TEST_F(DatabaseTest, ...)`
fixture style — read how it obtains `db` and seeds an image first; gtest rejects
mixing `TEST` and `TEST_F` for one suite name.

```cpp
TEST_F(DatabaseTest, GetFaceEmbeddingsForPersonReturnsOnlyThatPersonsFaces) {
    db->insert(make_image("/test/p1.jpg", "shaP1"));
    int64_t image_id = db->get_image_id("/test/p1.jpg");

    FaceInfo a{};
    a.x = 1; a.y = 1; a.width = 10; a.height = 10;
    a.embedding.fill(0.1f);
    FaceInfo b{};
    b.x = 2; b.y = 2; b.width = 10; b.height = 10;
    b.embedding.fill(0.2f);

    int64_t fa = db->insert_face(image_id, a);
    int64_t fb = db->insert_face(image_id, b);

    int64_t person = db->get_or_create_person("Alice");
    db->set_face_person(fa, person);
    // fb deliberately left unlabeled.

    auto rows = db->get_face_embeddings_for_person(person);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].face_id, fa);
    EXPECT_FLOAT_EQ(rows[0].embedding[0], 0.1f);

    auto unlabeled = db->get_unlabeled_face_embeddings();
    ASSERT_EQ(unlabeled.size(), 1u);
    EXPECT_EQ(unlabeled[0].face_id, fb);
    EXPECT_FLOAT_EQ(unlabeled[0].embedding[0], 0.2f);
}

TEST_F(DatabaseTest, EmbeddingQueriesSkipRowsWithATruncatedBlob) {
    db->insert(make_image("/test/p2.jpg", "shaP2"));
    int64_t image_id = db->get_image_id("/test/p2.jpg");

    FaceInfo good{};
    good.x = 1; good.y = 1; good.width = 10; good.height = 10;
    good.embedding.fill(0.5f);
    int64_t good_id = db->insert_face(image_id, good);

    // Corrupt one row's embedding to 4 bytes. Reading 128 floats from it would
    // run 508 bytes past the end of the blob.
    {
        sqlite3_stmt* st = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db->db_ptr(),
                                     "INSERT INTO faces (image_id, embedding, x, y, width, height) "
                                     "VALUES (?, ?, 0, 0, 1, 1)",
                                     -1, &st, nullptr),
                  SQLITE_OK);
        sqlite3_bind_int64(st, 1, image_id);
        const char junk[4] = {1, 2, 3, 4};
        sqlite3_bind_blob(st, 2, junk, sizeof(junk), SQLITE_TRANSIENT);
        ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
        sqlite3_finalize(st);
    }

    // The truncated row must be skipped, not read past.
    auto rows = db->get_unlabeled_face_embeddings();
    ASSERT_EQ(rows.size(), 1u) << "the short-blob row must be skipped";
    EXPECT_EQ(rows[0].face_id, good_id);
}
```

Check `get_or_create_person` and `set_face_person`'s exact signatures in
`src/database.h` before relying on them; adjust if they differ.

- [ ] **Step 3: Run to verify failure**

Run: `direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — the two new methods do not exist.

- [ ] **Step 4: Add the methods**

In `src/database.h`:

```cpp
    // One face's id and its embedding, read only when the stored blob is
    // exactly 128 floats — a short blob means a corrupt or partially written
    // row, and reading 128 floats from it would run off the end.
    struct FaceEmbeddingRow {
        int64_t face_id;
        std::array<float, 128> embedding;
    };

    // Reference embeddings for a named person, and every face with no person
    // assigned. Rows whose embedding blob is the wrong size are skipped.
    std::vector<FaceEmbeddingRow> get_face_embeddings_for_person(int64_t person_id);
    std::vector<FaceEmbeddingRow> get_unlabeled_face_embeddings();
```

Implement both in `src/database.cpp` using the same SQL the raw statements used,
and gate every blob read on the length:

```cpp
        const void* data = sqlite3_column_blob(stmt, 1);
        int bytes = sqlite3_column_bytes(stmt, 1);
        if (data == nullptr || bytes != static_cast<int>(128 * sizeof(float))) continue;
```

Apply the same guard at `src/database.cpp:1813` in `auto_label_face` — it reads
`sqlite3_column_blob(stmt, 0)` with only a null check.

Match the file's existing error handling: throw `std::runtime_error` with the
same message shape used by neighbouring methods on a prepare failure, and
finalize the statement on every path.

- [ ] **Step 5: Replace the raw SQL in `cmd_face identify`**

Rewrite `main.cpp:1947-1997` to call the two new methods. The matching logic —
`embedding_distance` against each reference, `db.set_face_person` on a match
within 0.6, and the counter it prints — stays exactly as it is. Only the data
access changes. Confirm `db_ptr()` has no remaining callers in `main.cpp`
afterward and report what you find; do **not** remove `db_ptr()` itself.

- [ ] **Step 6: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='DatabaseTest.*'`
Expected: all PASS, including the two new cases.

- [ ] **Step 7: Verify the length guard actually guards**

Prove the new check has teeth: temporarily remove the `bytes != 512` condition,
rebuild, and run `DatabaseTest.EmbeddingQueriesSkipRowsWithATruncatedBlob` under
AddressSanitizer. It should report a heap-buffer-overflow. Restore the condition,
confirm `git diff src/database.cpp` is clean, and report what ASan said.

```bash
direnv exec . cmake -B /tmp/b-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -g" >/dev/null
direnv exec . cmake --build /tmp/b-asan --target phig-tests 2>&1 | tail -2
/tmp/b-asan/phig-tests --gtest_filter='DatabaseTest.EmbeddingQueries*'
rm -rf /tmp/b-asan
```

- [ ] **Step 8: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . clang-format -i src/database.h src/database.cpp src/main.cpp tests/test_database.cpp
git add src/database.h src/database.cpp src/main.cpp tests/test_database.cpp
git commit -m "Add length-checked face-embedding queries; drop cmd_face identify's raw SQL"
```

---

### Task 3: `remove_exec`'s prompt becomes a callback

The core currently blocks on stdin. This is the precedent that has to go before
more code moves in.

**Files:**
- Modify: `src/remove_exec.h`, `src/remove_exec.cpp`, `src/main.cpp`
- Test: `tests/test_remove_exec.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `ConfirmFn` in `src/remove_exec.h`.

- [ ] **Step 1: Read what you are replacing**

`src/remove_exec.cpp:126` does `std::getline(std::cin, answer)` after printing
`"Type 'yes' to proceed: "` at `:124`. `remove_exec.cpp` is compiled into
`phig-core`, so the core blocks on stdin today. Read the surrounding function to
see exactly when the prompt fires and what it does with a "no".

Note `remove_exec.cpp` also has 13 `std::cout`/`std::cerr` calls. **Leave those
alone** — this task changes only the interactive prompt. The output-sink question
belongs with the engines, and widening this task would obscure the one change
that matters.

- [ ] **Step 2: Write the failing test**

Add to `tests/test_remove_exec.cpp`, following its existing style:

```cpp
TEST(RemoveExec, ConfirmCallbackRefusalCancelsTheOperation) {
    // A caller that declines must leave everything untouched.
    bool asked = false;
    ConfirmFn decline = [&](const std::string& prompt) {
        asked = true;
        EXPECT_FALSE(prompt.empty()) << "the callback should receive the prompt text";
        return false;
    };
    // ... build the same fixture the file's other broad-removal tests use, then
    // invoke the removal entry point with `decline` and assert nothing was
    // removed and `asked` is true.
}

TEST(RemoveExec, ConfirmCallbackAcceptanceProceeds) {
    ConfirmFn accept = [](const std::string&) { return true; };
    // ... same fixture; invoke with `accept` and assert the removal happened.
}
```

**Read the existing tests in this file first** and mirror how they build a
fixture and call the entry point — the exact function name and signature are
there, and inventing a different shape would not compile. Fill in the fixture
from those tests rather than writing new setup.

- [ ] **Step 3: Run to verify failure**

Run: `direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `ConfirmFn` does not exist.

- [ ] **Step 4: Add the callback**

In `src/remove_exec.h`:

```cpp
#include <functional>
#include <string>

// Asks the caller to confirm a destructive operation. Returns true to proceed.
// The core cannot prompt: a library has no terminal, and a GUI needs a dialog.
// The CLI supplies a stdin prompt; a GUI supplies its own confirmation.
using ConfirmFn = std::function<bool(const std::string& prompt)>;
```

Add a `ConfirmFn confirm` parameter to the removal entry point. Replace the
`std::cout` prompt and `std::getline` at `:124-126` with a call to it, passing
the same prompt text. Preserve the existing semantics exactly: what counted as
acceptance before (the string `"yes"`) must be what the CLI's callback checks
for, so behavior is unchanged from a user's point of view.

- [ ] **Step 5: Supply the callback from the CLI**

In `main.cpp`, pass a `ConfirmFn` that reproduces the old behavior verbatim —
print the prompt to `std::cout`, read a line from `std::cin`, and return whether
it equals `"yes"`. The user-visible interaction must be byte-identical.

- [ ] **Step 6: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='RemoveExec*'`
Expected: all PASS, including the two new cases.

- [ ] **Step 7: Verify the CLI prompt is unchanged**

Create a small scratch collection (under 50 files), scan it, and run a broad
`phig rm` that triggers the confirmation. Check that the prompt text, the
accept-on-"yes" behavior, and the refuse-on-anything-else behavior all match what
they were. Test both answers. Record the actual terminal interaction in your
report, then remove the scratch directory.

- [ ] **Step 8: Confirm the core no longer reads stdin**

```bash
grep -n "std::cin" src/*.cpp src/*/*.cpp
```

Expected: no hits in `src/remove_exec.cpp`. `main.cpp` still has its own, which
is correct.

- [ ] **Step 9: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . clang-format -i src/remove_exec.h src/remove_exec.cpp src/main.cpp tests/test_remove_exec.cpp
git add src/remove_exec.h src/remove_exec.cpp src/main.cpp tests/test_remove_exec.cpp
git commit -m "Take remove_exec's confirmation prompt out of the core"
```

---

### Task 4: Move the scan types and `process_file` into the core

Mechanical move, no logic change. Isolated so the `ScanEngine` diff in Task 5
contains only the restructuring.

**Files:**
- Create: `src/scan_engine.h`, `src/scan_engine.cpp`
- Modify: `src/main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ImageInfo` (`src/types.h`), `FaceInfo` and `FaceDetector` (`src/faces.h`).
- Produces: `ForceOptions`, `ScanOptions`, `ProcessResult`, `process_file` — all declared in `src/scan_engine.h`.

- [ ] **Step 1: Move the three types verbatim**

Copy into `src/scan_engine.h`, exactly as they are at `main.cpp:215-222`,
`:248-263`, and `:265-268`:

```cpp
struct ForceOptions {
    bool hash = false;
    bool thumbs = false;
    bool faces = false;

    bool any() const { return hash || thumbs || faces; }
    void set_all() { hash = thumbs = faces = true; }
};

struct ScanOptions {
    std::string directory;
    bool recursive = false;
    bool fail_on_error = false;
    ForceOptions force;
    bool faces = false;
    bool thumbs = false;
    bool ignore_mount_warning = false;
    std::string media;  // "", "image", or "video" ("" = all)
    bool include_hidden = false;
    int parallel = -1;  // -1 = auto
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

struct ProcessResult {
    ImageInfo info;
    std::vector<FaceInfo> faces;  // empty if faces not requested
};
```

Delete them from `main.cpp` and add `#include "scan_engine.h"` there.

**`parse_force` stays in `main.cpp`.** It parses an argv string (`"hash,thumbs"`),
and a library caller never has one. Leave it exactly where it is.

- [ ] **Step 2: Move `process_file` verbatim**

`main.cpp:270-337`. Declare it in `src/scan_engine.h`:

```cpp
// Hashes, phashes, and extracts metadata for one file, optionally detecting
// faces. Throws std::runtime_error if the file cannot be decoded.
ProcessResult process_file(const std::filesystem::path& filepath, FaceDetector* detector);
```

Copy the body into `src/scan_engine.cpp` **unchanged** — same branches, same
error messages, same order of operations. Check its exact current signature
before copying; if it takes different parameters than shown, use the real ones
and say so in your report. Add whatever includes it needs.

Then delete it from `main.cpp`.

- [ ] **Step 3: Add to the build and verify nothing changed**

Add `src/scan_engine.cpp` to the `add_library(phig-core STATIC ...)` sources.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests`
Expected: all 543 pass. This step must be a pure move — if any test changes
behavior, something was altered.

- [ ] **Step 4: Prove the move was verbatim**

Extract `process_file`'s body from `git show HEAD:src/main.cpp` and from
`src/scan_engine.cpp`, normalize whitespace, and diff. Report the result. Any
difference beyond a namespace qualifier or an include is a problem.

- [ ] **Step 5: Verify a scan still behaves identically**

Build the pre-change commit in a git worktree, create a scratch collection under
50 files, scan it with both binaries into separate databases, and compare: the
output text, and the row contents. Report the comparison. Remove the worktree and
the scratch directory afterward.

- [ ] **Step 6: Commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . clang-format -i src/scan_engine.h src/scan_engine.cpp src/main.cpp
git add src/scan_engine.h src/scan_engine.cpp src/main.cpp CMakeLists.txt
git commit -m "Move the scan option types and process_file into the core"
```

---

### Task 5: `ScanEngine`

The task this plan exists for. `cmd_scan`'s 438 lines become argv parsing and
output rendering over an engine in the core.

**Files:**
- Modify: `src/scan_engine.h`, `src/scan_engine.cpp`, `src/main.cpp`
- Create: `tests/test_scan_engine.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ProgressQueue<T>` (Task 1), `ScanOptions`/`ProcessResult`/`process_file` (Task 4), `CancelToken` (`src/cancel.h`), `parallel_for_each` (`src/parallel.h`).
- Produces: `ScanProgress`, `ScanResult`, `ScanEngine` (signatures in the Interface summary above).

- [ ] **Step 1: Read `cmd_scan` end to end and map its phases**

`src/main.cpp:339-776`. The phases, with the lines to move and the lines to keep:

| Phase | Lines | Goes to |
|---|---|---|
| Timer, `Database` open, banner prints | 340-358 | CLI keeps prints; engine gets the `Database&` |
| `rel_path` lambda | 349 | CLI — it is display-only (used at `:622`, `:625`, `:631`) |
| File walk, media filter | 361-378 | engine |
| Thread count, detector pool | 380-399 | engine |
| `CancelToken`, interrupt guard | 401-406 | CLI — signal handling stays in the CLI |
| Counters | 409-414 | engine (become `ScanResult` fields) |
| `flush_batch`, `process_one`, `parallel_for_each` | 416-663 | engine |
| Interrupted message | 671-673 | CLI, driven by `ScanResult::completed` |
| Cleanup: mount guard, relink, demote, delete | 675-743 | engine |
| Summary output | 745-773 | CLI, driven by `ScanResult` |

The engine emits `ScanProgress` to the queue; the CLI renders it. **The engine
does no printing at all** — but `log_warn`/`log_error`/`log_info` are permitted
and should stay where they are, including the mount-loss warning at `:691-694`.

- [ ] **Step 2: Move the progress emit out of the lock**

`main.cpp:511` prints while holding `db_mutex` — the `lock_guard` opens at `:491`
and its scope runs to `:515`, so both the print and the `return` at `:512` are
under the lock. Restructure so nothing is emitted while a lock is held: set a
flag inside the locked block, close the scope, then emit.

This is the specific hazard the queue exists to prevent. Even though a queue push
is cheap and would not deadlock, emitting under a lock is the pattern that
becomes a deadlock the moment anything downstream blocks — fix the site, not just
the mechanism.

- [ ] **Step 3: Write the failing tests**

Create `tests/test_scan_engine.cpp`. These are the first tests that have ever
exercised scan's orchestration, so they define the contract.

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"
#include "scan_engine.h"

namespace {
// A scratch directory with N tiny copies of a real fixture, removed on scope exit.
struct ScratchCollection {
    std::filesystem::path dir;
    std::string db_path;

    explicit ScratchCollection(int n) {
        dir = std::filesystem::temp_directory_path() /
              ("phig_scan_engine_" + std::to_string(::getpid()) + "_" +
               std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        db_path = (dir / "phig.db").string();
        for (int i = 0; i < n; ++i) {
            std::filesystem::copy_file("tests/data/test_red.mov",
                                       dir / ("v" + std::to_string(i) + ".mov"));
        }
    }
    ~ScratchCollection() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    ScanOptions options() const {
        ScanOptions o;
        o.directory = dir.string();
        o.db_path = db_path;
        o.recursive = false;
        o.parallel = 2;
        return o;
    }
};
}  // namespace

TEST(ScanEngine, ProcessesEveryFileOnAFirstScan) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanEngine engine(c.options(), db);
    ScanResult r = engine.run(cancel, nullptr);

    EXPECT_EQ(r.processed, 6u);
    EXPECT_EQ(r.skipped, 0u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_TRUE(r.completed);
    EXPECT_EQ(db.count(), 6);
}

TEST(ScanEngine, SkipsUnchangedFilesOnASecondScan) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanEngine(c.options(), db).run(cancel, nullptr);
    ScanResult second = ScanEngine(c.options(), db).run(cancel, nullptr);

    EXPECT_EQ(second.processed, 0u) << "unchanged files must not be reprocessed";
    EXPECT_EQ(second.skipped, 6u);
    EXPECT_EQ(db.count(), 6);
}

TEST(ScanEngine, ForceHashReprocessesEverything) {
    ScratchCollection c(4);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanEngine(c.options(), db).run(cancel, nullptr);

    ScanOptions forced = c.options();
    forced.force.hash = true;
    ScanResult r = ScanEngine(forced, db).run(cancel, nullptr);
    EXPECT_EQ(r.processed, 4u);
    EXPECT_EQ(r.skipped, 0u);
}

TEST(ScanEngine, ReportsProgressThroughTheQueue) {
    ScratchCollection c(5);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    ScanResult r = ScanEngine(c.options(), db).run(cancel, &q);
    q.close();

    std::vector<ScanProgress> seen;
    ScanProgress p;
    while (q.pop(p)) seen.push_back(p);

    EXPECT_EQ(r.processed, 5u);
    ASSERT_FALSE(seen.empty()) << "the engine must publish progress";
    for (const auto& s : seen) {
        EXPECT_EQ(s.total, 5u);
        EXPECT_LE(s.done, 5u);
    }
    EXPECT_EQ(seen.back().done, 5u) << "the last update must report completion";
}

TEST(ScanEngine, RunsWithoutAQueueWhenProgressIsNotWanted) {
    ScratchCollection c(3);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    // nullptr must be accepted — a caller that does not want progress should
    // not have to construct a queue to discard.
    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.processed, 3u);
}

TEST(ScanEngine, AnAlreadyCancelledTokenProcessesNothing) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    cancel.cancel();

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.processed, 0u);
    EXPECT_EQ(db.count(), 0) << "cancellation before any work must write nothing";
}

TEST(ScanEngine, CancellationLeavesTheDatabaseConsistent) {
    ScratchCollection c(40);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    // Cancel from a consumer thread once a few files are done.
    std::thread canceller([&] {
        ScanProgress p;
        while (q.pop(p)) {
            if (p.done >= 3) {
                cancel.cancel();
                break;
            }
        }
    });
    ScanResult r = ScanEngine(c.options(), db).run(cancel, &q);
    q.close();
    canceller.join();

    EXPECT_FALSE(r.completed);
    // Every row that exists must be complete and readable — a partial scan must
    // not leave a half-written row behind.
    for (const auto& img : db.get_all_images()) {
        EXPECT_FALSE(img.sha256.empty()) << img.path << " has no hash";
        EXPECT_FALSE(img.path.empty());
    }
}

TEST(ScanEngine, CountsAnUndecodableFileAsAnErrorAndContinues) {
    ScratchCollection c(4);
    // A text file with an image extension cannot be decoded.
    { std::ofstream(c.dir / "bad.jpg") << "not an image"; }
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(r.processed, 4u) << "one bad file must not stop the others";
    EXPECT_TRUE(r.completed);
}

TEST(ScanEngine, RemovesRowsForFilesThatDisappeared) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 6);

    // Delete two files, leaving four, so the mount-loss guard does not fire.
    std::filesystem::remove(c.dir / "v0.mov");
    std::filesystem::remove(c.dir / "v1.mov");

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.removed, 2u);
    EXPECT_EQ(db.count(), 4);
}

TEST(ScanEngine, MountLossGuardKeepsRowsWhenEveryFileVanishes) {
    ScratchCollection c(5);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 5);

    for (int i = 0; i < 5; ++i) {
        std::filesystem::remove(c.dir / ("v" + std::to_string(i) + ".mov"));
    }

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_TRUE(r.mount_warning) << "all rows vanishing should trip the guard";
    EXPECT_EQ(r.removed, 0u);
    EXPECT_EQ(db.count(), 5) << "an unmounted drive must not wipe metadata";
}
```

Tests run from the source root (`gtest_discover_tests` sets that), so
`tests/data/test_red.mov` resolves. Verify that fixture exists and is small
before relying on it; if it is absent, pick another from `tests/data/` and say
which.

- [ ] **Step 4: Register the test and run it to verify failure**

Add `tests/test_scan_engine.cpp` to the `phig-tests` sources.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `ScanEngine` does not exist.

- [ ] **Step 5: Declare the engine**

Add to `src/scan_engine.h`:

```cpp
#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

// One progress update. `path` is the full path of the file just finished; the
// CLI shortens it for display. `show_path` is false for the terse skip tick,
// which reproduces the CLI's existing two progress formats.
struct ScanProgress {
    size_t done = 0, total = 0;
    size_t processed = 0, skipped = 0, errors = 0;
    std::string path;
    bool show_path = false;
};

// Everything the caller needs to render a summary.
struct ScanResult {
    size_t processed = 0, skipped = 0, errors = 0;
    size_t images = 0, videos = 0, faces = 0, thumbs = 0;
    size_t removed = 0, relinked = 0, demoted = 0;
    bool completed = true;
    bool mount_warning = false;
};

// Scans a directory into a database: walks files, decides what needs
// reprocessing, hashes and extracts metadata in parallel, and reconciles rows
// for files that have moved or disappeared.
//
// Does no I/O of its own beyond the database and the diagnostic log. Progress is
// published to the queue if one is given; cancellation is polled from the token.
// The queue may be null. The engine owns the mutex serializing database access,
// so a caller must not touch `db` while run() is executing.
class ScanEngine {
   public:
    ScanEngine(const ScanOptions& opts, Database& db);

    ScanResult run(const CancelToken& cancel, ProgressQueue<ScanProgress>* progress);

   private:
    const ScanOptions& opts_;
    Database& db_;
};
```

- [ ] **Step 6: Implement it**

Move `cmd_scan`'s body into `ScanEngine::run` in `src/scan_engine.cpp`, phase by
phase, per Step 1's table. The lambdas become private members or stay as local
lambdas inside `run` — either is fine; what matters is that the captured locals
listed below become state `run` owns rather than `cmd_scan`'s stack.

The locals that move: `db_mutex`, `batch`, `batch_size`, `files`,
`face_detectors`, and the six counters (`processed`, `skipped`, `errors`,
`face_count`, `thumb_count`, `video_count`). The counters become `ScanResult`
fields at the end.

Preserve exactly:
- the three-way skip branching and every condition in it
- `flush_batch`'s single transaction covering the images and their faces
- both duplicate-face guards
- the video-versus-image thumbnail source choice
- the mount-loss guard's condition
- relink → demote → delete ordering, and the logged-versus-unlogged split
- `--on-error fail` behavior: the first worker exception propagates out of
  `parallel_for_each`, the completed batch is flushed, and it is rethrown for the
  caller to report

Replace every `std::cout` with a `ScanProgress` push (guarded by
`if (progress != nullptr)`). Keep the `log_*` calls where they are.

- [ ] **Step 7: Rewrite `cmd_scan` as a thin caller**

`cmd_scan` keeps: argv-derived `ScanOptions`, the `Database` construction, the
banner prints, the `CancelToken` and `ScopedInterruptHandler`, the `rel_path`
display helper, a consumer that drains the queue and prints progress, the
interrupted message, the summary block, and the `--on-error fail` catch that
prints `"Error: "` and returns 1.

Run the engine on the calling thread and drain the queue from a consumer thread,
or run the engine on a thread and drain on the main thread — either works.
Whichever you choose, the queue must be closed exactly once after `run` returns
and the consumer must be joined before the summary prints, or output will
interleave. Say which shape you used.

The summary lines must be byte-identical to before. Compare against the
pre-change binary in Step 9 rather than by eye.

- [ ] **Step 8: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='ScanEngine.*'`
Expected: 10 tests PASS.

Then the full suite: `direnv exec . ./build/phig-tests`
Expected: all pass. A failure elsewhere means the move changed shared behavior.

- [ ] **Step 9: Verify the CLI is unchanged, against a pre-change binary**

Build the commit before this task in a git worktree. Create a scratch collection
under 50 files. Then compare, recording actual output for each:

1. **First scan** — output text and row contents, both binaries, `--parallel 1`
   for deterministic ordering.
2. **Second scan** — both must report everything skipped.
3. **Removal** — delete two files, rescan, both must report `Removed: 2`.
4. **Mount guard** — delete all files, rescan, both must warn and remove nothing.
5. **Ctrl-C** — interrupt mid-scan, both must print "Interrupted. Progress
   saved.", exit 0, and leave a database that passes
   `sqlite3 <db> "PRAGMA integrity_check"`.
6. **`--on-error fail`** — with an undecodable file enumerated last, both must
   exit 1 with the same message and commit the same rows.

Remove the worktree and every scratch directory afterward, and confirm no phig
process is still running (`pgrep -f build/phig` should print nothing).

- [ ] **Step 10: Report the line count**

```bash
wc -l src/main.cpp src/scan_engine.cpp
grep -n "^int cmd_scan" src/main.cpp
```

Report `cmd_scan`'s new size and `main.cpp`'s total. This is the plan's headline
number and a reviewer will check it.

- [ ] **Step 11: Commit**

```bash
direnv exec . ./build/phig-tests && direnv exec . ./build/phig-perf-tests
direnv exec . ctest --test-dir build
direnv exec . clang-format -i src/scan_engine.h src/scan_engine.cpp src/main.cpp tests/test_scan_engine.cpp
git add src/scan_engine.h src/scan_engine.cpp src/main.cpp tests/test_scan_engine.cpp CMakeLists.txt
git commit -m "Extract ScanEngine into the core"
```

---

## Plan self-review

**Spec coverage.** This plan implements the spec's Part 2b decisions: the
purpose-built progress queue with its drop policy (Task 1), the `Database`
methods replacing `cmd_face identify`'s raw SQL plus all three unchecked blob
reads (Task 2), `remove_exec`'s prompt as a `ConfirmFn` (Task 3), the type moves
with `parse_force` deliberately staying in the CLI (Task 4), and `ScanEngine`
with progress emitted outside every lock (Task 5).

**Deferred to Part 2b-ii:** `ThumbsEngine` and `FacesEngine`. They are smaller
than `ScanEngine` — thumbs writes no DB rows and has one flat skip check; faces
rebuild has no skip logic and one transaction per item — and both become
straightforward once the queue and the engine pattern exist. `remove_exec`'s 13
`std::cout` calls also stay for now; that is the output-sink question, and it
belongs with the engines rather than with a prompt change.

**Three things an implementer should know:**
- **Task 5's tests are the first that have ever exercised scan's orchestration.**
  The research confirmed no test drives `cmd_scan`, `cmd_thumbs`, or `cmd_face` —
  they are not even linked into `phig-tests`. So these tests define the contract
  rather than confirming it, which means a test that passes for the wrong reason
  is worse here than elsewhere. This project has already found four such tests;
  when writing one, ask what mutation it would catch.
- **The lock-held progress print at `main.cpp:511` must be fixed, not merely
  worked around.** The queue makes a deadlock impossible, but emitting under a
  lock is the pattern that becomes a deadlock the moment anything downstream
  blocks.
- **Task 4 is a pure move and Task 5 is the restructuring.** Keeping them
  separate lets a reviewer confirm the move changed nothing before judging the
  redesign — the same split that made Part 1's duplicate-detection task
  reviewable.

**Type consistency check.** `ProgressQueue<T>` (Task 1) is used by `ScanEngine`
in Task 5 with `T = ScanProgress`. `ScanOptions`/`ProcessResult`/`process_file`
(Task 4) are consumed by `ScanEngine`'s constructor and body. `ConfirmFn` (Task 3)
is unrelated to the engine and used only by `remove_exec` and `main.cpp`.
`CancelToken` and `parallel_for_each` come from Part 1 unchanged;
`parallel_for_each`'s callback is `fn(size_t item, int worker_id)`, and
`ScanEngine` needs `worker_id` to index `face_detectors`. `FaceEmbeddingRow`
(Task 2) is used only by the two new `Database` methods and `cmd_face identify`.
