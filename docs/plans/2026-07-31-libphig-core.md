# libphig Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `libphig`, a C ABI shared library exposing phig's query, metadata, thumbnail-lookup, and small-mutation surface, so native clients (phig-gtk, later phig-web) can read a phig collection without touching its SQLite schema.

**Architecture:** A new `src/libphig/` layer wrapping the existing `Database` class. Paginated queries are pushed down into SQL against a new indexed `effective_date` column. The ABI is C-only at the boundary (opaque handles, flat structs, explicit free functions, no exceptions escaping) with C++ internals. No GUI code and no decode work — this plan is entirely within phig's repo.

**Tech Stack:** C++17, SQLite3, CMake, GoogleTest. No new third-party dependencies.

**Spec:** `../../../phig-gtk/docs/superpowers/specs/2026-07-31-phig-gtk-design.md` (sections "libphig C ABI" and "Required phig-side changes")

## Global Constraints

- **C++17**, matching phig's existing `CMakeLists.txt`. No newer language features.
- **No new third-party dependencies.** SQLite3 and GoogleTest are already present.
- **The public header `include/phig/phig.h` must be valid C**, compilable by a C99 compiler. No C++ types, no `bool` from `<stdbool.h>` omitted, no default arguments, no namespaces, no exceptions.
- **No exception may cross the ABI boundary.** Every public function wraps its body in `try/catch(...)` and converts to a `phig_status`.
- **Existing CLI behavior must not change.** All existing tests in `phig-tests` keep passing; the schema migration is additive.
- **Naming:** all public symbols prefixed `phig_`; all public types `phig_*`; enum values `PHIG_*`.
- **Formatting:** `.clang-format` at the repo root is authoritative; run `clang-format -i` on touched files before each commit.
- **Tests use GoogleTest** and are registered in `phig-tests` via `CMakeLists.txt`.
- **Dates are ISO-8601 TEXT** (`YYYY-MM-DD` or `YYYY:MM:DD HH:MM:SS`) everywhere, never epoch integers.
- **Build/test commands must be prefixed with `direnv exec .`** — phig's dependencies (OpenCV, LibRaw, spdlog, the GCC 15 toolchain) come from a Nix devenv shell and are NOT on the bare PATH. `pkg-config --exists opencv4` fails without it, so a bare `cmake` cannot configure. Every command in this plan:
  - Configure/build: `direnv exec . cmake -B build && direnv exec . cmake --build build`
  - Test: `direnv exec . ./build/phig-tests`
  - Format: `direnv exec . clang-format -i <files>`
  The existing `build/` directory is already configured with the Nix toolchain; reuse it rather than deleting it.
- **Two libraries, similar names — do not confuse them:**
  - `phig-lib` → `libphig-internal.a` — the pre-existing INTERNAL static library (all of `src/*.cpp`, compiled once and shared by the `phig` binary and `phig-tests`). Private to the build.
  - `phig_abi` → `libphig.so` — the NEW public C ABI shared library this plan creates. This is "libphig" as clients know it.

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `include/phig/phig.h` | The entire public C ABI. Installed header. Valid C99. |
| `src/libphig/context.cpp` | `phig_ctx` lifecycle: init, shutdown, error string storage, ABI version. |
| `src/libphig/query.cpp` | `phig_query` builder, `phig_query_count`, `phig_query_page`. |
| `src/libphig/records.cpp` | Converting `ImageInfo` → `phig_image_list` and owning its string storage. |
| `src/libphig/mutate.cpp` | Tag/gallery/favorite writes by image id. |
| `src/libphig/thumbs.cpp` | `phig_thumb_lookup`. |
| `src/libphig/internal.h` | Shared internal declarations (struct definitions, the `PHIG_TRY` guard). |
| `tests/test_libphig_query.cpp` | Pagination, sorting, filter combinations. |
| `tests/test_libphig_mutate.cpp` | Tag/gallery/favorite writes and read-only enforcement. |
| `tests/test_libphig_abi.cpp` | Lifecycle, error strings, version, null-argument handling. |
| `tests/test_libphig_c_compile.c` | Compiles the header as C99 to prove it is C-clean. |

**Modified files:**

| Path | Change |
|---|---|
| `src/database.h` | Add `effective_date` to `ImageInfo`; add `PageCriteria`/`sort` and `search_page`/`count_matching`; add id-based tag/gallery setters. |
| `src/database.cpp` | Schema v-next migration (add column + index + backfill); populate `effective_date` on insert; implement the new methods. |
| `src/scanner.cpp` | Compute `effective_date` when building `ImageInfo`. |
| `CMakeLists.txt` | `libphig` shared-library target, install rules, new test files. |

## Interface summary

Types and functions this plan produces, in dependency order. Later tasks depend on exactly these names.

```c
/* status */
typedef enum { PHIG_OK = 0, PHIG_ERR_INVALID_ARG, PHIG_ERR_NOT_FOUND,
               PHIG_ERR_DB, PHIG_ERR_READONLY, PHIG_ERR_INTERNAL } phig_status;
typedef enum { PHIG_READ_ONLY = 0, PHIG_READ_WRITE = 1 } phig_access_mode;
typedef enum { PHIG_MEDIA_ANY = 0, PHIG_MEDIA_IMAGE, PHIG_MEDIA_VIDEO } phig_media_type;
typedef enum { PHIG_SORT_DATE = 0, PHIG_SORT_PATH, PHIG_SORT_SIZE,
               PHIG_SORT_FILENAME } phig_sort_key;
```

---

### Task 1: Schema migration — indexed `effective_date`

Adds the column the gallery sorts by. Everything else depends on this.

**Files:**
- Modify: `src/database.h` (add `effective_date` to `ImageInfo` — note `ImageInfo` lives in `src/types.h`)
- Modify: `src/types.h`
- Modify: `src/database.cpp` (migration, insert)
- Modify: `src/scanner.cpp`
- Test: `tests/test_libphig_query.cpp` (new file; migration tests land here)

**Interfaces:**
- Consumes: nothing.
- Produces: `ImageInfo::effective_date` (`std::string`, ISO-8601, may be empty); an `images.effective_date` TEXT column; index `idx_effective_date` on `(effective_date DESC, id DESC)`; free function `std::string compute_effective_date(const std::string& metadata_json, const std::string& modified_at)` declared in `src/database.h`.

- [ ] **Step 1: Read the current migration code to find the schema version**

Open `src/database.cpp` and locate `init_schema()` and the `meta` table's `schema_version` handling (around lines 150–350). Note the highest existing version number `N`. Your migration is version `N+1`. Do not renumber existing migrations.

- [ ] **Step 2: Write the failing test for `compute_effective_date`**

Add to `tests/test_libphig_query.cpp`:

```cpp
#include <gtest/gtest.h>

#include "database.h"

TEST(EffectiveDate, PrefersDateTimeOriginal) {
    std::string meta = R"({"DateTimeOriginal":"2021:05:04 10:00:00",
                           "DateTimeDigitized":"2022:01:01 00:00:00"})";
    EXPECT_EQ(compute_effective_date(meta, "2023-09-09 09:09:09"),
              "2021:05:04 10:00:00");
}

TEST(EffectiveDate, FallsBackToDigitized) {
    std::string meta = R"({"DateTimeDigitized":"2022:01:01 00:00:00"})";
    EXPECT_EQ(compute_effective_date(meta, "2023-09-09 09:09:09"),
              "2022:01:01 00:00:00");
}

TEST(EffectiveDate, FallsBackToModifiedAt) {
    EXPECT_EQ(compute_effective_date("", "2023-09-09 09:09:09"),
              "2023-09-09 09:09:09");
}

TEST(EffectiveDate, EmptyWhenNothingAvailable) {
    EXPECT_EQ(compute_effective_date("", ""), "");
}
```

- [ ] **Step 3: Register the new test file and run it to verify it fails**

Add `tests/test_libphig_query.cpp` to the `add_executable(phig-tests ...)` list in `CMakeLists.txt`, then:

Run: `cmake -B build && cmake --build build 2>&1 | tail -20`
Expected: FAIL — compile error, `compute_effective_date` was not declared.

- [ ] **Step 4: Implement `compute_effective_date`**

Declare in `src/database.h`, above `class Database`:

```cpp
// phig's "best available date" for an image: EXIF DateTimeOriginal, then
// DateTimeDigitized, then the file's modified_at. Returns "" if none apply.
// ISO-8601 TEXT; EXIF form uses ':' date separators and sorts lexicographically
// alongside modified_at's '-' form only within its own family, so callers must
// not mix families when comparing. Stored in images.effective_date.
std::string compute_effective_date(const std::string& metadata_json,
                                   const std::string& modified_at);
```

Implement in `src/database.cpp`. There is an existing `json_get` helper used by `main.cpp` for the same purpose — reuse it if it is accessible, otherwise use the same `json_extract` semantics:

```cpp
std::string compute_effective_date(const std::string& metadata_json,
                                   const std::string& modified_at) {
    if (!metadata_json.empty()) {
        std::string d = json_get(metadata_json, "DateTimeOriginal");
        if (!d.empty()) return d;
        d = json_get(metadata_json, "DateTimeDigitized");
        if (!d.empty()) return d;
    }
    return modified_at;
}
```

If `json_get` is `static` in `main.cpp`, move it into `src/database.cpp` (or a shared spot) and declare it in the header rather than duplicating it. Note the mixed-separator caveat in the comment above — it is why Step 6 normalizes on write.

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='EffectiveDate.*'`
Expected: 4 tests PASS.

- [ ] **Step 6: Write the failing test for the migration and backfill**

The EXIF `YYYY:MM:DD` and mtime `YYYY-MM-DD` forms do not sort against each other correctly (`:` is 0x3A, `-` is 0x2D). Normalize to `-` separators in the date portion on write so all values live in one sortable family. Extend `compute_effective_date` to do that normalization, and add:

```cpp
TEST(EffectiveDate, NormalizesExifSeparators) {
    std::string meta = R"({"DateTimeOriginal":"2021:05:04 10:00:00"})";
    EXPECT_EQ(compute_effective_date(meta, ""), "2021-05-04 10:00:00");
}

TEST(EffectiveDate, SortsAcrossExifAndMtimeForms) {
    std::string exif = R"({"DateTimeOriginal":"2021:05:04 10:00:00"})";
    std::string a = compute_effective_date(exif, "");
    std::string b = compute_effective_date("", "2022-01-01 00:00:00");
    EXPECT_LT(a, b);
}
```

Then add a migration test that opens a database at the previous schema, inserts a row directly, migrates, and asserts the backfill ran:

```cpp
#include <cstdio>
#include <filesystem>
#include <string>

namespace {
// Unique temp DB path per test; removed by TempDb's destructor.
struct TempDb {
    std::string path;
    TempDb() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
    }
    ~TempDb() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
};
}  // namespace

TEST(Migration, BackfillsEffectiveDateForExistingRows) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        ImageInfo info;
        info.path = "/photos/a.jpg";
        info.filename = "a.jpg";
        info.extension = "jpg";
        info.file_size = 100;
        info.modified_at = "2020-03-03 03:03:03";
        info.sha256 = std::string(64, 'a');
        info.metadata_json = R"({"DateTimeOriginal":"2019:07:07 07:07:07"})";
        db.insert(info);
    }
    {
        Database db(tmp.path);
        db.init_schema();  // idempotent; must not lose data
        auto rows = db.get_all_images();
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].effective_date, "2019-07-07 07:07:07");
    }
}
```

- [ ] **Step 7: Run it to verify it fails**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='Migration.*:EffectiveDate.*'`
Expected: FAIL — `ImageInfo` has no member `effective_date`.

- [ ] **Step 8: Add the field, the migration, and the insert/read wiring**

In `src/types.h`, add to `ImageInfo`:

```cpp
    std::string effective_date;  // best available date; see compute_effective_date
```

In `src/database.cpp`, add migration version `N+1` following the existing migration style (inside `BEGIN IMMEDIATE`, with the in-transaction version recheck the existing code uses):

```cpp
exec("ALTER TABLE images ADD COLUMN effective_date TEXT");
exec(R"(UPDATE images SET effective_date = COALESCE(
            json_extract(metadata, '$.DateTimeOriginal'),
            json_extract(metadata, '$.DateTimeDigitized'),
            modified_at))");
// Normalize EXIF ':' date separators to '-' so all values sort in one family.
exec(R"(UPDATE images SET effective_date =
            substr(effective_date,1,4) || '-' || substr(effective_date,6,2) || '-' ||
            substr(effective_date,9)
        WHERE effective_date LIKE '____:__:__%')");
exec("CREATE INDEX IF NOT EXISTS idx_effective_date "
     "ON images(effective_date DESC, id DESC)");
```

Also add the column and index to the fresh-schema `CREATE TABLE images` path so new databases match migrated ones.

Then update `insert`/`insert_batch` to write `effective_date` (computing it via `compute_effective_date` when `info.effective_date` is empty), and update every `SELECT` that builds an `ImageInfo` to read the column. Grep for the column list `"SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash"` to find them all.

- [ ] **Step 9: Run the full suite**

Run: `cmake --build build && ./build/phig-tests`
Expected: all tests PASS, including the pre-existing ones. If `test_database.cpp` or `test_scanner.cpp` fail, the migration or a `SELECT` column list is wrong — fix before continuing.

- [ ] **Step 10: Populate `effective_date` at scan time**

In `src/scanner.cpp`, where `ImageInfo` is populated with `metadata_json` and `modified_at` (grep for `metadata_json =`), set:

```cpp
info.effective_date = compute_effective_date(info.metadata_json, info.modified_at);
```

Run: `cmake --build build && ./build/phig-tests`
Expected: PASS.

- [ ] **Step 11: Commit**

```bash
clang-format -i src/types.h src/database.h src/database.cpp src/scanner.cpp \
    tests/test_libphig_query.cpp
git add src/types.h src/database.h src/database.cpp src/scanner.cpp \
    tests/test_libphig_query.cpp CMakeLists.txt
git commit -m "Add indexed effective_date column for date-sorted pagination"
```

---

### Task 2: Paginated query in `Database`

The SQL-level pagination the ABI wraps. Kept in `Database` so the CLI can use it too.

**Files:**
- Modify: `src/database.h`, `src/database.cpp`
- Test: `tests/test_libphig_query.cpp`

**Interfaces:**
- Consumes: `ImageInfo::effective_date`, `idx_effective_date` (Task 1).
- Produces:

```cpp
// in class Database, public:
enum class SortKey { Date, Path, Size, Filename };
struct PageCriteria {
    SearchCriteria filter;         // reuses the existing filter struct
    SortKey sort = SortKey::Date;
    bool descending = true;
    int64_t offset = 0;
    int32_t limit = 200;
};
std::vector<ImageInfo> search_page(const PageCriteria& c);
int64_t count_matching(const SearchCriteria& filter);
```

- [ ] **Step 1: Write the failing pagination tests**

Add to `tests/test_libphig_query.cpp`. Reuse the `TempDb` helper from Task 1.

```cpp
namespace {
// Insert n images with predictable, distinct effective dates (ascending).
void seed(Database& db, int n) {
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "2020-01-%02d 00:00:00", i + 1);
        ImageInfo info;
        info.path = "/photos/img" + std::to_string(i) + ".jpg";
        info.filename = "img" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.file_size = 1000 + i;
        info.modified_at = buf;
        info.effective_date = buf;
        info.sha256 = std::string(63, '0') + std::to_string(i % 10);
        db.insert(info);
    }
}
}  // namespace

TEST(QueryPage, ReturnsRequestedPageSize) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 25);
    Database::PageCriteria c;
    c.limit = 10;
    EXPECT_EQ(db.search_page(c).size(), 10u);
}

TEST(QueryPage, OffsetWalksWithoutOverlapOrGaps) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 25);
    Database::PageCriteria c;
    c.limit = 10;
    std::vector<std::string> seen;
    for (int64_t off = 0; off < 30; off += 10) {
        c.offset = off;
        for (const auto& r : db.search_page(c)) seen.push_back(r.path);
    }
    EXPECT_EQ(seen.size(), 25u);
    std::set<std::string> uniq(seen.begin(), seen.end());
    EXPECT_EQ(uniq.size(), 25u);  // no duplicates across pages
}

TEST(QueryPage, DateDescendingIsNewestFirst) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 5);
    Database::PageCriteria c;
    c.sort = Database::SortKey::Date;
    c.descending = true;
    auto rows = db.search_page(c);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows.front().effective_date, "2020-01-05 00:00:00");
    EXPECT_EQ(rows.back().effective_date, "2020-01-01 00:00:00");
}

TEST(QueryPage, StableUnderEqualDates) {
    TempDb tmp; Database db(tmp.path); db.init_schema();
    for (int i = 0; i < 10; ++i) {
        ImageInfo info;
        info.path = "/photos/same" + std::to_string(i) + ".jpg";
        info.filename = "same" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.modified_at = "2020-06-06 06:06:06";
        info.effective_date = "2020-06-06 06:06:06";  // all identical
        info.sha256 = std::string(64, 'b');
        db.insert(info);
    }
    Database::PageCriteria c;
    c.limit = 5;
    auto p1 = db.search_page(c);
    c.offset = 5;
    auto p2 = db.search_page(c);
    std::set<std::string> uniq;
    for (const auto& r : p1) uniq.insert(r.path);
    for (const auto& r : p2) uniq.insert(r.path);
    EXPECT_EQ(uniq.size(), 10u);  // id tiebreaker prevents page shuffling
}

TEST(QueryPage, OffsetPastEndReturnsEmpty) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 5);
    Database::PageCriteria c;
    c.offset = 100;
    EXPECT_TRUE(db.search_page(c).empty());
}

TEST(QueryPage, RowsWithNoDateSortLastDescending) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 3);
    ImageInfo undated;
    undated.path = "/photos/undated.jpg";
    undated.filename = "undated.jpg";
    undated.extension = "jpg";
    undated.sha256 = std::string(64, 'c');
    db.insert(undated);  // no modified_at, no metadata
    Database::PageCriteria c;
    c.descending = true;
    auto rows = db.search_page(c);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows.back().path, "/photos/undated.jpg");
}

TEST(CountMatching, CountsWithoutFetching) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 25);
    EXPECT_EQ(db.count_matching(Database::SearchCriteria{}), 25);
}

TEST(CountMatching, RespectsFilter) {
    TempDb tmp; Database db(tmp.path); db.init_schema(); seed(db, 25);
    Database::SearchCriteria f;
    f.extension = "png";
    EXPECT_EQ(db.count_matching(f), 0);
    f.extension = "jpg";
    EXPECT_EQ(db.count_matching(f), 25);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -20`
Expected: FAIL — no member `search_page` / `PageCriteria` in `Database`.

- [ ] **Step 3: Extract the filter-to-SQL builder**

`Database::search()` (`src/database.cpp:1656`) builds its `WHERE` clause inline and binds via a `std::vector<std::pair<std::string,bool>>`. Extract that into a reusable private helper so `search`, `search_page`, and `count_matching` share one implementation — do not copy the clause-building logic three times.

Add to `src/database.h` under `private:`:

```cpp
    // Builds the WHERE clause (without the "WHERE" keyword; always starts
    // " AND ...") and appends the values to bind, in order.
    // {value, is_int} matches the existing convention in search().
    void build_filter_sql(const SearchCriteria& c, std::string& sql,
                          std::vector<std::pair<std::string, bool>>& binds);

    // Binds the values from build_filter_sql in order, advancing idx past the
    // last one so the caller can bind its own trailing params (LIMIT/OFFSET).
    void bind_filter_values(sqlite3_stmt* stmt,
                            const std::vector<std::pair<std::string, bool>>& binds,
                            int& idx);
```

Implement it once and call it from `search`, `search_page`, and
`count_matching` — the bind loop is not to be copied per call site:

```cpp
void Database::bind_filter_values(sqlite3_stmt* stmt,
                                  const std::vector<std::pair<std::string, bool>>& binds,
                                  int& idx) {
    for (const auto& b : binds) {
        if (b.second) {
            sqlite3_bind_int64(stmt, idx++, std::stoll(b.first));
        } else {
            sqlite3_bind_text(stmt, idx++, b.first.c_str(), -1, SQLITE_TRANSIENT);
        }
    }
}
```

Move the body of the existing clause-building code from `search()` into it verbatim, then have `search()` call it. Build and run the full suite to confirm the refactor changed nothing:

Run: `cmake --build build && ./build/phig-tests`
Expected: all pre-existing tests PASS.

- [ ] **Step 4: Implement `search_page` and `count_matching`**

In `src/database.cpp`:

```cpp
namespace {
const char* sort_column(Database::SortKey k) {
    switch (k) {
        case Database::SortKey::Path:     return "path";
        case Database::SortKey::Size:     return "file_size";
        case Database::SortKey::Filename: return "filename";
        case Database::SortKey::Date:
        default:                          return "effective_date";
    }
}
}  // namespace

std::vector<ImageInfo> Database::search_page(const PageCriteria& c) {
    std::string sql =
        "SELECT id, path, filename, extension, file_size, created_at, modified_at, "
        "effective_date, sha256, phash, width, height, metadata, media_type, "
        "duration_ms, gallery, blur_score FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;
    build_filter_sql(c.filter, sql, binds);

    const char* dir = c.descending ? "DESC" : "ASC";
    // id is the tiebreaker so equal sort keys never shuffle between pages.
    sql += std::string(" ORDER BY ") + sort_column(c.sort) + " " + dir +
           ", id " + dir + " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare page query: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    int idx = 1;
    bind_filter_values(stmt, binds, idx);
    sqlite3_bind_int(stmt, idx++, c.limit);
    sqlite3_bind_int64(stmt, idx++, c.offset);

    std::vector<ImageInfo> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_image_info(stmt));  // see Step 5
    }
    sqlite3_finalize(stmt);
    return out;
}

int64_t Database::count_matching(const SearchCriteria& filter) {
    std::string sql = "SELECT COUNT(*) FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds;
    build_filter_sql(filter, sql, binds);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare count: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    int idx = 1;
    bind_filter_values(stmt, binds, idx);
    int64_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}
```

Note: `search_page` selects `id`, which the existing `search()` does not. The ABI needs it for id-based mutation.

- [ ] **Step 5: Add the row-to-struct helper**

Several methods now read the same column list. Add one private helper in `src/database.cpp` and use it from `search_page`:

```cpp
// Column order must match the SELECT list in search_page().
ImageInfo Database::row_to_image_info(sqlite3_stmt* stmt) {
    auto text = [&](int col) -> std::string {
        const unsigned char* p = sqlite3_column_text(stmt, col);
        return p ? reinterpret_cast<const char*>(p) : "";
    };
    ImageInfo info;
    info.id             = sqlite3_column_int64(stmt, 0);
    info.path           = text(1);
    info.filename       = text(2);
    info.extension      = text(3);
    info.file_size      = sqlite3_column_int64(stmt, 4);
    info.created_at     = text(5);
    info.modified_at    = text(6);
    info.effective_date = text(7);
    info.sha256         = text(8);
    info.phash          = static_cast<uint64_t>(sqlite3_column_int64(stmt, 9));
    info.width          = sqlite3_column_int(stmt, 10);
    info.height         = sqlite3_column_int(stmt, 11);
    info.metadata_json  = text(12);
    info.media_type     = text(13);
    info.duration_ms    = sqlite3_column_int64(stmt, 14);
    info.gallery        = text(15);
    info.blur_score     = sqlite3_column_type(stmt, 16) == SQLITE_NULL
                              ? -1.0 : sqlite3_column_double(stmt, 16);
    return info;
}
```

Declare it `private` in `src/database.h`. This requires adding `int64_t id = 0;` to `ImageInfo` in `src/types.h` — the struct currently has no id field, and the ABI needs it.

- [ ] **Step 6: Run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='QueryPage.*:CountMatching.*'`
Expected: 8 tests PASS. If `RowsWithNoDateSortLastDescending` fails, note that SQLite sorts `NULL` **first** under `DESC`; store `''` rather than `NULL` for absent dates, or add `effective_date IS NULL` to the `ORDER BY`. Pick one and make the test pass.

- [ ] **Step 7: Run the full suite**

Run: `./build/phig-tests`
Expected: all PASS.

- [ ] **Step 8: Commit**

```bash
clang-format -i src/types.h src/database.h src/database.cpp tests/test_libphig_query.cpp
git add src/types.h src/database.h src/database.cpp tests/test_libphig_query.cpp
git commit -m "Add paginated search_page and count_matching to Database"
```

---

### Task 3: The public header and context lifecycle

**Files:**
- Create: `include/phig/phig.h`, `src/libphig/internal.h`, `src/libphig/context.cpp`
- Create: `tests/test_libphig_abi.cpp`, `tests/test_libphig_c_compile.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Database` (Tasks 1–2).
- Produces: `phig_ctx`, `phig_config`, `phig_status`, `phig_access_mode`, `phig_init`, `phig_shutdown`, `phig_last_error`, `phig_abi_version`, and the `PHIG_TRY`/`PHIG_CATCH` guard macros in `internal.h`.

- [ ] **Step 1: Write the header**

Create `include/phig/phig.h`. This is the whole ABI surface for this task; later tasks append to it.

```c
#ifndef PHIG_PHIG_H
#define PHIG_PHIG_H

/* libphig — C ABI for phig image collections.
 *
 * Clients must not read phig's SQLite schema or thumbnail cache layout
 * directly; this header is the contract.
 *
 * Thread safety: a phig_ctx is NOT thread-safe. Use one ctx per thread, or
 * serialize access externally.
 *
 * All dates are ISO-8601 TEXT ("YYYY-MM-DD HH:MM:SS"), never epoch integers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHIG_ABI_VERSION_MAJOR 1u
#define PHIG_ABI_VERSION_MINOR 0u

typedef enum {
    PHIG_OK = 0,
    PHIG_ERR_INVALID_ARG = 1,
    PHIG_ERR_NOT_FOUND = 2,
    PHIG_ERR_DB = 3,
    PHIG_ERR_READONLY = 4,
    PHIG_ERR_INTERNAL = 5
} phig_status;

typedef enum { PHIG_READ_ONLY = 0, PHIG_READ_WRITE = 1 } phig_access_mode;

typedef struct phig_ctx phig_ctx;

typedef struct {
    const char *db_path;     /* required */
    const char *thumbs_dir;  /* NULL = phig's XDG default */
    phig_access_mode mode;
} phig_config;

/* Returns (major << 16) | minor. Clients should compare the major half
 * against PHIG_ABI_VERSION_MAJOR at startup and refuse to run on mismatch. */
uint32_t phig_abi_version(void);

/* On success *out receives a context the caller must pass to phig_shutdown.
 * On failure *out is set to NULL and the return value says why; there is no
 * context to query for a message in that case. */
phig_status phig_init(const phig_config *cfg, phig_ctx **out);

/* Safe to call with NULL. */
void phig_shutdown(phig_ctx *ctx);

/* Message for the most recent failed call on this ctx. Never NULL; returns ""
 * when no error has occurred. Valid until the next call on the same ctx. */
const char *phig_last_error(phig_ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif /* PHIG_PHIG_H */
```

- [ ] **Step 2: Write the C-compilation test**

This guards the "header must be valid C" constraint. Create `tests/test_libphig_c_compile.c`:

```c
/* Compiles the public header as C99 to prove it contains no C++ constructs.
 * Also links every symbol so a missing definition fails the build. */
#include "phig/phig.h"

void phig_c_compile_check(void) {
    phig_config cfg;
    phig_ctx *ctx = NULL;
    cfg.db_path = "/nonexistent/phig.db";
    cfg.thumbs_dir = NULL;
    cfg.mode = PHIG_READ_ONLY;
    (void)phig_abi_version();
    (void)phig_init(&cfg, &ctx);
    (void)phig_last_error(ctx);
    phig_shutdown(ctx);
}
```

- [ ] **Step 3: Write the failing lifecycle tests**

Create `tests/test_libphig_abi.cpp`. Reuse a local `TempDb` (copy the helper — it is test-local and duplicating it across test files is normal in this suite).

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "phig/phig.h"

namespace {
struct TempDbPath {
    std::string path;
    TempDbPath() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_abi_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
    }
    ~TempDbPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
};
// Creates a real phig database at `path` via the C++ API.
void make_db(const std::string& path);  // defined in test_libphig_query.cpp
}  // namespace

TEST(Abi, VersionMajorMatchesHeader) {
    EXPECT_EQ(phig_abi_version() >> 16, PHIG_ABI_VERSION_MAJOR);
}

TEST(Abi, InitRejectsNullConfig) {
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(nullptr, &ctx), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitRejectsNullOut) {
    phig_config cfg{};
    cfg.db_path = "/tmp/whatever.db";
    EXPECT_EQ(phig_init(&cfg, nullptr), PHIG_ERR_INVALID_ARG);
}

TEST(Abi, InitRejectsNullDbPath) {
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.db_path = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitFailsOnUnopenableDatabase) {
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.db_path = "/nonexistent-dir-xyz/phig.db";
    cfg.mode = PHIG_READ_ONLY;
    EXPECT_NE(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_EQ(ctx, nullptr);
}

TEST(Abi, InitSucceedsAndShutdownIsClean) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    phig_ctx* ctx = nullptr;
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    ASSERT_NE(ctx, nullptr);
    EXPECT_STREQ(phig_last_error(ctx), "");
    phig_shutdown(ctx);
}

TEST(Abi, ShutdownToleratesNull) {
    phig_shutdown(nullptr);  // must not crash
}

TEST(Abi, LastErrorOnNullCtxReturnsEmptyString) {
    const char* msg = phig_last_error(nullptr);
    ASSERT_NE(msg, nullptr);
    EXPECT_STREQ(msg, "");
}
```

Add `make_db` to `tests/test_libphig_query.cpp` (non-static, declared in the anonymous-namespace-free scope so the linker finds it):

```cpp
// Creates an initialized, empty phig database at `path`.
void make_db(const std::string& path) {
    Database db(path);
    db.init_schema();
}
```

Remove the `namespace { ... }` wrapper around `make_db` in the query test file so it has external linkage, and drop the `namespace { }` around the declaration in `test_libphig_abi.cpp` accordingly.

- [ ] **Step 4: Add the CMake target and run to verify failure**

In `CMakeLists.txt`, first make the existing internal library's filename
unambiguous. The `phig-lib` target currently produces `libphig-lib.a`, which is
easy to mistake for the new public `libphig.so`. Rename only its output file —
leave the target name `phig-lib` and every `target_link_libraries(... phig-lib)`
reference alone:

```cmake
# on the existing phig-lib target, near line 47
set_target_properties(phig-lib PROPERTIES
    OUTPUT_NAME phig-internal
    # phig-lib is STATIC; linking it into a shared library requires
    # position-independent code, which a static target does not get by default.
    POSITION_INDEPENDENT_CODE ON
)
```

`phig-lib` links `phig-vendor` (also STATIC — sqlite-vec and inih), which is
pulled into the shared library transitively and needs the same treatment:

```cmake
set_target_properties(phig-vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
```

Without `POSITION_INDEPENDENT_CODE ON` the `phig_abi` link fails with
"relocation R_X86_64_32S against `...' can not be used when making a shared
object; recompile with -fPIC". Setting it on `phig-lib` also rebuilds the CLI and
tests from the same PIC objects, which is fine — a small code-size cost, no
behavior change. Verify the CLI still builds and its tests pass in Step 7.

Then add the new public library, after the existing shared-library section
(line ~47):

```cmake
# ---- libphig: C ABI shared library ----
add_library(phig_abi SHARED
    src/libphig/context.cpp
)
set_target_properties(phig_abi PROPERTIES
    OUTPUT_NAME phig
    VERSION 1.0.0
    SOVERSION 1
    # Hide everything not explicitly exported so the ABI surface is exactly
    # what the header declares.
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(phig_abi
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# phig-lib is the existing internal static library target (output:
# libphig-internal.a after the rename above).
target_link_libraries(phig_abi PRIVATE phig-lib)

install(TARGETS phig_abi LIBRARY DESTINATION lib)
install(DIRECTORY include/phig DESTINATION include)
```

Add to the `phig-tests` sources: `tests/test_libphig_abi.cpp` and `tests/test_libphig_c_compile.c`, link `phig_abi`, and enable C:

```cmake
# in project(...) LANGUAGES, add C alongside CXX
target_link_libraries(phig-tests PRIVATE phig_abi)
```

Run: `cmake -B build && cmake --build build 2>&1 | tail -20`
Expected: FAIL — `src/libphig/context.cpp` does not exist.

- [ ] **Step 5: Write `internal.h`**

Create `src/libphig/internal.h`:

```cpp
#pragma once

#include <memory>
#include <string>

#include "database.h"
#include "phig/phig.h"

struct phig_ctx {
    std::unique_ptr<Database> db;
    phig_access_mode mode = PHIG_READ_ONLY;
    std::string thumbs_dir;
    std::string last_error;
};

// Every public entry point wraps its body in these so no exception escapes
// the ABI boundary. Requires a `phig_ctx* ctx` in scope with a valid pointer.
#define PHIG_TRY(ctx)         \
    if ((ctx) == nullptr) return PHIG_ERR_INVALID_ARG; \
    (ctx)->last_error.clear(); \
    try {
#define PHIG_CATCH(ctx)                              \
    }                                                \
    catch (const std::exception& e) {                \
        (ctx)->last_error = e.what();                \
        return PHIG_ERR_DB;                          \
    }                                                \
    catch (...) {                                    \
        (ctx)->last_error = "unknown internal error"; \
        return PHIG_ERR_INTERNAL;                    \
    }
```

- [ ] **Step 6: Implement `context.cpp`**

```cpp
#include "internal.h"

extern "C" {

uint32_t phig_abi_version(void) {
    return (PHIG_ABI_VERSION_MAJOR << 16) | PHIG_ABI_VERSION_MINOR;
}

phig_status phig_init(const phig_config* cfg, phig_ctx** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (cfg == nullptr || cfg->db_path == nullptr || cfg->db_path[0] == '\0') {
        return PHIG_ERR_INVALID_ARG;
    }
    try {
        auto ctx = std::make_unique<phig_ctx>();
        ctx->mode = cfg->mode;
        ctx->thumbs_dir = cfg->thumbs_dir ? cfg->thumbs_dir : "";
        ctx->db = std::make_unique<Database>(cfg->db_path);
        // Read-only contexts must not migrate; a read-write one may.
        if (cfg->mode == PHIG_READ_WRITE) ctx->db->init_schema();
        *out = ctx.release();
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_DB;
    }
}

void phig_shutdown(phig_ctx* ctx) { delete ctx; }

const char* phig_last_error(phig_ctx* ctx) {
    return ctx ? ctx->last_error.c_str() : "";
}

}  // extern "C"
```

- [ ] **Step 7: Run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='Abi.*'`
Expected: 8 tests PASS.

Note: `Database`'s constructor opens the file and may create it. If `InitFailsOnUnopenableDatabase` passes only because the directory is missing, that is the intended signal — a bad path must not yield a usable context.

- [ ] **Step 8: Verify the exported symbol set**

Run: `nm -D --defined-only build/libphig.so | grep -v ' phig_' | grep ' T \| W '`
Expected: no output — every exported text symbol is `phig_`-prefixed. If C++ symbols leak, the visibility properties in Step 4 are not applied.

- [ ] **Step 9: Commit**

```bash
clang-format -i include/phig/phig.h src/libphig/internal.h src/libphig/context.cpp \
    tests/test_libphig_abi.cpp
git add include/ src/libphig/ tests/test_libphig_abi.cpp \
    tests/test_libphig_c_compile.c tests/test_libphig_query.cpp CMakeLists.txt
git commit -m "Add libphig C ABI shared library with context lifecycle"
```

---

### Task 3.5: Honor PHIG_READ_ONLY in the Database constructor

Added after Task 3's review found that `PHIG_READ_ONLY` does not open read-only:
`Database`'s constructor calls `sqlite3_open`, whose default flags create the
file if it is missing. A read-only context on a nonexistent path therefore
returns `PHIG_OK` and leaves an empty, unmigrated 4096-byte database on disk,
and `PHIG_ERR_READONLY` is never returned by anything.

This must be fixed before Task 4 because the GUI's entire read path uses a
read-only context — the spec has it open read-only precisely so it can never
contend with a running job's writer lock.

**Files:**
- Modify: `src/database.h`, `src/database.cpp` (constructor)
- Modify: `src/libphig/context.cpp`
- Test: `tests/test_libphig_abi.cpp`, `tests/test_database.cpp`

**Interfaces:**
- Consumes: `phig_ctx`, `phig_access_mode`, `phig_init` (Task 3).
- Produces: `Database(const std::string& path, bool read_only = false)`;
  `phig_init` honoring `PHIG_READ_ONLY`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_libphig_abi.cpp`:

```cpp
TEST(Abi, ReadOnlyInitDoesNotCreateDatabase) {
    TempDbPath tmp;  // path does not exist yet
    ASSERT_FALSE(std::filesystem::exists(tmp.path));

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    EXPECT_NE(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_EQ(ctx, nullptr);
    // The whole point: a read-only open must not create the file.
    EXPECT_FALSE(std::filesystem::exists(tmp.path));
}

TEST(Abi, ReadOnlyContextCannotWrite) {
    TempDbPath tmp;
    make_db(tmp.path);  // exists, migrated

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    ASSERT_NE(ctx, nullptr);
    phig_shutdown(ctx);
}

TEST(Abi, ReadWriteInitStillCreatesDatabase) {
    TempDbPath tmp;
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_WRITE;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    EXPECT_TRUE(std::filesystem::exists(tmp.path));
    phig_shutdown(ctx);
}
```

Add to `tests/test_database.cpp`:

```cpp
TEST(DatabaseTest, ReadOnlyOpenFailsOnMissingFileAndDoesNotCreateIt) {
    auto path = (std::filesystem::temp_directory_path() /
                 ("phig_ro_" + std::to_string(::getpid()) + ".db")).string();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ASSERT_FALSE(std::filesystem::exists(path));

    EXPECT_THROW(Database(path, /*read_only=*/true), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(DatabaseTest, ReadOnlyOpenRejectsWrites) {
    auto path = (std::filesystem::temp_directory_path() /
                 ("phig_ro2_" + std::to_string(::getpid()) + ".db")).string();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    { Database db(path); db.init_schema(); }  // create read-write

    Database ro(path, /*read_only=*/true);
    ImageInfo info;
    info.path = "/photos/nope.jpg";
    info.filename = "nope.jpg";
    info.extension = "jpg";
    info.sha256 = std::string(64, 'f');
    EXPECT_THROW(ro.insert(info), std::runtime_error);

    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + "-wal", ec);
    std::filesystem::remove(path + "-shm", ec);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `Database` has no two-argument constructor.

- [ ] **Step 3: Add the open mode to `Database`**

In `src/database.h`:

```cpp
    // read_only opens with SQLITE_OPEN_READONLY: the file must already exist
    // (no create-if-missing) and writes fail. Used by libphig's read-only
    // contexts so a GUI can never contend for the writer lock.
    explicit Database(const std::string& path, bool read_only = false);
```

The default argument keeps every existing CLI call site unchanged.

In `src/database.cpp`, replace the `sqlite3_open` call:

```cpp
    // Read-only callers must not create a database; read-write keeps the
    // historical create-if-missing behavior.
    const int flags = read_only ? SQLITE_OPEN_READONLY
                               : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
```

Guard the `fs::create_directories(parent)` call so a read-only open does not
create directories either — creating a parent directory for a database you
refuse to create is the same contract violation one level up.

Skip the WAL/`synchronous` pragmas when `read_only` is true: `PRAGMA
journal_mode=WAL` is itself a write and fails on a read-only handle. Reading a
WAL database through a read-only connection works without setting the pragma.
Keep `PRAGMA foreign_keys=ON` — it is connection-local and does not write.

- [ ] **Step 4: Honor the mode in `phig_init`**

In `src/libphig/context.cpp`, pass the mode through:

```cpp
        ctx->db = std::make_unique<Database>(cfg->db_path,
                                            cfg->mode == PHIG_READ_ONLY);
        // Only a read-write context may migrate; a read-only one cannot write
        // and must not silently operate on an unmigrated schema.
        if (cfg->mode == PHIG_READ_WRITE) ctx->db->init_schema();
```

- [ ] **Step 5: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='Abi.*:DatabaseTest.*'`
Expected: PASS, including the three new `Abi.*` and two new `DatabaseTest.*` tests.

- [ ] **Step 6: Run the full suite**

Run: `direnv exec . ./build/phig-tests`
Expected: all PASS. Every existing `Database` construction uses the default
argument and is unaffected; if anything fails, a call site was relying on
create-if-missing in a way the default should have preserved — investigate
rather than changing the test.

- [ ] **Step 7: Commit**

```bash
direnv exec . clang-format -i src/database.h src/database.cpp \
    src/libphig/context.cpp tests/test_libphig_abi.cpp tests/test_database.cpp
git add src/database.h src/database.cpp src/libphig/context.cpp \
    tests/test_libphig_abi.cpp tests/test_database.cpp
git commit -m "Open read-only databases with SQLITE_OPEN_READONLY"
```

---

### Task 4: Query ABI — builder, count, page

**Files:**
- Modify: `include/phig/phig.h`
- Create: `src/libphig/query.cpp`, `src/libphig/records.cpp`
- Modify: `src/libphig/internal.h`, `CMakeLists.txt`
- Test: `tests/test_libphig_query.cpp`

**Interfaces:**
- Consumes: `phig_ctx`, `PHIG_TRY`/`PHIG_CATCH` (Task 3); `Database::search_page`, `Database::count_matching`, `Database::PageCriteria` (Task 2).
- Produces: `phig_query`, `phig_image`, `phig_image_list`, `phig_media_type`, `phig_sort_key`, all `phig_query_*` setters, `phig_query_count`, `phig_query_page`, `phig_image_list_free`, `phig_image_metadata`, `phig_string_free`.

- [ ] **Step 1: Append the query API to the header**

Add to `include/phig/phig.h` before the closing `extern "C"`:

```c
typedef enum { PHIG_MEDIA_ANY = 0, PHIG_MEDIA_IMAGE = 1, PHIG_MEDIA_VIDEO = 2 }
    phig_media_type;

typedef enum { PHIG_SORT_DATE = 0, PHIG_SORT_PATH = 1, PHIG_SORT_SIZE = 2,
               PHIG_SORT_FILENAME = 3 } phig_sort_key;

typedef struct phig_query phig_query;

/* Strings are owned by the containing phig_image_list and are valid until it
 * is freed. Absent values are "" (never NULL). blur_score is -1 if not
 * computed. Dates are ISO-8601 TEXT; effective_date is phig's best available
 * date (EXIF DateTimeOriginal, then DateTimeDigitized, then modified_at) and
 * is what PHIG_SORT_DATE orders by. */
typedef struct {
    int64_t id;
    const char *path;
    const char *filename;
    const char *extension;
    const char *sha256;
    int64_t file_size;
    const char *created_at;
    const char *modified_at;
    const char *effective_date;
    int32_t width;
    int32_t height;
    phig_media_type media_type;
    int64_t duration_ms;
    const char *gallery;
    double blur_score;
    bool favorite;
} phig_image;

typedef struct {
    const phig_image *items;
    int64_t count;
} phig_image_list;

void phig_image_list_free(phig_image_list *list);

phig_status phig_query_new(phig_ctx *ctx, phig_query **out);
void phig_query_free(phig_query *q);

/* Setters ignore NULL query pointers. Passing NULL or "" for a string filter
 * clears that filter. */
void phig_query_set_extension(phig_query *q, const char *ext);
void phig_query_set_date_range(phig_query *q, const char *after, const char *before);
void phig_query_set_camera(phig_query *q, const char *model);
void phig_query_set_make(phig_query *q, const char *make);
void phig_query_set_size_range(phig_query *q, int64_t min_bytes, int64_t max_bytes);
void phig_query_set_media(phig_query *q, phig_media_type media);
void phig_query_set_duration_range(phig_query *q, int64_t min_ms, int64_t max_ms);
void phig_query_add_tag(phig_query *q, const char *tag);   /* AND semantics */
void phig_query_set_gallery(phig_query *q, const char *gallery);
void phig_query_set_sharpness(phig_query *q, double min_score, double max_score);
void phig_query_set_sort(phig_query *q, phig_sort_key key, bool descending);

/* Total matching rows, ignoring pagination. Needed to size a scrollbar before
 * any page has been fetched. */
phig_status phig_query_count(phig_ctx *ctx, const phig_query *q, int64_t *out);

/* One page. *out must be released with phig_image_list_free. A page past the
 * end yields an empty list, not an error. limit must be > 0. */
phig_status phig_query_page(phig_ctx *ctx, const phig_query *q,
                           int64_t offset, int32_t limit, phig_image_list **out);

/* Raw EXIF/container metadata JSON for one image. Deliberately separate from
 * phig_image: it is a large blob and fetching it per grid cell would waste
 * most of the query budget. *json_out must be released with phig_string_free.
 * Returns PHIG_ERR_NOT_FOUND if no such id. */
phig_status phig_image_metadata(phig_ctx *ctx, int64_t id, char **json_out);
void phig_string_free(char *s);
```

- [ ] **Step 2: Write the failing ABI query tests**

Append to `tests/test_libphig_query.cpp`:

```cpp
#include "phig/phig.h"

namespace {
// Opens a read-only ABI context on `path`. Caller shuts it down.
phig_ctx* open_ro(const std::string& path) {
    phig_config cfg{};
    cfg.db_path = path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    return ctx;
}
}  // namespace

TEST(AbiQuery, CountMatchesSeededRows) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); seed(db, 25); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    int64_t n = 0;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 25);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, PageReturnsLimitAndPopulatesFields) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); seed(db, 25); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count, 10);
    EXPECT_GT(list->items[0].id, 0);
    EXPECT_NE(list->items[0].path[0], '\0');
    EXPECT_NE(list->items[0].effective_date[0], '\0');
    EXPECT_EQ(list->items[0].media_type, PHIG_MEDIA_IMAGE);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, NoStringFieldIsNull) {
    TempDb tmp;
    {
        Database db(tmp.path); db.init_schema();
        ImageInfo info;  // minimal row: most text columns empty/NULL
        info.path = "/photos/bare.jpg";
        info.filename = "bare.jpg";
        info.extension = "jpg";
        info.sha256 = std::string(64, 'd');
        db.insert(info);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_EQ(list->count, 1);
    const phig_image& im = list->items[0];
    EXPECT_NE(im.path, nullptr);
    EXPECT_NE(im.filename, nullptr);
    EXPECT_NE(im.extension, nullptr);
    EXPECT_NE(im.sha256, nullptr);
    EXPECT_NE(im.created_at, nullptr);
    EXPECT_NE(im.modified_at, nullptr);
    EXPECT_NE(im.effective_date, nullptr);
    EXPECT_NE(im.gallery, nullptr);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, StringsStayValidUntilListFreed) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); seed(db, 3); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 3, &list), PHIG_OK);
    // Copy the pointer, then run another query; the first list must be intact.
    const char* first_path = list->items[0].path;
    std::string before(first_path);
    phig_image_list* other = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 3, &other), PHIG_OK);
    EXPECT_EQ(before, std::string(first_path));
    phig_image_list_free(other);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, PagePastEndIsEmptyNotError) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); seed(db, 3); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 500, 10, &list), PHIG_OK);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count, 0);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, RejectsNonPositiveLimit) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_image_list* list = nullptr;
    EXPECT_EQ(phig_query_page(ctx, q, 0, 0, &list), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_page(ctx, q, 0, -5, &list), PHIG_ERR_INVALID_ARG);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, ExtensionFilterApplies) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); seed(db, 5); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_query_set_extension(q, "png");
    int64_t n = -1;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 0);
    phig_query_set_extension(q, "jpg");
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 5);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, SortDateDescendingIsNewestFirst) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); seed(db, 5); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_query_set_sort(q, PHIG_SORT_DATE, true);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 5, &list), PHIG_OK);
    ASSERT_EQ(list->count, 5);
    EXPECT_STREQ(list->items[0].effective_date, "2020-01-05 00:00:00");
    EXPECT_STREQ(list->items[4].effective_date, "2020-01-01 00:00:00");
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, NullArgumentsAreRejected) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    int64_t n = 0;
    phig_image_list* list = nullptr;
    EXPECT_EQ(phig_query_count(nullptr, q, &n), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_count(ctx, nullptr, &n), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_count(ctx, q, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_page(ctx, q, 0, 10, nullptr), PHIG_ERR_INVALID_ARG);
    phig_query_set_extension(nullptr, "jpg");  // must not crash
    phig_query_free(nullptr);                  // must not crash
    phig_image_list_free(nullptr);             // must not crash
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, MetadataFetchedOnDemand) {
    TempDb tmp;
    int64_t id = 0;
    {
        Database db(tmp.path); db.init_schema();
        ImageInfo info;
        info.path = "/photos/meta.jpg";
        info.filename = "meta.jpg";
        info.extension = "jpg";
        info.sha256 = std::string(64, 'e');
        info.metadata_json = R"({"Model":"Canon EOS 5D"})";
        db.insert(info);
        auto rows = db.get_all_images();
        ASSERT_EQ(rows.size(), 1u);
        id = rows[0].id;
    }
    phig_ctx* ctx = open_ro(tmp.path);
    char* json = nullptr;
    ASSERT_EQ(phig_image_metadata(ctx, id, &json), PHIG_OK);
    ASSERT_NE(json, nullptr);
    EXPECT_NE(std::string(json).find("Canon EOS 5D"), std::string::npos);
    phig_string_free(json);
    phig_string_free(nullptr);  // must not crash
    phig_shutdown(ctx);
}

TEST(AbiQuery, MetadataMissingIdIsNotFound) {
    TempDb tmp;
    { Database db(tmp.path); db.init_schema(); }
    phig_ctx* ctx = open_ro(tmp.path);
    char* json = nullptr;
    EXPECT_EQ(phig_image_metadata(ctx, 999999, &json), PHIG_ERR_NOT_FOUND);
    phig_shutdown(ctx);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -20`
Expected: FAIL — `phig_query_new` and friends undefined.

- [ ] **Step 4: Add the query struct to `internal.h`**

```cpp
struct phig_query {
    Database::PageCriteria criteria;
};
```

- [ ] **Step 5: Implement `records.cpp`**

The list owns one contiguous `phig_image` array plus a string arena, so every
`const char*` stays valid until `phig_image_list_free`.

```cpp
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "internal.h"

namespace {
// Backing storage for one returned list. `strings` is a deque so that pushing
// more entries never reallocates existing ones — the phig_image pointers
// reference into these strings directly.
struct ListStorage {
    phig_image_list header;
    std::vector<phig_image> items;
    std::deque<std::string> strings;
};

const char* intern(ListStorage& st, const std::string& s) {
    st.strings.push_back(s);
    return st.strings.back().c_str();
}

phig_media_type media_from(const std::string& s) {
    if (s == "video") return PHIG_MEDIA_VIDEO;
    return PHIG_MEDIA_IMAGE;
}
}  // namespace

phig_image_list* phig_make_image_list(const std::vector<ImageInfo>& rows) {
    auto* st = new ListStorage();
    st->items.reserve(rows.size());
    for (const auto& r : rows) {
        phig_image im{};
        im.id = r.id;
        im.path = intern(*st, r.path);
        im.filename = intern(*st, r.filename);
        im.extension = intern(*st, r.extension);
        im.sha256 = intern(*st, r.sha256);
        im.file_size = r.file_size;
        im.created_at = intern(*st, r.created_at);
        im.modified_at = intern(*st, r.modified_at);
        im.effective_date = intern(*st, r.effective_date);
        im.width = r.width;
        im.height = r.height;
        im.media_type = media_from(r.media_type);
        im.duration_ms = r.duration_ms;
        im.gallery = intern(*st, r.gallery);
        im.blur_score = r.blur_score;
        im.favorite = r.favorite;
        st->items.push_back(im);
    }
    st->header.items = st->items.data();
    st->header.count = static_cast<int64_t>(st->items.size());
    // The header is the first member, so the list pointer round-trips to
    // ListStorage* in phig_image_list_free.
    return &st->header;
}

extern "C" void phig_image_list_free(phig_image_list* list) {
    if (list == nullptr) return;
    delete reinterpret_cast<ListStorage*>(list);
}

extern "C" void phig_string_free(char* s) { delete[] s; }
```

Declare `phig_image_list* phig_make_image_list(const std::vector<ImageInfo>&);` in `internal.h`.

- [ ] **Step 6: Implement `query.cpp`**

```cpp
#include <cstring>
#include <string>

#include "internal.h"

namespace {
Database::SortKey to_sort_key(phig_sort_key k) {
    switch (k) {
        case PHIG_SORT_PATH:     return Database::SortKey::Path;
        case PHIG_SORT_SIZE:     return Database::SortKey::Size;
        case PHIG_SORT_FILENAME: return Database::SortKey::Filename;
        case PHIG_SORT_DATE:
        default:                 return Database::SortKey::Date;
    }
}
std::string str_or_empty(const char* s) { return s ? s : ""; }
}  // namespace

extern "C" {

phig_status phig_query_new(phig_ctx* ctx, phig_query** out) {
    if (ctx == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    try {
        *out = new phig_query();
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

void phig_query_free(phig_query* q) { delete q; }

void phig_query_set_extension(phig_query* q, const char* ext) {
    if (q) q->criteria.filter.extension = str_or_empty(ext);
}

void phig_query_set_date_range(phig_query* q, const char* after, const char* before) {
    if (!q) return;
    q->criteria.filter.after = str_or_empty(after);
    q->criteria.filter.before = str_or_empty(before);
}

void phig_query_set_camera(phig_query* q, const char* model) {
    if (q) q->criteria.filter.camera = str_or_empty(model);
}

void phig_query_set_make(phig_query* q, const char* make) {
    if (q) q->criteria.filter.make = str_or_empty(make);
}

void phig_query_set_size_range(phig_query* q, int64_t min_bytes, int64_t max_bytes) {
    if (!q) return;
    q->criteria.filter.min_size = min_bytes;
    q->criteria.filter.max_size = max_bytes;
}

void phig_query_set_media(phig_query* q, phig_media_type media) {
    if (!q) return;
    switch (media) {
        case PHIG_MEDIA_IMAGE: q->criteria.filter.media = "image"; break;
        case PHIG_MEDIA_VIDEO: q->criteria.filter.media = "video"; break;
        default:               q->criteria.filter.media = ""; break;
    }
}

void phig_query_set_duration_range(phig_query* q, int64_t min_ms, int64_t max_ms) {
    if (!q) return;
    q->criteria.filter.min_duration_ms = min_ms;
    q->criteria.filter.max_duration_ms = max_ms;
}

void phig_query_add_tag(phig_query* q, const char* tag) {
    if (q && tag && tag[0] != '\0') q->criteria.filter.tags.push_back(tag);
}

void phig_query_set_gallery(phig_query* q, const char* gallery) {
    if (q) q->criteria.filter.gallery = str_or_empty(gallery);
}

void phig_query_set_sharpness(phig_query* q, double min_score, double max_score) {
    if (!q) return;
    q->criteria.filter.min_sharpness = min_score;
    q->criteria.filter.max_sharpness = max_score;
}

void phig_query_set_sort(phig_query* q, phig_sort_key key, bool descending) {
    if (!q) return;
    q->criteria.sort = to_sort_key(key);
    q->criteria.descending = descending;
}

phig_status phig_query_count(phig_ctx* ctx, const phig_query* q, int64_t* out) {
    if (q == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        *out = ctx->db->count_matching(q->criteria.filter);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_query_page(phig_ctx* ctx, const phig_query* q, int64_t offset,
                            int32_t limit, phig_image_list** out) {
    if (q == nullptr || out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (limit <= 0 || offset < 0) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        Database::PageCriteria c = q->criteria;
        c.offset = offset;
        c.limit = limit;
        *out = phig_make_image_list(ctx->db->search_page(c));
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_image_metadata(phig_ctx* ctx, int64_t id, char** json_out) {
    if (json_out == nullptr) return PHIG_ERR_INVALID_ARG;
    *json_out = nullptr;
    PHIG_TRY(ctx)
        std::string json;
        if (!ctx->db->get_metadata_json(id, json)) return PHIG_ERR_NOT_FOUND;
        char* buf = new char[json.size() + 1];
        std::memcpy(buf, json.c_str(), json.size() + 1);
        *json_out = buf;
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

}  // extern "C"
```

Note `PHIG_ERR_INVALID_ARG` for a null `ctx` comes from `PHIG_TRY` itself.

- [ ] **Step 7: Add `Database::get_metadata_json`**

In `src/database.h` (public) and `src/database.cpp`:

```cpp
// Reads images.metadata for one id. Returns false if no such row.
bool Database::get_metadata_json(int64_t id, std::string& out) {
    const char* sql = "SELECT metadata FROM images WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare metadata query: " +
                                 std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int64(stmt, 1, id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(stmt, 0);
        out = p ? reinterpret_cast<const char*>(p) : "";
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}
```

- [ ] **Step 8: Add the new sources to CMake and run the tests**

Add `src/libphig/query.cpp` and `src/libphig/records.cpp` to the `phig_abi` target.

Run: `cmake -B build && cmake --build build && ./build/phig-tests --gtest_filter='AbiQuery.*'`
Expected: 11 tests PASS.

- [ ] **Step 9: Verify no leaks under the query path**

Run: `valgrind --error-exitcode=1 --leak-check=full ./build/phig-tests --gtest_filter='AbiQuery.*' 2>&1 | tail -25`
Expected: no leaks from `phig_make_image_list` / `ListStorage`. If valgrind is unavailable, build with `-fsanitize=address` instead and confirm a clean run. Do not skip this step — the string-arena ownership is the easiest thing in this plan to get wrong.

- [ ] **Step 10: Run the full suite and commit**

```bash
./build/phig-tests
clang-format -i include/phig/phig.h src/libphig/*.h src/libphig/*.cpp \
    src/database.h src/database.cpp tests/test_libphig_query.cpp
git add include/ src/libphig/ src/database.h src/database.cpp \
    tests/test_libphig_query.cpp CMakeLists.txt
git commit -m "Add libphig query ABI with pagination and on-demand metadata"
```

---

### Task 5: Thumbnail lookup

**Files:**
- Modify: `include/phig/phig.h`
- Create: `src/libphig/thumbs.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_libphig_abi.cpp`

**Interfaces:**
- Consumes: `phig_ctx` (Task 3); `get_thumb_path`, `thumb_exists` from `src/thumbs.h`.
- Produces: `phig_thumb_lookup`.

- [ ] **Step 1: Append to the header**

```c
/* Resolves the cache path for an image's thumbnail. Returns a PATH rather than
 * bytes so a client can hand the file straight to its image loader with no
 * copy through this ABI, and so the cache layout stays private to phig.
 *
 * `sha256` must be a 64-char hex digest. On PHIG_OK, out_path holds the path
 * and *exists says whether the file is present; on false the client should
 * request generation rather than reading it. Returns PHIG_ERR_INVALID_ARG if
 * out_len is too small for the path plus its NUL. */
phig_status phig_thumb_lookup(phig_ctx *ctx, const char *sha256,
                             char *out_path, size_t out_len, bool *exists);
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/test_libphig_abi.cpp`:

```cpp
TEST(AbiThumb, ReturnsPathAndReportsMissing) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'a');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_NE(buf[0], '\0');
    EXPECT_FALSE(exists);  // nothing generated for this digest
    // Path must contain the digest so it is stable and cache-keyed by content.
    EXPECT_NE(std::string(buf).find(sha), std::string::npos);
    phig_shutdown(ctx);
}

TEST(AbiThumb, RejectsBadDigestAndSmallBuffer) {
    TempDbPath tmp;
    make_db(tmp.path);
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    char buf[4096];
    bool exists = false;
    EXPECT_EQ(phig_thumb_lookup(ctx, "nothex", buf, sizeof(buf), &exists),
              PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_thumb_lookup(ctx, nullptr, buf, sizeof(buf), &exists),
              PHIG_ERR_INVALID_ARG);

    std::string sha(64, 'a');
    char tiny[8];
    EXPECT_EQ(phig_thumb_lookup(ctx, sha.c_str(), tiny, sizeof(tiny), &exists),
              PHIG_ERR_INVALID_ARG);
    phig_shutdown(ctx);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -20`
Expected: FAIL — `phig_thumb_lookup` undefined.

- [ ] **Step 4: Implement `thumbs.cpp`**

```cpp
#include <cctype>
#include <cstring>
#include <string>

#include "internal.h"
#include "thumbs.h"

namespace {
bool is_hex64(const char* s) {
    if (s == nullptr) return false;
    size_t n = 0;
    for (; s[n] != '\0'; ++n) {
        if (n >= 64) return false;
        if (!std::isxdigit(static_cast<unsigned char>(s[n]))) return false;
    }
    return n == 64;
}
}  // namespace

extern "C" phig_status phig_thumb_lookup(phig_ctx* ctx, const char* sha256,
                                         char* out_path, size_t out_len,
                                         bool* exists) {
    if (out_path == nullptr || exists == nullptr || out_len == 0) {
        return PHIG_ERR_INVALID_ARG;
    }
    if (!is_hex64(sha256)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        std::string path = get_thumb_path(sha256);
        if (path.size() + 1 > out_len) return PHIG_ERR_INVALID_ARG;
        std::memcpy(out_path, path.c_str(), path.size() + 1);
        *exists = thumb_exists(sha256);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}
```

If `get_thumb_path` honors a configurable thumbs directory, pass `ctx->thumbs_dir` when it is non-empty; check the signature in `src/thumbs.h` and adapt. If it only reads the XDG default, leave `ctx->thumbs_dir` unused for now and note it in the header comment.

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='AbiThumb.*'`
Expected: 2 tests PASS.

- [ ] **Step 6: Commit**

```bash
clang-format -i include/phig/phig.h src/libphig/thumbs.cpp tests/test_libphig_abi.cpp
git add include/ src/libphig/thumbs.cpp tests/test_libphig_abi.cpp CMakeLists.txt
git commit -m "Add libphig thumbnail path lookup"
```

---

### Task 5.5: Honor `phig_config.thumbs_dir`

Added after Task 5's review: a client can set `phig_config.thumbs_dir`, and
`phig_init` stores it into `ctx->thumbs_dir`, but nothing reads it —
`phig_thumb_lookup` resolves phig's XDG default regardless. A silently ignored
config field is a footgun on a versioned ABI, and phig's own `config.ini`
already exposes `[paths] thumbs_dir`, so the CLI honors a custom directory while
libphig would not.

**The mechanism already exists.** `src/paths.h` declares
`set_path_overrides(db, models, thumbs)`; `thumbs_dir()` returns the override
when one is set, and the CLI calls it at startup from the config file. libphig
should use that same mechanism rather than inventing a second one — do NOT add a
directory parameter to `get_thumb_path()`, which would touch 13 call sites
across `main.cpp` and the tests for no benefit.

**Process-wide by design.** `set_path_overrides` is global and documented as
"called once at startup". That matches how libphig is used: a host reads its
config once, calls `phig_init` once, and never revisits it. `thumbs_dir` is
therefore a process-wide setting, not a per-context one, and the header must say
so. Two contexts wanting different thumbnail directories in one process is out
of scope; that would require per-context path resolution, a much larger change.

**Files:**
- Modify: `src/libphig/context.cpp`, `include/phig/phig.h`
- Test: `tests/test_libphig_abi.cpp`

**Interfaces:**
- Consumes: `set_path_overrides` (`src/paths.h`), `phig_config::thumbs_dir`,
  `phig_thumb_lookup` (Task 5).
- Produces: no new symbols; `thumbs_dir` becomes functional.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_libphig_abi.cpp`:

```cpp
#include "paths.h"

TEST(AbiThumb, HonorsConfiguredThumbsDir) {
    TempDbPath tmp;
    make_db(tmp.path);
    auto custom = (std::filesystem::temp_directory_path() /
                   ("phig_thumbs_" + std::to_string(::getpid()))).string();

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = custom.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'a');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_EQ(std::string(buf).rfind(custom, 0), 0u);  // path starts with custom dir
    EXPECT_FALSE(exists);

    phig_shutdown(ctx);
    set_path_overrides(std::nullopt, std::nullopt, std::nullopt);  // reset global state
}

TEST(AbiThumb, NullThumbsDirLeavesDefaultInPlace) {
    TempDbPath tmp;
    make_db(tmp.path);

    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.thumbs_dir = nullptr;  // must not clobber an existing override
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    std::string sha(64, 'b');
    char buf[4096];
    bool exists = true;
    ASSERT_EQ(phig_thumb_lookup(ctx, sha.c_str(), buf, sizeof(buf), &exists), PHIG_OK);
    EXPECT_NE(std::string(buf).find("thumbs"), std::string::npos);

    phig_shutdown(ctx);
    set_path_overrides(std::nullopt, std::nullopt, std::nullopt);
}
```

Note both tests reset the override in teardown. This global is process-wide, so
a test that leaves it set corrupts every test that runs after it — check whether
`tests/test_thumbs.cpp` or `tests/test_paths.cpp` already establish a reset
convention and follow it.

- [ ] **Step 2: Run to verify failure**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='AbiThumb.*'`
Expected: `HonorsConfiguredThumbsDir` FAILS — the path is under the XDG default,
not the custom directory.

- [ ] **Step 3: Apply the override in `phig_init`**

In `src/libphig/context.cpp`, after the config is validated and before the
`Database` is constructed:

```cpp
        // thumbs_dir is process-wide (see paths.h): phig resolves the cache
        // location through a global override, the same one the CLI sets from
        // config.ini. Only apply it when the caller supplied one, so a NULL
        // leaves any existing override untouched.
        if (cfg->thumbs_dir && cfg->thumbs_dir[0] != '\0') {
            set_path_overrides(std::nullopt, std::nullopt, std::string(cfg->thumbs_dir));
        }
```

Include `paths.h`. Keep storing the value in `ctx->thumbs_dir` — it is now a
record of what this context asked for, not dead state.

- [ ] **Step 4: Update the header documentation**

Replace Task 5's "not yet honored" note on `phig_thumb_lookup`, and document the
field on `phig_config`:

```c
    /* Thumbnail cache directory. NULL uses phig's default
       (~/.cache/phig/thumbs, honoring XDG_CACHE_HOME).

       PROCESS-WIDE: phig resolves the cache location through a global, so this
       applies to every context in the process, not just this one. Set it once,
       at startup, before creating any context — the way a host reads its config
       once. Passing different values for two contexts in one process is not
       supported; the last one set wins for all of them. */
    const char *thumbs_dir;
```

- [ ] **Step 5: Run the tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='AbiThumb.*'`
Expected: 4 tests PASS (Task 5's two plus these two).

- [ ] **Step 6: Run the full suite**

Run: `direnv exec . ./build/phig-tests`
Expected: all PASS. Watch specifically for failures in `test_thumbs.cpp` or
`test_paths.cpp` — those exercise the same global, and a leaked override from
the new tests would show up there. If you see order-dependent failures, the
teardown reset is missing or incomplete.

- [ ] **Step 7: Commit**

```bash
direnv exec . clang-format -i src/libphig/context.cpp include/phig/phig.h \
    tests/test_libphig_abi.cpp
git add src/libphig/context.cpp include/phig/phig.h tests/test_libphig_abi.cpp
git commit -m "Honor phig_config.thumbs_dir via phig's path override"
```

---

### Task 6: Mutation ABI — tags, gallery, favorites

**Files:**
- Modify: `include/phig/phig.h`, `src/database.h`, `src/database.cpp`
- Create: `src/libphig/mutate.cpp`
- Create: `tests/test_libphig_mutate.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `phig_ctx`, `phig_access_mode` (Task 3).
- Produces: `phig_tag_add`, `phig_tag_remove`, `phig_gallery_set`, `phig_gallery_clear`, `phig_favorite_set`; `Database::set_tag_by_ids`, `Database::set_gallery_by_ids`.

- [ ] **Step 1: Append to the header**

```c
/* Metadata writes. These take ids because a GUI always has a concrete
 * selection; the CLI's glob-based paths are unchanged.
 *
 * All require a PHIG_READ_WRITE context and return PHIG_ERR_READONLY on a
 * read-only one. Each call is one transaction: it applies to every id or to
 * none. Unknown ids are skipped, not an error. n == 0 is a no-op returning
 * PHIG_OK. Favorites are phig's built-in "favorite" tag. */
phig_status phig_tag_add(phig_ctx *ctx, const int64_t *ids, size_t n, const char *tag);
phig_status phig_tag_remove(phig_ctx *ctx, const int64_t *ids, size_t n, const char *tag);
phig_status phig_gallery_set(phig_ctx *ctx, const int64_t *ids, size_t n,
                            const char *gallery);
phig_status phig_gallery_clear(phig_ctx *ctx, const int64_t *ids, size_t n);
phig_status phig_favorite_set(phig_ctx *ctx, const int64_t *ids, size_t n, bool on);
```

- [ ] **Step 2: Write the failing tests**

Create `tests/test_libphig_mutate.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "database.h"
#include "phig/phig.h"

namespace {
struct TempDbPath {
    std::string path;
    TempDbPath() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_mut_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
    }
    ~TempDbPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
};

// Seeds three images and returns their ids in insertion order.
std::vector<int64_t> seed_three(const std::string& db_path) {
    Database db(db_path);
    db.init_schema();
    for (int i = 0; i < 3; ++i) {
        ImageInfo info;
        info.path = "/photos/m" + std::to_string(i) + ".jpg";
        info.filename = "m" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.modified_at = "2021-01-01 00:00:00";
        info.sha256 = std::string(63, '1') + std::to_string(i);
        db.insert(info);
    }
    // NOTE: do NOT use get_all_images() to collect ids — it does not SELECT the
    // id column, so every ImageInfo::id comes back 0 and the tests would assert
    // against nothing. get_image_id(path) is the correct lookup.
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(db.get_image_id("/photos/m" + std::to_string(i) + ".jpg"));
    }
    return ids;
}

phig_ctx* open_rw(const std::string& path) {
    phig_config cfg{};
    cfg.db_path = path.c_str();
    cfg.mode = PHIG_READ_WRITE;
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    return ctx;
}
}  // namespace

TEST(AbiMutate, TagAddThenRemove) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    ASSERT_EQ(ids.size(), 3u);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_tag_add(ctx, ids.data(), ids.size(), "landscape"), PHIG_OK);
    {
        Database db(tmp.path);
        EXPECT_EQ(db.get_tags_for_image("/photos/m0.jpg").size(), 1u);
    }
    ASSERT_EQ(phig_tag_remove(ctx, ids.data(), ids.size(), "landscape"), PHIG_OK);
    {
        Database db(tmp.path);
        EXPECT_TRUE(db.get_tags_for_image("/photos/m0.jpg").empty());
    }
    phig_shutdown(ctx);
}

TEST(AbiMutate, GallerySetThenClear) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_gallery_set(ctx, ids.data(), 1, "Trip 2024"), PHIG_OK);
    {
        Database db(tmp.path);
        auto rows = db.get_all_images();
        int matched = 0;
        for (const auto& r : rows) {
            if (r.path == "/photos/m0.jpg") {
                EXPECT_EQ(r.gallery, "Trip 2024");
                ++matched;
            }
        }
        EXPECT_EQ(matched, 1);
    }
    ASSERT_EQ(phig_gallery_clear(ctx, ids.data(), 1), PHIG_OK);
    {
        Database db(tmp.path);
        for (const auto& r : db.get_all_images()) {
            if (r.path == "/photos/m0.jpg") EXPECT_TRUE(r.gallery.empty());
        }
    }
    phig_shutdown(ctx);
}

TEST(AbiMutate, FavoriteToggleIsVisibleInQuery) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_favorite_set(ctx, ids.data(), 2, true), PHIG_OK);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    int favs = 0;
    for (int64_t i = 0; i < list->count; ++i) {
        if (list->items[i].favorite) ++favs;
    }
    EXPECT_EQ(favs, 2);
    phig_image_list_free(list);

    ASSERT_EQ(phig_favorite_set(ctx, ids.data(), 2, false), PHIG_OK);
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    favs = 0;
    for (int64_t i = 0; i < list->count; ++i) {
        if (list->items[i].favorite) ++favs;
    }
    EXPECT_EQ(favs, 0);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiMutate, ReadOnlyContextRefusesWrites) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    EXPECT_EQ(phig_tag_add(ctx, ids.data(), ids.size(), "x"), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_tag_remove(ctx, ids.data(), ids.size(), "x"), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_gallery_set(ctx, ids.data(), 1, "g"), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_gallery_clear(ctx, ids.data(), 1), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_favorite_set(ctx, ids.data(), 1, true), PHIG_ERR_READONLY);

    // And nothing was written.
    Database db(tmp.path);
    EXPECT_TRUE(db.get_tags_for_image("/photos/m0.jpg").empty());
    phig_shutdown(ctx);
}

TEST(AbiMutate, RejectsBadArguments) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    EXPECT_EQ(phig_tag_add(ctx, nullptr, 3, "t"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_tag_add(ctx, ids.data(), 3, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_tag_add(ctx, ids.data(), 3, ""), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_gallery_set(ctx, ids.data(), 1, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_tag_add(nullptr, ids.data(), 3, "t"), PHIG_ERR_INVALID_ARG);
    phig_shutdown(ctx);
}

TEST(AbiMutate, EmptyIdListIsNoOp) {
    TempDbPath tmp;
    seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);
    int64_t none = 0;
    EXPECT_EQ(phig_tag_add(ctx, &none, 0, "t"), PHIG_OK);
    EXPECT_EQ(phig_favorite_set(ctx, &none, 0, true), PHIG_OK);
    phig_shutdown(ctx);
}

TEST(AbiMutate, UnknownIdsAreSkippedNotAnError) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);
    std::vector<int64_t> mixed = {ids[0], 999999};
    EXPECT_EQ(phig_tag_add(ctx, mixed.data(), mixed.size(), "t"), PHIG_OK);
    Database db(tmp.path);
    EXPECT_EQ(db.get_tags_for_image("/photos/m0.jpg").size(), 1u);
    phig_shutdown(ctx);
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -20`
Expected: FAIL — mutation symbols undefined.

- [ ] **Step 4: Add id-based setters to `Database`**

The existing `set_tag` takes paths. Add id-based siblings in `src/database.h`
(public) and `src/database.cpp`. Both run in one transaction and take phig's
writer lock at the ABI layer, not here.

```cpp
// Adds or removes `tag` on the given image ids, auto-creating the tag row on
// add. Unknown ids are skipped. Returns the number of rows affected.
int Database::set_tag_by_ids(const std::vector<int64_t>& ids, const std::string& tag,
                             bool add) {
    if (ids.empty()) return 0;
    int affected = 0;
    begin_transaction();
    try {
        if (add) {
            const char* ins_tag = "INSERT OR IGNORE INTO tags (name) VALUES (?)";
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, ins_tag, -1, &st, nullptr) != SQLITE_OK) {
                throw std::runtime_error("prepare tag insert: " +
                                         std::string(sqlite3_errmsg(db_)));
            }
            sqlite3_bind_text(st, 1, tag.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        const char* sql = add
            ? "INSERT OR IGNORE INTO image_tags (image_id, tag_id) "
              "SELECT ?, id FROM tags WHERE name = ?"
            : "DELETE FROM image_tags WHERE image_id = ? AND tag_id = "
              "(SELECT id FROM tags WHERE name = ?)";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            throw std::runtime_error("prepare image_tags: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        for (int64_t id : ids) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_text(st, 2, tag.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                throw std::runtime_error("apply tag: " + std::string(sqlite3_errmsg(db_)));
            }
            affected += sqlite3_changes(db_);
        }
        sqlite3_finalize(st);
        commit_transaction();
    } catch (...) {
        rollback_transaction();
        throw;
    }
    return affected;
}

// Sets gallery on the given ids; an empty string clears it (stores NULL).
int Database::set_gallery_by_ids(const std::vector<int64_t>& ids,
                                 const std::string& gallery) {
    if (ids.empty()) return 0;
    int affected = 0;
    begin_transaction();
    try {
        const char* sql = "UPDATE images SET gallery = ? WHERE id = ?";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            throw std::runtime_error("prepare gallery update: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        for (int64_t id : ids) {
            sqlite3_reset(st);
            if (gallery.empty()) {
                sqlite3_bind_null(st, 1);
            } else {
                sqlite3_bind_text(st, 1, gallery.c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int64(st, 2, id);
            if (sqlite3_step(st) != SQLITE_DONE) {
                sqlite3_finalize(st);
                throw std::runtime_error("apply gallery: " +
                                         std::string(sqlite3_errmsg(db_)));
            }
            affected += sqlite3_changes(db_);
        }
        sqlite3_finalize(st);
        commit_transaction();
    } catch (...) {
        rollback_transaction();
        throw;
    }
    return affected;
}
```

Check the actual `image_tags` column names in the schema (`src/database.cpp:288`) and match them.

- [ ] **Step 5: Implement `mutate.cpp`**

`FAVORITE_TAG` is phig's built-in favorite tag name — grep for the literal
`"favorite"` in `src/database.cpp` and use the same constant.

```cpp
#include <string>
#include <vector>

#include "internal.h"

namespace {
const char* kFavoriteTag = "favorite";

bool bad_ids(const int64_t* ids, size_t n) { return n > 0 && ids == nullptr; }
bool bad_tag(const char* t) { return t == nullptr || t[0] == '\0'; }
}  // namespace

extern "C" {

phig_status phig_tag_add(phig_ctx* ctx, const int64_t* ids, size_t n, const char* tag) {
    if (bad_ids(ids, n) || bad_tag(tag)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
        if (n == 0) return PHIG_OK;
        ctx->db->set_tag_by_ids(std::vector<int64_t>(ids, ids + n), tag, true);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_tag_remove(phig_ctx* ctx, const int64_t* ids, size_t n,
                            const char* tag) {
    if (bad_ids(ids, n) || bad_tag(tag)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
        if (n == 0) return PHIG_OK;
        ctx->db->set_tag_by_ids(std::vector<int64_t>(ids, ids + n), tag, false);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_gallery_set(phig_ctx* ctx, const int64_t* ids, size_t n,
                             const char* gallery) {
    if (bad_ids(ids, n) || gallery == nullptr) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
        if (n == 0) return PHIG_OK;
        ctx->db->set_gallery_by_ids(std::vector<int64_t>(ids, ids + n), gallery);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_gallery_clear(phig_ctx* ctx, const int64_t* ids, size_t n) {
    if (bad_ids(ids, n)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
        if (n == 0) return PHIG_OK;
        ctx->db->set_gallery_by_ids(std::vector<int64_t>(ids, ids + n), "");
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

phig_status phig_favorite_set(phig_ctx* ctx, const int64_t* ids, size_t n, bool on) {
    if (bad_ids(ids, n)) return PHIG_ERR_INVALID_ARG;
    PHIG_TRY(ctx)
        if (ctx->mode != PHIG_READ_WRITE) return PHIG_ERR_READONLY;
        if (n == 0) return PHIG_OK;
        ctx->db->set_tag_by_ids(std::vector<int64_t>(ids, ids + n), kFavoriteTag, on);
        return PHIG_OK;
    PHIG_CATCH(ctx)
}

}  // extern "C"
```

- [ ] **Step 6: Run the tests**

Add `src/libphig/mutate.cpp` to `phig_abi` and `tests/test_libphig_mutate.cpp` to `phig-tests`.

Run: `cmake -B build && cmake --build build && ./build/phig-tests --gtest_filter='AbiMutate.*'`
Expected: 7 tests PASS.

If `FavoriteToggleIsVisibleInQuery` fails, `search_page` is not populating
`ImageInfo::favorite` — the existing `search()` derives it from the tag join.
Add the same `EXISTS` sub-select to `search_page`'s SELECT list and to
`row_to_image_info`, and re-run.

- [ ] **Step 7: Commit**

```bash
./build/phig-tests
clang-format -i include/phig/phig.h src/libphig/mutate.cpp src/database.h \
    src/database.cpp tests/test_libphig_mutate.cpp
git add include/ src/libphig/ src/database.h src/database.cpp \
    tests/test_libphig_mutate.cpp CMakeLists.txt
git commit -m "Add libphig metadata mutation ABI"
```

---

### Task 7: Writer lock, docs, and install verification

Closes the two loose ends: ABI writes must take phig's advisory lock, and the library must be consumable from outside the build tree.

**Files:**
- Modify: `src/libphig/mutate.cpp`, `src/libphig/internal.h`
- Modify: `include/phig/phig.h`
- Modify: `README.md`
- Create: `tests/test_libphig_install/CMakeLists.txt`, `tests/test_libphig_install/main.c`
- Test: `tests/test_libphig_mutate.cpp`

**Interfaces:**
- Consumes: `WriterLock` from `src/lockfile.h`; all prior tasks.
- Produces: no new public symbols; adds locking behavior and an install smoke check.

- [ ] **Step 1: Read the lock API**

Open `src/lockfile.h` and note `WriterLock`'s constructor signature and how
`main.cpp` acquires it for mutating commands (grep `WriterLock` in
`src/main.cpp`). Match that usage.

- [ ] **Step 2: Write the failing test**

A second lock holder must make an ABI write fail rather than corrupt or hang.
Append to `tests/test_libphig_mutate.cpp`:

```cpp
#include "lockfile.h"

TEST(AbiMutate, WriteFailsWhileAnotherProcessHoldsTheWriterLock) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    // Hold the lock the way a running CLI job would, then attempt an ABI write.
    WriterLock held(tmp.path, /*wait=*/false);
    ASSERT_TRUE(held.acquired());

    phig_status st = phig_tag_add(ctx, ids.data(), ids.size(), "contended");
    EXPECT_EQ(st, PHIG_ERR_DB);
    EXPECT_STRNE(phig_last_error(ctx), "");

    Database db(tmp.path);
    EXPECT_TRUE(db.get_tags_for_image("/photos/m0.jpg").empty());
    phig_shutdown(ctx);
}
```

Adapt `WriterLock held(...)` and `held.acquired()` to the real signature found
in Step 1. If the class throws instead of exposing `acquired()`, wrap
accordingly.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='AbiMutate.WriteFails*'`
Expected: FAIL — the write currently succeeds because the ABI never takes the lock.

- [ ] **Step 4: Take the lock in mutating ABI calls**

Add a helper to `src/libphig/internal.h`:

```cpp
// Acquires phig's process-wide advisory writer lock for one ABI mutation.
// Non-blocking: a GUI must not freeze behind a long-running CLI job, so a
// contended lock is reported as an error the caller can surface instead.
// Throws std::runtime_error when the lock is held elsewhere.
void phig_acquire_write_lock(phig_ctx* ctx, std::unique_ptr<WriterLock>& out);
```

Implement it in `mutate.cpp` and call it at the top of each mutating function,
after the read-only check:

```cpp
std::unique_ptr<WriterLock> lock;
phig_acquire_write_lock(ctx, lock);
```

The throw is converted to `PHIG_ERR_DB` with a message by `PHIG_CATCH`. Document
the behavior in the header, above the mutation declarations:

```c
/* If another phig process holds the writer lock (e.g. a running scan), these
 * return PHIG_ERR_DB immediately rather than blocking; phig_last_error explains.
 * A GUI should surface that and retry, never freeze waiting. */
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='AbiMutate.*'`
Expected: 8 tests PASS.

- [ ] **Step 6: Add the out-of-tree install smoke test**

This proves the installed header and library are usable by a client that knows
nothing about phig's source tree — the actual contract phig-gtk depends on.

Create `tests/test_libphig_install/main.c`:

```c
/* Consumes the INSTALLED libphig as a plain C client. */
#include <phig/phig.h>
#include <stdio.h>

int main(void) {
    if ((phig_abi_version() >> 16) != PHIG_ABI_VERSION_MAJOR) {
        fprintf(stderr, "ABI major mismatch\n");
        return 1;
    }
    phig_config cfg = {0};
    phig_ctx *ctx = NULL;
    cfg.db_path = "/nonexistent/phig.db";
    cfg.mode = PHIG_READ_ONLY;
    if (phig_init(&cfg, &ctx) == PHIG_OK) {
        fprintf(stderr, "expected init failure on bad path\n");
        phig_shutdown(ctx);
        return 1;
    }
    printf("ok\n");
    return 0;
}
```

Create `tests/test_libphig_install/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(libphig_install_check LANGUAGES C)
find_library(PHIG_LIB phig REQUIRED)
find_path(PHIG_INC phig/phig.h REQUIRED)
add_executable(install_check main.c)
target_include_directories(install_check PRIVATE ${PHIG_INC})
target_link_libraries(install_check PRIVATE ${PHIG_LIB})
```

- [ ] **Step 7: Run the install check**

```bash
cmake --install build --prefix /tmp/phig-prefix
cmake -S tests/test_libphig_install -B /tmp/phig-install-check \
      -DCMAKE_PREFIX_PATH=/tmp/phig-prefix
cmake --build /tmp/phig-install-check
/tmp/phig-install-check/install_check
```

Expected: prints `ok`. If the header is not found, the `install(DIRECTORY include/phig ...)` rule from Task 3 is wrong.

- [ ] **Step 8: Document libphig in the README**

Add a section after "Configuration", written plainly:

```markdown
## libphig

phig's functionality is available to other programs through `libphig`, a C ABI
shared library installed alongside the CLI (`libphig.so` plus
`include/phig/phig.h`). It covers paginated queries, per-image metadata,
thumbnail path lookup, and tag/gallery/favorite writes.

Clients link it instead of reading phig's database directly: the SQLite schema
and the thumbnail cache layout are private and may change, while the header is a
versioned contract (`phig_abi_version()`). Long-running operations — scan,
organize, remove, face detection — are not in the ABI; run them through the CLI,
which holds the writer lock and records the activity log.

Pixel decoding is deliberately absent: phig decodes arbitrary user files with
OpenCV, FFmpeg, and LibRaw, which can crash on malformed input, so a client that
needs decoded images should run it out-of-process.
```

- [ ] **Step 9: Full verification and commit**

```bash
cmake --build build && ./build/phig-tests
nm -D --defined-only build/libphig.so | grep ' T ' | grep -v ' phig_' || echo "clean"
clang-format -i include/phig/phig.h src/libphig/*.cpp src/libphig/internal.h \
    tests/test_libphig_mutate.cpp
git add include/ src/libphig/ tests/ README.md CMakeLists.txt
git commit -m "Take writer lock in libphig mutations; add install check and docs"
```

---

## Plan self-review

**Spec coverage.** Every item in the spec's "libphig C ABI" section maps to a task: lifecycle and versioning (Task 3), query builder / count / page / records / metadata (Task 4), thumbnails (Task 5), mutations (Task 6), and the four "Required phig-side changes" — `effective_date` (Task 1), pagination pushed into SQL (Task 2), criteria-to-SQL refactor (Task 2 Step 3), CMake shared-library target (Task 3 Step 4).

**Deliberately out of scope**, deferred to their own plans:
- `phig_duplicates_page`, `phig_person_list`, `phig_tag_list`, `phig_gallery_list`, `phig_log_page` — the spec lists these under "Additional read surfaces". They follow Task 4's shape exactly and are not needed until the phases that consume them (duplicates review, people/tags sidebar, activity view).
- `--porcelain` progress lines for long-running commands — belongs with phase 5 (Jobs), not this library.
- The `phig-decode` worker pool and the GUI — phases 2 and 3.

**Known gaps to resolve during implementation, not now:** `phig_query_set_person` is in the spec's builder list but the underlying `SearchCriteria` has no person field (face search goes through `search_faces` with an embedding). Wiring person-filtering into the paginated path needs a join that does not exist yet; it is deferred with the other face-related surfaces rather than half-built here.

**Type consistency check.** `Database::SortKey`/`PageCriteria` (Task 2) are the names used in Task 4's `to_sort_key`. `ImageInfo::id` and `ImageInfo::effective_date` are added in Tasks 2 and 1 respectively and consumed in Task 4's `phig_make_image_list`. `set_tag_by_ids`/`set_gallery_by_ids` (Task 6 Step 4) match their call sites in Step 5. `phig_make_image_list` is declared in `internal.h` (Task 4 Step 5) and defined in `records.cpp`. `compute_effective_date` (Task 1) is used in `scanner.cpp` and `database.cpp`.
