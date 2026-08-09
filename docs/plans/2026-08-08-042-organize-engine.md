# Organize Engine & cp/mv ABI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `phig cp` and `phig mv` run on an `OrganizeEngine` in `core/` with a
plan/apply split, and a foreign caller reaches both through libphig with per-file
conflict resolution.

**Architecture:** A new `core/organize_engine.{h,cpp}` holding the orchestration
that currently sits inline in `cli/main.cpp:618-770`. `plan()` writes nothing and
returns an `OrganizePlan` the caller owns and may edit; `apply()` consumes it,
re-verifies each decision, and executes through the existing
`execute_organize_action` / `carry_sidecars`. The ABI wraps the plan as an opaque
caller-owned value with `phig_op_copy_start` / `phig_op_move_start` over it.

**Tech Stack:** C++20, CMake, GoogleTest, SQLite.

## Global Constraints

- Owning plan: `plans/042-organize-engine.md`. Record deviations there, not here.
- Baseline before starting: **729/729 pass, 4 skipped** (3
  `MoveFileTest.CrossDevice*`, 1
  `ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing`), ~13s under
  `ctest --test-dir build --parallel`. Every task ends at or above this.
- Engine shape is fixed by `core/CLAUDE.md`: options struct stored **by value**,
  result struct out, `CancelToken` plus optional `ProgressQueue<T>*` which may be
  null. **No user-facing prose in any result or progress struct** — wording is
  the caller's.
- **Existing CLI behavior must not change.** In particular `--on-conflict
  overwrite` currently falls through to `rename` whenever the destination exists
  on disk, because `cmd_organize`'s `is_protected` returns true for any existing
  path (`cli/main.cpp:671-676`) and `resolve_conflict` renames around a protected
  destination (`paths.h:97-99`). Task 4 has a regression test pinning this.
- First-party code builds with `-Wall -Wextra`; CI adds `-DPHIG_WERROR=ON`.
- `clang-format -i` (Google, 4-space, 100-col) before every commit.
  `libphig/include/phig/phig.h` is **not** covered by CI's lint glob — format it
  by hand.
- Adding an ABI symbol bumps `PHIG_ABI_VERSION_MINOR` in `phig.h` and `VERSION`
  in `libphig/CMakeLists.txt`, never `SOVERSION`.
- No exception may cross the ABI boundary: wrap bodies in `PHIG_TRY`/`PHIG_CATCH`
  where a `phig_ctx*` is in scope.
- Tests get their temp directory from `make_temp_dir()` in `tests/temp_dir.h`.
  A hardcoded temp path passes alone and fails under `--parallel`.

## File Structure

| File | Responsibility |
|---|---|
| `core/organize_engine.h` | Types (`ConflictResolution`, `OrganizeOptions`, `OrganizeAction`, `OrganizePlan`, `OrganizeProgress`, `OrganizeResult`) and the `OrganizeEngine` class. |
| `core/organize_engine.cpp` | Planning, per-item override recomputation, apply loop. |
| `core/organize_exec.h/.cpp` | `carry_sidecars` gains a return count. Otherwise untouched. |
| `cli/main.cpp` | `cmd_organize` becomes argv → options → engine → rendering. |
| `libphig/organize.cpp` | ABI plan object and the two apply entry points. |
| `libphig/include/phig/phig.h` | Public declarations. |
| `tests/test_organize_engine.cpp` | Engine tests. |
| `tests/test_libphig_organize.cpp` | ABI tests. |

---

### Task 1: Engine types and the planner

Extraction only. The planning logic moves out of `cmd_organize` unchanged; no
behavior differs yet.

**Files:**
- Create: `core/organize_engine.h`, `core/organize_engine.cpp`
- Modify: `core/CMakeLists.txt`, `CMakeLists.txt`
- Test: `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: `apply_format` (`organize_plan.h`), `passes_selection`
  (`selection.h`), `resolve_conflict` (`paths.h`), `get_best_date` (`dates.h`).
- Produces: `OrganizeEngine::plan(const CancelToken&, ProgressQueue<OrganizeProgress>*)`
  returning `OrganizePlan`; the types below.

- [ ] **Step 1: Write the failing test**

`tests/test_organize_engine.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "cancel.h"
#include "database.h"
#include "organize_engine.h"
#include "progress_queue.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

class OrganizeEngineTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;

    void SetUp() override {
        dir = make_temp_dir("organize-engine");
        db_path = (dir / "phig.db").string();
    }
    void TearDown() override { fs::remove_all(dir); }

    // A real file on disk plus its row, so planning sees what production sees.
    // The date goes in metadata_json: get_best_date reads EXIF DateTimeOriginal
    // first and falls back to mtime (`core/dates.cpp:44-62`), so a created_at is
    // ignored and every fixture would file under today's date instead of the one
    // it names. EXIF format, not ISO.
    void add_image(Database& db, const std::string& name, const std::string& exif_date) {
        fs::path p = dir / "src" / name;
        fs::create_directories(p.parent_path());
        { std::ofstream f(p); f << name; }
        ImageInfo info;
        info.path = p.string();
        info.filename = name;
        info.extension = p.extension().string();
        info.sha256 = name;  // unique per file is all this needs
        info.metadata_json = R"({"DateTimeOriginal":")" + exif_date + R"("})";
        db.insert(info);
    }

    OrganizeOptions base_opts() {
        OrganizeOptions o;
        o.destination = (dir / "out").string();
        o.format = "%Y/%original";
        o.on_conflict = ConflictResolution::Rename;
        o.db_path = db_path;
        o.log_enabled = false;
        return o;
    }
};

TEST_F(OrganizeEngineTest, PlanProducesOneActionPerSelectedImage) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2020:01:02 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 2u);
    // Combined rather than asserted per index: get_all_images does not promise
    // an order, so pinning item 0 to 2019 would be testing the query planner.
    std::string all = plan.at(0).dest + "|" + plan.at(1).dest;
    EXPECT_NE(all.find("2019"), std::string::npos);
    EXPECT_NE(all.find("2020"), std::string::npos);
}

TEST_F(OrganizeEngineTest, PlanWritesNothingToDiskOrDatabase) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    const int64_t rows_before = db.count();

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_EQ(plan.size(), 1u);
    EXPECT_EQ(db.count(), rows_before);
    EXPECT_FALSE(fs::exists(dir / "out")) << "plan must not create the destination tree";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "plan must not move sources";
}

TEST_F(OrganizeEngineTest, PlanHonorsMatchAndExclude) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "keep.jpg", "2019:07:04 10:00:00");
    add_image(db, "drop.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts();
    o.excludes.push_back("*drop*");
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    EXPECT_NE(plan.at(0).source.find("keep.jpg"), std::string::npos);
}

TEST_F(OrganizeEngineTest, PlanFlagsAnOccupiedDestinationAsConflicting) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "occupant"; }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    EXPECT_TRUE(plan.at(0).conflicts);
    EXPECT_FALSE(plan.at(0).occupant_tracked) << "the occupant has no row";
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```

Expected: FAIL to compile — `organize_engine.h` does not exist. Add
`tests/test_organize_engine.cpp` to the `phig-tests` source list in the root
`CMakeLists.txt` (alphabetically, near `tests/test_organize_exec.cpp`) first, or
the test never builds at all.

- [ ] **Step 3: Write the header**

`core/organize_engine.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

// How one file's destination collision is resolved. Mirrors the strings
// resolve_conflict() takes; the engine converts at that call site rather than
// changing a primitive shared with other callers.
enum class ConflictResolution { Skip, Overwrite, Rename };

struct OrganizeOptions {
    std::string destination;
    std::string format;
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    ConflictResolution on_conflict = ConflictResolution::Rename;
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

// One planned move or copy. `dest` is empty when the item resolved to a skip.
struct OrganizeAction {
    std::string source;
    std::string dest;
    // The destination was occupied on disk, or already claimed by an earlier
    // item in this same plan.
    bool conflicts = false;
    // Whether the occupying file has a row in the database. Meaningless unless
    // `conflicts`. False means phig cannot restore it from its own records,
    // which is when a client should warn hardest.
    bool occupant_tracked = false;
    ConflictResolution resolution = ConflictResolution::Rename;
    // The caller chose this resolution explicitly rather than inheriting the
    // run-level policy. Carries the authority that lets apply() overwrite a
    // file that exists on disk (plan 042 decision 6).
    bool overridden = false;
};

// A caller-owned plan. Editable: a caller may override the resolution of any
// conflicting item before handing it to apply(). Owning the plan is what makes
// that safe — no engine thread shares this object.
class OrganizePlan {
   public:
    size_t size() const { return actions_.size(); }
    const OrganizeAction& at(size_t i) const { return actions_.at(i); }

    // Override one item's conflict resolution, recomputing its destination so
    // at(i).dest reflects the choice. Returns false, changing nothing, when `i`
    // is out of range, the item does not conflict, or the resulting destination
    // would be another image's not-yet-moved source file — which no caller
    // intent may overwrite (plan 042 decision 6).
    bool set_resolution(size_t i, ConflictResolution r);

   private:
    std::vector<OrganizeAction> actions_;
    // Every source path in this plan. A destination equal to one of these is a
    // tracked original that has not moved yet.
    std::set<std::string> batch_sources_;
    // Destinations already taken by earlier items, so two sources cannot land
    // on one path.
    std::set<std::string> claimed_;
    friend class OrganizeEngine;
};

// Published once when planning finishes, then once per file as apply() runs.
enum class OrganizeStage { FileDone, Planned };

struct OrganizeProgress {
    OrganizeStage stage = OrganizeStage::FileDone;
    // Parents — the photos the caller selected. Sidecars and attached JPEGs
    // travel with their parent and are counted in `files_written`, never here
    // (plan 042 decision 3).
    size_t done = 0, total = 0;
    size_t files_written = 0;
    size_t skipped = 0, errors = 0;
    std::string path;
    // Non-empty when this file failed: already-formatted warning text the
    // CALLER logs. Matches ScanProgress/ThumbsProgress — the engine does not
    // log from inside the loop where a caller is rendering progress.
    std::string error;
};

struct OrganizeResult {
    size_t applied = 0, skipped = 0, errors = 0;
    size_t files_written = 0;
    bool completed = true;
};

// Plans and executes cp/mv. `is_move` is a parameter of apply() rather than of
// the options because one plan serves both: destinations and conflicts are
// identical either way, and only the executor differs.
class OrganizeEngine {
   public:
    OrganizeEngine(const OrganizeOptions& opts, Database& db);

    // Writes nothing. Polls the token between items.
    OrganizePlan plan(const CancelToken& cancel, ProgressQueue<OrganizeProgress>* progress);

    OrganizeResult apply(const OrganizePlan& plan, bool is_move, const CancelToken& cancel,
                         ProgressQueue<OrganizeProgress>* progress);

   private:
    // By value: constructing from a temporary would otherwise dangle.
    OrganizeOptions opts_;
    Database& db_;
};
```

- [ ] **Step 4: Write the planner**

`core/organize_engine.cpp`. This is `cli/main.cpp:627-689` moved, with the
conflict outcome recorded on the action instead of only its destination:

```cpp
#include "organize_engine.h"

#include <algorithm>
#include <filesystem>

#include "dates.h"
#include "organize_plan.h"
#include "paths.h"
#include "selection.h"

namespace fs = std::filesystem;

namespace {

const char* policy_string(ConflictResolution r) {
    switch (r) {
        case ConflictResolution::Skip: return "skip";
        case ConflictResolution::Overwrite: return "overwrite";
        case ConflictResolution::Rename: return "rename";
    }
    return "rename";
}

}  // namespace

OrganizeEngine::OrganizeEngine(const OrganizeOptions& opts, Database& db)
    : opts_(opts), db_(db) {}

OrganizePlan OrganizeEngine::plan(const CancelToken& cancel,
                                  ProgressQueue<OrganizeProgress>* progress) {
    OrganizePlan out;

    auto images = db_.get_all_images();
    if (!opts_.matches.empty() || !opts_.excludes.empty()) {
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageInfo& img) {
                                        return !passes_selection(img.path, opts_.matches,
                                                                 opts_.excludes);
                                    }),
                     images.end());
    }

    for (const auto& img : images) out.batch_sources_.insert(img.path);

    for (const auto& img : images) {
        if (cancel.stop()) break;

        DateParts date = get_best_date(img);
        fs::path dest = fs::path(opts_.destination) / apply_format(opts_.format, img, date);

        // Protect another image's not-yet-moved source, and any real file on
        // disk. Clobbering an untracked file is silent data loss (audit #4);
        // clobbering a tracked one destroys an image the database still knows
        // about. A file may resolve to its own source, which is a no-op.
        const std::string& self_source = img.path;
        auto is_protected = [&](const std::string& path) -> bool {
            if (path == self_source) return false;
            if (out.batch_sources_.count(path)) return true;
            if (fs::exists(path)) return true;
            return false;
        };

        const bool occupied = fs::exists(dest) || out.claimed_.count(dest.string()) > 0;

        OrganizeAction act;
        act.source = img.path;
        act.resolution = opts_.on_conflict;
        act.conflicts = occupied;
        if (occupied) act.occupant_tracked = db_.get_image_id(dest.string()) >= 0;

        std::string resolved = resolve_conflict(dest.string(), policy_string(opts_.on_conflict),
                                                out.claimed_, is_protected);
        act.dest = resolved;  // empty means skip
        out.actions_.push_back(std::move(act));
    }

    if (progress) {
        OrganizeProgress p;
        p.stage = OrganizeStage::Planned;
        p.total = out.actions_.size();
        progress->push(p);
    }
    return out;
}
```

Leave `set_resolution` and `apply` unimplemented for now — Task 2 and Task 3
add them. Declare `set_resolution` returning `false` and `apply` returning `{}`
so the translation unit links:

```cpp
bool OrganizePlan::set_resolution(size_t, ConflictResolution) { return false; }

OrganizeResult OrganizeEngine::apply(const OrganizePlan&, bool, const CancelToken&,
                                     ProgressQueue<OrganizeProgress>*) {
    return {};
}
```

Add `organize_engine.cpp` to the `phig-core` source list in
`core/CMakeLists.txt`.

- [ ] **Step 5: Run and verify it passes**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='OrganizeEngineTest.*' 2>&1 | tail -10
```

Expected: 4 tests pass.

- [ ] **Step 6: Run the whole suite**

```bash
ctest --test-dir build --parallel 2>&1 | tail -6
```

Expected: 733/733, 4 skipped. No regressions.

- [ ] **Step 7: Commit**

```bash
clang-format -i core/organize_engine.h core/organize_engine.cpp tests/test_organize_engine.cpp
git add core/organize_engine.h core/organize_engine.cpp core/CMakeLists.txt \
    CMakeLists.txt tests/test_organize_engine.cpp
git commit -m "Add OrganizeEngine with a planning phase that writes nothing"
```

---

### Task 2: Per-item overrides and the protection bypass

**Files:**
- Modify: `core/organize_engine.cpp`
- Test: `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: `OrganizePlan` from Task 1.
- Produces: a working `OrganizePlan::set_resolution(size_t, ConflictResolution)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_organize_engine.cpp`:

```cpp
TEST_F(OrganizeEngineTest, SetResolutionRejectsANonConflictingItem) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    ASSERT_FALSE(plan.at(0).conflicts);
    EXPECT_FALSE(plan.set_resolution(0, ConflictResolution::Overwrite));
    EXPECT_FALSE(plan.at(0).overridden);
}

TEST_F(OrganizeEngineTest, SetResolutionRejectsAnIndexOutOfRange) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    EXPECT_FALSE(plan.set_resolution(99, ConflictResolution::Skip));
}

// Overriding to Overwrite must produce the plain destination, not a renamed
// one. Without the bypass, is_protected sees the existing file and
// resolve_conflict renames around it, silently discarding the caller's choice.
TEST_F(OrganizeEngineTest, OverridingToOverwriteTargetsTheOccupiedPath) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "occupant"; }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_EQ(plan.size(), 1u);
    ASSERT_TRUE(plan.at(0).conflicts);
    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Overwrite));
    EXPECT_TRUE(plan.at(0).overridden);
    EXPECT_EQ(plan.at(0).dest, (dir / "out" / "2019" / "a.jpg").string());
}

TEST_F(OrganizeEngineTest, OverridingToSkipEmptiesTheDestination) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "occupant"; }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Skip));
    EXPECT_TRUE(plan.at(0).dest.empty());
}

// The one bypass that must never be granted: the occupying file is another
// image's source, still waiting to be moved.
TEST_F(OrganizeEngineTest, SetResolutionRefusesToTargetABatchSource) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    // b.jpg lives exactly where a.jpg wants to land, and is itself indexed.
    fs::path collide = dir / "out" / "2019" / "a.jpg";
    fs::create_directories(collide.parent_path());
    { std::ofstream f(collide); f << "b"; }
    ImageInfo b;
    b.path = collide.string();
    b.filename = "a.jpg";
    b.extension = ".jpg";
    b.sha256 = "b-sha";
    b.created_at = "2021:05:05 10:00:00";
    db.insert_image(b);

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    // Find the item whose source is the src/ copy of a.jpg.
    size_t idx = plan.size();
    for (size_t i = 0; i < plan.size(); i++) {
        if (plan.at(i).source.find("/src/") != std::string::npos) idx = i;
    }
    ASSERT_LT(idx, plan.size());
    ASSERT_TRUE(plan.at(idx).conflicts);
    EXPECT_TRUE(plan.at(idx).occupant_tracked);
    EXPECT_FALSE(plan.set_resolution(idx, ConflictResolution::Overwrite))
        << "overwriting another image's un-moved source must be refused";
    EXPECT_TRUE(fs::exists(collide));
}
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='OrganizeEngineTest.*' 2>&1 | tail -12
```

Expected: the five new tests fail — `set_resolution` returns `false`
unconditionally, so the three expecting `true` fail immediately.

- [ ] **Step 3: Implement `set_resolution`**

Replace the stub in `core/organize_engine.cpp`:

```cpp
bool OrganizePlan::set_resolution(size_t i, ConflictResolution r) {
    if (i >= actions_.size()) return false;
    OrganizeAction& act = actions_[i];
    if (!act.conflicts) return false;

    // Release this item's current claim before recomputing, so its own earlier
    // destination does not look like someone else's.
    if (!act.dest.empty()) claimed_.erase(act.dest);

    // The unresolved destination: strip nothing, recompute from the original
    // target the planner recorded on the action.
    const std::string target = act.intended_dest;

    // An explicit override carries the authority to replace a file that exists
    // on disk — the caller saw the conflict in the plan and answered it. It
    // never carries authority over another image's un-moved source.
    const std::string& self_source = act.source;
    auto is_protected = [&](const std::string& path) -> bool {
        if (path == self_source) return false;
        if (batch_sources_.count(path)) return true;
        return false;  // fs::exists deliberately not consulted
    };

    if (r == ConflictResolution::Overwrite && batch_sources_.count(target) &&
        target != self_source) {
        // Restore the claim we released and refuse.
        if (!act.dest.empty()) claimed_.insert(act.dest);
        return false;
    }

    std::string resolved =
        resolve_conflict(target, policy_string(r), claimed_, is_protected);
    act.dest = resolved;
    act.resolution = r;
    act.overridden = true;
    return true;
}
```

This needs the pre-resolution destination, so add one field to
`OrganizeAction` in `core/organize_engine.h`:

```cpp
    // The destination before conflict resolution, kept so an override can be
    // recomputed from the same starting point rather than from a renamed path.
    std::string intended_dest;
```

and set it in `plan()` right after `act.source`:

```cpp
        act.intended_dest = dest.string();
```

`policy_string` is in an anonymous namespace above; move it above
`OrganizePlan::set_resolution` or make it a file-static free function so both
use it.

- [ ] **Step 4: Run and verify it passes**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='OrganizeEngineTest.*' 2>&1 | tail -12
```

Expected: 9 tests pass.

- [ ] **Step 5: Commit**

```bash
clang-format -i core/organize_engine.h core/organize_engine.cpp tests/test_organize_engine.cpp
git add core/organize_engine.h core/organize_engine.cpp tests/test_organize_engine.cpp
git commit -m "Let a caller override one item's conflict resolution"
```

---

### Task 3: The apply phase

**Files:**
- Modify: `core/organize_engine.cpp`, `core/organize_exec.h`, `core/organize_exec.cpp`
- Test: `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: `OrganizePlan` from Tasks 1-2, `execute_organize_action` and
  `carry_sidecars` from `organize_exec.h`.
- Produces: `OrganizeEngine::apply(...)` returning `OrganizeResult`;
  `carry_sidecars` now returns `size_t`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(OrganizeEngineTest, ApplyMovesFilesAndUpdatesRows) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, /*is_move=*/true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    EXPECT_EQ(result.errors, 0u);
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "a.jpg"));
    EXPECT_FALSE(fs::exists(dir / "src" / "a.jpg"));
    EXPECT_GE(db.get_image_id((dir / "out" / "2019" / "a.jpg").string()), 0);
}

TEST_F(OrganizeEngineTest, ApplyCopyLeavesTheSourceInPlace) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, /*is_move=*/false, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg"));
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "a.jpg"));
}

TEST_F(OrganizeEngineTest, ApplySkipsItemsWithNoDestination) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "occupant"; }

    OrganizeOptions o = base_opts();
    o.on_conflict = ConflictResolution::Skip;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 0u);
    EXPECT_EQ(result.skipped, 1u);
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg")) << "a skipped source stays put";
}

// Sidecars move with their parent and are counted separately, so the
// denominator a caller draws stays the photo count (plan 042 decision 3).
TEST_F(OrganizeEngineTest, SidecarsCountInFilesWrittenNotInApplied) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    { std::ofstream f(dir / "src" / "a.xmp"); f << "sidecar"; }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u) << "one photo";
    EXPECT_EQ(result.files_written, 2u) << "the photo and its sidecar";
    EXPECT_TRUE(fs::exists(dir / "out" / "2019" / "a.xmp"));
}

TEST_F(OrganizeEngineTest, AlreadyCancelledAppliesNothingAndReportsIncomplete) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    cancel.cancel();
    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 0u);
    EXPECT_FALSE(result.completed);
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg"));
}

TEST_F(OrganizeEngineTest, ApplyPublishesPlannedThenOneUpdatePerFile) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    add_image(db, "b.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    ProgressQueue<OrganizeProgress> q;
    OrganizePlan plan = engine.plan(cancel, &q);
    engine.apply(plan, true, cancel, &q);
    q.close();

    OrganizeProgress p;
    int planned = 0, file_done = 0;
    while (q.try_pop(p)) {
        if (p.stage == OrganizeStage::Planned) planned++;
        if (p.stage == OrganizeStage::FileDone) file_done++;
    }
    EXPECT_EQ(planned, 1);
    EXPECT_EQ(file_done, 2);
}
```

Verified against the headers: `CancelToken` exposes `cancel()` and `stop()`
(`core/cancel.h:20-21`), and `ProgressQueue` exposes `push`, `pop`, `try_pop`
and `close` (`core/progress_queue.h`). `Database::get_image_id` returns `-1` for
an absent path (`core/database.cpp:1364`), which is what `occupant_tracked`
relies on, and the insert method is `insert`, not `insert_image`.

- [ ] **Step 2: Run and verify it fails**

Expected: all six fail — `apply` returns a default-constructed `OrganizeResult`
and moves nothing.

- [ ] **Step 3: Make `carry_sidecars` report how many files it moved**

In `core/organize_exec.h`, change the declaration and extend the comment:

```cpp
// ...as before...
// Returns the number of files actually moved or copied, so a caller can count
// the work a parent dragged along without listing the directory again.
size_t carry_sidecars(Database& db, const std::string& parent_source,
                      const std::string& parent_dest, bool is_move, int64_t op_id);
```

In `core/organize_exec.cpp`, count each successful sidecar (and the attached
JPEG) and return the total. Returning a value where `void` was is
source-compatible, so existing callers and `tests/test_organize_exec.cpp` keep
compiling.

- [ ] **Step 4: Implement `apply`**

```cpp
OrganizeResult OrganizeEngine::apply(const OrganizePlan& plan, bool is_move,
                                     const CancelToken& cancel,
                                     ProgressQueue<OrganizeProgress>* progress) {
    OrganizeResult result;
    int64_t op_id = 0;
    if (opts_.log_enabled) {
        op_id = db_.log_begin_operation(is_move ? "mv" : "cp", opts_.argv_str, iso_utc_now());
    }

    for (size_t i = 0; i < plan.size(); i++) {
        if (cancel.stop()) {
            result.completed = false;
            break;
        }
        const OrganizeAction& act = plan.at(i);
        if (act.dest.empty()) {
            result.skipped++;
            continue;
        }

        OrganizeProgress p;
        p.stage = OrganizeStage::FileDone;
        p.total = plan.size();
        p.path = act.source;
        try {
            execute_organize_action(db_, act.source, act.dest, is_move, op_id);
            result.applied++;
            result.files_written++;
            result.files_written += carry_sidecars(db_, act.source, act.dest, is_move, op_id);
        } catch (const std::exception& e) {
            result.errors++;
            p.error = std::string("Failed to organize ") + act.source + ": " + e.what();
        }
        if (progress) {
            p.done = i + 1;
            p.files_written = result.files_written;
            p.skipped = result.skipped;
            p.errors = result.errors;
            progress->push(p);
        }
    }

    if (opts_.log_enabled) {
        db_.log_finish_operation(op_id, result.completed ? "completed" : "interrupted");
        db_.log_prune(opts_.log_max_entries);
    }
    return result;
}
```

Add `#include "organize_exec.h"` and `#include "paths.h"` (for `iso_utc_now`) to
`core/organize_engine.cpp`.

**`apply` does not re-verify yet** — Task 5 adds that, because it needs the ABI
tests to exercise a plan that aged. Note it in `plans/042` if you stop here.

- [ ] **Step 5: Run and verify it passes**

```bash
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -6
```

Expected: 739/739, 4 skipped.

- [ ] **Step 6: Commit**

```bash
clang-format -i core/organize_engine.cpp core/organize_exec.h core/organize_exec.cpp \
    tests/test_organize_engine.cpp
git add core/organize_engine.cpp core/organize_exec.h core/organize_exec.cpp \
    tests/test_organize_engine.cpp
git commit -m "Execute a plan through OrganizeEngine::apply"
```

---

### Task 4: Move the CLI onto the engine

The behavior-preservation task. `cmd_organize` keeps its output verbatim.

**Files:**
- Modify: `cli/main.cpp:618-770`
- Test: `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: nothing new.

- [ ] **Step 1: Write the regression test that pins today's behavior**

```cpp
// Today `--on-conflict overwrite` never replaces a file that exists on disk:
// is_protected covers any existing path and resolve_conflict renames around it.
// Extraction must not quietly turn overwrite into a destructive policy.
TEST_F(OrganizeEngineTest, OverwritePolicyStillRenamesAroundAnExistingFile) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "occupant"; }

    OrganizeOptions o = base_opts();
    o.on_conflict = ConflictResolution::Overwrite;
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    engine.apply(plan, true, cancel, nullptr);

    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_EQ(content, "occupant") << "run-level overwrite must not clobber an existing file";
}
```

- [ ] **Step 2: Run and verify it passes already**

This one passes before the CLI change — it pins Task 1's planner. Run it to
confirm the invariant holds, then keep it as the guard while rewriting
`cmd_organize`.

```bash
cmake --build build -j && \
  ./build/phig-tests --gtest_filter='*OverwritePolicyStillRenames*' 2>&1 | tail -5
```

- [ ] **Step 3: Rewrite `cmd_organize`**

Replace `cli/main.cpp:618-770` with argv → options → engine → rendering. Keep
every `std::cout` string exactly as it is today, including the `"Found N images
in database"` line, the `"After match/filter: N images"` line, the
`"Move: N files, skip: N (conflict/exists)"` summary, `[DRY RUN]` block, the
`\r[done/total] ` progress, and the final summary. Dry-run renders the plan and
returns 0 without calling `apply`. Keep `ScopedInterruptHandler` wrapping the
`apply` call so Ctrl-C still stops at a file boundary.

The skip reason the dry-run prints (`"already exists"`) now comes from
`act.dest.empty()`, not from a separate field.

- [ ] **Step 4: Verify the CLI end to end**

```bash
rm -rf /tmp/org-e2e && mkdir -p /tmp/org-e2e/src
cp tests/data/*.jpg /tmp/org-e2e/src/ 2>/dev/null || \
  (echo x > /tmp/org-e2e/src/a.jpg)
./build/phig scan /tmp/org-e2e/src --db /tmp/org-e2e/phig.db
./build/phig cp /tmp/org-e2e/out --db /tmp/org-e2e/phig.db --dry-run
./build/phig cp /tmp/org-e2e/out --db /tmp/org-e2e/phig.db
find /tmp/org-e2e/out -type f | head
```

Expected: dry-run lists the planned copies and writes nothing; the real run
creates them. Record the output in `plans/042`.

- [ ] **Step 5: Run the whole suite**

```bash
ctest --test-dir build --parallel 2>&1 | tail -6
```

Expected: 740/740, 4 skipped.

- [ ] **Step 6: Commit**

```bash
clang-format -i cli/main.cpp tests/test_organize_engine.cpp
git add cli/main.cpp tests/test_organize_engine.cpp
git commit -m "Run phig cp/mv through OrganizeEngine"
```

---

### Task 5: Apply re-verifies

**Files:**
- Modify: `core/organize_engine.cpp`
- Test: `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: `apply` from Task 3.
- Produces: no signature change; `OrganizeResult::skipped` now also counts
  items whose destination stopped being safe.

- [ ] **Step 1: Write the failing test**

```cpp
// 041's decision 4, inherited here: the plan is advisory and apply's own check
// is authoritative, so a destination occupied between plan and apply is not
// silently overwritten.
TEST_F(OrganizeEngineTest, ApplySkipsAnItemWhoseDestinationBecameOccupied) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_FALSE(plan.at(0).conflicts);

    // Someone else writes the destination after planning.
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "latecomer"; }

    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 0u);
    EXPECT_EQ(result.skipped, 1u);
    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_EQ(content, "latecomer") << "the file that arrived first must survive";
    EXPECT_TRUE(fs::exists(dir / "src" / "a.jpg"));
}

// An explicit override is the caller's authority to replace, so re-verification
// must not undo it.
TEST_F(OrganizeEngineTest, ApplyHonorsAnOverrideAgainstAFileThatStillExists) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");
    fs::create_directories(dir / "out" / "2019");
    { std::ofstream f(dir / "out" / "2019" / "a.jpg"); f << "occupant"; }

    OrganizeEngine engine(base_opts(), db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);
    ASSERT_TRUE(plan.set_resolution(0, ConflictResolution::Overwrite));

    OrganizeResult result = engine.apply(plan, true, cancel, nullptr);

    EXPECT_EQ(result.applied, 1u);
    std::ifstream f(dir / "out" / "2019" / "a.jpg");
    std::string content;
    f >> content;
    EXPECT_NE(content, "occupant") << "an explicit override replaces the occupant";
}
```

- [ ] **Step 2: Run and verify the first fails**

Expected: `ApplySkipsAnItemWhoseDestinationBecameOccupied` fails — `apply`
currently calls `execute_organize_action` regardless, and `move_file` refuses to
overwrite, so the item lands in `errors` rather than `skipped`.

- [ ] **Step 3: Add the re-check**

In `apply`, before executing an item:

```cpp
        // The plan is advisory; this check is authoritative (plan 041 decision
        // 4, inherited by 042). A destination that became occupied since
        // planning is skipped unless the caller explicitly authorized replacing
        // it.
        if (!act.overridden && fs::exists(act.dest)) {
            result.skipped++;
            if (progress) {
                p.done = i + 1;
                p.skipped = result.skipped;
                progress->push(p);
            }
            continue;
        }
```

- [ ] **Step 4: Run and verify both pass**

```bash
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -6
```

Expected: 742/742, 4 skipped.

- [ ] **Step 5: Commit**

```bash
clang-format -i core/organize_engine.cpp tests/test_organize_engine.cpp
git add core/organize_engine.cpp tests/test_organize_engine.cpp
git commit -m "Re-verify each destination at apply time"
```

---

### Task 6: ABI plan surface

**Files:**
- Create: `libphig/organize.cpp`, `tests/test_libphig_organize.cpp`
- Modify: `libphig/include/phig/phig.h`, `libphig/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `OrganizeEngine`, `OrganizePlan` from Tasks 1-5.
- Produces: `phig_organize_plan`, `phig_op_organize_plan_start`,
  `phig_op_organize_plan_take`, `phig_organize_plan_free`, and the accessors.

- [ ] **Step 1: Add the declarations**

In `libphig/include/phig/phig.h`, after the operations block, add exactly the
`## ABI shape` section from `plans/042-organize-engine.md` — the
`phig_organize_plan` opaque type, `phig_conflict_resolution`,
`phig_organize_options`, `phig_organize_result`, and every function through
`phig_organize_plan_set_resolution`. Bump `PHIG_ABI_VERSION_MINOR` and the
`VERSION` in `libphig/CMakeLists.txt`. Leave `SOVERSION` alone.

- [ ] **Step 2: Write the failing test**

`tests/test_libphig_organize.cpp`, following `tests/test_libphig_operations.cpp`
for context setup:

```cpp
TEST_F(AbiOrganizeTest, PlanListsOneItemPerImageAndWritesNothing) {
    phig_op* op = nullptr;
    phig_organize_options opts{};
    opts.destination = out_dir.c_str();
    opts.format = "%Y/%original";
    opts.on_conflict = PHIG_CONFLICT_RENAME;
    ASSERT_EQ(phig_op_organize_plan_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);
    ASSERT_EQ(phig_op_status(op), PHIG_OP_DONE);

    phig_organize_plan* plan = nullptr;
    ASSERT_EQ(phig_op_organize_plan_take(op, &plan), PHIG_OK);
    EXPECT_EQ(phig_organize_plan_count(plan), 1u);
    EXPECT_NE(std::string(phig_organize_plan_dest_at(plan, 0)).find("2019"), std::string::npos);

    phig_organize_plan_free(plan);
    phig_op_free(op);
}

// Decision 1: the plan outlives the operation that computed it.
TEST_F(AbiOrganizeTest, PlanStaysUsableAfterTheOperationIsFreed) {
    phig_op* op = nullptr;
    phig_organize_options opts{};
    opts.destination = out_dir.c_str();
    opts.format = "%Y/%original";
    opts.on_conflict = PHIG_CONFLICT_RENAME;
    ASSERT_EQ(phig_op_organize_plan_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_organize_plan* plan = nullptr;
    ASSERT_EQ(phig_op_organize_plan_take(op, &plan), PHIG_OK);
    phig_op_free(op);  // the operation goes away first

    ASSERT_EQ(phig_organize_plan_count(plan), 1u);
    EXPECT_STRNE(phig_organize_plan_source_at(plan, 0), "");
    phig_organize_plan_free(plan);
}

TEST_F(AbiOrganizeTest, SetResolutionRejectsANonConflictingItem) {
    /* …plan as above… */
    EXPECT_EQ(phig_organize_plan_set_resolution(plan, 0, PHIG_CONFLICT_OVERWRITE),
              PHIG_ERR_INVALID_ARG);
}
```

- [ ] **Step 3: Run and verify it fails**

Expected: FAIL to compile — the symbols do not exist.

- [ ] **Step 4: Implement `libphig/organize.cpp`**

`phig_organize_plan` wraps an owned `OrganizePlan` plus a `std::string` scratch
buffer per accessor so returned `const char*` stay valid until the next call on
that plan. `phig_op_organize_plan_take` **copies** the engine's plan out of the
operation, so the operation may be freed immediately. Wrap every body with a
`phig_ctx*` in `PHIG_TRY`/`PHIG_CATCH`; the plan accessors have no ctx, so wrap
them in a plain `try`/`catch (...)` returning a safe default.

Add `organize.cpp` to `libphig/CMakeLists.txt` and
`tests/test_libphig_organize.cpp` to the `phig-tests` list.

- [ ] **Step 5: Run and verify it passes**

```bash
cmake -B build && cmake --build build -j && \
  ./build/phig-tests --gtest_filter='AbiOrganize*' 2>&1 | tail -10
```

- [ ] **Step 6: Commit**

```bash
clang-format -i libphig/organize.cpp tests/test_libphig_organize.cpp
# phig.h is outside CI's lint glob — format it explicitly
clang-format -i libphig/include/phig/phig.h
git add libphig/ tests/test_libphig_organize.cpp CMakeLists.txt
git commit -m "Expose the organize plan through libphig"
```

---

### Task 7: ABI apply, docs, and the coverage table

**Files:**
- Modify: `libphig/organize.cpp`, `libphig/include/phig/phig.h`, `libphig/CLAUDE.md`,
  `CLAUDE.md`, `plans/042-organize-engine.md`, `plans/000-INDEX.md`
- Test: `tests/test_libphig_organize.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
TEST_F(AbiOrganizeTest, MoveAppliesThePlanAndTheSourceIsGone) {
    /* …take the plan as in Task 6… */
    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_move_start(ctx, plan, &apply_op), PHIG_OK);
    phig_op_wait(apply_op);
    ASSERT_EQ(phig_op_status(apply_op), PHIG_OP_DONE);

    phig_organize_result r{};
    ASSERT_EQ(phig_op_organize_result(apply_op, &r), PHIG_OK);
    EXPECT_EQ(r.applied, 1u);
    EXPECT_TRUE(r.completed);

    phig_op_free(apply_op);
    phig_organize_plan_free(plan);
}

// Decision 1 again, on the apply side: apply snapshots at start.
TEST_F(AbiOrganizeTest, PlanMayBeFreedImmediatelyAfterApplyStarts) {
    /* …take the plan… */
    phig_op* apply_op = nullptr;
    ASSERT_EQ(phig_op_copy_start(ctx, plan, &apply_op), PHIG_OK);
    phig_organize_plan_free(plan);  // before the operation finishes
    phig_op_wait(apply_op);
    EXPECT_EQ(phig_op_status(apply_op), PHIG_OP_DONE);
    phig_op_free(apply_op);
}
```

Run that second test under ASan — it is the one that catches a borrow where a
copy was intended:

```bash
cmake -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-san -j && \
  ASAN_OPTIONS=detect_leaks=0 ./build-san/phig-tests --gtest_filter='AbiOrganize*'
```

- [ ] **Step 2: Run and verify it fails**

Expected: FAIL to compile — `phig_op_move_start` does not exist.

- [ ] **Step 3: Implement the two apply entry points**

`phig_op_copy_start` and `phig_op_move_start` both copy the plan, then run
`OrganizeEngine::apply` on the operation thread with `is_move` false and true
respectively. Reuse the `phig_op` lifecycle from `libphig/operations.cpp`
unchanged — same poll, cancel, wait, free, and error retention.

- [ ] **Step 4: Run and verify it passes**

```bash
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -6
```

- [ ] **Step 5: Update the documentation**

- `libphig/CLAUDE.md`: delete the "Organize — cp/mv" row from the **What is
  missing** table, and update the symbol count in the opening paragraph.
- Root `CLAUDE.md`: nothing needed unless the CLI's flag surface changed, which
  it must not have.
- `plans/042-organize-engine.md`: set `status: completed`, record the Task 4
  real-run output, and note anything that deviated.
- `plans/000-INDEX.md`: update the 042 row's status.

- [ ] **Step 6: Commit**

```bash
clang-format -i libphig/organize.cpp tests/test_libphig_organize.cpp
clang-format -i libphig/include/phig/phig.h
git add libphig/ tests/test_libphig_organize.cpp libphig/CLAUDE.md \
    plans/042-organize-engine.md plans/000-INDEX.md
git commit -m "Expose copy and move as separate ABI operations"
```

---

## Self-Review

**Spec coverage.** Every decision in `plans/042` maps to a task: decision 1
(plan as caller-owned value) is Tasks 6-7, decision 2 (per-file resolution) is
Task 2 for the engine and Task 6 for the ABI, decision 3 (progress counts
parents) is Task 3's `SidecarsCountInFilesWrittenNotInApplied`, decision 4 (both
cp and mv get a plan phase) is Task 7's two entry points, decision 5
(cancellation at file boundaries) is Task 3's
`AlreadyCancelledAppliesNothingAndReportsIncomplete`, decision 6 (override
bypasses the on-disk guard, never batch sources) is Task 2's last two tests plus
Task 5's second. The `## Testing` section's regression case is Task 4 Step 1.

**Deliberate gaps.** `apply` does not re-verify until Task 5, so Tasks 3 and 4
ship a narrower guarantee than the plan states. Task 3 Step 4 says so and tells
the implementer to record it if they stop there.

**Type consistency.** `ConflictResolution` (engine) maps to
`phig_conflict_resolution` (ABI) with the same three values in the same order.
`OrganizeAction::intended_dest` is introduced in Task 2 Step 3 and set in the
planner written in Task 1 — an implementer doing Task 2 must edit Task 1's
`plan()`, which Task 2 Step 3 states explicitly. `carry_sidecars` changes from
`void` to `size_t` in Task 3 Step 3, and no earlier task depends on its return.

**Not verified while writing.** The exact spelling of `CancelToken`'s
stop-request method and `ProgressQueue`'s pop method — Task 3 Step 1 tells the
implementer to read `core/cancel.h` and `core/progress_queue.h` rather than
trusting the test code as written. `Database::get_image_id` returning `-1` for
an absent path is assumed from its use in `core/organize_exec.h`'s comment; the
planner's `occupant_tracked` depends on it.
