# phig-core Extraction, Part 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the core library, add the shared concurrency primitives every extracted engine will need, and fix three of the four audited defects — the stale test forks, the whole-file SHA256 read, and the O(n²) duplicate scan.

**Architecture:** This is the scaffolding half of the extraction. It creates `CancelToken` and `parallel_for_each` (which every later engine uses, and whose exception handling *is* the fix for the terminate-on-error abort), then moves three self-contained units out of `main.cpp` into the core, each fixing a defect on the way.

**Tech Stack:** C++17, CMake, GoogleTest. No new third-party dependencies.

**Spec:** `../specs/2026-08-01-phig-core-extraction-design.md` — this plan covers its sequencing steps 1–4. Steps 5–6 (`scan_engine`, `thumbs_engine`, `faces_engine`) are a separate plan that depends on this one.

## Global Constraints

- **The project builds as C++20** (`CMAKE_CXX_STANDARD 20`, `CMakeLists.txt:7`) — this predates the branch. Write code that compiles under it; there is no need to restrict yourself to C++17, and no need to change the standard. No new third-party dependencies.
- **The core performs no I/O and holds no global state.** Nothing moved into `src/*.cpp` by this plan may use `std::cout`, `std::cerr`, `std::cin`, or read a file-scope mutable global. Progress and cancellation are injected. `src/remove_exec.cpp` violates this today (13 `cout`/`cerr` calls and a `std::getline(std::cin)` prompt at `remove_exec.cpp:126`) — it is a cautionary precedent, **not** a template, and it is not modified by this plan.
- **Behavior visible to a CLI user must not change**, except where a defect fix requires it. `phig duplicates`, `phig cp/mv`, and `phig scan` must produce identical output before and after.
- **All 498 pre-existing tests must pass.** If a test encoded broken behavior, fix the test and say so explicitly rather than preserving the bug.
- **`.clang-format`** at the repo root is authoritative. CI does **not** lint `src/libphig/` or `include/`, so check those manually with `--dry-run --Werror`.
- **The C ABI is frozen.** `src/libphig/` and `include/phig/phig.h` gain no new surface in this plan. They must keep compiling and their tests keep passing as the core changes underneath.
- **Build/test commands must be prefixed with `direnv exec .`** — phig's dependencies come from a Nix devenv and are not on the bare PATH; a bare `cmake` fails on missing opencv4:
  - `direnv exec . cmake -B build && direnv exec . cmake --build build`
  - `direnv exec . ./build/phig-tests`
  - `direnv exec . clang-format -i <files>`
- **Commits are authorized on the working branch. Do NOT push to any remote and do NOT merge to master.**

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `src/cancel.h` | `CancelToken` — the injected replacement for the `g_interrupted` global. Header-only. |
| `src/parallel.h` | `parallel_for_each` declaration — the one thread-pool abstraction replacing three hand-rolled copies. |
| `src/parallel.cpp` | Its implementation, including first-exception capture and propagation. |
| `src/organize_plan.h` | `apply_format` declaration. |
| `src/organize_plan.cpp` | Its implementation, moved verbatim from `main.cpp`'s anonymous namespace. |
| `src/duplicates.h` | `DuplicateGroup`, `hamming_distance`, `find_near_duplicates` declarations. |
| `src/duplicates.cpp` | BK-tree implementation replacing the O(n²) loop. |
| `tests/test_parallel.cpp` | Cancellation, exception propagation, thread-count equivalence. |
| `tests/test_duplicates.cpp` | Old-vs-new equivalence over a generated corpus, plus a performance assertion. |

**Modified files:**

| Path | Change |
|---|---|
| `CMakeLists.txt` | Rename target; add new sources and test files. |
| `src/hasher.cpp` | Streaming SHA256. |
| `src/main.cpp` | Delete the moved code; include the new headers. |
| `tests/test_organize.cpp` | Delete the `apply_format` and `json_get` forks; test the real ones. |

## Interface summary

Names later tasks and the follow-on plan depend on.

```cpp
// src/cancel.h
struct CancelToken {
    std::atomic<bool> cancelled{false};
    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool stop() const { return cancelled.load(std::memory_order_relaxed); }
};

// src/parallel.h
// Runs fn(i) for i in [0, count). Returns early if `cancel` is signalled.
// If a worker throws, the first exception is captured, remaining work is
// abandoned, all threads are joined, and the exception is rethrown on the
// CALLING thread. Never calls std::terminate.
void parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t)>& fn);

// src/organize_plan.h
std::string apply_format(const std::string& fmt, const ImageInfo& img,
                         const DateParts& date);

// src/duplicates.h
struct DuplicateGroup {
    std::string type;   // "exact" or "near"
    int distance;       // 0 for exact
    std::vector<ImageInfo> images;
};
int hamming_distance(uint64_t a, uint64_t b);
std::vector<DuplicateGroup> find_near_duplicates(
    const std::vector<ImageInfo>& all_images, int threshold,
    const std::set<std::string>& exact_sha256s);
```

---

### Task 1: Rename the target; add `CancelToken` and `parallel_for_each`

**Files:**
- Create: `src/cancel.h`, `src/parallel.h`, `src/parallel.cpp`, `tests/test_parallel.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `CancelToken`, `parallel_for_each` (signatures above).

- [ ] **Step 1: Rename the CMake target**

In `CMakeLists.txt`, the target `phig-lib` becomes `phig-core` with output
`libphig-core.a`. It is a STATIC library — a convenience target linked into the
`phig` binary and `phig_abi`, with no runtime artifact.

Change `add_library(phig-lib STATIC` to `add_library(phig-core STATIC`, update
the `OUTPUT_NAME` property from `phig-internal` to `phig-core`, and update all
three `target_link_libraries` references (lines near 91, 148, 169) plus the
`phig-tests` link list near 211. Grep for `phig-lib` afterward and confirm zero
remaining references:

```bash
grep -rn "phig-lib" CMakeLists.txt scripts/ .github/ 2>/dev/null || echo "clean"
```

Also keep `POSITION_INDEPENDENT_CODE ON` on the renamed target — `phig_abi` is a
shared library and links this static one, so dropping PIC breaks that link.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests`
Expected: builds, 498 tests PASS. Nothing else changed yet.

- [ ] **Step 2: Write `src/cancel.h`**

```cpp
#pragma once

#include <atomic>

// Cooperative cancellation for long-running core operations.
//
// Replaces the CLI's file-scope g_interrupted flag: a global cannot work when
// two operations run concurrently, and a library caller cannot set one. The
// caller owns the token and signals it (from a signal handler, a GUI button, a
// test); the operation polls stop() and returns early.
struct CancelToken {
    std::atomic<bool> cancelled{false};

    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool stop() const { return cancelled.load(std::memory_order_relaxed); }
};
```

- [ ] **Step 3: Write the failing tests for `parallel_for_each`**

Create `tests/test_parallel.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include "cancel.h"
#include "parallel.h"

TEST(Parallel, RunsEveryIndexExactlyOnce) {
    const size_t n = 1000;
    std::vector<std::atomic<int>> seen(n);
    for (auto& s : seen) s.store(0);
    CancelToken tok;

    parallel_for_each(n, 4, tok, [&](size_t i) { seen[i].fetch_add(1); });

    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(seen[i].load(), 1) << "index " << i;
    }
}

TEST(Parallel, SingleThreadedMatchesMultiThreaded) {
    const size_t n = 500;
    std::set<size_t> one, many;
    std::mutex m;
    CancelToken tok;

    parallel_for_each(n, 1, tok, [&](size_t i) {
        std::lock_guard<std::mutex> g(m);
        one.insert(i);
    });
    parallel_for_each(n, 8, tok, [&](size_t i) {
        std::lock_guard<std::mutex> g(m);
        many.insert(i);
    });
    EXPECT_EQ(one, many);
    EXPECT_EQ(one.size(), n);
}

TEST(Parallel, ZeroCountIsANoOp) {
    CancelToken tok;
    int calls = 0;
    parallel_for_each(0, 4, tok, [&](size_t) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(Parallel, CancellationStopsEarly) {
    const size_t n = 10000;
    std::atomic<size_t> done{0};
    CancelToken tok;

    parallel_for_each(n, 4, tok, [&](size_t) {
        size_t d = done.fetch_add(1) + 1;
        if (d == 100) tok.cancel();
    });

    // Workers in flight when the token flips still finish their current item,
    // so allow slack — the point is that it stopped well short of n.
    EXPECT_LT(done.load(), n);
    EXPECT_GE(done.load(), 100u);
}

TEST(Parallel, WorkerExceptionPropagatesToCaller) {
    CancelToken tok;
    EXPECT_THROW(parallel_for_each(100, 4, tok,
                                   [&](size_t i) {
                                       if (i == 50) throw std::runtime_error("boom");
                                   }),
                 std::runtime_error);
}

TEST(Parallel, WorkerExceptionCarriesItsMessage) {
    CancelToken tok;
    try {
        parallel_for_each(100, 4, tok, [&](size_t i) {
            if (i == 7) throw std::runtime_error("specific message");
        });
        ADD_FAILURE() << "expected a throw";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "specific message");
    }
}

TEST(Parallel, AllThreadsJoinAfterAnException) {
    // The real defect this guards: an escaping exception in a std::thread
    // lambda calls std::terminate. If this test completes at all, the
    // implementation joined its workers instead of aborting.
    CancelToken tok;
    std::atomic<int> throwers{0};
    EXPECT_THROW(parallel_for_each(2000, 8, tok,
                                   [&](size_t) {
                                       throwers.fetch_add(1);
                                       throw std::runtime_error("everyone throws");
                                   }),
                 std::runtime_error);
    SUCCEED() << "returned normally rather than terminating";
}

TEST(Parallel, AlreadyCancelledRunsNothing) {
    CancelToken tok;
    tok.cancel();
    int calls = 0;
    parallel_for_each(100, 4, tok, [&](size_t) { ++calls; });
    EXPECT_EQ(calls, 0);
}
```

- [ ] **Step 4: Register the test and run it to verify failure**

Add `tests/test_parallel.cpp` to the `add_executable(phig-tests ...)` list.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `parallel.h` does not exist.

- [ ] **Step 5: Write `src/parallel.h`**

```cpp
#pragma once

#include <cstddef>
#include <functional>

#include "cancel.h"

// Runs fn(i) for every i in [0, count), across num_threads workers pulling
// from a shared atomic index.
//
// Cancellation: workers stop pulling new items once `cancel` is signalled. An
// item already in flight runs to completion.
//
// Exceptions: if fn throws, the FIRST exception is captured, further work is
// abandoned, every worker is joined, and the exception is rethrown on the
// calling thread. This is deliberate — an exception escaping a std::thread
// lambda calls std::terminate, which is how `phig scan --fail-on-error`
// currently aborts the process mid-write.
//
// num_threads <= 0 is treated as 1.
void parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t)>& fn);
```

- [ ] **Step 6: Implement `src/parallel.cpp`**

```cpp
#include "parallel.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

void parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    if (num_threads < 1) num_threads = 1;
    if (static_cast<size_t>(num_threads) > count) {
        num_threads = static_cast<int>(count);
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::exception_ptr first_error;
    std::mutex error_mutex;

    auto worker = [&]() {
        for (;;) {
            if (cancel.stop() || failed.load(std::memory_order_relaxed)) return;
            size_t i = next.fetch_add(1);
            if (i >= count) return;
            try {
                fn(i);
            } catch (...) {
                // Keep only the first failure; the rest are consequences.
                std::lock_guard<std::mutex> g(error_mutex);
                if (!first_error) {
                    first_error = std::current_exception();
                    failed.store(true, std::memory_order_relaxed);
                }
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_threads));
    for (int t = 0; t < num_threads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    // Rethrow on the caller's thread, after every worker has been joined.
    if (first_error) std::rethrow_exception(first_error);
}
```

- [ ] **Step 7: Add the source to the build and run the tests**

Add `src/parallel.cpp` to the `add_library(phig-core STATIC ...)` source list.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='Parallel.*'`
Expected: 8 tests PASS.

- [ ] **Step 8: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i src/cancel.h src/parallel.h src/parallel.cpp tests/test_parallel.cpp
git add src/cancel.h src/parallel.h src/parallel.cpp tests/test_parallel.cpp CMakeLists.txt
git commit -m "Rename phig-lib to phig-core; add CancelToken and parallel_for_each"
```

---

### Task 2: Move `apply_format` into the core and delete the test forks

Fixes the defect where seven tests pass against code that is not shipped.

**Files:**
- Create: `src/organize_plan.h`, `src/organize_plan.cpp`
- Modify: `src/main.cpp`, `tests/test_organize.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ImageInfo` (`src/types.h`), `DateParts` (`src/dates.h`), `json_get` (`src/dates.h:31`).
- Produces: `apply_format` (signature above).

- [ ] **Step 1: Understand the defect before changing anything**

`src/main.cpp:1044` defines `apply_format` inside an anonymous namespace, so no
test can reach it. `tests/test_organize.cpp:55` therefore defines its own copy
inside `namespace organize_helpers` — and that copy is a **stale fork**: it
handles `%original`, `%camera`, `%make`, then jumps straight to `%Y`. It is
missing `%gallery` and `%fav`, which the shipped version at `main.cpp:1060-1065`
has. The same file also shadows `json_get` at `test_organize.cpp:45`, duplicating
the real one in `src/dates.h`.

Read all three before editing: `main.cpp:1044-1090`, `test_organize.cpp:40-100`,
and `src/dates.h`.

- [ ] **Step 2: Write the failing tests against the real function**

In `tests/test_organize.cpp`, **delete** the `namespace organize_helpers` block
containing the forked `json_get` and `apply_format`, replace the fork's uses with
`#include "organize_plan.h"` and `#include "dates.h"`, and add coverage for the
two tokens that had none:

```cpp
TEST(FormatTest, GalleryTokenUsesTheGalleryName) {
    ImageInfo img;
    img.filename = "photo.jpg";
    img.gallery = "holiday";
    DateParts d;
    d.valid = true;
    d.year = "2024";
    d.month = "06";
    d.day = "15";
    EXPECT_EQ(apply_format("%gallery/%original", img, d), "holiday/photo.jpg");
}

TEST(FormatTest, GalleryTokenFallsBackWhenUnset) {
    ImageInfo img;
    img.filename = "photo.jpg";  // gallery left empty
    DateParts d;
    d.valid = true;
    EXPECT_EQ(apply_format("%gallery/%original", img, d), "no-gallery/photo.jpg");
}

TEST(FormatTest, FavTokenDistinguishesFavorites) {
    ImageInfo img;
    img.filename = "photo.jpg";
    DateParts d;
    d.valid = true;

    img.favorite = true;
    EXPECT_EQ(apply_format("%fav/%original", img, d), "favorites/photo.jpg");

    img.favorite = false;
    EXPECT_EQ(apply_format("%fav/%original", img, d), "unfavorited/photo.jpg");
}

TEST(FormatTest, GalleryAndFavCombine) {
    ImageInfo img;
    img.filename = "i0.png";
    img.gallery = "holiday";
    img.favorite = false;
    DateParts d;
    d.valid = true;
    EXPECT_EQ(apply_format("%gallery/%fav/%original", img, d),
              "holiday/unfavorited/i0.png");
}
```

Keep every existing `FormatTest` case — they must now pass against the real
implementation. If any fails, the fork had diverged in a second way; report it
rather than adjusting the assertion.

- [ ] **Step 3: Run to verify failure**

Run: `direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `organize_plan.h` does not exist.

- [ ] **Step 4: Create the header**

`src/organize_plan.h`:

```cpp
#pragma once

#include <string>

#include "dates.h"
#include "types.h"

// Expands an organize format string against one image.
//
// Tokens: %original (filename), %camera (EXIF Model), %make (EXIF Make),
// %gallery (gallery name), %fav (favorite state), %Y/%m/%d (date parts).
// Unknown %-sequences are emitted verbatim. Missing values fall back to
// "unknown-camera", "unknown-make", "no-gallery", "unfavorited", or
// "unsorted" for dates.
std::string apply_format(const std::string& fmt, const ImageInfo& img,
                         const DateParts& date);
```

- [ ] **Step 5: Move the implementation verbatim**

Create `src/organize_plan.cpp` containing `#include "organize_plan.h"` and the
body of `apply_format` **copied exactly** from `main.cpp:1044` — same token
order, same fallback strings, same index arithmetic. Do not "improve" it; this
task is a move plus test coverage, and any behavior change would alter
destination paths for `mv`/`cp`.

Then delete the function from `main.cpp`'s anonymous namespace and add
`#include "organize_plan.h"` to `main.cpp`.

- [ ] **Step 6: Add to the build and run the tests**

Add `src/organize_plan.cpp` to the `phig-core` source list.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='FormatTest.*'`
Expected: all pass, including the four new cases.

- [ ] **Step 7: Verify the CLI is unchanged**

The organize format decides where files land, so prove it end to end rather than
trusting the unit tests:

```bash
direnv exec . ./build/phig --help >/dev/null && echo "binary ok"
```

Then confirm `%gallery` and `%fav` still resolve by checking a dry run against a
scratch collection you create in `/tmp` — scan two files, set a gallery on one,
mark one favorite, and run `cp --dry-run --format "%gallery/%fav/%original"`.
Record the output in your report. Expected: `holiday/unfavorited/<name>` style
paths, matching the pre-change behavior.

- [ ] **Step 8: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i src/organize_plan.h src/organize_plan.cpp src/main.cpp tests/test_organize.cpp
git add src/organize_plan.h src/organize_plan.cpp src/main.cpp tests/test_organize.cpp CMakeLists.txt
git commit -m "Move apply_format into the core; test the shipped code, not a fork"
```

---

### Task 3: Stream SHA256 instead of reading whole files

**Files:**
- Modify: `src/hasher.cpp`
- Test: `tests/test_hasher.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: no signature change — `compute_sha256(path)` keeps its interface; only the implementation changes.

- [ ] **Step 1: Read the current implementation**

`src/hasher.cpp:150-164`. It stats the file, allocates
`std::vector<uint8_t> buffer(size)`, reads it all, checks for a short read, then
calls `sha256_bytes(buffer.data(), buffer.size())`.

Measured: **1.85 GB peak RSS on a 945 MB video** — roughly 2× the file size, and
that is *per worker thread*. Scan defaults to `hardware_concurrency()`, so a
16-core machine scanning large video can exhaust memory.

Note the existing short-read check exists for a real reason, stated in its
comment: a silently wrong hash drives duplicate detection, which drives user
deletions. Preserve that guarantee.

- [ ] **Step 2: Write the failing tests**

Add to `tests/test_hasher.cpp`:

```cpp
TEST(HasherTest, StreamingHashMatchesKnownVector) {
    // SHA-256 of "abc" — the canonical test vector.
    auto path = std::filesystem::temp_directory_path() /
                ("phig_hash_abc_" + std::to_string(::getpid()));
    { std::ofstream(path) << "abc"; }
    EXPECT_EQ(compute_sha256(path.string()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::filesystem::remove(path);
}

TEST(HasherTest, EmptyFileHashesToTheEmptyDigest) {
    auto path = std::filesystem::temp_directory_path() /
                ("phig_hash_empty_" + std::to_string(::getpid()));
    { std::ofstream f(path); }
    EXPECT_EQ(compute_sha256(path.string()),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::filesystem::remove(path);
}

TEST(HasherTest, HashIsBlockBoundaryIndependent) {
    // A file several blocks long, with a size that is deliberately not a
    // multiple of any plausible block size, so a boundary bug shows up.
    auto path = std::filesystem::temp_directory_path() /
                ("phig_hash_blocks_" + std::to_string(::getpid()));
    std::string data;
    data.reserve(200000);
    for (int i = 0; i < 200000; ++i) data += static_cast<char>('a' + (i % 26));
    { std::ofstream(path, std::ios::binary) << data; }

    // Compare against hashing the same bytes in one shot through the existing
    // in-memory path, which is unchanged by this task.
    std::string expected =
        sha256_bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    EXPECT_EQ(compute_sha256(path.string()), expected);
    std::filesystem::remove(path);
}

TEST(HasherTest, MissingFileThrows) {
    EXPECT_THROW(compute_sha256("/nonexistent-dir-xyz/file.bin"), std::runtime_error);
}
```

Add `#include <fstream>`, `#include <filesystem>`, and `#include <unistd.h>` to
the test file if not already present. Check `src/hasher.h` for the exact name and
signature of the in-memory helper (`sha256_bytes`) and match it; if it is not
declared in the header, hash the same bytes via a small temp file written in one
piece instead, and say so in your report.

- [ ] **Step 3: Run to verify the new tests pass against the OLD code**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='HasherTest.*'`
Expected: **PASS.** These tests characterize existing behavior — they must pass
before the change so that they are a genuine regression net afterward. If any
fails now, stop and report: the current implementation is already wrong.

- [ ] **Step 4: Replace the read with a streaming loop**

In `src/hasher.cpp`, replace the whole-file read. The exact incremental API
depends on which SHA-256 implementation the file uses — read the top of
`hasher.cpp` and use its update/finalize calls rather than inventing names. The
shape:

```cpp
    // Stream in fixed blocks: a whole-file buffer costs ~2x the file size in
    // RSS per worker thread, and scan runs one worker per core.
    constexpr size_t kBlock = 1 << 20;  // 1 MiB
    std::vector<uint8_t> buf(kBlock);
    size_t total_read = 0;

    // <initialize the hash context here>

    while (file) {
        file.read(reinterpret_cast<char*>(buf.data()),
                  static_cast<std::streamsize>(kBlock));
        std::streamsize got = file.gcount();
        if (got <= 0) break;
        total_read += static_cast<size_t>(got);
        // <update the hash context with buf.data(), got>
    }

    // A short read (I/O error, file shrank mid-scan) must not produce a
    // silently wrong hash — duplicates output drives user deletions.
    if (total_read != size) {
        throw std::runtime_error("Short read while hashing (file changed or I/O error): " + path);
    }

    // <finalize and return the hex digest>
```

Keep the existing size determination and its "Cannot determine file size"
error. Keep the short-read check and its comment — the guarantee it documents is
the reason it exists.

- [ ] **Step 5: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='HasherTest.*'`
Expected: all PASS, including the four from Step 2 which passed before the change
and must still pass after.

- [ ] **Step 6: Verify hashes are byte-identical on real files**

Unit tests on synthetic data are not enough — a hash change would silently
invalidate every row in an existing database. Verify against the repo's real test
fixtures:

```bash
cd /home/brett/devel/phig/phig
for f in tests/data/*; do
  echo -n "$f "
  sha256sum "$f" | cut -d' ' -f1
done
```

Compare each against what phig computes for the same files. Write a small probe
against `libphig-core.a` that calls `compute_sha256` on each fixture and prints
the digest, and confirm every one matches `sha256sum`. Include the comparison
table in your report. Any mismatch is a hard stop.

- [ ] **Step 7: Measure the memory improvement**

Create a large file and measure peak RSS before and after, so the fix is
demonstrated rather than assumed:

```bash
dd if=/dev/urandom of=/tmp/phig-big.bin bs=1M count=900 2>/dev/null
direnv exec . /usr/bin/time -v <your probe> /tmp/phig-big.bin 2>&1 | grep -i "maximum resident"
rm /tmp/phig-big.bin
```

Expected: peak RSS in the tens of MB rather than ~1.8 GB. Report the number.

- [ ] **Step 8: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i src/hasher.cpp tests/test_hasher.cpp
git add src/hasher.cpp tests/test_hasher.cpp
git commit -m "Stream SHA256 in blocks instead of reading whole files into memory"
```

---

### Task 4: Move duplicate detection into the core and replace the O(n²) scan

The riskiest change in this plan: it swaps an algorithm that produces
user-visible output. Equivalence is proven, not assumed.

**Files:**
- Create: `src/duplicates.h`, `src/duplicates.cpp`, `tests/test_duplicates.cpp`
- Modify: `src/main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ImageInfo` (`src/types.h`).
- Produces: `DuplicateGroup`, `hamming_distance`, `find_near_duplicates` (signatures above).

- [ ] **Step 1: Read the current implementation**

`src/main.cpp:751-820`. `hamming_distance` is `std::popcount(a ^ b)` at `:751`.
`DuplicateGroup` is declared at `:753`. `find_near_duplicates` at `:759` builds a
union-find over all images and runs an **all-pairs nested loop** at `:783-795`,
skipping pairs that are already exact duplicates.

Measured: 5k → 145 ms, 10k → 389 ms, 20k → 1.4 s, 40k → 5.2 s. Clean quadratic.
100k ≈ 33 s, 1M ≈ 54 min.

Note the exact-duplicate skip and the union-find semantics carefully — group
membership is transitive, so A~B and B~C puts A, B, C in one group even if A and
C are further apart than the threshold. The replacement must preserve that.

- [ ] **Step 2: Move the code unchanged, with an equivalence test**

First move `hamming_distance`, `DuplicateGroup`, and `find_near_duplicates`
verbatim into `src/duplicates.{h,cpp}` with **no algorithmic change**, delete them
from `main.cpp`, and add `#include "duplicates.h"` there.

Create `tests/test_duplicates.cpp` capturing current behavior:

```cpp
#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>
#include <vector>

#include "duplicates.h"

namespace {
// Builds n images with controlled phashes so grouping is predictable.
std::vector<ImageInfo> make_images(const std::vector<uint64_t>& hashes) {
    std::vector<ImageInfo> out;
    for (size_t i = 0; i < hashes.size(); ++i) {
        ImageInfo im;
        im.path = "/photos/img" + std::to_string(i) + ".jpg";
        im.filename = "img" + std::to_string(i) + ".jpg";
        im.sha256 = std::string(63, '0') + std::to_string(i % 10);
        im.phash = hashes[i];
        out.push_back(im);
    }
    return out;
}
}  // namespace

TEST(Duplicates, HammingDistanceCountsDifferingBits) {
    EXPECT_EQ(hamming_distance(0ULL, 0ULL), 0);
    EXPECT_EQ(hamming_distance(0ULL, 1ULL), 1);
    EXPECT_EQ(hamming_distance(0ULL, 0xFFULL), 8);
    EXPECT_EQ(hamming_distance(0xFFFFFFFFFFFFFFFFULL, 0ULL), 64);
}

TEST(Duplicates, IdenticalHashesGroupTogether) {
    auto imgs = make_images({0x1111ULL, 0x1111ULL, 0xFFFFULL});
    auto groups = find_near_duplicates(imgs, 5, {});
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].images.size(), 2u);
}

TEST(Duplicates, DistantHashesDoNotGroup) {
    auto imgs = make_images({0x0000ULL, 0xFFFFFFFFFFFFFFFFULL});
    auto groups = find_near_duplicates(imgs, 5, {});
    EXPECT_TRUE(groups.empty());
}

TEST(Duplicates, GroupingIsTransitiveAcrossTheThreshold) {
    // A~B (distance 1) and B~C (distance 1), but A~C is distance 2.
    // With threshold 1 all three still land in ONE group via union-find.
    auto imgs = make_images({0b000ULL, 0b001ULL, 0b011ULL});
    auto groups = find_near_duplicates(imgs, 1, {});
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].images.size(), 3u);
}

TEST(Duplicates, ExactDuplicatesAreSkipped) {
    std::vector<ImageInfo> imgs;
    for (int i = 0; i < 2; ++i) {
        ImageInfo im;
        im.path = "/photos/same" + std::to_string(i) + ".jpg";
        im.sha256 = std::string(64, 'a');  // identical content
        im.phash = 0x1234ULL;
        imgs.push_back(im);
    }
    std::set<std::string> exact{std::string(64, 'a')};
    auto groups = find_near_duplicates(imgs, 5, exact);
    EXPECT_TRUE(groups.empty()) << "pairs already reported as exact must not repeat as near";
}

TEST(Duplicates, EmptyInputYieldsNoGroups) {
    EXPECT_TRUE(find_near_duplicates({}, 5, {}).empty());
}

TEST(Duplicates, SingleImageYieldsNoGroups) {
    auto imgs = make_images({0x1ULL});
    EXPECT_TRUE(find_near_duplicates(imgs, 5, {}).empty());
}
```

Add both files to the build. Run:
`direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='Duplicates.*'`
Expected: 7 PASS against the unchanged algorithm. Commit this move separately
before touching the algorithm:

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i src/duplicates.h src/duplicates.cpp src/main.cpp tests/test_duplicates.cpp
git add src/duplicates.h src/duplicates.cpp src/main.cpp tests/test_duplicates.cpp CMakeLists.txt
git commit -m "Move duplicate detection into the core, unchanged"
```

- [ ] **Step 3: Write the equivalence harness before optimizing**

Add to `tests/test_duplicates.cpp` a test that compares the current
implementation against the one you are about to write, over a generated corpus.
Rename the moved function to `find_near_duplicates_bruteforce` (keep it, it is
the oracle) and have `find_near_duplicates` become the new one.

```cpp
namespace {
// Normalizes groups to a comparable form: each group as a sorted set of paths,
// and the collection of groups itself sorted, so ordering differences between
// implementations do not register as behavioral differences.
std::set<std::vector<std::string>> normalize(const std::vector<DuplicateGroup>& gs) {
    std::set<std::vector<std::string>> out;
    for (const auto& g : gs) {
        std::vector<std::string> paths;
        for (const auto& im : g.images) paths.push_back(im.path);
        std::sort(paths.begin(), paths.end());
        out.insert(paths);
    }
    return out;
}
}  // namespace

TEST(Duplicates, NewImplementationMatchesBruteForceOverRandomCorpora) {
    std::mt19937_64 rng(12345);  // fixed seed: reproducible failures
    for (int trial = 0; trial < 200; ++trial) {
        size_t n = 2 + (rng() % 60);
        std::vector<uint64_t> hashes;
        // Cluster around a few centers so near-duplicates actually occur.
        std::vector<uint64_t> centers;
        for (int c = 0; c < 4; ++c) centers.push_back(rng());
        for (size_t i = 0; i < n; ++i) {
            uint64_t h = centers[rng() % centers.size()];
            int flips = static_cast<int>(rng() % 10);
            for (int f = 0; f < flips; ++f) h ^= (1ULL << (rng() % 64));
            hashes.push_back(h);
        }
        auto imgs = make_images(hashes);
        int threshold = static_cast<int>(rng() % 12);

        auto expected = normalize(find_near_duplicates_bruteforce(imgs, threshold, {}));
        auto actual = normalize(find_near_duplicates(imgs, threshold, {}));
        ASSERT_EQ(expected, actual) << "trial " << trial << " n=" << n
                                    << " threshold=" << threshold;
    }
}

TEST(Duplicates, NewImplementationMatchesBruteForceWithExactSkips) {
    std::mt19937_64 rng(999);
    for (int trial = 0; trial < 50; ++trial) {
        size_t n = 5 + (rng() % 30);
        std::vector<ImageInfo> imgs;
        std::set<std::string> exact;
        uint64_t center = rng();
        for (size_t i = 0; i < n; ++i) {
            ImageInfo im;
            im.path = "/photos/i" + std::to_string(i) + ".jpg";
            // Deliberately give some images a shared sha256 and mark it exact.
            im.sha256 = (i % 5 == 0) ? std::string(64, 'x')
                                     : std::string(63, '0') + std::to_string(i % 10);
            uint64_t h = center;
            for (int f = 0; f < static_cast<int>(rng() % 8); ++f) h ^= (1ULL << (rng() % 64));
            im.phash = h;
            imgs.push_back(im);
        }
        exact.insert(std::string(64, 'x'));
        int threshold = static_cast<int>(rng() % 10);

        auto expected = normalize(find_near_duplicates_bruteforce(imgs, threshold, exact));
        auto actual = normalize(find_near_duplicates(imgs, threshold, exact));
        ASSERT_EQ(expected, actual) << "trial " << trial;
    }
}
```

Add `#include <algorithm>` and `#include <random>`.

- [ ] **Step 4: Run to verify the harness passes trivially**

At this point both names refer to the same code, so the equivalence tests should
pass. Run them and confirm:

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='Duplicates.*'`
Expected: PASS. This proves the harness itself is sound before it has to catch a
real difference.

- [ ] **Step 5: Implement the BK-tree**

Replace `find_near_duplicates`'s body with a BK-tree over Hamming distance.
Hamming distance is a metric (it satisfies the triangle inequality), which is what
makes BK-tree pruning valid.

Structure: each node holds one image index and a map from distance to child
node. To insert, walk from the root, computing the distance to the current node
and descending into the child at that exact distance, creating it if absent. To
query within `threshold` of a target, at each node compute `d`; report the node
if `d <= threshold`, then descend only into children whose edge distance lies in
`[d - threshold, d + threshold]` — everything outside that band cannot contain a
match.

Build the tree once over all images, then for each image query its neighbours and
`unite` them in the same union-find the brute-force version uses. Keep the exact
same union-find, the same exact-duplicate skip, and the same group assembly and
output ordering — only the neighbour-finding changes.

Keep `find_near_duplicates_bruteforce` in the file. It is small, it is the
oracle the equivalence tests need, and deleting it would remove the only
independent check on the optimized path.

- [ ] **Step 6: Run the equivalence tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='Duplicates.*'`
Expected: all PASS, including the two equivalence tests, which now compare two
genuinely different implementations. A failure here means the BK-tree pruning is
wrong — fix the algorithm; do not weaken the test.

- [ ] **Step 7: Add a performance assertion**

```cpp
TEST(Duplicates, FortyThousandImagesCompletesQuickly) {
    std::mt19937_64 rng(4242);
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 40000; ++i) hashes.push_back(rng());
    auto imgs = make_images(hashes);

    auto start = std::chrono::steady_clock::now();
    auto groups = find_near_duplicates(imgs, 5, {});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start).count();

    // The brute-force implementation took ~5200 ms for this input. A generous
    // ceiling still fails loudly if the BK-tree is accidentally linear-scanning.
    EXPECT_LT(ms, 2000) << "40k images took " << ms << " ms";
    (void)groups;
}
```

Add `#include <chrono>`. Run it and **report the actual timing** — the number
matters more than the assertion.

- [ ] **Step 8: Verify the CLI output is unchanged**

Build a scratch collection in `/tmp` with some near-duplicate images, run
`phig duplicates --type near` against it with the pre-change binary
(`git stash` the algorithm change, build, capture output) and the post-change
binary, and diff the two outputs. They must be identical, including group
ordering. Report the diff (or its absence).

- [ ] **Step 9: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i src/duplicates.h src/duplicates.cpp tests/test_duplicates.cpp
git add src/duplicates.h src/duplicates.cpp tests/test_duplicates.cpp
git commit -m "Replace O(n^2) near-duplicate scan with a BK-tree"
```

---

## Plan self-review

**Spec coverage.** This plan implements the spec's sequencing steps 1–4: the
rename and concurrency primitives (Task 1), `apply_format` plus the test forks
(Task 2, spec defect 4), streaming SHA256 (Task 3, spec defect 3), and the
BK-tree (Task 4, spec defect 2). `parallel_for_each`'s exception handling is the
mechanism that fixes spec defect 1, but the defect is not *observably* fixed
until `cmd_scan` uses it — that lands in Part 2.

**Deliberately deferred to Part 2** (the follow-on plan): `scan_engine`,
`thumbs_engine`, `faces_engine`, folding `cmd_face identify`'s inline SQL into
`Database`, moving `remove_exec.cpp`'s `std::cin` prompt out to the CLI, and the
unchecked 128-float blob reads at `database.cpp:1811` and `main.cpp:2043/2064`.
Part 2 depends on Tasks 1 and 3 of this plan.

**Two things an implementer should know:**
- Task 4 is split into two commits on purpose — move first, optimize second — so
  a reviewer can see that the move changed nothing before judging the algorithm.
  The equivalence harness is written while both names still point at the same
  code, which proves the harness works before it has to catch a real difference.
- Task 3's tests are written to pass against the *old* implementation first.
  That is deliberate: they characterize existing behavior, so they are a genuine
  regression net rather than a description of whatever the new code happens to do.

**Type consistency check.** `CancelToken` (Task 1) is used by `parallel_for_each`
in the same task and by Part 2's engines. `apply_format`'s signature (Task 2)
matches the shipped `main.cpp:1044` definition and `src/dates.h`'s `DateParts`.
`DuplicateGroup`, `hamming_distance`, and `find_near_duplicates` (Task 4) keep
the exact names and signatures they have at `main.cpp:751-761`, so `main.cpp`'s
call site at `:966` needs no change beyond the new include.
