# Query Selection — Globs and `phig_query_ids` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `phig_query` the glob predicates it lacks and a way to resolve any
query to image ids, so a client can search by filename and so every later
surface can take ids — plan 053's work-order step 2.

**Architecture:** `matches`/`excludes` go on `Database::SearchCriteria`, so the
one shared `build_filter_sql` gives globs to `search`, `search_page` and
`count_matching` together. Matching happens in SQL through a registered
`phig_glob` function that calls phig's existing `glob_match`, so a pattern means
in SQL exactly what it means on the command line. `Database::ids_matching`
returns ids without materializing rows, and three ABI entry points expose the
lot.

**Tech Stack:** C++20, SQLite (system libsqlite3 3.51.0 on macOS), GoogleTest,
CMake.

## Why matching is in SQL and not in memory

`search_page` applies `LIMIT`/`OFFSET`. Filtering after the query returns short
pages and skips rows, so a paged client silently loses results — the same trap
`SearchQuery` already documents at `core/search_engine.h:22-29`, where the
SQL-level limit is suppressed whenever a path selection is pending. Paging is
not optional at the ABI, so the predicate has to be in the statement.

## Why a registered function and not SQLite's `GLOB`

They are different languages. phig's `glob_match` (`core/selection.cpp:5`)
supports `*`, `?` and backslash escapes; SQLite's `GLOB` supports `*`, `?` and
`[...]` character classes and has no escape. Translating patterns between them
would change which files an existing `--match` selects, quietly and only for
patterns containing `[`, `]` or `\`. Registering `glob_match` itself means one
implementation and no divergence to maintain.

The cost is that `phig_glob(?, path)` is opaque to the planner, so it cannot use
`idx_path` and the filter is a full scan. Glob matching against arbitrary
patterns is a scan regardless of how it is spelled — but this is measured in
Task 4 rather than assumed.

## Global Constraints

- First-party code builds with `-Wall -Wextra`; CI configures `-DPHIG_WERROR=ON`.
- Format with the pinned clang-format 21.1.8 before every commit:
  `/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format`.
  `libphig/include/phig/phig.h` is below CI's `-maxdepth 1` glob and must be
  formatted by hand.
- Adding an ABI symbol bumps `PHIG_ABI_VERSION_MINOR` in `phig.h` **and** the
  CMake `VERSION` in `libphig/CMakeLists.txt`, never `SOVERSION`. CI compares
  the three.
- Export lists match on the `phig_` prefix, so a new entry point needs no edit
  to `phig.version` or `phig.exported_symbols`.
- No C++ exception may cross an ABI entry point (R62). An entry point returning
  something other than `phig_status` still needs a `try`/`catch (...)`.
- `phig_op_progress` must not gain a field; irrelevant here, but the reason
  applies to any caller-allocated struct: `phig_id_list` is engine-allocated and
  returned by pointer, which is why it may carry a count.
- Commit messages: plain prose, no attribution trailers, no mention of AI.

---

### Task 1: `phig_glob` in SQL, and globs on `SearchCriteria`

**Files:**
- Modify: `core/database.h:135-151` (`SearchCriteria`)
- Modify: `core/database.cpp:296-308` (register the function in the constructor)
- Modify: `core/database.cpp:2062-2133` (`build_filter_sql`)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: `glob_match` from `core/selection.h`.
- Produces: `SearchCriteria::matches` and `SearchCriteria::excludes`
  (`std::vector<std::string>`), honoured by `search`, `search_page` and
  `count_matching`. Task 2 and Task 3 both rely on these.

- [x] **Step 1: Write the failing tests**

Add to `tests/test_database.cpp`. These pin the three properties that matter:
the filter reaches SQL, exclude beats match, and — the one that would be missed
by an in-memory implementation — paging stays correct under a glob.

```cpp
TEST_F(DatabaseTest, SearchMatchesByGlobOnTheFullPath) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    db->insert(make_image("/a/two.cr2", "sha2"));
    db->insert(make_image("/b/three.jpg", "sha3"));

    Database::SearchCriteria c;
    c.matches = {"/a/*"};
    auto rows = db->search(c);
    ASSERT_EQ(rows.size(), 2u);

    c.matches = {"*.cr2"};
    rows = db->search(c);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, "/a/two.cr2");
}

TEST_F(DatabaseTest, ExcludeBeatsMatchJustLikePassesSelection) {
    db->insert(make_image("/a/keep.jpg", "sha1"));
    db->insert(make_image("/a/skip.jpg", "sha2"));

    Database::SearchCriteria c;
    c.matches = {"/a/*"};
    c.excludes = {"*skip*"};
    auto rows = db->search(c);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, "/a/keep.jpg");
}

TEST_F(DatabaseTest, AnExcludeAloneSelectsEverythingElse) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    db->insert(make_image("/a/two.jpg", "sha2"));

    Database::SearchCriteria c;
    c.excludes = {"*two*"};
    EXPECT_EQ(db->search(c).size(), 1u);
}

TEST_F(DatabaseTest, GlobMatchingUsesPhigSemanticsNotSqliteGlob) {
    // A literal '[' in a filename. SQLite's GLOB would read it as the start of
    // a character class and fail to match; phig's glob_match treats it as an
    // ordinary character. This is the case that makes translating patterns
    // wrong rather than merely different.
    db->insert(make_image("/a/set[1].jpg", "sha1"));
    db->insert(make_image("/a/plain.jpg", "sha2"));

    Database::SearchCriteria c;
    c.matches = {"/a/set[1].jpg"};
    auto rows = db->search(c);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].path, "/a/set[1].jpg");
}

TEST_F(DatabaseTest, GlobFilteredPagesAreFullAndDoNotSkipRows) {
    // The reason the predicate has to be in SQL. Ten matching rows among
    // twenty; filtering after LIMIT/OFFSET would return short pages and skip
    // rows entirely.
    for (int i = 0; i < 20; i++) {
        std::string dir = (i % 2 == 0) ? "/keep/" : "/drop/";
        db->insert(make_image(dir + std::to_string(i) + ".jpg", "sha" + std::to_string(i)));
    }

    Database::PageCriteria pc;
    pc.filter.matches = {"/keep/*"};
    pc.sort = Database::SortKey::Path;
    pc.descending = false;
    pc.limit = 4;

    std::vector<std::string> seen;
    for (int64_t off = 0;; off += 4) {
        pc.offset = off;
        auto page = db->search_page(pc);
        if (page.empty()) break;
        for (const auto& r : page) seen.push_back(r.path);
    }

    EXPECT_EQ(seen.size(), 10u) << "paging under a glob lost or duplicated rows";
    for (const auto& p : seen) {
        EXPECT_EQ(p.rfind("/keep/", 0), 0u) << p << " should not have matched";
    }
    EXPECT_EQ(std::set<std::string>(seen.begin(), seen.end()).size(), seen.size())
        << "a row appeared on two pages";
}

TEST_F(DatabaseTest, CountMatchingHonoursGlobs) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    db->insert(make_image("/a/two.jpg", "sha2"));
    db->insert(make_image("/b/three.jpg", "sha3"));

    Database::SearchCriteria c;
    c.matches = {"/a/*"};
    EXPECT_EQ(db->count_matching(c), 2);
}
```

`<set>` is needed for the paging test; add it to the includes at the top of the
file if it is not already there.

- [x] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='DatabaseTest.*Glob*:DatabaseTest.*Exclude*:DatabaseTest.SearchMatchesByGlob*:DatabaseTest.CountMatchingHonoursGlobs'
```

Expected: compile error — `no member named 'matches' in 'Database::SearchCriteria'`.

- [x] **Step 3: Add the fields**

In `core/database.h`, inside `SearchCriteria`, after `tags`:

```cpp
        // Glob selection over the full path, with phig's glob semantics (see
        // core/selection.h). Empty matches selects everything; an exclude wins
        // where the two overlap. Applied in SQL through the phig_glob function
        // registered in the constructor, because search_page applies
        // LIMIT/OFFSET and filtering afterwards would return short pages.
        std::vector<std::string> matches;
        std::vector<std::string> excludes;
```

- [x] **Step 4: Register the SQL function**

In `core/database.cpp`, add the include beside the others at the top:

```cpp
#include "selection.h"
```

Then in the constructor, immediately after `sqlite3_vec_init(db_, nullptr, nullptr);`:

```cpp
    // phig's own glob, callable from SQL as phig_glob(pattern, path). Not
    // SQLite's GLOB: that supports [...] character classes and no backslash
    // escape, where phig supports the escape and treats brackets literally, so
    // translating patterns would change which files an existing --match
    // selects. Deterministic and pure, so SQLITE_DETERMINISTIC lets the planner
    // hoist it out of a loop where it can.
    sqlite3_create_function(
        db_, "phig_glob", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
        [](sqlite3_context* ctx, int argc, sqlite3_value** argv) {
            if (argc != 2 || sqlite3_value_type(argv[0]) == SQLITE_NULL ||
                sqlite3_value_type(argv[1]) == SQLITE_NULL) {
                sqlite3_result_int(ctx, 0);
                return;
            }
            const auto* pattern = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
            const auto* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[1]));
            if (pattern == nullptr || path == nullptr) {
                sqlite3_result_int(ctx, 0);
                return;
            }
            sqlite3_result_int(ctx, glob_match(pattern, path) ? 1 : 0);
        },
        nullptr, nullptr);
```

- [x] **Step 5: Emit the clauses**

In `core/database.cpp`, at the end of `build_filter_sql`, after the sharpness
bounds:

```cpp
    // Exclude first and unconditionally, mirroring passes_selection: an
    // exclude wins over a match, and an exclude with no matches still narrows.
    for (const auto& e : c.excludes) {
        sql += " AND NOT phig_glob(?, path)";
        binds.push_back({e, false});
    }
    // An empty match list selects everything, so the OR group is only emitted
    // when there is something to match.
    if (!c.matches.empty()) {
        sql += " AND (";
        for (size_t i = 0; i < c.matches.size(); i++) {
            sql += (i == 0) ? "phig_glob(?, path)" : " OR phig_glob(?, path)";
            binds.push_back({c.matches[i], false});
        }
        sql += ")";
    }
```

- [x] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='DatabaseTest.*Glob*:DatabaseTest.*Exclude*:DatabaseTest.SearchMatchesByGlob*:DatabaseTest.CountMatchingHonoursGlobs'
```

Expected: 6 tests PASS.

- [x] **Step 7: Run the whole suite**

```bash
ctest --test-dir build --parallel
```

Expected: PASS. `search` gained two optional fields that default empty, so no
existing caller changes behaviour. If something fails, check first whether the
test builds a `SearchCriteria` by aggregate initialization — adding fields
shifts positional initializers.

- [x] **Step 8: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i core/database.cpp core/database.h tests/test_database.cpp
git add core/database.cpp core/database.h tests/test_database.cpp
git commit -m "Glob selection in SQL, through phig's own matcher

SearchCriteria gains matches/excludes, so search, search_page and
count_matching all honour them from the one shared build_filter_sql.
Matching goes through a registered phig_glob function calling glob_match
rather than SQLite's GLOB, which has character classes and no escape and
would change which files an existing --match selects.

In SQL rather than in memory because search_page applies LIMIT/OFFSET:
filtering afterwards returns short pages and skips rows."
```

---

### Task 2: `Database::ids_matching`

**Files:**
- Modify: `core/database.h` (declare, beside `count_matching`)
- Modify: `core/database.cpp` (define, beside `count_matching`)
- Test: `tests/test_database.cpp`

**Interfaces:**
- Consumes: `build_filter_sql`, `bind_filter_values` and the glob support from
  Task 1.
- Produces: `std::vector<int64_t> Database::ids_matching(const PageCriteria& c);`
  — the ids a query selects, in the query's sort order, honouring its
  offset/limit when `limit >= 0`. Task 3's `phig_query_ids` calls it.

- [x] **Step 1: Write the failing tests**

```cpp
TEST_F(DatabaseTest, IdsMatchingReturnsIdsInSortOrder) {
    db->insert(make_image("/a/c.jpg", "sha3"));
    db->insert(make_image("/a/a.jpg", "sha1"));
    db->insert(make_image("/a/b.jpg", "sha2"));

    Database::PageCriteria pc;
    pc.sort = Database::SortKey::Path;
    pc.descending = false;
    pc.limit = -1;

    auto ids = db->ids_matching(pc);
    ASSERT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], db->get_image_id("/a/a.jpg"));
    EXPECT_EQ(ids[1], db->get_image_id("/a/b.jpg"));
    EXPECT_EQ(ids[2], db->get_image_id("/a/c.jpg"));
}

TEST_F(DatabaseTest, IdsMatchingHonoursFiltersAndGlobs) {
    db->insert(make_image("/a/one.jpg", "sha1"));
    db->insert(make_image("/a/two.cr2", "sha2"));
    db->insert(make_image("/b/three.jpg", "sha3"));

    Database::PageCriteria pc;
    pc.filter.matches = {"/a/*"};
    pc.limit = -1;
    EXPECT_EQ(db->ids_matching(pc).size(), 2u);

    pc.filter.extension = "cr2";
    auto ids = db->ids_matching(pc);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], db->get_image_id("/a/two.cr2"));
}

TEST_F(DatabaseTest, IdsMatchingWithNoLimitReturnsEveryMatchNotAPage) {
    // The default PageCriteria::limit is 200. Resolving a selection must not
    // silently stop there, which is the whole difference between this and
    // paging.
    for (int i = 0; i < 250; i++) {
        db->insert(make_image("/a/" + std::to_string(i) + ".jpg", "sha" + std::to_string(i)));
    }
    Database::PageCriteria pc;
    pc.limit = -1;
    EXPECT_EQ(db->ids_matching(pc).size(), 250u);
}
```

- [x] **Step 2: Run to verify they fail**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='DatabaseTest.IdsMatching*'
```

Expected: compile error — `no member named 'ids_matching' in 'Database'`.

- [x] **Step 3: Declare it**

In `core/database.h`, after `count_matching`:

```cpp
    // The ids a query selects, in the same order search_page would return them,
    // without materializing rows. `c.limit < 0` means every match rather than a
    // page — resolving a selection is not paging, and stopping at the default
    // page size would silently truncate it.
    std::vector<int64_t> ids_matching(const PageCriteria& c);
```

- [x] **Step 4: Define it**

In `core/database.cpp`, after `count_matching`:

```cpp
std::vector<int64_t> Database::ids_matching(const PageCriteria& c) {
    std::string sql = "SELECT id FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;
    build_filter_sql(c.filter, sql, binds);

    const char* dir = c.descending ? "DESC" : "ASC";
    sql += std::string(" ORDER BY ") + sort_column(c.sort) + " " + dir + ", id " + dir;
    if (c.limit >= 0) sql += " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare id query: " + std::string(sqlite3_errmsg(db_)));
    }
    int idx = 1;
    bind_filter_values(stmt, binds, idx);
    if (c.limit >= 0) {
        sqlite3_bind_int(stmt, idx++, c.limit);
        sqlite3_bind_int64(stmt, idx++, c.offset);
    }

    std::vector<int64_t> out;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        out.push_back(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    // A mid-iteration error would otherwise return a short list as if it were
    // the whole selection — the failure mode that matters most here, because a
    // caller acts on this list.
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to read ids: " + std::string(sqlite3_errmsg(db_)));
    }
    return out;
}
```

- [x] **Step 5: Run to verify they pass, then the whole suite**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='DatabaseTest.IdsMatching*'
ctest --test-dir build --parallel
```

Expected: 3 new tests PASS, whole suite PASS.

- [x] **Step 6: Format and commit**

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i core/database.cpp core/database.h tests/test_database.cpp
git add core/database.cpp core/database.h tests/test_database.cpp
git commit -m "Add Database::ids_matching

Resolves a query to the ids it selects without materializing rows, which
is what a caller needs to name a set of images for a later operation. A
negative limit means every match rather than a page: the default
PageCriteria limit is 200, and stopping there would truncate a selection
without saying so."
```

---

### Task 3: The ABI — glob setters, `phig_query_ids`, `phig_id_list`

**Files:**
- Modify: `libphig/include/phig/phig.h` (the query section, ~line 176-195)
- Modify: `libphig/query.cpp`
- Modify: `libphig/records.cpp` (the list-free convention lives here)
- Modify: `libphig/CMakeLists.txt:17` (`VERSION 1.3.0` → `1.4.0`)
- Test: `tests/test_abi_query.cpp` (or the existing ABI query test file)

**Interfaces:**
- Consumes: `Database::ids_matching` and `SearchCriteria::matches`/`excludes`.
- Produces: `phig_query_add_match`, `phig_query_add_exclude`, `phig_query_ids`,
  `phig_id_list`, `phig_id_list_free`. Later 053 steps and plans 062-065 take
  ids produced here.

- [x] **Step 1: Find the ABI query test file**

```bash
ls tests/ | grep -i abi
```

Add the tests below to whichever file holds the existing `phig_query_*` tests;
if there is none, create `tests/test_abi_query.cpp` and add it to the
`phig-tests` target in the root `CMakeLists.txt` beside the other test sources.

- [x] **Step 2: Write the failing tests**

```cpp
TEST_F(AbiQueryTest, AddMatchFiltersByGlob) {
    // (fixture supplies ctx over a database with /a/one.jpg, /a/two.cr2,
    // /b/three.jpg — mirror whatever the neighbouring tests use)
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/a/*");

    int64_t n = 0;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 2);
    phig_query_free(q);
}

TEST_F(AbiQueryTest, QueryIdsReturnsEveryMatchingId) {
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/a/*");

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    ASSERT_NE(ids, nullptr);
    EXPECT_EQ(ids->count, 2);
    EXPECT_NE(ids->items, nullptr);
    phig_id_list_free(ids);
    phig_query_free(q);
}

TEST_F(AbiQueryTest, QueryIdsOnAnEmptyResultIsAnEmptyListNotAnError) {
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/nothing/*");

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    ASSERT_NE(ids, nullptr);
    EXPECT_EQ(ids->count, 0);
    phig_id_list_free(ids);
    phig_query_free(q);
}

TEST_F(AbiQueryTest, ExcludeWinsOverMatch) {
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/a/*");
    phig_query_add_exclude(q, "*.cr2");

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    EXPECT_EQ(ids->count, 1);
    phig_id_list_free(ids);
    phig_query_free(q);
}

TEST_F(AbiQueryTest, TheSettersIgnoreNullsLikeTheirNeighbours) {
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(nullptr, "/a/*");  // must not crash
    phig_query_add_match(q, nullptr);       // must not crash, adds nothing
    phig_query_add_exclude(q, nullptr);

    int64_t n = 0;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 3) << "a NULL pattern must not narrow the query";
    phig_query_free(q);
}

TEST_F(AbiQueryTest, QueryIdsRejectsNullArguments) {
    phig_id_list* ids = nullptr;
    EXPECT_EQ(phig_query_ids(nullptr, nullptr, &ids), PHIG_ERR_INVALID_ARG);
    phig_id_list_free(nullptr);  // must not crash
}
```

- [x] **Step 3: Run to verify they fail**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='AbiQueryTest.*'
```

Expected: compile errors — the four new names are undeclared.

- [x] **Step 4: Declare the surface**

In `libphig/include/phig/phig.h`, after `phig_query_set_sort`:

```c
/* Glob selection over the full path, with phig's glob semantics: `*` and `?`
 * are wildcards, a backslash escapes the next character, and brackets are
 * literal. NOT SQLite's GLOB. Repeatable: an image passes when it matches at
 * least one match pattern (or when none was added) and no exclude pattern.
 * An exclude wins where the two overlap. NULL or "" adds nothing. */
PHIG_API void phig_query_add_match(phig_query* q, const char* glob);
PHIG_API void phig_query_add_exclude(phig_query* q, const char* glob);

/* The ids a query selects, in the query's sort order and ignoring pagination —
 * this resolves a selection, it does not page one. Use it to name a set of
 * images for an operation that takes ids.
 *
 * *out receives an engine-allocated list the caller must release with
 * phig_id_list_free; an empty selection yields a list with count 0, not an
 * error. */
typedef struct {
    const int64_t* items;
    int64_t count;
} phig_id_list;

PHIG_API phig_status phig_query_ids(phig_ctx* ctx, const phig_query* q, phig_id_list** out);
PHIG_API void phig_id_list_free(phig_id_list* list);
```

Then bump `PHIG_ABI_VERSION_MINOR` from `3u` to `4u`.

- [x] **Step 5: Implement the setters**

In `libphig/query.cpp`, after `phig_query_set_sort`. Both allocate, so both get
a handler — R62 names exactly this shape as the hole, and this plan must not
widen it:

```c
void phig_query_add_match(phig_query* q, const char* glob) {
    if (q == nullptr || glob == nullptr || *glob == '\0') return;
    try {
        q->criteria.filter.matches.push_back(glob);
    } catch (...) {
        // Allocation failure. Nothing to report through a void return, but an
        // exception must not cross the boundary (R62).
    }
}

void phig_query_add_exclude(phig_query* q, const char* glob) {
    if (q == nullptr || glob == nullptr || *glob == '\0') return;
    try {
        q->criteria.filter.excludes.push_back(glob);
    } catch (...) {
    }
}
```

- [x] **Step 6: Implement `phig_query_ids` and its free**

In `libphig/records.cpp`, beside `phig_image_list_free`, following the same
header-first storage trick so the pointer round-trips:

```cpp
namespace {
struct IdListStorage {
    phig_id_list header;  // must be first
    std::vector<int64_t> items;
};
}  // namespace

phig_id_list* make_id_list(std::vector<int64_t> ids) {
    auto* st = new IdListStorage();
    st->items = std::move(ids);
    st->header.items = st->items.data();
    st->header.count = static_cast<int64_t>(st->items.size());
    return &st->header;
}

extern "C" void phig_id_list_free(phig_id_list* list) {
    if (list == nullptr) return;
    delete reinterpret_cast<IdListStorage*>(list);
}
```

Declare `make_id_list` in `libphig/internal.h` beside the other helpers, then in
`libphig/query.cpp`:

`PHIG_TRY(ctx)` expands to an `if`, a clear, and an opening `try {`, and
`PHIG_CATCH(ctx)` supplies the closing brace and every handler — so neither
takes braces of its own at the call site, and `PHIG_CATCH` takes the context.
Copy the shape of `phig_query_count` (`libphig/query.cpp:103-109`):

```c
phig_status phig_query_ids(phig_ctx* ctx, const phig_query* q, phig_id_list** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (q == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
    Database::PageCriteria c = q->criteria;
    c.limit = -1;  // resolve the whole selection, not a page
    c.offset = 0;
    *out = make_id_list(ctx->db->ids_matching(c));
    return PHIG_OK;
    PHIG_CATCH(ctx)
}
```

- [x] **Step 7: Bump the CMake version**

`libphig/CMakeLists.txt:17`: `VERSION 1.3.0` → `VERSION 1.4.0`. Leave
`SOVERSION 1` — this is additive.

- [x] **Step 8: Run the tests, then the whole suite**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='AbiQueryTest.*'
ctest --test-dir build --parallel
```

Expected: the six new tests PASS, whole suite PASS.

- [x] **Step 9: Format and commit**

`phig.h` is below CI's `-maxdepth 1` glob, so format it explicitly.

```bash
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF -i libphig/include/phig/phig.h libphig/query.cpp libphig/records.cpp libphig/internal.h tests/test_abi_query.cpp
git add libphig/ tests/ CMakeLists.txt
git commit -m "ABI: glob predicates on a query, and resolve one to ids

phig_query had no way to express a filename search, which is why the ABI
had no equivalent of --match. Adds phig_query_add_match and
phig_query_add_exclude with phig's glob semantics, and phig_query_ids to
resolve any query — not just a glob — to the ids it selects.

Ids are how every later surface will name a set of images, so resolving
ignores pagination: a selection is not a page.

Both setters allocate, which is the shape R62 names as the unwrapped hole,
so both carry a handler. ABI minor 3 -> 4, soname unchanged."
---

### Task 4: Measure the scan, and update what the docs claim

The plan asserted that a glob filter is a full scan and that this is acceptable.
That is reasoning, not measurement, and root `CLAUDE.md` is explicit that
performance claims are measured. This task settles it and corrects the two
documents that currently say glob selection is missing.

**Files:**
- Modify: `libphig/CLAUDE.md` (the "What is missing" table)
- Modify: `plans/053-libphig-feature-parity.md` (work order step 2)
- Test: none — this is measurement and documentation

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: nothing later tasks depend on.

- [x] **Step 1: Measure a glob query at collection scale**

The real collection database is empty, so build a synthetic one with the binary
(so the schema is whatever phig actually creates, not a hand-written copy that
could drift), then insert rows directly:

```bash
DB=/tmp/globbench.db
rm -f "$DB"*
./build/phig search --count --db "$DB" >/dev/null 2>&1   # creates + migrates to v9
python3 - "$DB" <<'PY'
import sqlite3, sys, time
db = sqlite3.connect(sys.argv[1])
rows = [(f"/photos/{i//1000:04d}/IMG_{i:07d}.jpg", f"IMG_{i:07d}.jpg", "jpg",
         1000, f"sha{i:064x}", i, "image") for i in range(200000)]
db.executemany(
    "INSERT INTO images (path, filename, extension, file_size, sha256, phash, media_type)"
    " VALUES (?,?,?,?,?,?,?)", rows)
db.commit()
PY
```

Then time both shapes with the built binary:

```bash
time ./build/phig search --count --db /tmp/globbench.db
time ./build/phig search --count --match '/photos/0042/*' --db /tmp/globbench.db
time ./build/phig search --count --match '*IMG_0123456*' --db /tmp/globbench.db
```

Record the three numbers. What matters is the ratio between the unfiltered
count and the glob-filtered ones at 200k rows, and whether the absolute time is
tolerable for a GUI that calls `phig_query_count` to size a scrollbar.

- [x] **Step 2: Write the measurement into plan 053**

Add the numbers to decision 2 in `plans/053-libphig-feature-parity.md`, under
the paragraph about globs being evaluated in SQL. State the figures and the row
count. If the glob query is materially slower than the plain count, say so and
name it as 044's problem rather than hiding it — 044 already owns query cost at
scale, and an honest slow number is more useful than an omitted one.

- [x] **Step 3: Correct `libphig/CLAUDE.md`**

Its "What is missing" table has a row reading:

```
| Glob and path selection on queries | `SearchQuery::matches`/`excludes` | 053 |
```

Remove that row. Then correct the paragraph below the table, which currently
says query selection "is an inconsistency inside the ABI rather than only a
shortfall: `phig_thumbs_options` and `phig_faces_options` take `match`/`exclude`
while `phig_query` does not, so a client can restrict a thumbnail run by glob
but cannot search for a filename." That is now half wrong — `phig_query` takes
globs. Replace it with the current state: queries take globs, the operations
still take them too, and 053's step 3 is what moves the operations onto ids.

Do not claim the selection model is finished. It is not until step 3 lands.

- [x] **Step 4: Mark the work-order step done**

In `plans/053-libphig-feature-parity.md`, work order step 2:

```markdown
2. `phig_query_ids` and the glob predicates, on the SQLite function — done,
   executed via `docs/plans/2026-08-14-053-query-selection.md`.
```

- [x] **Step 5: Run the full gate and commit**

```bash
ctest --test-dir build --parallel
python3 scripts/rules.py --check --emit
python3 -m unittest discover -s scripts -p 'test_*.py'
git add libphig/CLAUDE.md plans/053-libphig-feature-parity.md docs/plans/2026-08-14-053-query-selection.md rules/
git commit -m "Record the glob scan cost, and correct what the docs claim

libphig/CLAUDE.md listed glob selection on queries as missing and
described the ABI's inconsistency in terms that no longer hold. Queries
take globs now; the operations still take them, which step 3 changes."
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
CF=/Users/brett/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
$CF --dry-run --Werror core/database.cpp core/database.h tests/test_database.cpp \
    libphig/include/phig/phig.h libphig/query.cpp libphig/records.cpp
cmake -B build-werror -DPHIG_WERROR=ON && cmake --build build-werror -j && rm -rf build-werror
```

CI additionally checks that `PHIG_ABI_VERSION_MAJOR`/`MINOR` agree with the
CMake `VERSION`/`SOVERSION`. After Task 3 those are 1/4 and `1.4.0`/`1`.

What this does **not** verify, and should be said rather than assumed:

- **No rule covers the new surface.** Glob selection on a query is a read, so it
  destroys nothing and needs no record on its own. But R78 — a metadata write
  must name its subject — was written expecting globs to arrive on the writes.
  They do not, per 053's decision 2, and 053's scope item 8 already records why.
  Nothing here changes R78's status; step 5 does.
- **Sanitizers and macOS do not run on a PR.** The registered SQL function is
  called once per row per pattern and takes pointers from SQLite; that is
  exactly the shape ASan would have an opinion about. Worth
  `gh workflow run CI --ref <branch>` before merging rather than after.
- **Nothing measures the ABI path end to end at scale.** Task 4 times the CLI,
  which reaches the same `build_filter_sql`. `phig_query_ids` over a large
  selection — hundreds of thousands of ids marshalled into one allocation — is
  untested for cost.
