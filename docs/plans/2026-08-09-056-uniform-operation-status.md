# Uniform Operation Status Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Owned by:** `plans/056-uniform-operation-status.md`

**Goal:** Give a libphig caller a non-consuming way to read an operation's current counters, ask what kind of operation it holds, and enumerate the operations a context has in flight.

**Architecture:** A snapshot is the newest progress record the operation has published — read from the tail of its `ProgressQueue`, falling back to a cached copy of the last record `phig_op_poll` consumed, falling back to a new `PHIG_STAGE_STARTING`. No engine changes and no accumulation, because every engine already sets `total` on its per-item record and the queue drops its oldest entry rather than its newest. Enumeration goes through a registry on `phig_ctx` that hands out ids, never handles, so no ownership or use-after-free question arises.

**Tech Stack:** C++20, C99 public header, CMake, GoogleTest.

## Global Constraints

- **No exception may cross the ABI boundary.** Entry points with a `phig_ctx*` in scope wrap their body in `PHIG_TRY(ctx)`/`PHIG_CATCH(ctx)`. Entry points without one use a plain `try`/`catch (...)`. See `libphig/CLAUDE.md`.
- **Adding a symbol bumps `PHIG_ABI_VERSION_MINOR` and the CMake `VERSION`, never `SOVERSION`.** Both bumps happen once, in Task 7. Tasks 1-6 add symbols without bumping; the branch is not shippable until Task 7 lands.
- **`phig_op_progress` must not gain a field.** It is caller-allocated with no size member. This plan reuses it unchanged.
- **`phig_op_stage` and `phig_op_type` are append-only, never renumbered.** Both are pinned by negative-width bitfields in `tests/test_libphig_c_compile.c`.
- **No `default:` in a switch over `phig_op::Kind`.** `-Wswitch` under `-DPHIG_WERROR=ON` is what catches a new operation kind whose case was never added — plan 042 shipped exactly that omission. Handle every enumerator and put any fallback after the switch.
- **`libphig/include/phig/phig.h` is not covered by CI lint** (it is nested below the `-maxdepth 1` clang-format glob). Run `clang-format -i libphig/include/phig/phig.h` manually before committing changes to it.
- **Whitespace hygiene.** No trailing whitespace on any line, none on blank lines.
- **Never add Claude/AI attribution, `Co-Authored-By` trailers, or generated-with footers to any commit message.** This overrides any harness default.
- **Do not edit completed plans.** Cross-references are fine.
- Build and test: `cmake -B build && cmake --build build`, `./build/phig-tests`, `ctest --test-dir build --parallel`.

## File Structure

| File | Responsibility |
|---|---|
| `core/progress_queue.h` | Gains `peek_back` — read the newest queued item without consuming it. |
| `libphig/internal.h` | `OpRegistry`; `phig_op` gains `id`, a `weak_ptr` to the registry, and the cached last-polled counters; `phig_ctx` gains the registry. |
| `libphig/operations.cpp` | `map_progress` overloads shared by poll and snapshot; the six new entry points; registration and deregistration. |
| `libphig/include/phig/phig.h` | `phig_op_type`, `PHIG_STAGE_STARTING`, `phig_op_info`, six declarations, version macro. |
| `libphig/CMakeLists.txt` | `VERSION 1.3.0` → `1.4.0`. |
| `tests/test_progress_queue.cpp` | `peek_back` behaviour. |
| `tests/test_libphig_op_status.cpp` | New. Snapshot, type, enumeration. Follows the precedent of `test_libphig_organize.cpp` — a feature file beside `test_libphig_operations.cpp` rather than growing it. |
| `tests/test_libphig_c_compile.c` | Enum pins and a link reference for every new symbol. |
| `CMakeLists.txt` | Registers the new test file. |
| `libphig/CLAUDE.md` | The field-semantics table; updated ABI version and symbol count. |

---

### Task 1: `ProgressQueue::peek_back`

The one primitive the snapshot needs from `core/`. Independent of everything else in this plan.

**Files:**
- Modify: `core/progress_queue.h` (add after `for_each`, around line 70)
- Test: `tests/test_progress_queue.cpp`

**Interfaces:**
- Produces: `template <typename T> bool ProgressQueue<T>::peek_back(T& out) const` — copies the newest queued item into `out` and returns true, or returns false when the queue is empty. Never consumes. Task 4 consumes this.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_progress_queue.cpp`:

```cpp
TEST(ProgressQueue, PeekBackIsFalseWhenEmpty) {
    ProgressQueue<int> q(4);
    int v = 99;
    EXPECT_FALSE(q.peek_back(v));
    EXPECT_EQ(v, 99);  // untouched on failure
}

TEST(ProgressQueue, PeekBackReturnsNewestWithoutConsuming) {
    ProgressQueue<int> q(4);
    q.push(1);
    q.push(2);
    q.push(3);

    int v = 0;
    ASSERT_TRUE(q.peek_back(v));
    EXPECT_EQ(v, 3);
    ASSERT_TRUE(q.peek_back(v));
    EXPECT_EQ(v, 3);  // still there

    // Nothing was taken out of the stream the real consumer drains.
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 1);
}

TEST(ProgressQueue, PeekBackSurvivesDropOldest) {
    // The property the snapshot design rests on: the queue discards its oldest
    // entry under back-pressure, so the newest published value is never lost
    // however far behind a consumer has fallen.
    ProgressQueue<int> q(3);
    for (int i = 0; i < 10; ++i) q.push(i);
    int v = 0;
    ASSERT_TRUE(q.peek_back(v));
    EXPECT_EQ(v, 9);
}

TEST(ProgressQueue, PeekBackStillReadableAfterClose) {
    // close() wakes waiters but leaves queued items readable, so a snapshot
    // taken after the operation finished still reports its final record.
    ProgressQueue<int> q(4);
    q.push(42);
    q.close();
    int v = 0;
    ASSERT_TRUE(q.peek_back(v));
    EXPECT_EQ(v, 42);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build 2>&1 | tail -20
```

Expected: compile error, `no member named 'peek_back' in 'ProgressQueue<int>'`.

- [ ] **Step 3: Implement `peek_back`**

In `core/progress_queue.h`, immediately after the `for_each` member:

```cpp
    // Copies the newest queued item without consuming it. False when empty.
    // push() drops the OLDEST under back-pressure, so this is the latest value
    // the producer published however far behind the consumer has fallen —
    // which is what lets a second reader ask "where is this now?" without
    // taking anything out of the stream the real consumer drains.
    bool peek_back(T& out) const {
        std::lock_guard<std::mutex> lock(m_);
        if (items_.empty()) return false;
        out = items_.back();
        return true;
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ./build/phig-tests --gtest_filter='ProgressQueue.*'
```

Expected: PASS, including the four pre-existing `ProgressQueue` tests.

- [ ] **Step 5: Commit**

```bash
git add core/progress_queue.h tests/test_progress_queue.cpp
git commit -m "Add ProgressQueue::peek_back

Reads the newest queued item without consuming it. push() drops the
oldest under back-pressure, so the tail is the latest value the producer
published however far behind the consumer has fallen. Plan 056's
snapshot rests on that property."
```

---

### Task 2: Extract poll's per-kind mapping into a shared helper

A pure refactor with no behaviour change. It comes before the new entry points because it is what stops poll and snapshot from ever disagreeing about what a record means.

**Files:**
- Modify: `libphig/operations.cpp` (the anonymous namespace around lines 20-89, and `phig_op_poll` at 204-269)
- Test: `tests/test_libphig_operations.cpp` — no new tests; the existing 22 must stay green

**Interfaces:**
- Produces:
  - `struct ProgressCounters { phig_op_stage stage; uint64_t done, total, processed, skipped, errors; }` — the kind-independent part of one record.
  - `struct MappedProgress { ProgressCounters counters; std::string path; std::string error; std::string text; }` — `error` is the failure to retain, `text` is what poll displays (an engine's warning rides `text` but is not an `error`).
  - `MappedProgress map_progress(const ScanProgress&)` and the same for `ThumbsProgress`, `FacesProgress`, `OrganizeProgress`.

  Tasks 4 and 6 consume `ProgressCounters` and `map_progress`.

- [ ] **Step 1: Confirm the existing tests pass before touching anything**

```bash
cmake --build build && ./build/phig-tests --gtest_filter='LibphigOp*:LibphigOrganize*'
```

Expected: PASS. This is the baseline the refactor must preserve. Record the test count.

- [ ] **Step 2: Add the mapping helpers**

In `libphig/operations.cpp`, inside the anonymous namespace, above `fill`:

```cpp
// The kind-independent part of one engine progress record. Every engine sets
// `total` on its per-item record, not only on its one-shot stage record, so a
// single record is a complete picture rather than a fragment — which is what
// lets a snapshot report the newest one instead of accumulating.
struct ProgressCounters {
    phig_op_stage stage = PHIG_STAGE_STARTING;
    uint64_t done = 0, total = 0, processed = 0, skipped = 0, errors = 0;
};

// One record, mapped. `error` is the failure to retain and is empty unless the
// file failed; `text` is what phig_op_poll shows a caller, which for the two
// engines that have warnings is the warning when there is no error.
struct MappedProgress {
    ProgressCounters counters;
    std::string path;
    std::string error;
    std::string text;
};

MappedProgress map_progress(const ScanProgress& p) {
    MappedProgress m;
    m.counters.stage = PHIG_STAGE_FILE_DONE;
    if (p.stage == ScanStage::Walked) {
        m.counters.stage = PHIG_STAGE_WALKED;
    } else if (p.stage == ScanStage::LoadingFaceModels) {
        m.counters.stage = PHIG_STAGE_LOADING_FACE_MODELS;
    }
    m.counters.done = p.done;
    m.counters.total = p.total;
    m.counters.processed = p.processed;
    m.counters.skipped = p.skipped;
    m.counters.errors = p.errors;
    m.path = p.path;
    m.error = p.error;
    m.text = p.error;
    return m;
}

MappedProgress map_progress(const ThumbsProgress& p) {
    MappedProgress m;
    m.counters.stage = p.stage == ThumbsStage::Selected ? PHIG_STAGE_SELECTED : PHIG_STAGE_FILE_DONE;
    m.counters.done = p.done;
    m.counters.total = p.total;
    // `processed` is the running count of thumbnails generated, matching
    // `generated` on the result.
    m.counters.processed = p.generated;
    m.counters.skipped = p.skipped;
    m.counters.errors = p.errors;
    m.path = p.path;
    m.error = p.error;
    // A decode that succeeded with a complaint is reported, not counted:
    // `warning` rides the field a caller renders, while `errors` stays the
    // count of files that actually failed.
    m.text = p.error.empty() ? p.warning : p.error;
    return m;
}

MappedProgress map_progress(const FacesProgress& p) {
    MappedProgress m;
    m.counters.stage = PHIG_STAGE_FILE_DONE;
    if (p.stage == FacesStage::Selected) {
        m.counters.stage = PHIG_STAGE_SELECTED;
    } else if (p.stage == FacesStage::LoadingFaceModels) {
        m.counters.stage = PHIG_STAGE_LOADING_FACE_MODELS;
    }
    m.counters.done = p.done;
    m.counters.total = p.total;
    // Faces reports no processed/skipped split — a selected file either yields
    // its faces or fails — so those stay 0. See the header.
    m.counters.errors = p.errors;
    m.path = p.path;
    m.error = p.error;
    m.text = p.error.empty() ? p.warning : p.error;
    return m;
}

MappedProgress map_progress(const OrganizeProgress& p) {
    MappedProgress m;
    m.counters.stage =
        p.stage == OrganizeStage::Planned ? PHIG_STAGE_SELECTED : PHIG_STAGE_FILE_DONE;
    // `done`/`total` count parents — the photos the caller selected — while
    // `processed` carries files actually written, including the sidecars and
    // attached JPEGs that travel with a parent (plan 042 decision 3). A caller
    // renders "412 of 1,204 photos, 1,238 files".
    m.counters.done = p.done;
    m.counters.total = p.total;
    m.counters.processed = p.files_written;
    m.counters.skipped = p.skipped;
    m.counters.errors = p.errors;
    m.path = p.path;
    m.error = p.error;
    m.text = p.error;
    return m;
}
```

- [ ] **Step 3: Rewrite `fill` to take the mapped record**

Replace the existing `fill` (lines 71-87) with:

```cpp
// Fills the caller's record from one mapped update. `path` and `error` are
// staged in the handle so the pointers stay valid until the next poll, which is
// what the header promises.
void fill(phig_op* op, phig_op_progress* out, const MappedProgress& m) {
    op->poll_path = m.path;
    op->poll_error = m.text;
    out->stage = m.counters.stage;
    out->done = m.counters.done;
    out->total = m.counters.total;
    out->processed = m.counters.processed;
    out->skipped = m.counters.skipped;
    out->errors = m.counters.errors;
    out->path = op->poll_path.c_str();
    out->error = op->poll_error.c_str();
}
```

- [ ] **Step 4: Rewrite `phig_op_poll` to use them**

Replace the body of `phig_op_poll` (lines 204-269) with:

```cpp
bool phig_op_poll(phig_op* op, phig_op_progress* out) {
    if (op == nullptr || out == nullptr) return false;

    MappedProgress m;
    switch (op->kind) {
        case phig_op::Kind::Index: {
            ScanProgress p;
            if (!op->index_progress.try_pop(p)) return false;
            m = map_progress(p);
            break;
        }
        case phig_op::Kind::Thumbs: {
            ThumbsProgress p;
            if (!op->thumbs_progress.try_pop(p)) return false;
            m = map_progress(p);
            break;
        }
        case phig_op::Kind::Faces: {
            FacesProgress p;
            if (!op->faces_progress.try_pop(p)) return false;
            m = map_progress(p);
            break;
        }
        case phig_op::Kind::Plan:
        case phig_op::Kind::Copy:
        case phig_op::Kind::Move: {
            OrganizeProgress p;
            if (!op->organize_progress.try_pop(p)) return false;
            m = map_progress(p);
            break;
        }
    }
    if (!m.error.empty()) op->retain_error(m.error);
    fill(op, out, m);
    return true;
}
```

Note the switch has no `default:`, so a new `Kind` is a `-Wswitch` error rather than a silent `return false`. That is deliberate — plan 042 shipped a poll switch missing its new cases.

There is one behaviour subtlety to preserve: an update popped from a kind whose switch arm returns early leaves `m` default-constructed. That cannot happen here, because every arm either returns or assigns `m`.

- [ ] **Step 5: Run the full suite to verify nothing changed**

```bash
cmake --build build && ctest --test-dir build --parallel
```

Expected: the same pass/skip counts as Step 1's baseline. A refactor that changes a count has changed behaviour.

- [ ] **Step 6: Commit**

```bash
git add libphig/operations.cpp
git commit -m "Extract phig_op_poll's per-kind mapping into map_progress

One mapping site per engine record, shared rather than inline in poll's
switch, so the snapshot added next cannot drift from what poll reports.
Plan 042 shipped a poll switch missing its new cases entirely and no test
caught it, which is the failure this shape removes.

No behaviour change."
```

---

### Task 3: `phig_op_type` and `phig_op_get_type`

**Files:**
- Modify: `libphig/include/phig/phig.h` (after the `phig_op_state` enum, around line 414)
- Modify: `libphig/operations.cpp`
- Modify: `tests/test_libphig_c_compile.c`
- Create: `tests/test_libphig_op_status.cpp`
- Modify: `CMakeLists.txt` (test source list, around line 154)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `phig_op_type` with `PHIG_OP_TYPE_INDEX = 0`, `THUMBS = 1`, `FACES = 2`, `ORGANIZE_PLAN = 3`, `COPY = 4`, `MOVE = 5`.
  - `phig_status phig_op_get_type(phig_op* op, phig_op_type* out)`.
  - `phig_op_type kind_to_type(phig_op::Kind)` in `operations.cpp`'s anonymous namespace — Task 6 consumes it.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_libphig_op_status.cpp`. The fixture is copied from `tests/test_libphig_operations.cpp` rather than shared, because that file keeps it in an anonymous namespace; duplicating ~40 lines is cheaper than introducing a test-only header for two files.

```cpp
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "database.h"
#include "phig/phig.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A scratch collection plus a redirected thumbnail cache, so an operation
// writes into the fixture instead of the user's real cache.
struct StatusFixture {
    fs::path dir;
    std::string db_path;
    std::string orig_cache;
    bool had_cache = false;

    explicit StatusFixture(int images = 0) {
        dir = make_temp_dir("libphig-op-status");
        db_path = (dir / "phig.db").string();

        const char* xdg = std::getenv("XDG_CACHE_HOME");
        had_cache = xdg != nullptr;
        if (xdg) orig_cache = xdg;
        setenv("XDG_CACHE_HOME", dir.c_str(), 1);

        Database db(db_path);
        db.init_schema();
        for (int i = 0; i < images; ++i) {
            fs::path p = dir / ("img" + std::to_string(i) + ".jpg");
            fs::copy_file("tests/data/faces_small.jpg", p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            info.sha256 = std::string(62, '0') + (i < 10 ? "0" : "") + std::to_string(i);
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db.insert(info);
        }
    }

    ~StatusFixture() {
        if (had_cache) {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        } else {
            unsetenv("XDG_CACHE_HOME");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    phig_ctx* open() {
        phig_config cfg{};
        cfg.db_path = db_path.c_str();
        cfg.thumbs_dir = nullptr;
        cfg.models_dir = nullptr;
        cfg.mode = PHIG_READ_WRITE;
        phig_ctx* ctx = nullptr;
        EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
        return ctx;
    }
};

// Starts a thumbs operation over the fixture. Thumbs is the cheapest operation
// that publishes a Selected record and then per-file records.
phig_op* start_thumbs(phig_ctx* ctx) {
    phig_thumbs_options opts{};
    opts.match = nullptr;
    opts.match_count = 0;
    opts.exclude = nullptr;
    opts.exclude_count = 0;
    opts.size = 0;
    opts.force = false;
    opts.parallel = 1;
    phig_op* op = nullptr;
    EXPECT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    return op;
}

}  // namespace

TEST(LibphigOpType, ReportsIndexType) {
    StatusFixture fx;
    phig_ctx* ctx = fx.open();

    phig_index_options opts{};
    opts.directory = fx.dir.c_str();
    opts.recursive = false;
    opts.include_hidden = false;
    opts.force = false;
    opts.media = PHIG_MEDIA_ANY;
    opts.parallel = 1;
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &opts, &op), PHIG_OK);

    phig_op_type type = PHIG_OP_TYPE_MOVE;
    EXPECT_EQ(phig_op_get_type(op, &type), PHIG_OK);
    EXPECT_EQ(type, PHIG_OP_TYPE_INDEX);

    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpType, ReportsThumbsType) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);

    phig_op_type type = PHIG_OP_TYPE_MOVE;
    EXPECT_EQ(phig_op_get_type(op, &type), PHIG_OK);
    EXPECT_EQ(type, PHIG_OP_TYPE_THUMBS);

    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpType, ReportsFacesType) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();

    phig_faces_options opts{};
    opts.match = nullptr;
    opts.match_count = 0;
    opts.exclude = nullptr;
    opts.exclude_count = 0;
    opts.parallel = 1;
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_faces_start(ctx, &opts, &op), PHIG_OK);

    phig_op_type type = PHIG_OP_TYPE_MOVE;
    EXPECT_EQ(phig_op_get_type(op, &type), PHIG_OK);
    EXPECT_EQ(type, PHIG_OP_TYPE_FACES);

    phig_op_cancel(op);
    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpType, ReportsOrganizePlanCopyAndMoveTypes) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    const std::string dest = (fx.dir / "out").string();

    phig_organize_options opts{};
    opts.destination = dest.c_str();
    opts.format = nullptr;
    opts.match = nullptr;
    opts.match_count = 0;
    opts.exclude = nullptr;
    opts.exclude_count = 0;
    opts.path_prefix = nullptr;

    phig_op* plan_op = nullptr;
    ASSERT_EQ(phig_op_organize_plan_start(ctx, &opts, &plan_op), PHIG_OK);
    phig_op_type type = PHIG_OP_TYPE_INDEX;
    EXPECT_EQ(phig_op_get_type(plan_op, &type), PHIG_OK);
    EXPECT_EQ(type, PHIG_OP_TYPE_ORGANIZE_PLAN);

    phig_op_wait(plan_op);
    phig_organize_plan* plan = nullptr;
    ASSERT_EQ(phig_op_organize_plan_take(plan_op, &plan), PHIG_OK);
    phig_op_free(plan_op);

    phig_op* copy_op = nullptr;
    ASSERT_EQ(phig_op_copy_start(ctx, plan, &copy_op), PHIG_OK);
    EXPECT_EQ(phig_op_get_type(copy_op, &type), PHIG_OK);
    EXPECT_EQ(type, PHIG_OP_TYPE_COPY);
    phig_op_wait(copy_op);
    phig_op_free(copy_op);

    phig_op* move_op = nullptr;
    ASSERT_EQ(phig_op_move_start(ctx, plan, &move_op), PHIG_OK);
    EXPECT_EQ(phig_op_get_type(move_op, &type), PHIG_OK);
    EXPECT_EQ(type, PHIG_OP_TYPE_MOVE);
    phig_op_wait(move_op);
    phig_op_free(move_op);

    phig_organize_plan_free(plan);
    phig_shutdown(ctx);
}

TEST(LibphigOpType, RejectsNullArguments) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);

    phig_op_type type = PHIG_OP_TYPE_MOVE;
    EXPECT_EQ(phig_op_get_type(nullptr, &type), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(type, PHIG_OP_TYPE_MOVE);  // left alone, not overwritten with 0
    EXPECT_EQ(phig_op_get_type(op, nullptr), PHIG_ERR_INVALID_ARG);

    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}
```

- [ ] **Step 2: Register the new test file**

In `CMakeLists.txt`, in the `phig-tests` source list beside `tests/test_libphig_operations.cpp`:

```cmake
    tests/test_libphig_op_status.cpp
```

- [ ] **Step 3: Run to verify it fails**

```bash
cmake -B build && cmake --build build 2>&1 | tail -20
```

Expected: compile error, `use of undeclared identifier 'phig_op_get_type'`.

- [ ] **Step 4: Declare the enum and entry point in the header**

In `libphig/include/phig/phig.h`, after the `phig_op_state` enum:

```c
/* Which operation a handle refers to, so a caller managing several can dispatch
 * to the right typed result getter without having remembered what it started.
 * Values are published ABI: append only, never renumber. */
typedef enum {
    PHIG_OP_TYPE_INDEX = 0,
    PHIG_OP_TYPE_THUMBS = 1,
    PHIG_OP_TYPE_FACES = 2,
    PHIG_OP_TYPE_ORGANIZE_PLAN = 3,
    PHIG_OP_TYPE_COPY = 4,
    PHIG_OP_TYPE_MOVE = 5
} phig_op_type;

/* Returns PHIG_ERR_INVALID_ARG and leaves *out alone for a NULL argument. This
 * returns a status rather than a phig_op_type because no type value is an
 * honest error sentinel — PHIG_OP_TYPE_INDEX is 0. */
PHIG_API phig_status phig_op_get_type(phig_op* op, phig_op_type* out);
```

- [ ] **Step 5: Implement it**

In `libphig/operations.cpp`'s anonymous namespace:

```cpp
// No `default:` — a new Kind must be a -Wswitch error here rather than silently
// reporting itself as an index operation. The return after the switch is
// unreachable and exists only to satisfy compilers that cannot see that.
phig_op_type kind_to_type(phig_op::Kind k) {
    switch (k) {
        case phig_op::Kind::Index:
            return PHIG_OP_TYPE_INDEX;
        case phig_op::Kind::Thumbs:
            return PHIG_OP_TYPE_THUMBS;
        case phig_op::Kind::Faces:
            return PHIG_OP_TYPE_FACES;
        case phig_op::Kind::Plan:
            return PHIG_OP_TYPE_ORGANIZE_PLAN;
        case phig_op::Kind::Copy:
            return PHIG_OP_TYPE_COPY;
        case phig_op::Kind::Move:
            return PHIG_OP_TYPE_MOVE;
    }
    return PHIG_OP_TYPE_INDEX;
}
```

And in the `extern "C"` block, beside `phig_op_status`:

```cpp
phig_status phig_op_get_type(phig_op* op, phig_op_type* out) {
    if (op == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = kind_to_type(op->kind);
    return PHIG_OK;
}
```

- [ ] **Step 6: Pin the enum and reference the symbol from the C compile check**

In `tests/test_libphig_c_compile.c`, add a pin struct after `phig_op_abi_pin`:

```c
/* Operation types are published ABI for the same reason as the states above. */
struct phig_op_type_abi_pin {
    int index : (PHIG_OP_TYPE_INDEX == 0) ? 1 : -1;
    int thumbs : (PHIG_OP_TYPE_THUMBS == 1) ? 1 : -1;
    int faces : (PHIG_OP_TYPE_FACES == 2) ? 1 : -1;
    int plan : (PHIG_OP_TYPE_ORGANIZE_PLAN == 3) ? 1 : -1;
    int copy : (PHIG_OP_TYPE_COPY == 4) ? 1 : -1;
    int move : (PHIG_OP_TYPE_MOVE == 5) ? 1 : -1;
};
```

And inside the operations block of `phig_c_compile_check`, beside `(void)phig_op_status(op);`:

```c
        {
            phig_op_type optype = PHIG_OP_TYPE_INDEX;
            (void)phig_op_get_type(op, &optype);
        }
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
clang-format -i libphig/include/phig/phig.h
cmake --build build && ./build/phig-tests --gtest_filter='LibphigOpType.*'
```

Expected: PASS, 5 tests.

- [ ] **Step 8: Commit**

```bash
git add libphig/include/phig/phig.h libphig/operations.cpp \
        tests/test_libphig_op_status.cpp tests/test_libphig_c_compile.c CMakeLists.txt
git commit -m "Add phig_op_type and phig_op_get_type

A caller holding several operations can now ask which typed result getter
applies rather than having to remember what it started. Returns a status
rather than the type because PHIG_OP_TYPE_INDEX is 0 and would be a
dishonest sentinel for a null handle.

kind_to_type's switch has no default, so a new operation kind is a
-Wswitch error rather than a silent misreport."
```

---

### Task 4: `PHIG_STAGE_STARTING` and `phig_op_snapshot`

**Files:**
- Modify: `libphig/include/phig/phig.h` (the `phig_op_stage` enum around line 431; declaration after `phig_op_poll`)
- Modify: `libphig/internal.h` (`phig_op`, after `poll_error` around line 212)
- Modify: `libphig/operations.cpp`
- Modify: `tests/test_libphig_op_status.cpp`
- Modify: `tests/test_libphig_c_compile.c`

**Interfaces:**
- Consumes: `ProgressQueue::peek_back` (Task 1); `ProgressCounters` and `map_progress` (Task 2).
- Produces:
  - `PHIG_STAGE_STARTING = 4`.
  - `phig_status phig_op_snapshot(phig_op* op, phig_op_progress* out)`.
  - `bool newest_counters(phig_op* op, ProgressCounters& out)` in the anonymous namespace — Task 6 consumes it.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_libphig_op_status.cpp`:

```cpp
TEST(LibphigOpSnapshot, ReportsStartingBeforeAnythingIsPublished) {
    // An index operation over an empty directory reaches its first published
    // record only after the walk. Reading immediately after start is racy by
    // nature, so this asserts the honest set: either nothing has been published
    // (STARTING, all zero) or something has, and never FILE_DONE-with-zeros,
    // which is the misleading default this stage exists to avoid.
    StatusFixture fx;
    phig_ctx* ctx = fx.open();

    phig_index_options opts{};
    opts.directory = fx.dir.c_str();
    opts.recursive = false;
    opts.include_hidden = false;
    opts.force = false;
    opts.media = PHIG_MEDIA_ANY;
    opts.parallel = 1;
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &opts, &op), PHIG_OK);

    phig_op_progress snap{};
    ASSERT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);
    if (snap.stage == PHIG_STAGE_STARTING) {
        EXPECT_EQ(snap.done, 0u);
        EXPECT_EQ(snap.total, 0u);
        EXPECT_EQ(snap.processed, 0u);
        EXPECT_EQ(snap.skipped, 0u);
        EXPECT_EQ(snap.errors, 0u);
    }
    // A snapshot's strings are static empties, never pointers into the handle.
    ASSERT_NE(snap.path, nullptr);
    ASSERT_NE(snap.error, nullptr);
    EXPECT_STREQ(snap.path, "");
    EXPECT_STREQ(snap.error, "");

    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, EmptySelectionIsSelectedWithZeroTotalNotStarting) {
    // The distinction PHIG_STAGE_STARTING exists for: a run that has not
    // started yet versus one that started and has no work to do. A thumbs run
    // whose glob matches nothing publishes Selected with total = 0.
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();

    static const char* const globs[1] = {"*.nomatch"};
    phig_thumbs_options opts{};
    opts.match = globs;
    opts.match_count = 1;
    opts.exclude = nullptr;
    opts.exclude_count = 0;
    opts.size = 0;
    opts.force = false;
    opts.parallel = 1;
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_op_progress snap{};
    ASSERT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);
    EXPECT_EQ(snap.stage, PHIG_STAGE_SELECTED);
    EXPECT_EQ(snap.total, 0u);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, IsNonConsuming) {
    StatusFixture fx(2);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    phig_op_wait(op);

    phig_op_progress a{}, b{};
    ASSERT_EQ(phig_op_snapshot(op, &a), PHIG_OK);
    ASSERT_EQ(phig_op_snapshot(op, &b), PHIG_OK);
    EXPECT_EQ(a.stage, b.stage);
    EXPECT_EQ(a.done, b.done);
    EXPECT_EQ(a.total, b.total);

    // Snapshotting took nothing out of the stream poll drains.
    phig_op_progress p{};
    EXPECT_TRUE(phig_op_poll(op, &p));

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, ReportsNewestNotOldestWhenCallerNeverPolled) {
    // The drop-oldest property made observable: with several records queued and
    // nothing polled, a snapshot reports the last one, not the first.
    StatusFixture fx(3);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    phig_op_wait(op);

    phig_op_progress snap{};
    ASSERT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);

    // Drain the whole stream and keep the last update poll yields.
    phig_op_progress last{};
    phig_op_progress p{};
    bool any = false;
    while (phig_op_poll(op, &p)) {
        last = p;
        any = true;
    }
    ASSERT_TRUE(any);
    EXPECT_EQ(snap.stage, last.stage);
    EXPECT_EQ(snap.done, last.done);
    EXPECT_EQ(snap.total, last.total);
    EXPECT_EQ(snap.processed, last.processed);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, ReportsLastPolledRecordOnceTheQueueIsDrained) {
    StatusFixture fx(3);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    phig_op_wait(op);

    phig_op_progress last{};
    phig_op_progress p{};
    bool any = false;
    while (phig_op_poll(op, &p)) {
        last = p;
        any = true;
    }
    ASSERT_TRUE(any);
    EXPECT_FALSE(phig_op_poll(op, &p));  // drained

    phig_op_progress snap{};
    ASSERT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);
    EXPECT_EQ(snap.stage, last.stage);
    EXPECT_EQ(snap.done, last.done);
    EXPECT_EQ(snap.total, last.total);
    EXPECT_EQ(snap.processed, last.processed);
    EXPECT_NE(snap.stage, PHIG_STAGE_STARTING);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, StillReportsAfterTheOperationIsDone) {
    StatusFixture fx(2);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    phig_op_wait(op);
    ASSERT_EQ(phig_op_status(op), PHIG_OP_DONE);

    phig_op_progress snap{};
    ASSERT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);
    EXPECT_NE(snap.stage, PHIG_STAGE_STARTING);
    EXPECT_GT(snap.total, 0u);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, RejectsNullArguments) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);

    phig_op_progress snap{};
    EXPECT_EQ(phig_op_snapshot(nullptr, &snap), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_snapshot(op, nullptr), PHIG_ERR_INVALID_ARG);

    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpSnapshot, ReportsForEveryOperationType) {
    // The conformance shape plan 042's missing poll cases slipped through for
    // want of: exercise every arm of newest_counters' switch, so an operation
    // kind whose arm was never added fails here rather than reporting STARTING
    // forever to a caller nobody wrote a test for.
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    const std::string dest = (fx.dir / "out").string();

    auto expect_published = [](phig_op* op, phig_op_type expected) {
        phig_op_type type = PHIG_OP_TYPE_MOVE;
        ASSERT_EQ(phig_op_get_type(op, &type), PHIG_OK);
        EXPECT_EQ(type, expected);
        phig_op_progress snap{};
        ASSERT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);
        // Every operation publishes at least one record before it finishes, so
        // a completed one reporting STARTING means its arm is missing.
        EXPECT_NE(snap.stage, PHIG_STAGE_STARTING);
    };

    phig_index_options iopts{};
    iopts.directory = fx.dir.c_str();
    iopts.recursive = false;
    iopts.include_hidden = false;
    iopts.force = false;
    iopts.media = PHIG_MEDIA_ANY;
    iopts.parallel = 1;
    phig_op* index_op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &iopts, &index_op), PHIG_OK);
    phig_op_wait(index_op);
    expect_published(index_op, PHIG_OP_TYPE_INDEX);
    phig_op_free(index_op);

    phig_op* thumbs_op = start_thumbs(ctx);
    phig_op_wait(thumbs_op);
    expect_published(thumbs_op, PHIG_OP_TYPE_THUMBS);
    phig_op_free(thumbs_op);

    phig_faces_options fopts{};
    fopts.match = nullptr;
    fopts.match_count = 0;
    fopts.exclude = nullptr;
    fopts.exclude_count = 0;
    fopts.parallel = 1;
    phig_op* faces_op = nullptr;
    ASSERT_EQ(phig_op_faces_start(ctx, &fopts, &faces_op), PHIG_OK);
    phig_op_wait(faces_op);
    // Faces publishes Selected before it touches a model, so this holds whether
    // or not the detector models are present on this machine.
    expect_published(faces_op, PHIG_OP_TYPE_FACES);
    phig_op_free(faces_op);

    phig_organize_options oopts{};
    oopts.destination = dest.c_str();
    oopts.format = nullptr;
    oopts.match = nullptr;
    oopts.match_count = 0;
    oopts.exclude = nullptr;
    oopts.exclude_count = 0;
    oopts.path_prefix = nullptr;

    phig_op* plan_op = nullptr;
    ASSERT_EQ(phig_op_organize_plan_start(ctx, &oopts, &plan_op), PHIG_OK);
    phig_op_wait(plan_op);
    expect_published(plan_op, PHIG_OP_TYPE_ORGANIZE_PLAN);
    phig_organize_plan* plan = nullptr;
    ASSERT_EQ(phig_op_organize_plan_take(plan_op, &plan), PHIG_OK);
    phig_op_free(plan_op);

    phig_op* copy_op = nullptr;
    ASSERT_EQ(phig_op_copy_start(ctx, plan, &copy_op), PHIG_OK);
    phig_op_wait(copy_op);
    expect_published(copy_op, PHIG_OP_TYPE_COPY);
    phig_op_free(copy_op);

    phig_op* move_op = nullptr;
    ASSERT_EQ(phig_op_move_start(ctx, plan, &move_op), PHIG_OK);
    phig_op_wait(move_op);
    expect_published(move_op, PHIG_OP_TYPE_MOVE);
    phig_op_free(move_op);

    phig_organize_plan_free(plan);
    phig_shutdown(ctx);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build 2>&1 | tail -20
```

Expected: compile error, `use of undeclared identifier 'phig_op_snapshot'`.

- [ ] **Step 3: Add the stage value and declaration to the header**

In `libphig/include/phig/phig.h`, append to the `phig_op_stage` enum (note the comma after the existing last enumerator):

```c
    PHIG_STAGE_LOADING_FACE_MODELS = 3,
    /* No update has been published yet. Only phig_op_snapshot reports this;
       phig_op_poll never does, because it has a record in hand whenever it
       returns true. Distinct from SELECTED with total = 0, which is a run that
       has started and has no work to do. */
    PHIG_STAGE_STARTING = 4
} phig_op_stage;
```

And after the `phig_op_poll` declaration:

```c
/* The operation's current absolute counters, without consuming anything from
 * the update stream. Safe from any thread at any time — before the operation
 * has published anything, while it runs, and after it finishes.
 *
 * `path` and `error` point at static empty strings: a snapshot reports totals,
 * not the most recent file. Unlike phig_op_poll's output they carry no validity
 * window and need not be copied.
 *
 * This is what a caller repainting on a timer wants. phig_op_poll stays for a
 * caller that needs every update, including per-file paths and error text. */
PHIG_API phig_status phig_op_snapshot(phig_op* op, phig_op_progress* out);
```

- [ ] **Step 4: Add the cached last-polled counters to the handle**

In `libphig/internal.h`, in `struct phig_op` after `poll_error`:

```cpp
    // The counters from the last record phig_op_poll consumed, so a snapshot
    // taken once the queue is drained still reports the newest published value
    // rather than falling back to STARTING. Counters only — no pointers — so
    // this carries no lifetime rule. Guarded because poll and snapshot may run
    // on different threads.
    std::mutex last_mutex;
    bool have_last = false;
    ProgressCounters last;
```

`ProgressCounters` currently lives in `operations.cpp`'s anonymous namespace. Move it to `internal.h`, above `struct phig_op`, and delete it from the anonymous namespace (leave `MappedProgress` and the `map_progress` overloads where they are — only `phig_op` needs the counters type):

```cpp
// The kind-independent part of one engine progress record. Every engine sets
// `total` on its per-item record, not only on its one-shot stage record, so a
// single record is a complete picture rather than a fragment — which is what
// lets a snapshot report the newest one instead of accumulating.
struct ProgressCounters {
    phig_op_stage stage = PHIG_STAGE_STARTING;
    uint64_t done = 0, total = 0, processed = 0, skipped = 0, errors = 0;
};
```

- [ ] **Step 5: Implement the snapshot**

In `libphig/operations.cpp`'s anonymous namespace, after the `map_progress` overloads:

```cpp
// The newest counters this operation has published: the tail of its queue if
// anything is queued, otherwise whatever poll last consumed. False when the
// operation has published nothing at all.
//
// The tail is always at least as new as poll's cache, because poll pops from
// the front — so checking the queue first is what makes this the newest rather
// than merely a recent value.
bool newest_counters(phig_op* op, ProgressCounters& out) {
    switch (op->kind) {
        case phig_op::Kind::Index: {
            ScanProgress p;
            if (op->index_progress.peek_back(p)) {
                out = map_progress(p).counters;
                return true;
            }
            break;
        }
        case phig_op::Kind::Thumbs: {
            ThumbsProgress p;
            if (op->thumbs_progress.peek_back(p)) {
                out = map_progress(p).counters;
                return true;
            }
            break;
        }
        case phig_op::Kind::Faces: {
            FacesProgress p;
            if (op->faces_progress.peek_back(p)) {
                out = map_progress(p).counters;
                return true;
            }
            break;
        }
        case phig_op::Kind::Plan:
        case phig_op::Kind::Copy:
        case phig_op::Kind::Move: {
            OrganizeProgress p;
            if (op->organize_progress.peek_back(p)) {
                out = map_progress(p).counters;
                return true;
            }
            break;
        }
    }
    std::lock_guard<std::mutex> g(op->last_mutex);
    if (!op->have_last) return false;
    out = op->last;
    return true;
}

// Writes counters into a caller's record with static empty strings, so the
// result carries no validity window.
void fill_snapshot(phig_op_progress* out, const ProgressCounters& c) {
    static const char kEmpty[] = "";
    out->stage = c.stage;
    out->done = c.done;
    out->total = c.total;
    out->processed = c.processed;
    out->skipped = c.skipped;
    out->errors = c.errors;
    out->path = kEmpty;
    out->error = kEmpty;
}
```

In `phig_op_poll`, record the mapped counters just before `fill`:

```cpp
    if (!m.error.empty()) op->retain_error(m.error);
    {
        std::lock_guard<std::mutex> g(op->last_mutex);
        op->last = m.counters;
        op->have_last = true;
    }
    fill(op, out, m);
    return true;
```

And the entry point, beside `phig_op_poll`:

```cpp
phig_status phig_op_snapshot(phig_op* op, phig_op_progress* out) {
    if (op == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    // No phig_ctx in scope, so a plain try/catch rather than PHIG_TRY, per
    // libphig/CLAUDE.md. map_progress copies the record's strings and can
    // therefore throw bad_alloc, and no exception may cross the boundary.
    try {
        ProgressCounters c;  // defaults to STARTING with every counter zero
        newest_counters(op, c);
        fill_snapshot(out, c);
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}
```

- [ ] **Step 6: Pin the stage and reference the symbol from the C compile check**

In `tests/test_libphig_c_compile.c`, add to `struct phig_op_abi_pin`:

```c
    int starting : (PHIG_STAGE_STARTING == 4) ? 1 : -1;
```

And in the operations block, beside `(void)phig_op_poll(op, &prog);`:

```c
        (void)phig_op_snapshot(op, &prog);
```

- [ ] **Step 7: Run the tests to verify they pass**

```bash
clang-format -i libphig/include/phig/phig.h
cmake --build build && ./build/phig-tests --gtest_filter='LibphigOpSnapshot.*'
```

Expected: PASS, 8 tests. `ReportsForEveryOperationType` runs whether or not the
face detector models are present — the faces engine publishes its `Selected`
record before it touches a model, so the assertion holds even when the
operation then fails for a missing model.

- [ ] **Step 8: Run the full suite**

```bash
ctest --test-dir build --parallel
```

Expected: no regressions against Task 2's baseline, plus the new tests.

- [ ] **Step 9: Commit**

```bash
git add libphig/include/phig/phig.h libphig/internal.h libphig/operations.cpp \
        tests/test_libphig_op_status.cpp tests/test_libphig_c_compile.c
git commit -m "Add phig_op_snapshot and PHIG_STAGE_STARTING

A snapshot is the newest record the operation published: the tail of its
queue if anything is queued, otherwise whatever poll last consumed. Every
engine already sets total on its per-item record, so a single record is a
complete picture, and the queue drops its oldest entry rather than its
newest, so the tail is the latest value however far behind a caller has
fallen. No engine change and no accumulation.

PHIG_STAGE_STARTING covers the case where nothing has been published.
FILE_DONE is 0 and would claim a file finished when none has, and the
state is genuinely distinct from SELECTED with total = 0.

A snapshot's path and error are static empty strings, so unlike poll's
output they carry no validity window."
```

---

### Task 5: Operation ids and the context registry

No public enumeration yet — this task establishes the registry and proves its lifetime rules, so Task 6 is only the read surface on top.

**Files:**
- Modify: `libphig/internal.h` (`OpRegistry`; `phig_ctx`; `phig_op`)
- Modify: `libphig/operations.cpp` (`make_op`, a `publish_op` helper, the six `_start` entry points, `phig_op_free`)
- Modify: `libphig/include/phig/phig.h`
- Modify: `tests/test_libphig_op_status.cpp`
- Modify: `tests/test_libphig_c_compile.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `struct OpRegistry { std::mutex m; std::map<uint64_t, phig_op*> ops; }` in `internal.h`.
  - `phig_ctx::registry` as `std::shared_ptr<OpRegistry>`.
  - `phig_op::id` (`uint64_t`) and `phig_op::registry` (`std::weak_ptr<OpRegistry>`).
  - `uint64_t phig_op_id(phig_op* op)` — 0 for a null handle.
  - `void publish_op(phig_ctx*, std::unique_ptr<phig_op>&, phig_op**)` in the anonymous namespace. Task 6 relies on the registry it populates.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_libphig_op_status.cpp`:

```cpp
TEST(LibphigOpId, IsNonZeroAndUniqueAcrossOperations) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();

    phig_op* a = start_thumbs(ctx);
    phig_op* b = start_thumbs(ctx);

    const uint64_t ida = phig_op_id(a);
    const uint64_t idb = phig_op_id(b);
    EXPECT_NE(ida, 0u);
    EXPECT_NE(idb, 0u);
    EXPECT_NE(ida, idb);
    EXPECT_EQ(phig_op_id(a), ida);  // stable across calls

    phig_op_wait(a);
    phig_op_wait(b);
    phig_op_free(a);
    phig_op_free(b);
    phig_shutdown(ctx);
}

TEST(LibphigOpId, IsZeroForNull) { EXPECT_EQ(phig_op_id(nullptr), 0u); }

TEST(LibphigOpId, IsNotReusedAfterFree) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();

    phig_op* a = start_thumbs(ctx);
    const uint64_t ida = phig_op_id(a);
    phig_op_wait(a);
    phig_op_free(a);

    phig_op* b = start_thumbs(ctx);
    EXPECT_NE(phig_op_id(b), ida);
    phig_op_wait(b);
    phig_op_free(b);
    phig_shutdown(ctx);
}

TEST(LibphigOpRegistry, ContextMayBeFreedWhileAnOperationIsStillAlive) {
    // The case the weak_ptr exists for. Today an operation and a context are
    // independent — phig_shutdown is a plain delete and make_op never stores
    // the ctx — and the registry must not turn that into an ordering rule.
    // Run this under ASan and TSan, not only the default build.
    StatusFixture fx(2);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);

    phig_shutdown(ctx);  // context gone first, deliberately

    phig_op_wait(op);
    phig_op_progress snap{};
    EXPECT_EQ(phig_op_snapshot(op, &snap), PHIG_OK);
    phig_op_free(op);  // deregistration finds no registry and does nothing
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build 2>&1 | tail -20
```

Expected: compile error, `use of undeclared identifier 'phig_op_id'`.

- [ ] **Step 3: Add the registry to `internal.h`**

Above `struct phig_ctx`:

```cpp
// The operations one context has in flight, so a client that lost a handle can
// still find them. Keyed by id and ordered, so enumeration is stable and
// matches the order the operations were started.
//
// Held by the context as a shared_ptr and by each operation as a weak_ptr. That
// asymmetry is load-bearing: phig_shutdown is a plain delete and make_op never
// stored the ctx, so an operation currently outlives its context legally. A
// back-pointer would turn that into a "free every operation first" rule; a
// weak_ptr makes a deregistration after the context is gone a no-op instead.
struct OpRegistry {
    std::mutex m;
    std::map<uint64_t, phig_op*> ops;
};
```

`phig_op` is only forward-declared at that point, which is all a pointer needs. Add `struct phig_op;` above `OpRegistry` if the file does not already forward-declare it, and add `#include <map>` and `#include <memory>` to the includes if absent.

In `struct phig_ctx`:

```cpp
    std::shared_ptr<OpRegistry> registry = std::make_shared<OpRegistry>();
```

In `struct phig_op`, beside `kind`:

```cpp
    // Assigned at start from a process-wide counter and never reused, so an id
    // never names a different operation. 0 is never a valid id.
    uint64_t id = 0;
    std::weak_ptr<OpRegistry> registry;
```

- [ ] **Step 4: Assign ids and register**

In `libphig/operations.cpp`, at the top of the anonymous namespace:

```cpp
// Process-wide rather than per-context, which makes an id unique across every
// context in the process at no extra cost. Starts at 1 so 0 is never valid.
std::atomic<uint64_t> next_op_id{1};
```

Add `#include <atomic>` if absent.

Extend `make_op`:

```cpp
phig_status make_op(phig_ctx* ctx, phig_op::Kind kind, std::unique_ptr<phig_op>& op) {
    if (ctx == nullptr) return PHIG_ERR_INVALID_ARG;
    if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
    op = std::make_unique<phig_op>(kind);
    op->db = std::make_unique<Database>(ctx->db_path);
    op->id = next_op_id.fetch_add(1, std::memory_order_relaxed);
    op->registry = ctx->registry;
    return PHIG_OK;
}
```

Add the publish helper beside it:

```cpp
// Registers the operation and hands ownership to the caller. Registration is
// last: an operation the caller never receives must never be reachable by id.
void publish_op(phig_ctx* ctx, std::unique_ptr<phig_op>& op, phig_op** out) {
    phig_op* raw = op.get();
    {
        std::lock_guard<std::mutex> g(ctx->registry->m);
        ctx->registry->ops[raw->id] = raw;
    }
    *out = op.release();
}
```

In each of the six `_start` entry points, replace the final `*out = op.release();` with:

```cpp
    publish_op(ctx, op, out);
```

The six are `phig_op_index_start`, `phig_op_thumbs_start`, `phig_op_faces_start`, `phig_op_organize_plan_start`, `phig_op_copy_start` and `phig_op_move_start`. Grep for `op.release()` to confirm none is missed:

```bash
grep -n "op.release()" libphig/operations.cpp
```

Expected after the change: one hit, inside `publish_op`.

- [ ] **Step 5: Deregister in `phig_op_free`**

```cpp
void phig_op_free(phig_op* op) {
    if (op == nullptr) return;
    // Deregister first, and under the registry lock, so a by-id read either
    // completed before this point or fails to find the id. A reader holding
    // that lock keeps this call waiting, which is what makes the pointer it is
    // reading through safe for the whole read. The weak_ptr is empty when the
    // context was freed first, which stays legal — see OpRegistry.
    if (auto reg = op->registry.lock()) {
        std::lock_guard<std::mutex> g(reg->m);
        reg->ops.erase(op->id);
    }
    // Joins rather than detaches: the worker holds a raw pointer to this handle
    // and to its Database, so returning while it still runs would leave it
    // writing through freed memory. A caller that does not want to wait calls
    // phig_op_cancel first.
    if (op->worker.joinable()) op->worker.join();
    delete op;
}
```

- [ ] **Step 6: Declare and implement `phig_op_id`**

In `libphig/include/phig/phig.h`, after `phig_op_get_type`:

```c
/* Stable for the operation's lifetime and never reused, so an id never names a
 * different operation. 0 is never valid and is what a NULL handle returns. */
PHIG_API uint64_t phig_op_id(phig_op* op);
```

In `libphig/operations.cpp`:

```cpp
uint64_t phig_op_id(phig_op* op) { return op == nullptr ? 0 : op->id; }
```

- [ ] **Step 7: Reference the symbol from the C compile check**

In `tests/test_libphig_c_compile.c`, in the operations block:

```c
        (void)phig_op_id(op);
```

- [ ] **Step 8: Run the tests to verify they pass**

```bash
clang-format -i libphig/include/phig/phig.h
cmake --build build && ./build/phig-tests --gtest_filter='LibphigOpId.*:LibphigOpRegistry.*'
```

Expected: PASS, 4 tests.

- [ ] **Step 9: Run the context-lifetime test under ASan**

```bash
cmake -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-san
ASAN_OPTIONS=detect_leaks=0 GTEST_DEATH_TEST_STYLE=threadsafe \
  ./build-san/phig-tests --gtest_filter='LibphigOpRegistry.*:LibphigOpId.*'
```

Expected: PASS, no ASan report. Leak detection is off because it reports through OpenCV and LibRaw, which this gate cannot fix.

- [ ] **Step 10: Commit**

```bash
git add libphig/include/phig/phig.h libphig/internal.h libphig/operations.cpp \
        tests/test_libphig_op_status.cpp tests/test_libphig_c_compile.c
git commit -m "Give operations ids and register them with their context

Ids come from a process-wide counter starting at 1 and are never reused,
so an id never names a different operation. The context holds the
registry by shared_ptr and each operation holds it weakly, which keeps
freeing a context with operations still running legal: phig_shutdown is a
plain delete and make_op never stored the ctx, so that independence
exists today and a back-pointer would have removed it.

phig_op_free deregisters under the registry lock before it joins, so a
by-id read either completes first or fails to find the id.

No enumeration surface yet."
```

---

### Task 6: `phig_op_list`, `phig_op_info_by_id`, `phig_op_cancel_by_id`

**Files:**
- Modify: `libphig/include/phig/phig.h`
- Modify: `libphig/operations.cpp`
- Modify: `tests/test_libphig_op_status.cpp`
- Modify: `tests/test_libphig_c_compile.c`

**Interfaces:**
- Consumes: `kind_to_type` (Task 3), `newest_counters` and `fill_snapshot` (Task 4), `OpRegistry` and `phig_ctx::registry` (Task 5).
- Produces: `phig_op_info`; `phig_op_list`; `phig_op_info_by_id`; `phig_op_cancel_by_id`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_libphig_op_status.cpp`:

```cpp
TEST(LibphigOpList, CountsWithoutWritingWhenCapIsZero) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* a = start_thumbs(ctx);
    phig_op* b = start_thumbs(ctx);

    size_t total = 0;
    ASSERT_EQ(phig_op_list(ctx, nullptr, sizeof(phig_op_info), 0, &total), PHIG_OK);
    EXPECT_EQ(total, 2u);

    phig_op_wait(a);
    phig_op_wait(b);
    phig_op_free(a);
    phig_op_free(b);
    phig_shutdown(ctx);
}

TEST(LibphigOpList, FillsInIdOrderAndReportsTheTrueTotalForAShortBuffer) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* a = start_thumbs(ctx);
    phig_op* b = start_thumbs(ctx);
    phig_op* c = start_thumbs(ctx);
    const uint64_t ida = phig_op_id(a);

    phig_op_info got[1];
    size_t total = 0;
    ASSERT_EQ(phig_op_list(ctx, got, sizeof(phig_op_info), 1, &total), PHIG_OK);
    EXPECT_EQ(total, 3u);          // true total, not what fit
    EXPECT_EQ(got[0].id, ida);     // ordered by id, so oldest first
    EXPECT_EQ(got[0].type, PHIG_OP_TYPE_THUMBS);

    phig_op_wait(a);
    phig_op_wait(b);
    phig_op_wait(c);
    phig_op_free(a);
    phig_op_free(b);
    phig_op_free(c);
    phig_shutdown(ctx);
}

TEST(LibphigOpList, DropsAnOperationOnceItIsFreed) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* a = start_thumbs(ctx);
    phig_op* b = start_thumbs(ctx);

    size_t total = 0;
    ASSERT_EQ(phig_op_list(ctx, nullptr, sizeof(phig_op_info), 0, &total), PHIG_OK);
    EXPECT_EQ(total, 2u);

    phig_op_wait(a);
    phig_op_free(a);

    ASSERT_EQ(phig_op_list(ctx, nullptr, sizeof(phig_op_info), 0, &total), PHIG_OK);
    EXPECT_EQ(total, 1u);

    phig_op_wait(b);
    phig_op_free(b);
    phig_shutdown(ctx);
}

TEST(LibphigOpList, WritesOnlyTheRequestedPrefixAndStridesByIt) {
    // Decision 6's whole point: struct_size is the array stride and the write
    // limit, so an older client that allocated a shorter record gets only the
    // prefix it has room for. This is the test that fails if the size is ever
    // read out of the record instead of taken from the parameter.
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* a = start_thumbs(ctx);
    phig_op* b = start_thumbs(ctx);

    // A short record: everything up to and including `type`.
    const size_t kShort = offsetof(phig_op_info, state);
    ASSERT_LT(kShort, sizeof(phig_op_info));

    unsigned char buf[3 * sizeof(phig_op_info)];
    std::memset(buf, 0xAB, sizeof(buf));
    size_t total = 0;
    ASSERT_EQ(phig_op_list(ctx, reinterpret_cast<phig_op_info*>(buf), kShort, 2, &total), PHIG_OK);
    EXPECT_EQ(total, 2u);

    // Element 0 and element 1 sit kShort apart, and the byte after element 1
    // is untouched — so nothing was written past what the caller allocated.
    uint64_t id0 = 0, id1 = 0;
    std::memcpy(&id0, buf + 0 * kShort, sizeof(id0));
    std::memcpy(&id1, buf + 1 * kShort, sizeof(id1));
    EXPECT_EQ(id0, phig_op_id(a));
    EXPECT_EQ(id1, phig_op_id(b));
    EXPECT_EQ(buf[2 * kShort], 0xAB);

    phig_op_wait(a);
    phig_op_wait(b);
    phig_op_free(a);
    phig_op_free(b);
    phig_shutdown(ctx);
}

TEST(LibphigOpInfoById, ReportsTypeStateAndProgress) {
    StatusFixture fx(2);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    const uint64_t id = phig_op_id(op);
    phig_op_wait(op);

    phig_op_info info{};
    ASSERT_EQ(phig_op_info_by_id(ctx, id, &info, sizeof(info)), PHIG_OK);
    EXPECT_EQ(info.id, id);
    EXPECT_EQ(info.type, PHIG_OP_TYPE_THUMBS);
    EXPECT_EQ(info.state, PHIG_OP_DONE);
    EXPECT_NE(info.progress.stage, PHIG_STAGE_STARTING);
    EXPECT_GT(info.progress.total, 0u);
    // Same rule as phig_op_snapshot: static empties, no validity window.
    EXPECT_STREQ(info.progress.path, "");
    EXPECT_STREQ(info.progress.error, "");

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(LibphigOpInfoById, IsNotFoundAfterTheOperationIsFreed) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    const uint64_t id = phig_op_id(op);

    phig_op_info info{};
    ASSERT_EQ(phig_op_info_by_id(ctx, id, &info, sizeof(info)), PHIG_OK);

    phig_op_wait(op);
    phig_op_free(op);

    EXPECT_EQ(phig_op_info_by_id(ctx, id, &info, sizeof(info)), PHIG_ERR_NOT_FOUND);
    phig_shutdown(ctx);
}

TEST(LibphigOpInfoById, IsNotFoundForAnIdFromAnotherContext) {
    StatusFixture fx(1);
    phig_ctx* a = fx.open();
    phig_ctx* b = fx.open();
    phig_op* op = start_thumbs(a);
    const uint64_t id = phig_op_id(op);

    phig_op_info info{};
    EXPECT_EQ(phig_op_info_by_id(b, id, &info, sizeof(info)), PHIG_ERR_NOT_FOUND);

    phig_op_wait(op);
    phig_op_free(op);
    phig_shutdown(a);
    phig_shutdown(b);
}

TEST(LibphigOpCancelById, CancelsAndIsNotFoundForAnUnknownId) {
    StatusFixture fx(3);
    phig_ctx* ctx = fx.open();
    phig_op* op = start_thumbs(ctx);
    const uint64_t id = phig_op_id(op);

    EXPECT_EQ(phig_op_cancel_by_id(ctx, id), PHIG_OK);
    phig_op_wait(op);
    EXPECT_EQ(phig_op_status(op), PHIG_OP_CANCELLED);

    phig_op_free(op);
    EXPECT_EQ(phig_op_cancel_by_id(ctx, id), PHIG_ERR_NOT_FOUND);
    phig_shutdown(ctx);
}

TEST(LibphigOpList, RejectsNullArguments) {
    StatusFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op_info info{};
    size_t total = 0;

    EXPECT_EQ(phig_op_list(nullptr, &info, sizeof(info), 1, &total), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_list(ctx, &info, sizeof(info), 1, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_list(ctx, nullptr, sizeof(info), 1, &total), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_list(ctx, &info, 0, 1, &total), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_info_by_id(nullptr, 1, &info, sizeof(info)), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_info_by_id(ctx, 1, nullptr, sizeof(info)), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_cancel_by_id(nullptr, 1), PHIG_ERR_INVALID_ARG);

    phig_shutdown(ctx);
}

TEST(LibphigOpList, IsSafeAgainstConcurrentFree) {
    // Run under TSan. phig_op_free deregisters under the registry lock before
    // it destroys anything, and every by-id read holds that lock for the whole
    // read, so a list running beside a free must never touch a freed handle.
    StatusFixture fx(2);
    phig_ctx* ctx = fx.open();

    std::vector<phig_op*> ops;
    for (int i = 0; i < 8; ++i) ops.push_back(start_thumbs(ctx));
    for (phig_op* op : ops) phig_op_wait(op);

    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load()) {
            phig_op_info got[8];
            size_t total = 0;
            (void)phig_op_list(ctx, got, sizeof(phig_op_info), 8, &total);
        }
    });

    for (phig_op* op : ops) phig_op_free(op);
    stop.store(true);
    reader.join();

    size_t total = 0;
    ASSERT_EQ(phig_op_list(ctx, nullptr, sizeof(phig_op_info), 0, &total), PHIG_OK);
    EXPECT_EQ(total, 0u);
    phig_shutdown(ctx);
}
```

Add these includes at the top of the file if not already present: `<atomic>`, `<cstddef>`, `<cstring>`, `<thread>`, `<vector>`.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build 2>&1 | tail -20
```

Expected: compile error, `use of undeclared identifier 'phig_op_list'`.

- [ ] **Step 3: Declare the record and the three entry points**

In `libphig/include/phig/phig.h`, after `phig_op_id`:

```c
/* One operation, by value. Everything a generic "operations in flight" view
 * needs, with no per-type knowledge.
 *
 * `progress` follows phig_op_snapshot's rule: its path and error are static
 * empty strings, so this record carries no validity window and may be copied
 * or kept freely.
 *
 * This record may gain fields later. Both calls below take the caller's
 * sizeof explicitly, and the library writes at most that many bytes per
 * element, so an older client gets only the prefix it allocated. */
typedef struct {
    uint64_t id;
    phig_op_type type;
    phig_op_state state;
    phig_op_progress progress;
} phig_op_info;

/* The operations this context has in flight, oldest first. Fills up to `cap`
 * entries and writes the true count to *n_total, so a caller sizes with
 * cap = 0 and a NULL buffer, then calls again.
 *
 * `struct_size` is the caller's sizeof(phig_op_info). It is both the array
 * stride and the per-element write limit, which is why it is a parameter
 * rather than a member of the record: the library has to know the element size
 * before it can address the second element, so it cannot recover that size
 * from elements it has not reached.
 *
 * No operation handle is returned and none is implied. phig_op_free stays the
 * only way to free an operation, and only for the handle the caller already
 * holds. */
PHIG_API phig_status phig_op_list(phig_ctx* ctx, phig_op_info* out, size_t struct_size,
                                  size_t cap, size_t* n_total);

/* One operation by id. PHIG_ERR_NOT_FOUND once it has been freed, and for an
 * id belonging to a different context. */
PHIG_API phig_status phig_op_info_by_id(phig_ctx* ctx, uint64_t id, phig_op_info* out,
                                        size_t struct_size);

/* Cancel is reachable by id because it only sets an atomic flag the operation
 * polls. Nothing else that mutates an operation is reachable this way. */
PHIG_API phig_status phig_op_cancel_by_id(phig_ctx* ctx, uint64_t id);
```

- [ ] **Step 4: Implement them**

In `libphig/operations.cpp`'s anonymous namespace, after `fill_snapshot`:

```cpp
// Builds one record and copies at most `struct_size` bytes of it to `out`.
// Called with the registry mutex held, which is what makes `op` safe to read:
// phig_op_free takes the same mutex to deregister before it destroys anything.
void write_info(void* out, size_t struct_size, phig_op* op) {
    phig_op_info info{};
    info.id = op->id;
    info.type = kind_to_type(op->kind);
    info.state = op->state.load(std::memory_order_acquire);
    ProgressCounters c;
    newest_counters(op, c);
    fill_snapshot(&info.progress, c);
    std::memcpy(out, &info, std::min(struct_size, sizeof(phig_op_info)));
}
```

Add `#include <algorithm>` and `#include <cstring>` if absent.

In the `extern "C"` block:

```cpp
phig_status phig_op_list(phig_ctx* ctx, phig_op_info* out, size_t struct_size, size_t cap,
                         size_t* n_total) {
    if (ctx == nullptr || n_total == nullptr) return PHIG_ERR_INVALID_ARG;
    if (cap > 0 && (out == nullptr || struct_size == 0)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        OpRegistry& reg = *ctx->registry;
        std::lock_guard<std::mutex> g(reg.m);
        *n_total = reg.ops.size();
        auto* base = reinterpret_cast<unsigned char*>(out);
        size_t i = 0;
        for (auto& entry : reg.ops) {
            if (i >= cap) break;
            write_info(base + i * struct_size, struct_size, entry.second);
            ++i;
        }
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_op_info_by_id(phig_ctx* ctx, uint64_t id, phig_op_info* out,
                               size_t struct_size) {
    if (ctx == nullptr || out == nullptr || struct_size == 0) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        OpRegistry& reg = *ctx->registry;
        std::lock_guard<std::mutex> g(reg.m);
        auto it = reg.ops.find(id);
        if (it == reg.ops.end()) return PHIG_ERR_NOT_FOUND;
        write_info(out, struct_size, it->second);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_op_cancel_by_id(phig_ctx* ctx, uint64_t id) {
    if (ctx == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        OpRegistry& reg = *ctx->registry;
        std::lock_guard<std::mutex> g(reg.m);
        auto it = reg.ops.find(id);
        if (it == reg.ops.end()) return PHIG_ERR_NOT_FOUND;
        it->second->cancel.cancel();
        return PHIG_OK;
    PHIG_CATCH(ctx)
}
```

- [ ] **Step 5: Reference the symbols from the C compile check**

In `tests/test_libphig_c_compile.c`, in the operations block:

```c
        {
            phig_op_info info;
            size_t n_total = 0;
            (void)phig_op_list(ctx, &info, sizeof(info), 1, &n_total);
            (void)phig_op_info_by_id(ctx, phig_op_id(op), &info, sizeof(info));
            (void)phig_op_cancel_by_id(ctx, phig_op_id(op));
        }
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
clang-format -i libphig/include/phig/phig.h
cmake --build build && ./build/phig-tests --gtest_filter='LibphigOpList.*:LibphigOpInfoById.*:LibphigOpCancelById.*'
```

Expected: PASS, 10 tests.

- [ ] **Step 7: Run the concurrency test under TSan**

```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan
OMP_NUM_THREADS=1 GTEST_DEATH_TEST_STYLE=threadsafe \
  ./build-tsan/phig-tests --gtest_filter='LibphigOp*'
```

Expected: PASS, no TSan report. `OMP_NUM_THREADS=1` is mandatory — `libgomp` is not TSan-instrumented, so OpenCV's parallel regions inside decode produce roughly 120 false positives that would bury a genuine finding.

- [ ] **Step 8: Commit**

```bash
git add libphig/include/phig/phig.h libphig/operations.cpp \
        tests/test_libphig_op_status.cpp tests/test_libphig_c_compile.c
git commit -m "Add phig_op_list, phig_op_info_by_id and phig_op_cancel_by_id

Enumeration hands out ids and records by value, never handles, so no
question arises about who owns the list, what a freed operation does to
it, or whether a handle obtained this way may be freed twice. The caller
owns the buffer it passed in; phig_op_free stays the only way to free an
operation.

struct_size is a parameter rather than a member of phig_op_info because
the library must know the element size before it can address the second
element of a caller-allocated array. It is both the stride and the write
limit, so an older client gets only the prefix it allocated.

Every by-id read holds the registry mutex across the whole read, and
phig_op_free deregisters under that same mutex before it joins, so a
lookup that succeeds is guaranteed its target outlives the read."
```

---

### Task 7: Version bump and documentation

The branch is not shippable until this lands: Tasks 3-6 added symbols without bumping the ABI version.

**Files:**
- Modify: `libphig/include/phig/phig.h:50` (`PHIG_ABI_VERSION_MINOR`)
- Modify: `libphig/CMakeLists.txt:17` (`VERSION`)
- Modify: `libphig/CLAUDE.md`

**Interfaces:**
- Consumes: every symbol from Tasks 3-6.
- Produces: nothing code depends on.

- [ ] **Step 1: Count the exported symbols**

```bash
grep -c "^PHIG_API" libphig/include/phig/phig.h
```

`libphig/CLAUDE.md` currently records 69. Expect 75 — six new entry points. If the count differs, find out why before editing the doc; a mismatch means a symbol was declared without `PHIG_API` and is therefore hidden and unlinkable.

- [ ] **Step 2: Bump the version in both places**

`libphig/include/phig/phig.h`:

```c
#define PHIG_ABI_VERSION_MINOR 4u
```

`libphig/CMakeLists.txt`:

```cmake
    VERSION 1.4.0
```

`SOVERSION` stays at 1. Every addition here is backward compatible, so existing callers need no rebuild.

- [ ] **Step 3: Update `libphig/CLAUDE.md`'s header line**

Replace `ABI 1.3, soname 1, 69 exported symbols.` with `ABI 1.4, soname 1, 75 exported symbols.` (or the count Step 1 produced).

- [ ] **Step 4: Add the field-semantics table**

Plan 056 records this as a documentation fix rather than an API change: `processed` means four different things and each is documented in a different comment in the header. Add to `libphig/CLAUDE.md`, in the Rules section after the `phig_op_progress` rule:

```markdown
- **What `phig_op_progress`'s fields mean, per operation.** `done`, `total` and
  `errors` are filled by every operation. The rest are not uniform, and the
  header documents each in a different comment — this table is the one place
  they are together.

  | Operation | `done` / `total` | `processed` | `skipped` |
  |---|---|---|---|
  | index | files finished / files the walk found | files processed | unchanged since a previous run, or attached to a RAW |
  | thumbs | files finished / files selected | thumbnails generated | already cached |
  | faces | files finished / files selected | always 0 | always 0 |
  | organize (plan, copy, move) | parents finished / parents selected | files written, including carried sidecars and attached JPEGs | parents not acted on |

  `phig_op_snapshot` fills the same record from the same mapping, so the table
  applies to a snapshot unchanged. A snapshot's `path` and `error` are always
  static empty strings.
```

- [ ] **Step 5: Note the new symbols against plan 054**

Plan 054's `rules/operations.toml` classifies every ABI symbol, and its check 4 fails a build when one is neither an operation surface nor on the read-only allowlist. Add to `libphig/CLAUDE.md`'s Rules section:

```markdown
- **A new entry point has to be classified in `rules/operations.toml`** (plan
  054) as either a surface of a declared operation or an entry on the read-only
  allowlist. Nothing in this directory enforces that; the rules check does.
```

Do not edit `rules/operations.toml` from this branch — plan 054 owns that file and is in progress. Land the allowlist entries after 054, per plan 056's Relationship section.

- [ ] **Step 6: Verify the version is consistent everywhere**

```bash
grep -n "PHIG_ABI_VERSION_MINOR" libphig/include/phig/phig.h
grep -n "VERSION\|SOVERSION" libphig/CMakeLists.txt
grep -n "^ABI\|ABI 1\." libphig/CLAUDE.md | head -3
```

Expected: minor 4, `VERSION 1.4.0`, `SOVERSION 1`, doc says ABI 1.4.

- [ ] **Step 7: Run the strictest gate**

```bash
cmake -B build-strict -DPHIG_WERROR=ON
cmake --build build-strict
ctest --test-dir build-strict --parallel
```

Expected: clean build with no warnings, and all tests pass. `-DPHIG_WERROR=ON` is what CI builds with and what a default local build does not, so this is the run that decides whether the branch is green.

- [ ] **Step 8: Run the lint gate**

```bash
clang-format --dry-run --Werror $(find core libphig cli tests -maxdepth 1 \( -name '*.cpp' -o -name '*.h' -o -name '*.c' \))
clang-format --dry-run --Werror libphig/include/phig/phig.h
./scripts/clang-tidy.sh
```

Expected: no output from clang-format, no findings from clang-tidy. The public header is checked separately because CI's glob is `-maxdepth 1` and does not reach it.

- [ ] **Step 9: Commit**

```bash
git add libphig/include/phig/phig.h libphig/CMakeLists.txt libphig/CLAUDE.md
git commit -m "Bump libphig to ABI 1.4 and document the operation status surface

Six additive entry points, so the minor and the CMake VERSION move and
SOVERSION stays at 1: existing callers need no rebuild.

Also lands plan 056's documentation fix. What phig_op_progress's fields
mean varies by operation — processed alone means four different things —
and each meaning was documented in a separate comment in the header. One
table in libphig/CLAUDE.md replaces them, and it applies to
phig_op_snapshot unchanged since both fill the record from the same
mapping."
```

---

## Verification Checklist

Run before calling the plan done, not as part of the last task:

- [ ] `ctest --test-dir build-strict --parallel` passes with `-DPHIG_WERROR=ON`.
- [ ] The skip list matches the machine's expected set — with face models present in `~/.cache/phig/models`, the only skip is `ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing`. A new skip means a new test is silently not running.
- [ ] ASan+UBSan: `ASAN_OPTIONS=detect_leaks=0 GTEST_DEATH_TEST_STYLE=threadsafe ctest --test-dir build-san --parallel -E DuplicatesPerf`.
- [ ] TSan: `OMP_NUM_THREADS=1 GTEST_DEATH_TEST_STYLE=threadsafe ctest --test-dir build-tsan --parallel -E DuplicatesPerf`.
- [ ] `phig --version` still reports `phig 0.3.0` — this plan changes the library version, not the tool's.
- [ ] Every new entry point is reachable from `tests/test_libphig_c_compile.c`, so a missing definition fails the build rather than surfacing at a client's link step.
- [ ] `phig_op_poll`'s behaviour is unchanged: the tests that existed before Task 2 all still pass.

## Known Gaps

- **`rules/operations.toml` is not updated here.** Plan 054 owns that file and is in progress in another worktree. Five of the six new entry points belong on its read-only allowlist, with `phig_op_cancel_by_id` beside `phig_op_cancel`. Land them after 054 merges.
- **No `gtk/` client uses any of this yet.** The ABI gains the capability; adopting it in the GTK client is plan 047's business, not this one's.
- **Enumeration is per-context.** An id from context A is `PHIG_ERR_NOT_FOUND` in context B, which is tested. A process-wide view is not offered and nothing asks for one.
