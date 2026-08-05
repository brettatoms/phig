# phig-core Part 2b-ii: ThumbsEngine and FacesEngine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the parallel bodies of `phig thumbs rebuild` and `phig face rebuild` out of `main.cpp` into `ThumbsEngine` and `FacesEngine` in phig-core, published through `ProgressQueue`, so both are testable and reusable by libphig and the GTK app.

**Architecture:** Both engines follow the `ScanEngine` pattern established in Part 2b-i: a plain options struct in, a result struct out, progress published to an optional `ProgressQueue<T>*`, cancellation polled from a `const CancelToken&`, and no user-facing prose or `std::cout` inside the engine. Neither engine needs `ScanEngine`'s `run_workers`/`run_reconcile` split — that split exists only because scan's reconcile phase deletes metadata uncancellably, and neither of these engines has a second phase.

**Tech Stack:** C++20, GCC 15 via Nix devenv, GoogleTest, OpenCV (`cv::Mat`, DNN face detection), SQLite.

## Global Constraints

- **Every build, test, and format command must be prefixed with `direnv exec .`** from `~/devel/phig/phig`. Bare `cmake`/`ctest` fails on missing opencv4 — the Nix devenv supplies OpenCV, LibRaw, spdlog, and GCC 15.
- **CLI flag order is `phig <subcommand> <args> --db <path>`.** `phig --db X thumbs rebuild` fails with "Unknown command: --db".
- **Engines contain no user-facing prose and no `std::cout`.** All wording and rendering stays in `main.cpp`. This is the rule that makes the engines reusable by a GUI.
- **Engines take `const CancelToken&`, never a `CancelToken&`** and never install signal handlers. `ScopedInterruptHandler` stays in the CLI.
- **`ProgressQueue<T>*` may be null.** Every publish site must tolerate it.
- **Options structs are stored by value, not by reference.** A `ScanEngine`-era defect: `const ScanOptions& opts_` dangles when constructed from a temporary.
- **CI lints only `find src tests -maxdepth 1`,** so `src/libphig/` and `include/` are never auto-linted. Not relevant to this plan's files (all at `src/` depth 1), but run the formatter anyway.
- **CI builds `macos-latest` as well as `ubuntu-latest`.** No GNU-ld-only linker flags.
- **New tests go in `phig-tests`** (the fast ~1s suite), not `phig-perf-tests`. Both are registered with `ctest --test-dir build`.
- **Behavior preservation is the acceptance bar.** Each engine task must prove the CLI's observable output is unchanged against a pre-change binary, per Task 1.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/thumbs_engine.h` (create) | `ThumbsOptions`, `ThumbsProgress`, `ThumbsResult`, `ThumbsEngine` declarations |
| `src/thumbs_engine.cpp` (create) | Thumbnail rebuild: select images, generate in parallel, publish progress |
| `src/faces_engine.h` (create) | `FacesRebuildOptions`, `FacesProgress`, `FacesResult`, `FacesEngine` declarations |
| `src/faces_engine.cpp` (create) | Face rebuild: per-worker detectors, detect, atomic delete-then-reinsert |
| `src/main.cpp` (modify) | Keeps argv parsing, writer lock, all rendering; calls the two engines |
| `tests/test_thumbs_engine.cpp` (create) | `ThumbsEngine` behavior |
| `tests/test_faces_engine.cpp` (create) | `FacesEngine` behavior |
| `tests/data/raw_pair/` (create) | RAW+JPEG fixture pair for the relink/demote ordering gap (Task 5) |
| `tests/test_relink.cpp` (modify) | Ordering test using the new fixture |
| `src/hasher.cpp` (modify) | `StderrSuppressor` fix (Task 6) |
| `CMakeLists.txt` (modify) | Register new sources and test files |

Task order is deliberate: Task 1 captures the baseline both engine tasks are measured against; Tasks 2–3 do thumbs (no DB writes, the simpler engine); Task 4 does faces (transactions, per-worker detectors); Tasks 5–6 close the two gaps carried forward from Part 2b-i.

---

### Task 1: Capture the pre-change CLI baseline

No production code changes. This task produces the artifact Tasks 3 and 4 are graded against. Without it, "behavior is unchanged" is an unverifiable claim.

**Files:**
- Create: `/tmp/phig-2bii-baseline/` (scratch, not committed)

**Interfaces:**
- Consumes: nothing.
- Produces: `/tmp/phig-2bii-baseline/thumbs-rebuild.txt`, `faces-rebuild.txt`, and `phig-baseline` (a copy of the pre-change binary). Tasks 3 and 4 diff against these.

- [ ] **Step 1: Build the current binary and stash a copy**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
mkdir -p /tmp/phig-2bii-baseline
cp build/phig /tmp/phig-2bii-baseline/phig-baseline
```

- [ ] **Step 2: Build a scratch collection and record `thumbs rebuild` output**

Keep it small — 12 files. Past runs left 287 MB of `/tmp` fixtures and a runaway `phig scan` at 1041% CPU.

```bash
mkdir -p /tmp/phig-2bii-baseline/coll
for i in $(seq 1 12); do
  cp tests/data/test_red.mov /tmp/phig-2bii-baseline/coll/v$i.mov
done
cp tests/data/faces_small.jpg /tmp/phig-2bii-baseline/coll/
cp tests/data/faces_large.jpg /tmp/phig-2bii-baseline/coll/
DB=/tmp/phig-2bii-baseline/base.db
/tmp/phig-2bii-baseline/phig-baseline scan /tmp/phig-2bii-baseline/coll --db $DB > /dev/null
/tmp/phig-2bii-baseline/phig-baseline thumbs rebuild --parallel 4 --db $DB \
  > /tmp/phig-2bii-baseline/thumbs-rebuild.txt 2>&1
/tmp/phig-2bii-baseline/phig-baseline thumbs rebuild --parallel 4 --db $DB \
  > /tmp/phig-2bii-baseline/thumbs-rebuild-cached.txt 2>&1
```

The second run is the all-skipped path, which uses a different progress format (terse tick, no path). Both formats must survive.

- [ ] **Step 3: Record `faces rebuild` output**

```bash
DB=/tmp/phig-2bii-baseline/base.db
/tmp/phig-2bii-baseline/phig-baseline face rebuild --parallel 2 --db $DB \
  > /tmp/phig-2bii-baseline/faces-rebuild.txt 2>&1
```

If this prints "face models not found", run `direnv exec . ./build/phig face rebuild` once to trigger the download, or note in your report that the faces baseline could not be captured — do not fabricate it.

- [ ] **Step 4: Confirm no stray processes and record sizes**

```bash
pgrep -a phig || echo "no phig processes: clean"
du -sh /tmp/phig-2bii-baseline
```

Expected: no `phig` processes; total well under 100 MB.

- [ ] **Step 5: Report the baseline contents**

Print each captured file. These strings are the contract. Do not commit anything in this task — there is nothing to commit.

---

### Task 2: `ThumbsEngine` — types and the skip decision

Splits the engine's decision logic from its parallel execution so the skip rule is tested without generating a single thumbnail. Thumbs is the simpler engine: it writes **no database rows at all** (thumbnails are filesystem cache artifacts), so it needs no mutex and no transaction.

**Files:**
- Create: `src/thumbs_engine.h`, `src/thumbs_engine.cpp`
- Create: `tests/test_thumbs_engine.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ImageInfo` (`src/types.h`), `thumb_exists(const std::string& sha256)` (`src/thumbs.h`), `CancelToken` (`src/cancel.h`), `ProgressQueue<T>` (`src/progress_queue.h`).
- Produces: `ThumbsOptions`, `ThumbsProgress`, `ThumbsStage`, `ThumbsResult`, `ThumbsEngine`, and the free function `bool thumb_needs_generating(const ImageInfo&, bool force)`. Task 3 calls all of these.

- [ ] **Step 1: Write the failing test**

Create `tests/test_thumbs_engine.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "thumbs_engine.h"
#include "types.h"

namespace {
ImageInfo with_sha(const std::string& sha) {
    ImageInfo info;
    info.path = "/tmp/nonexistent/x.jpg";
    info.sha256 = sha;
    return info;
}
}  // namespace

TEST(ThumbsEngine, EmptyShaIsSkippedEvenUnderForce) {
    // A row with no hash has no cache key, so there is nowhere to write a
    // thumbnail. --force must not override this: it is not a staleness
    // question, it is a missing-identifier question.
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), false));
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), true));
}

TEST(ThumbsEngine, ForceRegeneratesEvenWhenTheThumbExists) {
    // Uses a sha that cannot have a cached thumb, then asserts the force path
    // does not consult the cache at all: a hashed row under force is always
    // generated. Distinguishes `force || !exists` from `!exists`.
    ImageInfo hashed = with_sha("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    EXPECT_TRUE(thumb_needs_generating(hashed, true));
}

TEST(ThumbsEngine, UnhashedRowIsSkippedBeforeTheCacheIsConsulted) {
    // Guards ordering: if the implementation checked thumb_exists("") first it
    // would stat a path built from an empty key. The empty-sha branch must win.
    EXPECT_FALSE(thumb_needs_generating(with_sha(""), false));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j 2>&1 | tail -20
```

Expected: FAIL — `thumbs_engine.h: No such file or directory`. (The test file is not yet in `CMakeLists.txt`; add it in Step 3 so the failure is a compile error about the missing header, not a silently-skipped file.)

- [ ] **Step 3: Write the header**

Create `src/thumbs_engine.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"
#include "types.h"

struct ThumbsOptions {
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    bool force = false;
    int parallel = -1;  // -1 = auto
};

// Whether one row needs a thumbnail generated.
//
// A row with an empty sha256 is always skipped: the sha is the thumbnail cache
// key, so there is no path to write to. `force` does not override that — it
// overrides the freshness check only.
//
// Free function, not a private method, so the decision is testable without
// touching the thumbnail cache on disk.
bool thumb_needs_generating(const ImageInfo& img, bool force);

// Progress carries either a finished file or the count of selected images,
// published once before the parallel phase begins so a caller can size a
// progress bar without pre-counting.
enum class ThumbsStage { FileDone, Selected };

struct ThumbsProgress {
    ThumbsStage stage = ThumbsStage::FileDone;
    size_t done = 0, total = 0;
    size_t generated = 0, skipped = 0, errors = 0;
    std::string path;
    // False for the terse tick emitted when a file is skipped, reproducing the
    // CLI's two progress formats.
    bool show_path = false;
};

struct ThumbsResult {
    size_t generated = 0, skipped = 0, errors = 0;
    size_t total_selected = 0;
    bool completed = true;
};

// Generates missing thumbnails for the images a database already knows about.
//
// Reads the database once up front and writes none of it back — thumbnails are
// filesystem cache artifacts, so this engine needs no transaction and no
// database mutex. Publishes to the queue if one is given; the queue may be null.
// Polls the token between items.
class ThumbsEngine {
   public:
    ThumbsEngine(const ThumbsOptions& opts, Database& db);

    ThumbsResult run(const CancelToken& cancel, ProgressQueue<ThumbsProgress>* progress);

   private:
    // By value: constructing from a temporary would otherwise dangle.
    ThumbsOptions opts_;
    Database& db_;
};
```

- [ ] **Step 4: Write the minimal implementation**

Create `src/thumbs_engine.cpp` with only the free function for now; `ThumbsEngine::run` lands in Task 3.

```cpp
#include "thumbs_engine.h"

#include "thumbs.h"

bool thumb_needs_generating(const ImageInfo& img, bool force) {
    if (img.sha256.empty()) return false;
    if (force) return true;
    return !thumb_exists(img.sha256);
}
```

- [ ] **Step 5: Register the new files in CMake**

In `CMakeLists.txt`, add `src/thumbs_engine.cpp` to the phig-core source list next to `src/scan_engine.cpp` (line ~73), and add `tests/test_thumbs_engine.cpp` to the `phig-tests` source list next to `tests/test_scan_engine.cpp` (line ~205).

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='ThumbsEngine.*'
```

Expected: 3 tests PASS.

- [ ] **Step 7: Mutation-check the skip rule**

Cap: two mutations, on the riskiest assertion only.

1. Change `if (force) return true;` to `if (force) return !thumb_exists(img.sha256);` → `ForceRegeneratesEvenWhenTheThumbExists` must fail.
2. Move the `force` check above the empty-sha check → `EmptyShaIsSkippedEvenUnderForce` must fail.

Revert both. If either mutation survives, the test asserts shape rather than value — fix the test before continuing.

- [ ] **Step 8: Format and commit**

```bash
cd ~/devel/phig/phig
direnv exec . clang-format -i src/thumbs_engine.h src/thumbs_engine.cpp tests/test_thumbs_engine.cpp
git add src/thumbs_engine.h src/thumbs_engine.cpp tests/test_thumbs_engine.cpp CMakeLists.txt
git commit -m "phig-core: add ThumbsEngine types and the thumbnail skip decision"
```

---

### Task 3: `ThumbsEngine::run` and the CLI switchover

**Files:**
- Modify: `src/thumbs_engine.h` (no signature change expected), `src/thumbs_engine.cpp`
- Modify: `src/main.cpp` — replace the `thumbs rebuild` engine body (the `auto worker = ...` lambda through the summary block, currently `:1293-1348`)
- Modify: `tests/test_thumbs_engine.cpp`

**Interfaces:**
- Consumes: everything Task 2 produced; `parallel_for_each(size_t count, int num_threads, const CancelToken&, fn(size_t item, int worker_id)) -> bool` (`src/parallel.h`); `generate_thumb(const cv::Mat&, const std::string& sha256)` and `generate_thumb(const std::string& image_path, const std::string& sha256)` (`src/thumbs.h`); `extract_video_keyframe(const std::string& path)` returning `VideoKeyframe` with a `.frame` member (`src/hasher.h`); `passes_selection(const std::string& path, const std::vector<std::string>& matches, const std::vector<std::string>& excludes)` (`src/main.cpp`-adjacent helper — confirm its declaring header with `grep -rn "passes_selection" src/*.h`).
- Produces: a working `ThumbsEngine::run`. Nothing later depends on it.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_thumbs_engine.cpp`. Note the fixture copies **real** files and generates **real** thumbnails — 6 files keeps it fast.

```cpp
#include <unistd.h>

#include <cstdint>
#include <filesystem>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

namespace fs = std::filesystem;

namespace {
// A scratch collection with N copies of a real fixture, scanned into a database.
struct ScratchThumbs {
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    explicit ScratchThumbs(int n) {
        dir = fs::temp_directory_path() / ("phig_thumbs_engine_" + std::to_string(::getpid()) +
                                          "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
        for (int i = 0; i < n; ++i) {
            fs::path p = dir / ("img" + std::to_string(i) + ".jpg");
            fs::copy_file("tests/data/faces_small.jpg", p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            // Distinct shas so each row gets its own cache entry.
            info.sha256 = std::string(63, '0') + std::to_string(i);
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db->insert_image(info);
        }
    }
    ~ScratchThumbs() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
}  // namespace

TEST(ThumbsEngineRun, GeneratesOneThumbnailPerSelectedImage) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.generated, 4u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_EQ(r.total_selected, 4u);
    EXPECT_TRUE(r.completed);
}

TEST(ThumbsEngineRun, SecondRunSkipsEverythingItAlreadyGenerated) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    CancelToken tok;

    ThumbsEngine(opts, *coll.db).run(tok, nullptr);
    ThumbsResult second = ThumbsEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(second.skipped, 4u);
    EXPECT_EQ(second.generated, 0u);
}

TEST(ThumbsEngineRun, ForceRegeneratesWhatTheSecondRunWouldSkip) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    CancelToken tok;

    ThumbsEngine(opts, *coll.db).run(tok, nullptr);
    opts.force = true;
    ThumbsResult forced = ThumbsEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(forced.generated, 4u);
    EXPECT_EQ(forced.skipped, 0u);
}

TEST(ThumbsEngineRun, AlreadyCancelledGeneratesNothingAndReportsIncomplete) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;
    tok.cancel();

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.generated, 0u);
}

TEST(ThumbsEngineRun, PublishesSelectedCountBeforeAnyFileCompletes) {
    // The Selected update must arrive first, so a caller can size a progress
    // bar before the first file lands. Asserts order, not just presence.
    ScratchThumbs coll(3);
    ThumbsOptions opts;
    opts.parallel = 1;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<ThumbsProgress> q(64);

    engine.run(tok, &q);
    q.close();

    ThumbsProgress first;
    ASSERT_TRUE(q.pop(first));
    EXPECT_EQ(first.stage, ThumbsStage::Selected);
    EXPECT_EQ(first.total, 3u);

    size_t file_done = 0;
    ThumbsProgress p;
    while (q.pop(p)) {
        if (p.stage == ThumbsStage::FileDone) ++file_done;
    }
    EXPECT_EQ(file_done, 3u);
}

TEST(ThumbsEngineRun, SelectionFiltersRestrictWhatIsProcessed) {
    ScratchThumbs coll(4);
    ThumbsOptions opts;
    opts.parallel = 2;
    opts.matches.push_back((coll.dir / "img0.jpg").string());
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
    EXPECT_EQ(r.generated, 1u);
}

TEST(ThumbsEngineRun, PublishesNothingWhenGivenANullQueue) {
    // The null-queue path is the one libphig's synchronous callers use; it must
    // not be a crash.
    ScratchThumbs coll(2);
    ThumbsOptions opts;
    opts.parallel = 1;
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    EXPECT_NO_THROW(engine.run(tok, nullptr));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j 2>&1 | tail -20
```

Expected: FAIL — undefined reference to `ThumbsEngine::ThumbsEngine` and `ThumbsEngine::run`.

- [ ] **Step 3: Implement `ThumbsEngine::run`**

Append to `src/thumbs_engine.cpp`. This is a faithful move of the CLI body: same counters, same video-keyframe branch, same `catch (...)` to errors. The `std::cout` writes become queue publishes, and `images.size()` becomes `total`.

```cpp
#include <atomic>

#include "hasher.h"
#include "parallel.h"
#include "selection.h"  // passes_selection — confirm the actual header name

ThumbsEngine::ThumbsEngine(const ThumbsOptions& opts, Database& db) : opts_(opts), db_(db) {}

ThumbsResult ThumbsEngine::run(const CancelToken& cancel, ProgressQueue<ThumbsProgress>* progress) {
    ThumbsResult result;

    std::vector<ImageInfo> images = db_.get_all_images();
    if (!opts_.matches.empty() || !opts_.excludes.empty()) {
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageInfo& img) {
                                        return !passes_selection(img.path, opts_.matches,
                                                                 opts_.excludes);
                                    }),
                     images.end());
    }
    result.total_selected = images.size();

    if (progress) {
        ThumbsProgress p;
        p.stage = ThumbsStage::Selected;
        p.total = images.size();
        progress->push(p);
    }

    int num_threads = opts_.parallel > 0
                          ? opts_.parallel
                          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::atomic<size_t> generated{0}, skipped{0}, errors{0};

    auto publish = [&](const ImageInfo& img, bool show_path) {
        if (!progress) return;
        ThumbsProgress p;
        p.stage = ThumbsStage::FileDone;
        p.generated = generated.load();
        p.skipped = skipped.load();
        p.errors = errors.load();
        p.done = p.generated + p.skipped + p.errors;
        p.total = images.size();
        p.path = img.path;
        p.show_path = show_path;
        progress->push(p);
    };

    auto worker = [&](size_t idx, int) {
        const ImageInfo& img = images[idx];

        if (!thumb_needs_generating(img, opts_.force)) {
            skipped.fetch_add(1);
            // An unhashed row produced no progress line in the CLI at all; a
            // cache hit produced the terse tick. Preserve both.
            if (!img.sha256.empty()) publish(img, false);
            return;
        }

        try {
            bool ok = false;
            if (img.media_type == "video") {
                auto kf = extract_video_keyframe(img.path);
                if (!kf.frame.empty()) {
                    ok = generate_thumb(kf.frame, img.sha256);
                }
            } else {
                ok = generate_thumb(img.path, img.sha256);
            }
            if (ok) {
                generated.fetch_add(1);
            } else {
                errors.fetch_add(1);
            }
        } catch (...) {
            errors.fetch_add(1);
        }

        publish(img, true);
    };

    result.completed = parallel_for_each(images.size(), num_threads, cancel, worker);
    result.generated = generated.load();
    result.skipped = skipped.load();
    result.errors = errors.load();
    return result;
}
```

Add `#include <algorithm>` and `#include <thread>` to the top of the file if the build complains.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='ThumbsEngine*'
```

Expected: all 10 PASS (3 from Task 2, 7 new).

- [ ] **Step 5: Switch the CLI over**

In `src/main.cpp`, in `cmd_thumbs`'s `rebuild` branch, replace everything from `auto worker = [&](size_t idx, int) {` through the summary `std::cout` block (currently `:1293-1348`) with a `ThumbsEngine` call plus a drain loop. Keep the surrounding argv parsing, `acquire_writer_lock`, `db.count() == 0` check, the "Generating thumbnails for N images..." line, `CancelToken`, `ScopedInterruptHandler`, and `start_time` exactly as they are. Add `#include "thumbs_engine.h"`.

The rendering must reproduce the baseline byte-for-byte:
- skipped-with-sha → `"\r[" << done << "/" << total << "] "`
- generated/errored → `"\r\033[K[" << done << "/" << total << "] " << display`
- `display` is `(p.parent_path().filename() / p.filename()).string()`

Run the engine on a worker thread and drain the queue on the main thread, or run the engine on the main thread with a null queue and no per-file progress — **the first is required**, because progress lines are part of the baseline. Follow whatever pattern `cmd_scan` already uses for its drain loop (`src/main.cpp`, search `ProgressQueue<ScanProgress>`) rather than inventing a second one.

- [ ] **Step 6: Prove the CLI output is unchanged**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
rm -rf /tmp/phig-2bii-verify && mkdir -p /tmp/phig-2bii-verify/coll
for i in $(seq 1 12); do cp tests/data/test_red.mov /tmp/phig-2bii-verify/coll/v$i.mov; done
cp tests/data/faces_small.jpg tests/data/faces_large.jpg /tmp/phig-2bii-verify/coll/
DB=/tmp/phig-2bii-verify/base.db
./build/phig scan /tmp/phig-2bii-verify/coll --db $DB > /dev/null
./build/phig thumbs rebuild --parallel 4 --db $DB > /tmp/phig-2bii-verify/thumbs.txt 2>&1
./build/phig thumbs rebuild --parallel 4 --db $DB > /tmp/phig-2bii-verify/thumbs-cached.txt 2>&1
diff <(grep -v 'Done in' /tmp/phig-2bii-baseline/thumbs-rebuild.txt) \
     <(grep -v 'Done in' /tmp/phig-2bii-verify/thumbs.txt) && echo "FIRST RUN: identical"
diff <(grep -v 'Done in' /tmp/phig-2bii-baseline/thumbs-rebuild-cached.txt) \
     <(grep -v 'Done in' /tmp/phig-2bii-verify/thumbs-cached.txt) && echo "CACHED RUN: identical"
```

`Done in Xs` is filtered because it is a wall-clock timing that varies run to run.

**Two corrections from the Task 1 capture — read before interpreting the diff.**

*First, `--parallel 4` progress bytes are not a contract.* The captured baseline contains `^M[^M[78//14] ` and `12/14] 14]` — two workers' progress lines overwriting each other mid-write. So diff the **summary counts** (`Generating`, `Generated`, `Skipped`, `Errors`) at any parallelism, and diff the **full progress bytes only at `--parallel 1`**.

*Second, the runs above never exercise the video keyframe branch.* `thumbs-rebuild.txt` reports `Generated: 2, Skipped: 12`: all 12 `.mov` files already had thumbnails from `scan`, so `extract_video_keyframe` runs zero times. Without the `--force` runs below, this task's diff would pass with that branch completely unexercised. Run these too:

```bash
DB=/tmp/phig-2bii-verify/base.db
./build/phig thumbs rebuild --force --parallel 4 --db $DB \
  > /tmp/phig-2bii-verify/thumbs-force.txt 2>&1
./build/phig thumbs rebuild --force --parallel 1 --db $DB \
  > /tmp/phig-2bii-verify/thumbs-force-p1.txt 2>&1
# Summary counts must match at parallel 4 (expect Generated: 14, Skipped: 0, Errors: 0)
diff <(grep -aE 'Generating|Generated|Skipped|Errors' /tmp/phig-2bii-baseline/thumbs-rebuild-force.txt) \
     <(grep -aE 'Generating|Generated|Skipped|Errors' /tmp/phig-2bii-verify/thumbs-force.txt) \
  && echo "FORCE COUNTS: identical"
# Full bytes must match at parallel 1 — this is the real byte-for-byte contract
diff <(grep -av 'Done in' /tmp/phig-2bii-baseline/thumbs-rebuild-force-p1.txt) \
     <(grep -av 'Done in' /tmp/phig-2bii-verify/thumbs-force-p1.txt) \
  && echo "FORCE BYTES (p1): identical"
```

`Generated: 14` on the force runs is what proves the video branch survived the move. If it reports 2, the keyframe branch is broken and the plain runs would not have caught it. Any difference in summary counts is a real regression.

- [ ] **Step 7: Run the whole suite and clean up**

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | tail -15
rm -rf /tmp/phig-2bii-verify
pgrep -a phig || echo "no phig processes: clean"
```

Expected: all tests pass. Report the total count.

- [ ] **Step 8: Format and commit**

```bash
cd ~/devel/phig/phig
direnv exec . clang-format -i src/thumbs_engine.h src/thumbs_engine.cpp \
  tests/test_thumbs_engine.cpp src/main.cpp
git add src/thumbs_engine.h src/thumbs_engine.cpp tests/test_thumbs_engine.cpp src/main.cpp
git commit -m "phig-core: move thumbs rebuild into ThumbsEngine"
```

---

### Task 4: `FacesEngine` and the CLI switchover

The riskier of the two engines: it writes database rows inside a per-item transaction, and it needs one `FaceDetector` per worker because `cv::FaceDetectorYN` is not thread-safe and each instance holds ~150 MB of mutable DNN state. Per the calibration, this task gets full-depth review — it touches both a database write path and a concurrency primitive.

**Files:**
- Create: `src/faces_engine.h`, `src/faces_engine.cpp`
- Create: `tests/test_faces_engine.cpp`
- Modify: `src/main.cpp` — replace the `face rebuild` engine body (`:1616-1659`)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `detector_for_worker(const std::vector<std::unique_ptr<FaceDetector>>&, int worker_id) -> FaceDetector*` (`src/scan_engine.h`) — **reuse it, do not write a second one**; it throws on out-of-range, which is the bug it exists to catch. Also `decode_image(const std::string& path) -> cv::Mat` (`src/hasher.h`), `FaceDetector::detect(const cv::Mat&) -> std::vector<FaceInfo>` (`src/faces.h`), `Database::get_image_id`, `delete_faces_for_image`, `insert_face`, `auto_label_face`, `begin_transaction`, `commit_transaction`, `rollback_transaction` (`src/database.h`), `face_models_exist()` and `yunet_model_path()` (`src/models.h`).
- Produces: `FacesRebuildOptions`, `FacesProgress`, `FacesStage`, `FacesResult`, `FacesEngine`.

- [ ] **Step 1: Write the failing tests**

Create `tests/test_faces_engine.cpp`. The fixture uses the two face fixtures at **deliberately different dimensions** — `FaceDetector::Impl::ensure_input_size` (`src/faces.cpp:36-42`) caches `last_width`/`last_height`, so same-size images would hide detector sharing entirely.

```cpp
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "cancel.h"
#include "database.h"
#include "faces_engine.h"
#include "models.h"
#include "progress_queue.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {
// Alternating face fixtures at different dimensions (320x320 and 448x280).
// Same-size images would let a shared detector pass, because the detector
// caches its input size and only reconfigures when the size changes.
struct ScratchFaces {
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    explicit ScratchFaces(int n) {
        dir = fs::temp_directory_path() / ("phig_faces_engine_" + std::to_string(::getpid()) + "_" +
                                          std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
        for (int i = 0; i < n; ++i) {
            const char* src =
                (i % 2 == 0) ? "tests/data/faces_small.jpg" : "tests/data/faces_large.jpg";
            fs::path p = dir / ("f" + std::to_string(i) + ".jpg");
            fs::copy_file(src, p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            info.sha256 = std::string(63, '0') + std::to_string(i);
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db->insert_image(info);
        }
    }
    ~ScratchFaces() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

bool models_or_skip() { return face_models_exist(); }

// Total rows in the faces table. Database exposes insert_face and
// delete_faces_for_image but no per-image getter, so read the table directly.
size_t count_face_rows(const ScratchFaces& coll) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(coll.db_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        ADD_FAILURE() << "could not open " << coll.db_path;
        if (raw) sqlite3_close(raw);
        return 0;
    }
    sqlite3_stmt* stmt = nullptr;
    size_t n = 0;
    if (sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM faces", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            n = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
        }
    } else {
        ADD_FAILURE() << "prepare failed: " << sqlite3_errmsg(raw);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    return n;
}
}  // namespace

TEST(FacesEngineRun, DetectsFacesInEveryImageAcrossWorkers) {
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 4u);
    EXPECT_GT(r.faces_detected, 0u);
    EXPECT_TRUE(r.completed);
}

TEST(FacesEngineRun, RerunReplacesRatherThanAccumulatesFaceRows) {
    // The delete-then-reinsert contract. If the delete were dropped, a second
    // rebuild would double every image's face count.
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    CancelToken tok;

    FacesResult first = FacesEngine(opts, *coll.db).run(tok, nullptr);
    FacesResult second = FacesEngine(opts, *coll.db).run(tok, nullptr);

    EXPECT_EQ(second.faces_detected, first.faces_detected);

    // And the stored rows match the reported count, not twice it. Database has
    // no per-image face getter, so count directly. The db handle must be closed
    // first — this opens a second connection to the same file.
    size_t stored = count_face_rows(coll);
    EXPECT_EQ(stored, second.faces_detected);
}

TEST(FacesEngineRun, AlreadyCancelledDetectsNothingAndReportsIncomplete) {
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 2;
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    tok.cancel();

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.faces_detected, 0u);
}

TEST(FacesEngineRun, PublishesModelLoadThenSelectedThenFiles) {
    // Ordering matters for rendering: the CLI prints "Loading face recognition
    // models (N instances)..." before the per-file lines, and model loading is
    // the slowest part of a small rebuild.
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(2);
    FacesRebuildOptions opts;
    opts.parallel = 1;
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;
    ProgressQueue<FacesProgress> q(64);

    engine.run(tok, &q);
    q.close();

    FacesProgress p;
    ASSERT_TRUE(q.pop(p));
    EXPECT_EQ(p.stage, FacesStage::Selected);
    EXPECT_EQ(p.total, 2u);

    ASSERT_TRUE(q.pop(p));
    EXPECT_EQ(p.stage, FacesStage::LoadingModels);
    EXPECT_EQ(p.total, 1u);  // one detector instance for parallel=1

    size_t files = 0;
    while (q.pop(p)) {
        if (p.stage == FacesStage::FileDone) ++files;
    }
    EXPECT_EQ(files, 2u);
}

TEST(FacesEngineRun, UndecodableFileIsSkippedWithoutFailingTheRun) {
    // A file that cannot be decoded must not abort the rebuild or write rows.
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(2);
    fs::path junk = coll.dir / "broken.jpg";
    { std::ofstream(junk) << "not an image"; }
    ImageInfo info;
    info.path = junk.string();
    info.filename = "broken.jpg";
    info.extension = "jpg";
    info.file_size = fs::file_size(junk);
    info.media_type = "image";
    info.sha256 = std::string(63, '9') + "9";
    info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z";
    coll.db->insert_image(info);

    FacesRebuildOptions opts;
    opts.parallel = 2;
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r;
    EXPECT_NO_THROW(r = engine.run(tok, nullptr));
    EXPECT_EQ(r.total_selected, 3u);
    EXPECT_TRUE(r.completed);
}

TEST(FacesEngineRun, SelectionFiltersRestrictWhatIsProcessed) {
    if (!models_or_skip()) GTEST_SKIP() << "face models not found at " << yunet_model_path();
    ScratchFaces coll(4);
    FacesRebuildOptions opts;
    opts.parallel = 1;
    opts.matches.push_back((coll.dir / "f0.jpg").string());
    FacesEngine engine(opts, *coll.db);
    CancelToken tok;

    FacesResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 1u);
}
```

Add `#include <fstream>` for the undecodable-file test and `#include <sqlite3.h>` for `count_face_rows`.

`count_face_rows` opens its own read-only connection because `Database` exposes no per-image face getter. `faces` is the real row table — `delete_faces_for_image` runs `DELETE FROM faces WHERE image_id = ?` (`src/database.cpp:1574`), so counting it is exactly the delete-then-reinsert assertion. Do **not** count `vec_faces`; that is the vec0 virtual table and its row accounting differs.

The test holds `coll.db` open while counting. Two connections to one SQLite file is fine for a read, but if WAL mode makes the count lag the writes, call `coll.db.reset()` before counting and stop using the handle afterward.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j 2>&1 | tail -20
```

Expected: FAIL — `faces_engine.h: No such file or directory`. Add `tests/test_faces_engine.cpp` and `src/faces_engine.cpp` to `CMakeLists.txt` in this step so the failure is the missing header.

- [ ] **Step 3: Write the header**

Create `src/faces_engine.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

enum class FacesStage { FileDone, Selected, LoadingModels };

struct FacesRebuildOptions {
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    int parallel = -1;  // -1 = auto
};

struct FacesProgress {
    FacesStage stage = FacesStage::FileDone;
    size_t done = 0, total = 0;
    std::string path;
};

struct FacesResult {
    size_t total_selected = 0;
    size_t faces_detected = 0;
    bool completed = true;
};

// Re-detects faces for images a database already knows about, replacing each
// image's existing face rows.
//
// Per image the delete and the reinsert run in ONE transaction: if the delete
// ran outside it and the process were interrupted before commit, that image's
// faces would be gone with no re-detection to restore them. The engine owns the
// mutex serializing database access, so a caller must not touch `db` while
// run() is executing.
//
// Each worker gets its own FaceDetector: cv::FaceDetectorYN is not thread-safe
// and holds ~150MB of mutable model state, so sharing one across workers is a
// data race. Publishes to the queue if one is given; the queue may be null.
class FacesEngine {
   public:
    FacesEngine(const FacesRebuildOptions& opts, Database& db);

    FacesResult run(const CancelToken& cancel, ProgressQueue<FacesProgress>* progress);

   private:
    // By value: constructing from a temporary would otherwise dangle.
    FacesRebuildOptions opts_;
    Database& db_;
};
```

- [ ] **Step 4: Implement `FacesEngine::run`**

Create `src/faces_engine.cpp`. This is a faithful move of `main.cpp:1616-1655`, with `std::cout` replaced by queue publishes. Note the thread-count default differs from thumbs: `hardware_concurrency()/2` capped at 4, because each detector instance is memory-heavy.

```cpp
#include "faces_engine.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "faces.h"
#include "hasher.h"
#include "parallel.h"
#include "scan_engine.h"  // detector_for_worker
#include "selection.h"    // passes_selection — confirm the actual header name

FacesEngine::FacesEngine(const FacesRebuildOptions& opts, Database& db)
    : opts_(opts), db_(db) {}

FacesResult FacesEngine::run(const CancelToken& cancel, ProgressQueue<FacesProgress>* progress) {
    FacesResult result;

    std::vector<ImageInfo> all = db_.get_all_images();
    std::vector<ImageInfo> images;
    for (const auto& img : all) {
        if (passes_selection(img.path, opts_.matches, opts_.excludes)) {
            images.push_back(img);
        }
    }
    result.total_selected = images.size();

    if (progress) {
        FacesProgress p;
        p.stage = FacesStage::Selected;
        p.total = images.size();
        progress->push(p);
    }
    if (images.empty()) return result;

    int num_threads;
    if (opts_.parallel > 0) {
        num_threads = opts_.parallel;
    } else {
        num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
        num_threads = std::max(num_threads, 1);
    }

    if (progress) {
        FacesProgress p;
        p.stage = FacesStage::LoadingModels;
        p.total = static_cast<size_t>(num_threads);
        progress->push(p);
    }

    std::vector<std::unique_ptr<FaceDetector>> detectors;
    detectors.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        detectors.push_back(std::make_unique<FaceDetector>());
    }

    std::atomic<size_t> total_faces{0};
    std::atomic<size_t> done{0};
    std::mutex db_mutex;

    auto worker = [&](size_t idx, int worker_id) {
        const ImageInfo& img_info = images[idx];
        cv::Mat img = decode_image(img_info.path);
        if (img.empty()) {
            done.fetch_add(1);
            return;
        }

        auto faces = detector_for_worker(detectors, worker_id)->detect(img);

        {
            std::lock_guard<std::mutex> lock(db_mutex);
            int64_t image_id = db_.get_image_id(img_info.path);
            if (image_id >= 0) {
                // Delete-then-reinsert must be atomic: if the delete ran
                // outside the transaction and we were interrupted before
                // commit, the old faces would be gone with no re-detection.
                db_.begin_transaction();
                try {
                    db_.delete_faces_for_image(image_id);
                    for (const auto& face : faces) {
                        int64_t face_id = db_.insert_face(image_id, face);
                        db_.auto_label_face(face_id, face.embedding);
                    }
                    db_.commit_transaction();
                    total_faces.fetch_add(faces.size());
                } catch (...) {
                    db_.rollback_transaction();
                    throw;
                }
            }
        }

        size_t d = done.fetch_add(1) + 1;
        if (progress) {
            FacesProgress p;
            p.stage = FacesStage::FileDone;
            p.done = d;
            p.total = images.size();
            p.path = img_info.path;
            progress->push(p);
        }
    };

    result.completed = parallel_for_each(images.size(), num_threads, cancel, worker);
    result.faces_detected = total_faces.load();
    return result;
}
```

Two deliberate changes from the CLI original, both of which the tests cover: the progress counter is now an atomic `done` rather than `idx + 1` (with `parallel_for_each`'s shared cursor, `idx + 1` reports out-of-order numbers that jump backwards), and the undecodable-file early return now counts toward `done` so the final tally reaches `total`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='FacesEngine*'
```

Expected: 6 PASS (or SKIP with a models message — if they skip, say so explicitly in your report and do not claim the engine is verified).

- [ ] **Step 6: Mutation-check the transaction contract**

Cap: two mutations, on the riskiest assertion.

1. Delete the `db_.delete_faces_for_image(image_id);` line → `RerunReplacesRatherThanAccumulatesFaceRows` must fail (stored rows double).
2. Replace `detector_for_worker(detectors, worker_id)` with `detectors[0].get()` → run `--gtest_filter='FacesEngine*' --gtest_repeat=3`. This shares one detector across workers. Note honestly whether it fails: a data race is not guaranteed to be caught, and if it passes, the mixed-dimension fixture did not surface it. Report the actual result either way.

Revert both.

- [ ] **Step 7: Switch the CLI over**

In `src/main.cpp`, in `cmd_face`'s `rebuild` branch, replace `:1616-1655` (the `auto worker` lambda through `std::cout << "\n\nDone. " ...`) with a `FacesEngine` call plus a drain loop. Keep argv parsing, the image selection message, `CancelToken`, and `ScopedInterruptHandler`. Delete the now-duplicated selection filtering, thread-count calculation, and detector construction from the CLI — they moved into the engine. Add `#include "faces_engine.h"`.

Reproduce the baseline strings exactly:
- `"Rebuilding face data for N images...\n"` (from `total_selected`)
- `"Loading face recognition models (N instances)...\n"` (from the `LoadingModels` update)
- per file: `"\r\033[K[" << done << "/" << total << "] " << display`
- `"\n\nInterrupted. Progress saved.\n"` when `!completed`
- `"\n\nDone. " << faces_detected << " faces detected.\n"`

- [ ] **Step 8: Prove the CLI output is unchanged**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
rm -rf /tmp/phig-2bii-verify && mkdir -p /tmp/phig-2bii-verify/coll
cp tests/data/faces_small.jpg tests/data/faces_large.jpg /tmp/phig-2bii-verify/coll/
DB=/tmp/phig-2bii-verify/base.db
./build/phig scan /tmp/phig-2bii-verify/coll --db $DB > /dev/null
./build/phig face rebuild --parallel 2 --db $DB > /tmp/phig-2bii-verify/faces.txt 2>&1
diff /tmp/phig-2bii-baseline/faces-rebuild.txt /tmp/phig-2bii-verify/faces.txt \
  && echo "FACES: identical"
```

The baseline was captured on a 14-file collection and this run uses 2, so compare the shape and the per-line format, not the counts — or better, re-run the baseline binary on this same 2-file collection first and diff that. Do the latter; it is one extra command and makes the diff meaningful:

```bash
cp -r /tmp/phig-2bii-verify/coll /tmp/phig-2bii-verify/coll-base
BDB=/tmp/phig-2bii-verify/base-cmp.db
/tmp/phig-2bii-baseline/phig-baseline scan /tmp/phig-2bii-verify/coll-base --db $BDB > /dev/null
/tmp/phig-2bii-baseline/phig-baseline face rebuild --parallel 2 --db $BDB \
  > /tmp/phig-2bii-verify/faces-base.txt 2>&1
diff <(sed 's|coll-base|coll|g' /tmp/phig-2bii-verify/faces-base.txt) \
     /tmp/phig-2bii-verify/faces.txt && echo "FACES: identical"
```

The progress counter change from Step 4 means per-file line NUMBERS may legitimately differ at `--parallel 2` (the old code printed `idx + 1`, the new one prints completion order). If the diff shows only that, confirm the set of displayed filenames and the final "Done. N faces detected." line match, and report the counter change as an intentional improvement.

- [ ] **Step 9: Run the whole suite and clean up**

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | tail -15
rm -rf /tmp/phig-2bii-verify
pgrep -a phig || echo "no phig processes: clean"
```

- [ ] **Step 10: Format and commit**

```bash
cd ~/devel/phig/phig
direnv exec . clang-format -i src/faces_engine.h src/faces_engine.cpp \
  tests/test_faces_engine.cpp src/main.cpp
git add src/faces_engine.h src/faces_engine.cpp tests/test_faces_engine.cpp src/main.cpp CMakeLists.txt
git commit -m "phig-core: move face rebuild into FacesEngine"
```

---

### Task 5: Close the relink/demote ordering gap

Carried forward from Part 2b-i: swapping relink and demote in the reconcile phase passes all 580 tests. **The original diagnosis — "no fixture has a RAW+JPEG sharing a basename" — was wrong.** `tests/test_relink.cpp:134` already has `DemotesJpegToSameBasenameRaw`, which builds exactly that pair.

The real gap: every existing test calls `relink_moved_rows` and `demote_jpeg_rows` **directly and in isolation**. Nothing exercises them *in sequence* through the reconcile path, which is where the ordering lives. So no fixture is needed — the missing thing is a sequencing test.

**Files:**
- Modify: `tests/test_relink.cpp` (add one test to the existing `RelinkTest` fixture)

**Interfaces:**
- Consumes: `RelinkTest`'s existing `add_row(rel, sha, on_disk)` helper and `count_rows(handle, sql)`; `relink_moved_rows(Database&, const std::vector<std::string>& removed_paths, int64_t op_id) -> int` (`src/relink.h:14`); `demote_jpeg_rows(Database&, const std::string& scan_prefix, int64_t op_id) -> std::vector<std::string>` (`src/relink.h:24`).
- Produces: nothing later depends on this.

**Facts already established — do not re-derive:**
- Demotion keys off `is_raw_extension` (`src/scanner.cpp:81`), the **extension only**. It never decodes the file, so `add_row`'s fake `"bytes-<sha>"` content is sufficient. Do NOT add a real RAW fixture; `tests/data/truncated_ojpeg.cr2` exists but is deliberately corrupt and is not needed here.
- The ordering appears **twice** in `src/scan_engine.cpp`: the logged branch at `:526-527` and the unlogged branch at `:548-549`. A swap in only one is still a bug, so the test must cover the path the test actually runs.
- `ScanEngine::run_reconcile` is already driven from a test at `tests/test_scan_engine.cpp:412` — that is the seam that exercises the real sequence.

- [ ] **Step 1: Read the two files before writing**

```bash
cd ~/devel/phig/phig
sed -n '1,66p' tests/test_relink.cpp        # RelinkTest fixture + helpers
sed -n '134,192p' tests/test_relink.cpp     # the existing demote tests
sed -n '495,560p' src/scan_engine.cpp       # reconcile: relink then demote
sed -n '380,420p' tests/test_scan_engine.cpp # how run_reconcile is driven
```

- [ ] **Step 2: Construct the state where order changes the outcome**

The two operations interact through one row set. Build a case where a JPEG is demotable **and** a RAW row's file has moved, so that `relink_moved_rows` rewrites the RAW row's path in the same pass that `demote_jpeg_rows` looks for a same-basename RAW on disk.

Concretely: insert a row for `a.cr2` whose file is NOT at the recorded path but IS on disk at a new path (so it is in `removed_paths` and has a relink candidate), and insert a row for `a.jpg` whose file sits on disk next to the RAW's **new** location. Relink-then-demote moves the RAW row to the new path first, so the JPEG then has an indexed same-basename RAW and gets demoted. Demote-then-relink runs the demote check while the RAW row still points at the old path, so the JPEG's sidecar lookup resolves differently.

Verify your setup actually distinguishes the orders **before** asserting anything: run it once in each order (Step 4 swaps for you) and confirm the observable state differs. If both orders produce the same rows, the state you built does not exercise the ordering and the test cannot close the gap — rebuild the state.

- [ ] **Step 3: Write the test**

Add one `TEST_F(RelinkTest, ...)` calling `relink_moved_rows` then `demote_jpeg_rows` in the reconcile order, on the state from Step 2. Assert the **exact** final path of the surviving primary row and the **exact** tag/metadata transfer — `db->get_tags_for_image(<path>)` as `DemotesJpegToSameBasenameRaw` does at `:145-148`. Row counts alone are how this gap survived nine reviews; do not assert only counts.

Match the file's existing style: `TEST_F(RelinkTest, ...)`, the `add_row` helper, `count_rows(db->db_ptr(), ...)` for operation-entry checks.

- [ ] **Step 4: Prove it catches the swap — this is the gate**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j && direnv exec . ./build/phig-tests --gtest_filter='*elink*'
```

Expected: PASS on correct code. Then swap the two calls **in your test body** (that is the sequence under test) and confirm it FAILS. Revert.

Then do the same in production code: swap `src/scan_engine.cpp:526-527`, rebuild, and run the full suite. Report whether anything fails. If your new test does not fail on the production swap, say so plainly — a test that only catches a swap in its own body does not protect `scan_engine.cpp`, and the honest result is worth more than a test that appears to close the gap. Revert the production swap.

- [ ] **Step 5: Run the whole suite, format, and commit**

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | tail -15
direnv exec . clang-format -i tests/test_relink.cpp
git add tests/test_relink.cpp
git commit -m "tests: cover the relink-then-demote sequence, not just each in isolation"
```

---

### Task 6: Stop `StderrSuppressor` from hiding other threads' warnings

Carried forward from Part 2b-i. `src/hasher.cpp:169-194` `dup2`s fd 2 to `/dev/null` **process-wide** for the duration of every `cv::imread`, so concurrent workers swallow each other's `log_warn` output. Measured: 2–5 of 8 expected warnings reach the terminal at `--parallel 8`; 8 of 8 at `--parallel 1`.

**Corrected premise — read this before Step 1.** An earlier description of this bug called it an unserialized save/restore race. That is wrong: `StderrSuppressor` already holds a static `std::mutex` for the entire suppressed region, and its own comment explains why. The real mechanism is narrower:

- The suppressor's mutex serializes suppressors against **each other**, so fd 2 is never left dangling at `/dev/null`.
- But `log_warn` (`src/log.cpp:97`) → spdlog → its stderr sink writes fd 2 **without taking that mutex**. A warning emitted by worker B while worker A holds the suppressor across a full `cv::imread` goes to `/dev/null` and is gone.

This changes the option ranking below. Option 1 gains: deleting the redirection also removes a mutex **held across every decode**, a serialization point on the hot path. Option 2 loses: sharing one mutex with `log_warn` makes that contention worse.

The only two call sites are `src/hasher.cpp:284` and `src/hasher.cpp:387`.

**Files:**
- Modify: `src/hasher.cpp:169-177` (and its call sites)
- Test: `tests/test_hasher.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing later depends on this.

- [ ] **Step 1: Read the current implementation and all its call sites**

```bash
cd ~/devel/phig/phig
sed -n '150,200p' src/hasher.cpp
grep -n "StderrSuppressor" src/*.cpp src/*.h
```

- [ ] **Step 2: Reproduce the loss**

**Already measured — reproduce to confirm, then move on.** 10 undecodable files (`printf 'not an image' > bad$i.jpg`), scanned at each parallelism:

| run | warnings on stderr |
|---|---|
| `--parallel 1` | **10 of 10** |
| `--parallel 8` | **8 of 10** (`bad2`, `bad10` swallowed) |
| `--parallel 8` again | **9 of 10** — nondeterministic, as expected |

```bash
mkdir -p /tmp/phig-stderr-test/coll && cd /tmp/phig-stderr-test/coll
for i in $(seq 1 10); do printf 'not an image at all' > bad$i.jpg; done
cd ~/devel/phig/phig
direnv exec . ./build/phig scan /tmp/phig-stderr-test/coll --parallel 8 \
  --db /tmp/phig-stderr-test/t.db 2>&1 >/dev/null | grep -c warning
```

**Nothing is actually lost.** On the same `--parallel 8` run that showed 9 of 10 on stderr, the rotating file sink captured **10 of 10**. The defect is confined to the terminal copy — which is why the human partner ruled a measurement sufficient rather than requiring a data-safety test. Verify this yourself with `XDG_STATE_HOME` pointed at a scratch dir, and report both numbers from one run.

- [ ] **Step 3: Choose and state the fix**

The purpose of `StderrSuppressor` is to hide LibRaw's and OpenCV's chatty C-level writes to fd 2 during decode. The options, in preference order:

1. ~~**Silence the libraries at their source**~~ — **RULED OUT BY MEASUREMENT, do not attempt.** The noise is libjpeg's `"Premature end of JPEG file"` written *directly to fd 2*, plus GStreamer at the video call site. Neither goes through OpenCV's logging API, so `cv::redirectError` and `setLogLevel` cannot suppress them. (`install_cv_error_handler` already handles OpenCV's own messages separately, via the `t_cv_warning` thread-local.) Reproducing the noise needs a *genuinely truncated* JPEG — a short "not an image" file is rejected before libjpeg engages.
2. **Serialize suppression against warning emission** — one mutex held by both the suppressor and `log_warn`. Correct, but the suppressor's mutex is already held across a whole `cv::imread`, so this adds contention to the hot path. Least attractive of the three.
3. **Route phig's own warnings somewhere fd 2 cannot swallow** — **verified viable and probably the smallest fix.** `log.cpp:64` uses `spdlog::sinks::stderr_sink_mt`, which writes through `FILE* stderr` and so resolves fd 2 *at write time* — exactly why suppression catches phig's warnings as collateral. spdlog's `stdout_sink_base` has `explicit stdout_sink_base(FILE*)` (spdlog 1.17.0, `stdout_sinks.h:23`), so a sink can own a private `FILE*`: `fdopen(dup(STDERR_FILENO), "w")` once at `log_init` keeps pointing at the original stderr target while fd 2 is redirected. libjpeg keeps writing to fd 2 and stays suppressed.

**Prefer option 3.** If you pick option 2 instead, justify it against the contention point above. Implement exactly one.

Things option 3 must check, none of which are settled: interleaving with the `std::cout` progress lines (a different `FILE*` buffer), whether the duplicated descriptor needs explicit flushing, and behavior when stderr is a pipe rather than a tty.

- [ ] **Step 4: Write a test for the chosen fix**

The test must fail before the fix. Reproducing a cross-thread fd race deterministically in a unit test is hard — if the honest answer is that this is only verifiable by the `--parallel 8` measurement from Step 2, say so and record the before/after counts in the commit message instead. **The human partner has already ruled that the measurement is sufficient here** (this is terminal fidelity, not data safety — see Step 2's finding that the file sink loses nothing). Do not write a shape-only test to satisfy the step.

- [ ] **Step 5: Implement, then re-measure**

Apply the fix, then re-run Step 2's measurement at both `--parallel 8` and `--parallel 1`. Both should now report all 10 warnings. Report the numbers.

- [ ] **Step 6: Confirm the noise is still suppressed**

This is the half of the job that a warning-count check cannot see: the fix must not trade one problem for the other. Build truncated fixtures and confirm `"Premature end of JPEG file"` still does **not** appear on stderr:

```bash
mkdir -p /tmp/phig-stderr-test/coll2
cd ~/devel/phig/phig
for i in 1 2 3; do head -c 8000 tests/data/faces_small.jpg > /tmp/phig-stderr-test/coll2/trunc$i.jpg; done
direnv exec . ./build/phig scan /tmp/phig-stderr-test/coll2 --parallel 1 \
  --db /tmp/phig-stderr-test/t2.db 2>&1 >/dev/null | grep -i "premature" \
  && echo "REGRESSION: libjpeg noise is leaking" || echo "libjpeg noise still suppressed"
```

Also scan a video fixture (`tests/data/test_red.mov`) to confirm the GStreamer suppression at the second call site still holds.

- [ ] **Step 7: Run the whole suite, clean up, format, and commit**

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | tail -15
rm -rf /tmp/phig-stderr-test
pgrep -a phig || echo "no phig processes: clean"
direnv exec . clang-format -i src/hasher.cpp tests/test_hasher.cpp
git add src/hasher.cpp tests/test_hasher.cpp
git commit -m "hasher: stop decode-time stderr suppression from hiding worker warnings"
```

---

## Review Calibration

Applies to every task in this plan.

**Full-depth review** (two-stage, careful model): Task 4 only — it touches a database write path, a per-item transaction, and a concurrency primitive. Also Task 6 if the chosen fix adds a mutex.

**Single-pass review**: Tasks 1, 2, 3, 5. Type moves, argv wiring, output formatting, and test additions.

**Skip re-review**: any diff that is test-only or comment-only and verifiable in one command.

**Keep regardless of task**: both mutation questions ("what mutation makes my test fail?" and "what change to the implementation *survives* the whole suite?"), proving behavior preservation against the Task 1 baseline binary, and requiring implementers to state plainly what they could **not** verify.

**Do not**: re-measure what the implementer already measured unless the number is load-bearing; exceed two mutations per task; cap scratch fixtures above ~50 files; leave a `phig` process running (`pgrep -a phig` before reporting done).

**Known false positive**: subagents that commit may trip a "no explicit user approval" warning because the authorization is not visible in their own transcript. Per-task commits are authorized for this plan. **Pushing and merging are not** — do neither.

---

## Self-Review

**Spec coverage.** The extraction design's Part 2b-ii scope is `ThumbsEngine` and `FacesEngine` — Tasks 2–3 and Task 4. The spec's engine contract (options in, result out, optional queue, `const CancelToken&`, no prose, options by value) is in Global Constraints and enforced by each header. The two carried-forward items flagged at the 2b-i merge are Tasks 5 and 6. No spec requirement is unassigned.

**Placeholder scan.** No TBDs. Every code step carries real code. Three steps deliberately end in investigation rather than a fixed answer — Task 3 Step 5's drain-loop pattern, Task 5 Step 2's fixture choice, and Task 6 Step 3's fix choice — because each depends on a fact in the tree that must be read first, and each says exactly what to read and what to report. Task 6 Step 4 explicitly permits "no test, record the measurement instead" over a test that cannot fail.

**Type consistency.** `ThumbsOptions`/`ThumbsProgress`/`ThumbsStage`/`ThumbsResult`/`ThumbsEngine` and `thumb_needs_generating` are consistent between Task 2's header and Task 3's use. `FacesRebuildOptions`/`FacesProgress`/`FacesStage`/`FacesResult`/`FacesEngine` are consistent between Task 4's header and implementation. `detector_for_worker` is reused from `scan_engine.h` with its real signature rather than redeclared. Both engines name their entry point `run` and their progress field set `done`/`total`, matching `ScanEngine`.

**Both assumptions checked before handoff.** `passes_selection` is declared in `src/selection.h:14`, so the `#include "selection.h"` in Tasks 3 and 4 is correct and no extraction is needed. `Database::get_faces_for_image` does **not** exist — the class has `insert_face` and `delete_faces_for_image` but no per-image getter — so Task 4's test counts the `faces` table through its own read-only connection instead. Confirmed `faces` is the plain row table that `delete_faces_for_image` targets (`src/database.cpp:1574`), not the `vec_faces` vec0 virtual table.

**One thing this plan does not settle.** Task 6 Step 3 picks between three fixes for `StderrSuppressor` based on what noise actually appears with the suppressor removed. I have not run that measurement, so the plan states the preference order and requires the implementer to report findings before implementing — rather than naming a fix I cannot yet justify.
