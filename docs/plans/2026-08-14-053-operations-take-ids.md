# Operations Take Ids Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert the three long-running operations from glob selection to id
selection, so every surface whose subject is existing rows names them the same
way — plan 053's work-order step 3, and what 062-065 are blocked on.

**Architecture:** `ThumbsOptions`, `FacesRebuildOptions` and `OrganizeOptions`
drop `matches`/`excludes` for a `std::vector<int64_t> ids`, and their engines
load exactly those rows through a new `Database::get_images_by_ids` instead of
pulling the whole collection and filtering it. `cli/` resolves its globs to ids
with `ids_matching` before calling in. The ABI's three option structs mirror the
change.

**Tech Stack:** C++20, SQLite, GoogleTest, CMake.

## Two decisions this plan is built on

**An empty id list selects nothing.** Today an empty `matches` selects
everything, so a caller that forgets to set a selection silently operates on the
whole collection — and for faces that is R60's label destruction across every
photo. Inverted, forgetting yields a no-op. `cli/` preserves its own behaviour by
resolving every id when no `--match` is given, which the earlier measurement
puts at 8 MB for a million images, against the 3.8 KB/row the engine was already
loading for all of them.

**Version numbers are deliberately untouched.** Nothing outside the repo links
libphig and `gtk/` uses no operation entry point, so there is no compatibility
to preserve. Changing struct *fields* adds no symbols, so `kAbiSymbolCounts`'s
`{1, 4, 73}` row stays correct and the macro-versus-CMake comparison stays
green with no edit. Do not bump anything; both gates already pass.

## Global Constraints

- First-party code builds with `-Wall -Wextra`; CI configures `-DPHIG_WERROR=ON`.
- Format with the pinned clang-format 21.1.8 before every commit:
  `/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format`.
  `libphig/include/phig/phig.h` is below CI's `-maxdepth 1` glob — format it by
  hand.
- Engine options are stored **by value** (`core/CLAUDE.md`), so an id vector
  copied into the options struct is owned by the engine and safe across threads.
- Engines carry no user-facing prose; wording stays in `cli/`.
- The rules gate must stay green: `python3 scripts/rules.py --check --emit`.
- Commit messages: plain prose, no attribution trailers, no mention of AI.

---

### Task 1: `Database::get_images_by_ids`

**Files:**
- Modify: `core/database.h` (declare, beside `ids_matching`)
- Modify: `core/database.cpp` (define, beside `ids_matching`)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `std::vector<ImageInfo> Database::get_images_by_ids(const std::vector<int64_t>& ids);`
  — the rows for those ids, in ascending id order, silently skipping ids with no
  row. An empty input yields an empty vector. Tasks 2, 3 and 4 all call it.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_F(DatabaseTest, GetImagesByIdsReturnsOnlyThoseRows) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    db->insert(make_image("/a/two.jpg", "sha2"));
    db->insert(make_image("/a/three.jpg", "sha3"));

    int64_t one = db->get_image_id("/a/one.jpg");
    int64_t three = db->get_image_id("/a/three.jpg");

    auto rows = db->get_images_by_ids({one, three});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].path, "/a/one.jpg");
    EXPECT_EQ(rows[1].path, "/a/three.jpg");
    EXPECT_EQ(rows[0].sha256, "sha1");
}

TEST_F(DatabaseTest, GetImagesByIdsOnAnEmptyListReturnsNothing) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    EXPECT_TRUE(db->get_images_by_ids({}).empty())
        << "an empty selection must select nothing, not everything";
}

TEST_F(DatabaseTest, GetImagesByIdsSkipsIdsWithNoRow) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    int64_t one = db->get_image_id("/a/one.jpg");

    // A caller resolved ids, something deleted a row, then the caller acted.
    // Schema v9 means the missing id names nothing rather than a later photo
    // (R84), so the right behaviour is to skip it, not to fail the whole run.
    auto rows = db->get_images_by_ids({one, one + 99999});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, "/a/one.jpg");
}

TEST_F(DatabaseTest, GetImagesByIdsHandlesMoreIdsThanSqliteTakesParameters) {
    // set_gallery_by_ids steps one statement per id rather than building an
    // IN (...) list, and this must scale the same way: SQLite's bound-parameter
    // ceiling is not a limit a caller should have to know about.
    for (int i = 0; i < 1200; i++) {
        db->insert(make_image("/a/" + std::to_string(i) + ".jpg", "sha" + std::to_string(i)));
    }
    Database::PageCriteria pc;
    pc.limit = -1;
    auto ids = db->ids_matching(pc);
    ASSERT_EQ(ids.size(), 1200u);
    EXPECT_EQ(db->get_images_by_ids(ids).size(), 1200u);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='DatabaseTest.GetImagesByIds*'
```

Expected: compile error — `no member named 'get_images_by_ids' in 'Database'`.

- [ ] **Step 3: Declare it**

In `core/database.h`, after `ids_matching`:

```cpp
    // The rows for `ids`, ascending by id, skipping any id with no row — a
    // caller that resolved ids and then acted may find one deleted in between,
    // and schema v9 means a missing id names nothing rather than some later
    // photo (R84). An empty `ids` returns nothing: a selection says what it
    // means, and the engines that call this treat empty as "no work".
    std::vector<ImageInfo> get_images_by_ids(const std::vector<int64_t>& ids);
```

- [ ] **Step 4: Define it**

In `core/database.cpp`, after `ids_matching`. One prepared statement stepped per
id, matching `set_gallery_by_ids` (`core/database.cpp:1547`), so there is no
bound-parameter ceiling to design around:

```cpp
std::vector<ImageInfo> Database::get_images_by_ids(const std::vector<int64_t>& ids) {
    std::vector<ImageInfo> out;
    if (ids.empty()) return out;

    // Same column list and order as search_page's, so row_to_image_info reads
    // it correctly — the two must not drift.
    const char* sql =
        "SELECT id, path, filename, extension, file_size, created_at, modified_at, "
        "effective_date, sha256, phash, width, height, metadata, media_type, "
        "duration_ms, gallery, blur_score, "
        "EXISTS(SELECT 1 FROM image_tags it JOIN tags t ON t.id = it.tag_id "
        "WHERE it.image_id = images.id AND t.name = 'favorite') "
        "FROM images WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare id fetch: " + std::string(sqlite3_errmsg(db_)));
    }

    std::vector<int64_t> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    out.reserve(sorted.size());
    for (int64_t id : sorted) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            out.push_back(row_to_image_info(stmt));
        } else if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            throw std::runtime_error("Failed to read row by id: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
    }
    sqlite3_finalize(stmt);
    return out;
}
```

`<algorithm>` is needed for `sort`/`unique`; check the includes at the top of
`core/database.cpp` and add it if absent.

- [ ] **Step 5: Run to verify they pass, then the whole suite**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='DatabaseTest.GetImagesByIds*'
ctest --test-dir build --parallel
```

Expected: 4 new tests PASS, whole suite PASS.

- [ ] **Step 6: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i core/database.cpp core/database.h tests/test_database.cpp
git add core/database.cpp core/database.h tests/test_database.cpp
git commit -m "Add Database::get_images_by_ids

Loads exactly the rows a caller named, instead of the whole collection
followed by a filter — which is what the three engines do today, and
044's finding 6. Steps one statement per id like set_gallery_by_ids, so
a large selection needs no IN (...) list.

An id with no row is skipped rather than failing the call: a caller that
resolved ids and then acted can find one deleted in between, and schema
v9 means the gap names nothing rather than a later photo."
```

---

### Task 2: `ThumbsEngine` takes ids

**Files:**
- Modify: `core/thumbs_engine.h:13-19` (`ThumbsOptions`)
- Modify: `core/thumbs_engine.cpp:24-32` (selection)
- Modify: `cli/main.cpp:1160` (`thumbs rebuild` resolves first)
- Test: `tests/test_thumbs_engine.cpp`

**Interfaces:**
- Consumes: `Database::get_images_by_ids` from Task 1.
- Produces: `ThumbsOptions::ids` (`std::vector<int64_t>`) replacing
  `matches`/`excludes`. Task 5 maps the ABI struct onto it.

- [ ] **Step 1: Convert the two existing glob tests**

Exactly two tests use globs, both plain `TEST` in the `ThumbsEngineRun` suite
(not the `ThumbsEngineTest` fixture, which is a different set):

- `ThumbsEngineRun.SelectionFiltersRestrictWhatIsProcessed` (~line 290) sets
  `opts.matches.push_back((coll.dir / "img0.jpg").string())`
- `ThumbsEngineRun.AnExcludedRowGetsNoThumbnail` (~line 324) sets
  `opts.excludes = {"*img1*"}`

Both use `ScratchThumbs coll(n)`, which owns `coll.db` (a `unique_ptr<Database>`)
and `coll.dir`. Keep each test's intent and resolve the same set through the
database it already has:

```cpp
    // was: opts.matches.push_back((coll.dir / "img0.jpg").string());
    Database::PageCriteria pc;
    pc.filter.matches = {(coll.dir / "img0.jpg").string()};
    pc.limit = -1;
    opts.ids = coll.db->ids_matching(pc);
```

and for the exclude case:

```cpp
    // was: opts.excludes = {"*img1*"};
    Database::PageCriteria pc;
    pc.filter.excludes = {"*img1*"};
    pc.limit = -1;
    opts.ids = coll.db->ids_matching(pc);
```

Do not delete either test; their assertions are unchanged.

- [ ] **Step 2: Add the empty-selection test**

The inversion is the risky part of this plan, so it gets its own test:

```cpp
TEST(ThumbsEngineRun, AnEmptySelectionGeneratesNothing) {
    // Empty means nothing, not everything. A caller that forgot to set a
    // selection gets a no-op rather than a collection-wide run.
    ScratchThumbs coll(3);
    ThumbsOptions opts;
    opts.ids = {};
    ThumbsEngine engine(opts, *coll.db);
    CancelToken tok;

    ThumbsResult r = engine.run(tok, nullptr);

    EXPECT_EQ(r.total_selected, 0u);
    EXPECT_EQ(r.generated, 0u);
}
```

- [ ] **Step 3: Run to verify they fail**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='ThumbsEngine*'
```

Expected: compile error — `no member named 'ids' in 'ThumbsOptions'`.

- [ ] **Step 4: Change the options struct**

`core/thumbs_engine.h`:

```cpp
struct ThumbsOptions {
    // The rows to act on. Empty selects nothing — a caller names its subject,
    // and forgetting to is a no-op rather than a whole-collection run. cli/
    // resolves its --match/--exclude globs through Database::ids_matching
    // before constructing this.
    std::vector<int64_t> ids;
    bool force = false;
    int parallel = -1;                   // -1 = auto
    int size = PHIG_DEFAULT_THUMB_SIZE;  // longest edge, in pixels
};
```

`<cstdint>` and `<vector>` are needed; check the header's includes.

- [ ] **Step 5: Change the engine**

`core/thumbs_engine.cpp`, replacing the `get_all_images()` call and the
`passes_selection` filter:

```cpp
    std::vector<ImageInfo> images = db_.get_images_by_ids(opts_.ids);
```

Remove the now-unused `#include "selection.h"` if nothing else in the file uses
it.

- [ ] **Step 6: Resolve in the CLI**

`cli/main.cpp`, where `ThumbsOptions` is filled (~line 1160). The globs were
being handed to the engine; now they resolve first:

```cpp
        Database::PageCriteria pc;
        pc.filter.matches = matches;
        pc.filter.excludes = excludes;
        pc.limit = -1;
        opts.ids = db.ids_matching(pc);
```

This needs a `Database` in scope at that point. If the command opens one later,
move the open earlier rather than opening a second connection.

- [ ] **Step 7: Verify**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='ThumbsEngine*'
ctest --test-dir build --parallel
```

Expected: PASS.

- [ ] **Step 8: Check the CLI end to end**

A unit test does not exercise argv. Confirm the command still restricts:

```bash
rm -f /tmp/t.db*
./build/phig search --count --db /tmp/t.db   # creates it
./build/phig thumbs rebuild --db /tmp/t.db   # empty collection: 0, no error
```

Expected: no crash, no error, and a report of nothing to do.

- [ ] **Step 9: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i core/thumbs_engine.h core/thumbs_engine.cpp cli/main.cpp tests/test_thumbs_engine.cpp
git add core/thumbs_engine.h core/thumbs_engine.cpp cli/main.cpp tests/test_thumbs_engine.cpp
git commit -m "ThumbsEngine takes ids

Was loading every row and filtering it in memory. Now it loads the rows
it was given. cli/ resolves --match/--exclude to ids first, which is the
same work with the resolution out of the engine.

Empty selects nothing rather than everything: a caller names its subject,
and forgetting to is a no-op."
```

---

### Task 3: `FacesEngine` takes ids

Identical in shape to Task 2, on `core/faces_engine.h:13-17`,
`core/faces_engine.cpp:78-84` and `cli/main.cpp:1419`.

**Files:**
- Modify: `core/faces_engine.h` (`FacesRebuildOptions`)
- Modify: `core/faces_engine.cpp` (selection)
- Modify: `cli/main.cpp:1419` (`face rebuild` resolves first)
- Test: `tests/test_faces_engine.cpp`

**Interfaces:**
- Consumes: `Database::get_images_by_ids` from Task 1.
- Produces: `FacesRebuildOptions::ids` replacing `matches`/`excludes`.

- [ ] **Step 1: Update the two existing glob references and add the empty test**

Exactly two tests use globs, both plain `TEST` in the `FacesEngineRun` suite:

- `FacesEngineRun.SelectionFiltersRestrictWhatIsProcessed` (~line 327)
- `FacesEngineRun.AnExcludedImagesFacesAreLeftAlone` (~line 505)

Convert both as in Task 2, resolving through the database each test already
builds, and add:

```cpp
TEST(FacesEngineRun, AnEmptySelectionDetectsNothing) {
    // Matters more here than anywhere: a faces rebuild deletes every selected
    // image's face rows, including person labels someone typed (R60). An
    // unset selection must not mean the whole collection.
    FacesRebuildOptions opts;
    opts.ids = {};
    FacesEngine engine(opts, *coll.db);  // match the neighbouring tests' scratch helper
    CancelToken tok;
    FacesResult r = engine.run(tok, nullptr);
    EXPECT_EQ(r.total_selected, 0u);
}
```

Several `FacesEngineRun` tests skip when the face models are absent. This one
must not need them: an empty selection never reaches a detector, so do not copy
whatever model guard the neighbours use.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='*Faces*'
```

Expected: compile error — `no member named 'ids' in 'FacesRebuildOptions'`.

- [ ] **Step 3: Change the struct**

```cpp
struct FacesRebuildOptions {
    // The rows to act on. Empty selects nothing. This engine deletes and
    // re-derives face rows for everything it selects, including labels a person
    // typed (R60), so an unset selection meaning "the whole collection" would
    // be the worst possible default.
    std::vector<int64_t> ids;
    int parallel = -1;  // -1 = auto
};
```

- [ ] **Step 4: Change the engine**

`core/faces_engine.cpp`, replacing the `get_all_images()` + `passes_selection`
loop:

```cpp
    std::vector<ImageInfo> images = db_.get_images_by_ids(opts_.ids);
```

- [ ] **Step 5: Resolve in the CLI, then verify**

`cli/main.cpp:1419`, same pattern as Task 2.

```bash
cmake --build build -j && ctest --test-dir build --parallel
```

Expected: PASS, with the usual faces skips if models are absent.

- [ ] **Step 6: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i core/faces_engine.h core/faces_engine.cpp cli/main.cpp tests/test_faces_engine.cpp
git add core/faces_engine.h core/faces_engine.cpp cli/main.cpp tests/test_faces_engine.cpp
git commit -m "FacesEngine takes ids

Same conversion as thumbs, and the one where an empty selection matters
most: a rebuild deletes every selected image's face rows, including the
person labels R60 records as unrecoverable. Empty now selects nothing."
```

---

### Task 4: `OrganizeEngine` takes ids

**Files:**
- Modify: `core/organize_engine.h:18-28` (`OrganizeOptions`)
- Modify: `core/organize_engine.cpp:72-80` (selection)
- Modify: `cli/main.cpp:2014` (cp/mv resolve first)
- Test: `tests/test_organize_engine.cpp`

**Interfaces:**
- Consumes: `Database::get_images_by_ids` from Task 1.
- Produces: `OrganizeOptions::ids` replacing `matches`/`excludes`.

- [ ] **Step 1: Update the two glob references and add the empty test**

Exactly two tests use globs, both `TEST_F` on the `OrganizeEngineTest` fixture,
which supplies `add_image(db, name, exif_date)` and `base_opts()`:

- `OrganizeEngineTest.PlanHonorsMatchAndExclude` (~line 95) — **rename it**.
  The engine no longer honours match or exclude, so the name becomes a lie.
  `PlanHonorsItsIdSelection` says what it now checks.
- `OrganizeEngineTest.ApplyLeavesAnExcludedFileAndItsRowAlone` (~line 714) —
  the name still reads correctly, since the file is still excluded, just by the
  caller rather than the engine.

```cpp
TEST_F(OrganizeEngineTest, AnEmptySelectionPlansNothing) {
    Database db(db_path);
    db.init_schema();
    add_image(db, "a.jpg", "2019:07:04 10:00:00");

    OrganizeOptions o = base_opts();
    o.ids = {};
    OrganizeEngine engine(o, db);
    CancelToken cancel;
    OrganizePlan plan = engine.plan(cancel, nullptr);

    EXPECT_EQ(plan.size(), 0u) << "an empty selection must plan nothing";
}
```

Note `base_opts()` is the fixture's helper — check whether it sets globs, and
have it set no ids so each test opts in explicitly.

- [ ] **Step 2: Run to verify it fails, then change the struct**

```cpp
struct OrganizeOptions {
    std::string destination;
    std::string format;
    // The rows to act on. Empty selects nothing; cli/ resolves its globs
    // through Database::ids_matching before filling this.
    std::vector<int64_t> ids;
    ConflictResolution on_conflict = ConflictResolution::Rename;
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};
```

- [ ] **Step 3: Change the engine**

`core/organize_engine.cpp`:

```cpp
    auto images = db_.get_images_by_ids(opts_.ids);
```

`batch_sources_` is built from the same `images` vector immediately below and
needs no change.

- [ ] **Step 4: Resolve in the CLI, then verify**

`cli/main.cpp:2014`, same pattern. Note `cmd_organize` receives a filled
`OrganizeOptions`, so the resolution happens where the struct is built.

```bash
cmake --build build -j && ctest --test-dir build --parallel
```

- [ ] **Step 5: Check cp and mv end to end**

Organize moves real files, so argv coverage matters more here than anywhere:

```bash
rm -rf /tmp/org && mkdir -p /tmp/org/src /tmp/org/out
cp tests/data/*.jpg /tmp/org/src/ 2>/dev/null || echo "(use any jpg)"
rm -f /tmp/org/db*
./build/phig scan /tmp/org/src --db /tmp/org/db
./build/phig cp /tmp/org/out --db /tmp/org/db --dry-run
./build/phig cp /tmp/org/out --match '*nothing*' --db /tmp/org/db --dry-run
```

Expected: the first dry run lists every scanned file, the second lists none.
That is the whole conversion, observed from outside.

- [ ] **Step 6: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i core/organize_engine.h core/organize_engine.cpp cli/main.cpp tests/test_organize_engine.cpp
git add core/organize_engine.h core/organize_engine.cpp cli/main.cpp tests/test_organize_engine.cpp
git commit -m "OrganizeEngine takes ids

Last of the three. plan() loaded every row and filtered it; it now loads
what it was given. cp and mv resolve their globs to ids first."
```

---

### Task 5: The ABI's three option structs take ids

**Files:**
- Modify: `libphig/include/phig/phig.h` (`phig_thumbs_options`,
  `phig_faces_options`, `phig_organize_options`)
- Modify: `libphig/operations.cpp` (`phig_op_thumbs_start`,
  `phig_op_faces_start`)
- Modify: `libphig/organize.cpp` (`phig_op_organize_plan_start`)
- Test: `tests/test_libphig_operations.cpp`, `tests/test_libphig_organize.cpp`

**Interfaces:**
- Consumes: the three `core/` structs from Tasks 2-4.
- Produces: the ABI's id-based selection. Plans 062-065 build on it.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_libphig_operations.cpp`, matching its existing fixture style:

```cpp
TEST(AbiOperations, ThumbsRunsOnlyTheIdsItIsGiven) {
    // (seed a database with three rows, resolve two of them with
    // phig_query_ids, and start the operation on those)
    phig_thumbs_options o{};
    o.ids = ids->items;
    o.id_count = static_cast<size_t>(ids->count);
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &o, &op), PHIG_OK);
    ASSERT_EQ(phig_op_wait(op, 30000), PHIG_OK);
    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    EXPECT_EQ(r.total_selected, 2u);
    phig_op_free(op);
}

TEST(AbiOperations, ThumbsWithNoIdsSelectsNothing) {
    phig_thumbs_options o{};  // ids NULL, id_count 0
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &o, &op), PHIG_OK);
    ASSERT_EQ(phig_op_wait(op, 30000), PHIG_OK);
    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    EXPECT_EQ(r.total_selected, 0u)
        << "an unset selection must not mean the whole collection";
    phig_op_free(op);
}
```

Read the file's existing operation tests first and match how they build a
context and wait — the sketch above names the calls, not the fixture.

- [ ] **Step 2: Change the header**

Replace the glob members in all three structs. For thumbs:

```c
typedef struct {
    /* The images to act on. NULL/0 selects nothing — a caller names its
       subject, and an unset selection is a no-op rather than a whole-collection
       run. Resolve a set with phig_query_ids. The array need only outlive the
       _start call. */
    const int64_t* ids;
    size_t id_count;
    /* Longest edge in pixels. 0 uses phig's default. Sizes are independent —
       generating one never invalidates another. */
    int size;
    /* Regenerate thumbnails that already exist in the cache. */
    bool force;
    int parallel;
} phig_thumbs_options;
```

`phig_faces_options` and `phig_organize_options` take the same two members in
place of theirs, keeping their remaining fields unchanged.

- [ ] **Step 3: Change the three entry points**

In `libphig/operations.cpp`, `phig_op_thumbs_start` currently does:

```cpp
    topts.matches = copy_globs(opts->match, opts->match_count);
    topts.excludes = copy_globs(opts->exclude, opts->exclude_count);
```

Replace with a copy of the id array — by value, because the caller's array need
only outlive the `_start` call while the engine runs on a worker:

```cpp
    if (opts->ids != nullptr && opts->id_count > 0) {
        topts.ids.assign(opts->ids, opts->ids + opts->id_count);
    }
```

Do the same in `phig_op_faces_start` and in `organize.cpp`'s plan start. If
`copy_globs` is left with no callers, remove it.

- [ ] **Step 4: Verify**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='Abi*'
ctest --test-dir build --parallel
python3 scripts/rules.py --check
```

Expected: PASS, and the rules gate green — no symbol was added or removed, so
`kAbiSymbolCounts` and the version macros need no edit.

- [ ] **Step 5: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i libphig/include/phig/phig.h libphig/operations.cpp libphig/organize.cpp \
      tests/test_libphig_operations.cpp tests/test_libphig_organize.cpp
git add libphig/ tests/
git commit -m "ABI operations take ids

The last surface still naming its subject with globs. thumbs, faces and
organize now take an id array, which is what phig_query_ids produces, so
queries and operations finally speak one vocabulary.

NULL/0 selects nothing. No symbol changed, so the recorded symbol count
and the version macros are untouched."
```

---

### Task 6: Say what changed

**Files:**
- Modify: `libphig/CLAUDE.md` (the selection paragraph)
- Modify: `plans/053-libphig-feature-parity.md` (work order step 3)

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Correct `libphig/CLAUDE.md`**

Its selection paragraph currently ends by saying the operations still take
globs and that 053's step 3 will convert them. That is now done. Rewrite it to
state the finished rule: everything whose subject is existing rows takes ids,
globs are query predicates only, and index is the one surface that keeps a
directory because its subject is files with no row yet. Delete the sentence
warning not to read the operations' shape as intended — there is no longer a
gap to warn about.

- [ ] **Step 2: Mark the work-order step done**

```markdown
3. Operations converted to ids; `cli/` resolves ahead of them — done, executed
   via `docs/plans/2026-08-14-053-operations-take-ids.md`.
```

- [ ] **Step 3: Full gate and commit**

```bash
ctest --test-dir build --parallel
./build/phig-tests --gtest_output=xml:/tmp/phig-tests.xml
python3 scripts/rules.py --check --emit --test-results /tmp/phig-tests.xml
python3 -m unittest discover -s scripts -p 'test_*.py'
git add libphig/CLAUDE.md plans/053-libphig-feature-parity.md docs/plans/
git commit -m "Record that the operations now take ids"
```

---

## Verification

```bash
cmake -B build && cmake --build build -j
ctest --test-dir build --parallel
python3 -m unittest discover -s scripts -p 'test_*.py'
./build/phig-tests --gtest_output=xml:/tmp/phig-tests.xml
python3 scripts/rules.py --check --emit --test-results /tmp/phig-tests.xml
git diff --exit-code rules/
cmake -B build-werror -DPHIG_WERROR=ON && cmake --build build-werror -j && rm -rf build-werror
```

What this does **not** verify, and should be said rather than assumed:

- **`select_paths` is untouched.** `cli/main.cpp:1536` still loads every row and
  filters in memory for `tag`, `gallery` and `fav`. That is R78 and 053's step
  5, which the work order pairs with this one but which is a separate change.
- **`cmd_rm` is untouched** — plan 064.
- **No measurement here.** The claim that loading selected rows beats loading
  all of them and filtering is 044's finding 6 and is not re-measured; a narrow
  selection should now be dramatically cheaper, and nobody has timed it.
- **Sanitizers and macOS do not run on a PR.** Three engines changed how they
  acquire their work set; that is worth a `gh workflow run CI --ref <branch>`
  before merging rather than after.
