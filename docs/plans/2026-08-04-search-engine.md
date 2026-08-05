# SearchEngine Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `cmd_search`'s four query modes out of `main.cpp` into a `SearchEngine` in phig-core, fixing the face-match dedup bug on the way.

**Architecture:** `SearchEngine` follows the established engine pattern (options in, result out, options stored by value, no user-facing prose) but needs none of the concurrency machinery the other three engines required — search is single-threaded, does no database writes, and reports no progress. It is also the first engine that never touches pixels: the caller decodes the query image and passes embeddings or a phash in.

**Tech Stack:** C++20, GCC 15 via Nix devenv, GoogleTest, SQLite (+ sqlite-vec for face KNN), OpenCV (in the CLI only, not the engine).

## Facts already verified — do not re-derive these

Five lookups the tasks below would otherwise each repeat, resolved against the tree on 2026-08-04:

- **`hamming_distance(uint64_t, uint64_t) -> int` is declared in `src/duplicates.h:12`**, not in a hashing header. Include `duplicates.h`.
- **`FaceInfo` lives in `src/faces.h:10`**, not `types.h`. Fields: `std::array<float, 128> embedding;` and `int x, y, width, height;`. All four box ints must be set for a fixture insert.
- **The count flag is `--count`** (`main.cpp:1843`), setting `opts.count_only`. There is no `--count-only`.
- **Only Filter and Similar apply `--match`/`--exclude`/`--limit`.** Verified by reading `main.cpp`: the shared selection-and-limit block lives at `:1130-1143`, and **Person returns at `:984`** while **Face returns at `:1070`** — both before reaching it. So person mode ignores `--match`, `--exclude`, and `--limit` entirely, and face mode ignores selection (it has its own `per_face_limit` handling). Preserve that in both modes; do not add selection to either, and do not "fix" the asymmetry during this extraction.
- **`Database::get_image_id(const std::string&) -> int64_t`** returns -1 when absent; fixtures must `ASSERT_GE(id, 0)` before inserting faces.
- **`Database::search` ends its SQL with `ORDER BY path` and appends `LIMIT n` only when `c.limit > 0`.** So Filter mode is deterministic, and the limit interlock is concrete: pushing `limit` into SQL truncates the *path-ordered* set before path selection runs, which is why `criteria.limit` must stay `-1` while a `--match`/`--exclude` is pending.
- **Equal distances are common in face results, not rare.** The Task 1 baseline has a two-face query returning four matches all at `distance: 0` (duplicate images). Face-result ordering must therefore be deterministic — `dedupe_face_matches` uses `std::stable_sort` for this reason. Do not "simplify" it to `std::sort`.

## Global Constraints

- **Every build, test, and format command must be prefixed with `direnv exec .`** from `~/devel/phig/phig`. Bare `cmake`/`ctest` fails on missing opencv4.
- **CLI flag order is `phig <subcommand> <args> --db <path>`.** `phig --db X search ...` fails with "Unknown command: --db".
- **The engine contains no user-facing prose and no `std::cout`.** All five output formats stay in `main.cpp`.
- **The engine never decodes pixels and never calls OpenCV.** No `decode_image`, no `compute_phash`, no `detect_faces`, no `preload_face_models`. Those stay in the CLI, which passes the engine already-extracted embeddings and an already-computed phash.
- **Options structs are stored by value, not by reference.** A `const SearchQuery& query_` member dangles when constructed from a temporary; this defect has shipped in this project before.
- **`SearchEngine` takes no `CancelToken` and no `ProgressQueue`.** Search is single-shot; adding them would be speculative. Do not add them "for symmetry" with the other engines.
- **Behavior preservation is the acceptance bar for every mode except `--face`.** `--face` result ordering changes because the dedup bug is fixed; every other mode's output must be byte-identical to the pre-change binary.
- **New tests go in `phig-tests`** (the fast suite), not `phig-perf-tests`.
- **CI lints only `find src tests -maxdepth 1`** — `src/libphig/` and `include/` are never auto-linted. Not relevant here (all files are at depth 1), but run the formatter anyway.
- **Comments describe the present.** No "this used to", no plan or task numbers as justification.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/search_engine.h` (create) | `SearchMode`, `SearchQuery`, `SearchResult`, `SearchEngine` declarations |
| `src/search_engine.cpp` (create) | Four query modes, path filtering, limit application, face dedup |
| `src/main.cpp` (modify) | Keeps argv parsing, query-image decode, all five output formats |
| `tests/test_search_engine.cpp` (create) | Engine behavior, including the dedup fix and the limit interlock |
| `CMakeLists.txt` (modify) | Register the new source and test files |

Task order: Task 1 captures the baseline the later tasks are graded against. Task 2 builds the types and the two pure helpers (dedup, path filtering) with no database involved. Task 3 implements the three simple modes. Task 4 implements face mode, which carries the bug fix. Task 5 switches the CLI over and proves output preservation.

---

### Task 1: Capture the pre-change CLI baseline

No production code. This produces the artifact Tasks 3-5 are graded against.

**Files:**
- Create: `/tmp/phig-search-baseline/` (scratch, not committed)

**Interfaces:**
- Consumes: nothing.
- Produces: `/tmp/phig-search-baseline/phig-baseline` (pre-change binary), a scanned database, and one captured output file per mode/format combination. Task 5 diffs against these.

- [ ] **Step 1: Build and stash the current binary**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
mkdir -p /tmp/phig-search-baseline
cp build/phig /tmp/phig-search-baseline/phig-baseline
```

- [ ] **Step 2: Build a scratch collection with face and phash variety**

Keep it small. A prior agent in this project left 287 MB of `/tmp` fixtures and an orphaned `phig scan` at 1041% CPU.

```bash
B=/tmp/phig-search-baseline
mkdir -p $B/coll
cd ~/devel/phig/phig
# Two real faces at different dimensions, plus near-duplicates for phash search.
cp tests/data/faces_small.jpg $B/coll/face_a.jpg
cp tests/data/faces_large.jpg $B/coll/face_b.jpg
cp tests/data/faces_small.jpg $B/coll/face_a_copy.jpg
cp tests/data/test_red.mov $B/coll/clip.mov
for i in 1 2 3; do cp tests/data/faces_large.jpg $B/coll/dup$i.jpg; done
DB=$B/base.db
$B/phig-baseline scan $B/coll --faces --db $DB > /dev/null 2>&1
$B/phig-baseline face rebuild --db $DB > /dev/null 2>&1
sqlite3 $DB "SELECT COUNT(*) FROM images;" | sed 's/^/images: /'
sqlite3 $DB "SELECT COUNT(*) FROM faces;"  | sed 's/^/faces: /'
```

Report both counts. If `faces` is 0, face-mode baselines are meaningless — say so and stop rather than capturing empty output.

- [ ] **Step 3: Capture every mode and format**

```bash
B=/tmp/phig-search-baseline; DB=$B/base.db; P=$B/phig-baseline
# Filter mode, all five renderers
$P search --db $DB                          > $B/filter-text.txt    2>&1
$P search --format csv --db $DB             > $B/filter-csv.txt     2>&1
$P search --format json --db $DB            > $B/filter-json.txt    2>&1
$P search --porcelain --db $DB              > $B/filter-porcelain.txt 2>&1
$P search --print0 --db $DB                 > $B/filter-print0.bin  2>&1
# Filter criteria and the limit interlock
$P search --media image --db $DB            > $B/filter-media.txt   2>&1
$P search --limit 2 --db $DB                > $B/filter-limit.txt   2>&1
$P search --limit 2 --match '*dup*' --db $DB > $B/filter-limit-match.txt 2>&1
$P search --count --db $DB                  > $B/filter-count.txt   2>&1
# Similar mode, both entry points
$P search --similar $B/coll/face_b.jpg --db $DB       > $B/similar-file.txt 2>&1
$P search --similar-hash 0000000000000000 --db $DB    > $B/similar-hash.txt 2>&1
# Person mode (label a face first so it returns rows)
$P face name --db $DB > /dev/null 2>&1 || true
# Face mode — the mode whose ordering this plan intentionally changes
$P search --face $B/coll/face_a.jpg --db $DB          > $B/face-text.txt 2>&1
$P search --face $B/coll/face_a.jpg --format json --db $DB > $B/face-json.txt 2>&1
$P search --face $B/coll/face_a.jpg --limit 2 --db $DB > $B/face-limit.txt 2>&1
wc -c $B/*.txt $B/*.bin
```

`--count` is the correct flag (verified at `main.cpp:1843`).

For person mode: find how `phig face name` takes its arguments (`phig face name --help`), label one face, then capture `search --person <name>`. If labeling cannot be scripted, say so and skip that baseline explicitly rather than faking it.

- [ ] **Step 4: Confirm cleanliness**

```bash
pgrep -a phig || echo "no phig processes: clean"
du -sh /tmp/phig-search-baseline
```

Expected: no processes, total well under 100 MB.

- [ ] **Step 5: Report**

Print the byte size of every captured file and the full contents of `face-text.txt` (the mode whose ordering changes — Task 4 needs to know exactly what the old ranking produced). Nothing to commit.

---

### Task 2: Types and the two pure helpers

Builds the header plus the two decisions that are testable with no database: face-match dedup (the bug fix) and path selection. Splitting these out means the bug fix is proven before any query code exists.

**Files:**
- Create: `src/search_engine.h`, `src/search_engine.cpp`
- Create: `tests/test_search_engine.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Database::SearchCriteria` and `Database::FaceSearchResult` (`src/database.h:128` and the `search_faces` declaration above it), `ImageInfo` (`src/types.h`), `passes_selection` (`src/selection.h`).
- Produces: `SearchMode`, `SearchQuery`, `SearchResult`, `SearchEngine` (declared, not yet defined), and the free function `std::vector<Database::FaceSearchResult> dedupe_face_matches(std::vector<Database::FaceSearchResult>)`. Tasks 3-5 use all of these.

- [ ] **Step 1: Write the failing test**

Create `tests/test_search_engine.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "search_engine.h"

namespace {
Database::FaceSearchResult match(const std::string& path, float distance) {
    Database::FaceSearchResult r;
    r.path = path;
    r.distance = distance;
    r.face_x = r.face_y = r.face_w = r.face_h = 0;
    return r;
}
}  // namespace

TEST(SearchEngineDedupe, KeepsTheClosestMatchPerPathNotTheFirstSeen) {
    // The defect this fixes: matches arrive as one sorted run per query face, so
    // dedupe spans separate runs. A photo matching query face A at 0.8 and query
    // face B at 0.3 must be kept at 0.3, not at whichever face was detected first.
    std::vector<Database::FaceSearchResult> in{match("/p/both.jpg", 0.8f),
                                               match("/p/other.jpg", 0.5f),
                                               match("/p/both.jpg", 0.3f)};

    auto out = dedupe_face_matches(in);

    ASSERT_EQ(out.size(), 2u);
    // Sorted ascending by distance, so the deduped 0.3 entry comes first.
    EXPECT_EQ(out[0].path, "/p/both.jpg");
    EXPECT_FLOAT_EQ(out[0].distance, 0.3f);
    EXPECT_EQ(out[1].path, "/p/other.jpg");
    EXPECT_FLOAT_EQ(out[1].distance, 0.5f);
}

TEST(SearchEngineDedupe, SortsAscendingByDistance) {
    std::vector<Database::FaceSearchResult> in{match("/p/c.jpg", 0.9f), match("/p/a.jpg", 0.1f),
                                               match("/p/b.jpg", 0.5f)};

    auto out = dedupe_face_matches(in);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_FLOAT_EQ(out[0].distance, 0.1f);
    EXPECT_FLOAT_EQ(out[1].distance, 0.5f);
    EXPECT_FLOAT_EQ(out[2].distance, 0.9f);
}

TEST(SearchEngineDedupe, PreservesTheFaceBoxOfTheClosestMatch) {
    // Keeping the minimum distance must carry that match's box, not the box of
    // the entry it replaced — a caller drawing the rectangle needs the right one.
    Database::FaceSearchResult far_match = match("/p/x.jpg", 0.8f);
    far_match.face_x = 11;
    Database::FaceSearchResult near_match = match("/p/x.jpg", 0.2f);
    near_match.face_x = 77;

    auto out = dedupe_face_matches({far_match, near_match});

    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].distance, 0.2f);
    EXPECT_EQ(out[0].face_x, 77);
}

TEST(SearchEngineDedupe, EmptyInputYieldsEmptyOutput) {
    EXPECT_TRUE(dedupe_face_matches({}).empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add `src/search_engine.cpp` and `tests/test_search_engine.cpp` to `CMakeLists.txt` in this step (source list near `src/faces_engine.cpp`; test list near `tests/test_faces_engine.cpp`), so the failure is a missing header rather than a silently-skipped file.

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j 2>&1 | tail -20
```

Expected: FAIL — `search_engine.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/search_engine.h`:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "database.h"
#include "types.h"

// Which query to run. Exactly one mode is active per call.
enum class SearchMode { Filter, Person, Face, Similar };

struct SearchQuery {
    SearchMode mode = SearchMode::Filter;

    // Reused rather than restated: Database::SearchCriteria already declares
    // every filter field the CLI populates. A parallel copy here would be a
    // second struct to keep in sync.
    //
    // `filter.limit` is the SQL-level limit and is only set when no path
    // selection is pending — see `limit` below.
    Database::SearchCriteria filter;

    // Path selection and the final limit apply to every mode, not just Filter,
    // so they sit outside `filter`. `limit` is applied after path selection;
    // pushing it into SQL while a selection is pending would truncate before
    // filtering and return fewer rows than asked for.
    std::vector<std::string> matches, excludes;
    int limit = -1;

    // Person mode.
    std::string person;

    // Face mode: embeddings the CALLER extracted from the query image. The
    // engine neither decodes nor detects — it holds no OpenCV dependency at all,
    // so a malformed query file cannot crash it and a GUI caller chooses its own
    // decode isolation.
    std::vector<std::array<float, 128>> face_embeddings;
    float face_threshold = 0.6f;

    // Similar mode: a phash the caller computed from a file or parsed from a hex
    // argument.
    uint64_t phash = 0;
    int phash_threshold = 5;
};

// Which vector is populated depends on the mode. One result type carries all
// three rather than three near-identical types.
//
// Person mode yields bare paths because Database::get_images_for_person returns
// std::vector<std::string>; widening it to ImageInfo would change what
// --format json emits for --person, which is a user-visible change unrelated to
// moving this code.
struct SearchResult {
    std::vector<ImageInfo> images;                       // Filter and Similar
    std::vector<Database::FaceSearchResult> matches;     // Face
    std::vector<std::string> paths;                      // Person
};

// Collapses face matches to one entry per path, keeping the SMALLEST distance,
// then sorts ascending by distance.
//
// Matches arrive as one sorted run per query face, so a path can appear in
// several runs at different distances. Deduplicating on first-seen would rank an
// image by whichever query face happened to be detected first rather than by its
// best match, and under a limit could drop it entirely.
//
// Free function, not a private method, so the rule is testable without a
// database or face models.
std::vector<Database::FaceSearchResult> dedupe_face_matches(
    std::vector<Database::FaceSearchResult> matches);

// Runs one query against a database. Reads only: no writes, no transaction, no
// mutex. Single-threaded and single-shot, so it takes no CancelToken and
// publishes no progress.
class SearchEngine {
   public:
    SearchEngine(const SearchQuery& query, Database& db);

    SearchResult run();

   private:
    // By value: constructing from a temporary would otherwise dangle.
    SearchQuery query_;
    Database& db_;
};
```

- [ ] **Step 4: Write the minimal implementation**

Create `src/search_engine.cpp` with only the free function. `SearchEngine::run` lands in Tasks 3-4.

```cpp
#include "search_engine.h"

#include <algorithm>
#include <unordered_map>

std::vector<Database::FaceSearchResult> dedupe_face_matches(
    std::vector<Database::FaceSearchResult> matches) {
    std::unordered_map<std::string, size_t> best;  // path -> index into `out`
    std::vector<Database::FaceSearchResult> out;
    out.reserve(matches.size());

    for (auto& m : matches) {
        auto it = best.find(m.path);
        if (it == best.end()) {
            best.emplace(m.path, out.size());
            out.push_back(std::move(m));
        } else if (m.distance < out[it->second].distance) {
            out[it->second] = std::move(m);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Database::FaceSearchResult& a, const Database::FaceSearchResult& b) {
                  return a.distance < b.distance;
              });
    return out;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='SearchEngineDedupe.*'
```

Expected: 4 PASS.

- [ ] **Step 6: Mutation-check the fix — required gate**

Cap: two mutations, on the riskiest assertion.

1. Replace the `else if (m.distance < ...)` branch with a bare `continue` (restoring first-seen-wins) → `KeepsTheClosestMatchPerPathNotTheFirstSeen` must FAIL.
2. Delete the `std::sort` → `SortsAscendingByDistance` must FAIL.

Revert both. If either survives, the test asserts shape rather than value — strengthen it before continuing.

- [ ] **Step 7: Format and commit**

```bash
cd ~/devel/phig/phig
direnv exec . clang-format -i src/search_engine.h src/search_engine.cpp tests/test_search_engine.cpp
git add src/search_engine.h src/search_engine.cpp tests/test_search_engine.cpp CMakeLists.txt
git commit -m "phig-core: add SearchEngine types and closest-match face dedup"
```

---

### Task 3: Filter, Person, and Similar modes

The three modes with no bug fix attached. Each is a faithful move.

**Files:**
- Modify: `src/search_engine.cpp`
- Modify: `tests/test_search_engine.cpp`

**Interfaces:**
- Consumes: everything Task 2 produced; `Database::search(const SearchCriteria&) -> std::vector<ImageInfo>` (`database.h:145`); `Database::get_images_for_person(const std::string&) -> std::vector<std::string>` (`database.h:237`); `Database::get_all_images()`; `hamming_distance(uint64_t, uint64_t) -> int` (`src/duplicates.h:12`); `passes_selection` (`src/selection.h`).
- Produces: `SearchEngine::run` handling three of four modes. Task 4 adds Face.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_search_engine.cpp`. The fixture inserts rows directly — no scanning, no decoding — because the engine only reads the database.

```cpp
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace fs = std::filesystem;

namespace {
// A scratch database with rows inserted directly. The engine reads only, so no
// real files and no decoding are needed.
struct ScratchSearch {
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    ScratchSearch() {
        dir = fs::temp_directory_path() / ("phig_search_engine_" + std::to_string(::getpid()) +
                                           "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }
    ~ScratchSearch() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Distinct sha per row so nothing collides.
    std::string add(const std::string& rel, const std::string& ext, uint64_t phash,
                    const std::string& media = "image", int64_t size = 1000) {
        ImageInfo info;
        info.path = (dir / rel).string();
        info.filename = fs::path(rel).filename().string();
        info.extension = ext;
        info.file_size = size;
        info.media_type = media;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = std::string(63, '0') + std::to_string(db->count() % 10);
        info.phash = phash;
        info.width = 100;
        info.height = 100;
        info.metadata_json = "{}";
        db->insert(info);
        return info.path;
    }
};
}  // namespace

TEST(SearchEngineFilter, ReturnsEveryRowWhenNoCriteriaAreSet) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0);
    s.add("b.jpg", "jpg", 0);
    SearchQuery q;
    EXPECT_EQ(SearchEngine(q, *s.db).run().images.size(), 2u);
}

TEST(SearchEngineFilter, MediaCriterionExcludesVideos) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0, "image");
    s.add("clip.mov", "mov", 0, "video");
    SearchQuery q;
    q.filter.media = "image";
    auto r = SearchEngine(q, *s.db).run();
    ASSERT_EQ(r.images.size(), 1u);
    EXPECT_EQ(r.images[0].extension, "jpg");
}

TEST(SearchEngineFilter, MatchPatternSelectsBySubpath) {
    ScratchSearch s;
    s.add("keep_1.jpg", "jpg", 0);
    s.add("drop_1.jpg", "jpg", 0);
    SearchQuery q;
    q.matches.push_back("*keep*");
    auto r = SearchEngine(q, *s.db).run();
    ASSERT_EQ(r.images.size(), 1u);
    EXPECT_NE(r.images[0].path.find("keep_1"), std::string::npos);
}

TEST(SearchEngineFilter, LimitAppliesAfterPathSelectionNotBeforeIt) {
    // The interlock: pushing `limit` into SQL while a selection is pending would
    // truncate the wrong rows and return fewer than asked for. Four rows, two of
    // which match; a limit of 2 must yield exactly the 2 matching rows.
    ScratchSearch s;
    s.add("drop_1.jpg", "jpg", 0);
    s.add("drop_2.jpg", "jpg", 0);
    s.add("keep_1.jpg", "jpg", 0);
    s.add("keep_2.jpg", "jpg", 0);
    SearchQuery q;
    q.matches.push_back("*keep*");
    q.limit = 2;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.images.size(), 2u);
    for (const auto& img : r.images) {
        EXPECT_NE(img.path.find("keep_"), std::string::npos) << img.path;
    }
}

TEST(SearchEngineFilter, LimitTruncatesWhenNoSelectionIsPending) {
    ScratchSearch s;
    for (int i = 0; i < 5; ++i) s.add("f" + std::to_string(i) + ".jpg", "jpg", 0);
    SearchQuery q;
    q.limit = 3;
    EXPECT_EQ(SearchEngine(q, *s.db).run().images.size(), 3u);
}

TEST(SearchEngineSimilar, ReturnsRowsWithinThresholdSortedByDistance) {
    ScratchSearch s;
    s.add("exact.jpg", "jpg", 0xFFFFFFFFFFFFFFFFull);   // distance 0
    s.add("near.jpg", "jpg", 0xFFFFFFFFFFFFFFFEull);    // distance 1
    s.add("far.jpg", "jpg", 0x0000000000000000ull);     // distance 64
    SearchQuery q;
    q.mode = SearchMode::Similar;
    q.phash = 0xFFFFFFFFFFFFFFFFull;
    q.phash_threshold = 5;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.images.size(), 2u);
    EXPECT_NE(r.images[0].path.find("exact"), std::string::npos) << "closest must sort first";
    EXPECT_NE(r.images[1].path.find("near"), std::string::npos);
}

TEST(SearchEngineSimilar, ThresholdIsInclusive) {
    ScratchSearch s;
    s.add("at_threshold.jpg", "jpg", 0xFFFFFFFFFFFFFFF0ull);  // distance 4
    SearchQuery q;
    q.mode = SearchMode::Similar;
    q.phash = 0xFFFFFFFFFFFFFFFFull;
    q.phash_threshold = 4;
    EXPECT_EQ(SearchEngine(q, *s.db).run().images.size(), 1u);
}

TEST(SearchEnginePerson, UnknownPersonYieldsNoPaths) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0);
    SearchQuery q;
    q.mode = SearchMode::Person;
    q.person = "nobody-by-this-name";
    auto r = SearchEngine(q, *s.db).run();
    EXPECT_TRUE(r.paths.empty());
    EXPECT_TRUE(r.images.empty()) << "person mode fills `paths`, not `images`";
}
```

Verify the distances in the `Similar` tests before relying on them: `0xFFFF...FE` differs from `0xFFFF...FF` in one bit (distance 1) and `0xFFFF...F0` in four bits (distance 4). If `hamming_distance` has a different convention, fix the fixture values and say so.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j 2>&1 | tail -20
```

Expected: FAIL — undefined reference to `SearchEngine::SearchEngine` and `SearchEngine::run`.

- [ ] **Step 3: Implement the three modes**

Append to `src/search_engine.cpp`. This is a faithful move of `main.cpp:919-1158` minus the face branch and minus all output. Read that range before writing.

```cpp
#include "selection.h"
// plus whatever header declares hamming_distance

SearchEngine::SearchEngine(const SearchQuery& query, Database& db) : query_(query), db_(db) {}

SearchResult SearchEngine::run() {
    SearchResult result;
    const bool selecting = !query_.matches.empty() || !query_.excludes.empty();

    switch (query_.mode) {
        case SearchMode::Person:
            // Person mode applies neither path selection nor the limit, mirroring
            // the CLI: its branch returns before the shared selection block, so
            // --match, --exclude, and --limit have never affected person results.
            result.paths = db_.get_images_for_person(query_.person);
            return result;

        case SearchMode::Similar: {
            struct Scored {
                ImageInfo info;
                int distance;
            };
            std::vector<Scored> scored;
            for (auto& img : db_.get_all_images()) {
                int dist = hamming_distance(query_.phash, img.phash);
                if (dist <= query_.phash_threshold) scored.push_back({std::move(img), dist});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const Scored& a, const Scored& b) { return a.distance < b.distance; });
            for (auto& s : scored) result.images.push_back(std::move(s.info));
            break;
        }

        case SearchMode::Face:
            // Task 4.
            break;

        case SearchMode::Filter: {
            Database::SearchCriteria criteria = query_.filter;
            // Only push the limit into SQL when no path selection is pending:
            // truncating before selection would return fewer rows than asked for.
            criteria.limit = selecting ? -1 : query_.limit;
            result.images = db_.search(criteria);
            break;
        }
    }

    if (selecting) {
        result.images.erase(std::remove_if(result.images.begin(), result.images.end(),
                                           [&](const ImageInfo& img) {
                                               return !passes_selection(img.path, query_.matches,
                                                                        query_.excludes);
                                           }),
                            result.images.end());
    }
    if (query_.limit > 0 && static_cast<int>(result.images.size()) > query_.limit) {
        result.images.resize(query_.limit);
    }
    return result;
}
```

Note the CLI sets `criteria.limit = opts.limit` only when no selection is pending, and otherwise leaves it at its `-1` default. Reproduce that exactly — do not pass `query_.limit` through unconditionally.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='SearchEngine*'
```

Expected: all PASS (4 from Task 2, 8 new).

- [ ] **Step 5: Mutation-check the limit interlock — required gate**

Cap: two mutations.

1. Change `criteria.limit = selecting ? -1 : query_.limit;` to `criteria.limit = query_.limit;` → `LimitAppliesAfterPathSelectionNotBeforeIt` must FAIL.
2. Delete the post-selection `resize` → `LimitTruncatesWhenNoSelectionIsPending` must FAIL.

Revert both. Report the observed result of each.

- [ ] **Step 6: Format and commit**

```bash
cd ~/devel/phig/phig
direnv exec . clang-format -i src/search_engine.cpp tests/test_search_engine.cpp
git add src/search_engine.cpp tests/test_search_engine.cpp
git commit -m "phig-core: implement SearchEngine filter, person, and similar modes"
```

---

### Task 4: Face mode

The mode carrying the bug fix. It is the only mode whose CLI output legitimately changes.

**Files:**
- Modify: `src/search_engine.cpp`
- Modify: `tests/test_search_engine.cpp`

**Interfaces:**
- Consumes: `dedupe_face_matches` from Task 2; `Database::search_faces(const std::array<float,128>&, float threshold, int limit) -> std::vector<FaceSearchResult>`; `Database::insert_face`, `Database::insert` for fixtures.
- Produces: `SearchEngine::run` handling all four modes.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_search_engine.cpp`. Insert face rows directly with synthetic embeddings — no models, no decoding.

```cpp
#include <array>

namespace {
// A 128-float embedding that is all `v`. Two such embeddings are at a
// predictable distance from each other, which is what makes threshold and
// ordering assertions checkable without real face data.
std::array<float, 128> flat_embedding(float v) {
    std::array<float, 128> e{};
    e.fill(v);
    return e;
}
}  // namespace

TEST(SearchEngineFace, ReturnsMatchesInMatchesNotImages) {
    ScratchSearch s;
    std::string p = s.add("a.jpg", "jpg", 0);
    int64_t id = s.db->get_image_id(p);
    ASSERT_GE(id, 0);
    FaceInfo f;
    f.embedding = flat_embedding(0.1f);
    s.db->insert_face(id, f);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.1f));
    q.face_threshold = 10.0f;  // generous: this test is about plumbing, not ranking

    auto r = SearchEngine(q, *s.db).run();

    EXPECT_FALSE(r.matches.empty()) << "face mode fills `matches`";
    EXPECT_TRUE(r.images.empty()) << "face mode must not fill `images`";
}

TEST(SearchEngineFace, CrossBatchConflictKeepsTheClosestMatch) {
    // The bug this fixes, end to end.
    //
    // Both the decoy image and `limit = 2` are load-bearing. `search_faces` runs
    // once per query embedding and sorts within a call, so a bug that keeps the
    // first-seen match is only observable when a path's BEST match arrives in a
    // LATER batch than a worse one. With `per_face_limit = 2`, query 1's batch is
    // [decoy@0.0, target@4.53] — the target appears only via its poor face — and
    // query 2's exact match on the target arrives afterwards.
    //
    // Do not simplify this fixture. A single-image version cannot fail: query 1's
    // batch would already contain both of that image's faces sorted ascending, so
    // it would hold the true minimum and there would be nothing to lose. A larger
    // limit pulls the target's exact face into batch 1 and hides the bug the same
    // way. Measured: closest-wins yields 0.0, first-seen-wins yields 4.53.
    ScratchSearch s;
    std::string target = s.add("target.jpg", "jpg", 0);
    int64_t tid = s.db->get_image_id(target);
    ASSERT_GE(tid, 0);
    FaceInfo poor;
    poor.embedding = flat_embedding(0.50f);
    poor.x = poor.y = poor.width = poor.height = 1;
    s.db->insert_face(tid, poor);
    FaceInfo exact;
    exact.embedding = flat_embedding(0.10f);
    exact.x = exact.y = exact.width = exact.height = 1;
    s.db->insert_face(tid, exact);

    std::string decoy = s.add("decoy.jpg", "jpg", 0);
    int64_t did = s.db->get_image_id(decoy);
    ASSERT_GE(did, 0);
    FaceInfo d;
    d.embedding = flat_embedding(0.90f);
    d.x = d.y = d.width = d.height = 1;
    s.db->insert_face(did, d);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.90f));  // exact on decoy, poor on target
    q.face_embeddings.push_back(flat_embedding(0.10f));  // exact on target
    q.face_threshold = 100.0f;
    q.limit = 2;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.matches.size(), 2u);
    float target_distance = -1.0f;
    for (const auto& m : r.matches) {
        if (m.path.find("target") != std::string::npos) target_distance = m.distance;
    }
    EXPECT_NEAR(target_distance, 0.0f, 1e-3)
        << "kept distance must be the closest match, not the first batch's";
}

TEST(SearchEngineFace, RespectsTheDistanceThreshold) {
    ScratchSearch s;
    std::string p = s.add("a.jpg", "jpg", 0);
    int64_t id = s.db->get_image_id(p);
    FaceInfo f;
    f.embedding = flat_embedding(0.9f);
    s.db->insert_face(id, f);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.1f));
    q.face_threshold = 0.001f;  // far too tight for this pair

    EXPECT_TRUE(SearchEngine(q, *s.db).run().matches.empty());
}

TEST(SearchEngineFace, AppliesTheLimitAfterDeduplication) {
    // One image carries TWO faces that both match the single query embedding, so
    // it contributes two rows to the one batch; a second image contributes one.
    // With limit = 2, applying the limit BEFORE dedupe truncates to two rows that
    // are the same path and collapses to 1 result; applying it after dedupe
    // yields 2 distinct paths.
    //
    // Do not simplify to one face per image. With no repeated path, dedupe is a
    // no-op and this test passes whichever order the limit is applied in.
    ScratchSearch s;
    std::string twice = s.add("twice.jpg", "jpg", 0);
    int64_t tid = s.db->get_image_id(twice);
    ASSERT_GE(tid, 0);
    FaceInfo a;
    a.embedding = flat_embedding(0.10f);
    a.x = a.y = a.width = a.height = 1;
    s.db->insert_face(tid, a);
    FaceInfo b;
    b.embedding = flat_embedding(0.11f);
    b.x = b.y = b.width = b.height = 1;
    s.db->insert_face(tid, b);

    std::string other = s.add("other.jpg", "jpg", 0);
    int64_t oid = s.db->get_image_id(other);
    ASSERT_GE(oid, 0);
    FaceInfo c;
    c.embedding = flat_embedding(0.12f);
    c.x = c.y = c.width = c.height = 1;
    s.db->insert_face(oid, c);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.10f));
    q.face_threshold = 100.0f;
    q.limit = 2;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.matches.size(), 2u);
    EXPECT_NE(r.matches[0].path, r.matches[1].path) << "both entries must be distinct paths";
}

TEST(SearchEngineFace, NoQueryEmbeddingsYieldsNoMatches) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0);
    SearchQuery q;
    q.mode = SearchMode::Face;
    EXPECT_TRUE(SearchEngine(q, *s.db).run().matches.empty());
}
```

`FaceInfo` is in `src/faces.h` with `embedding` plus `int x, y, width, height` — set all four box ints (any non-negative values) so `insert_face` gets a well-formed row.

The exact distances `sqlite-vec` returns for these synthetic embeddings are not specified here. `OneImageMatchingTwoQueryFacesIsKeptAtItsCLOSESTDistance` asserts the *closest* distance is kept, which holds whatever the absolute values are, and an exact-match pair should be ~0. If the KNN returns something surprising, report the actual numbers rather than loosening the assertion until it passes.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='SearchEngineFace.*'
```

Expected: FAIL — face mode currently returns an empty result (the Task 3 stub).

- [ ] **Step 3: Implement face mode**

Replace the `case SearchMode::Face:` stub in `src/search_engine.cpp`:

```cpp
        case SearchMode::Face: {
            // One KNN query per detected query face, then collapse to one entry
            // per path keeping the closest distance. `search_faces` sorts within
            // a single call, so the runs must be merged rather than concatenated.
            const int per_face_limit = query_.limit > 0 ? query_.limit : 100;
            std::vector<Database::FaceSearchResult> all;
            for (const auto& embedding : query_.face_embeddings) {
                auto batch = db_.search_faces(embedding, query_.face_threshold, per_face_limit);
                all.insert(all.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            }
            result.matches = dedupe_face_matches(std::move(all));
            if (query_.limit > 0 && static_cast<int>(result.matches.size()) > query_.limit) {
                result.matches.resize(query_.limit);
            }
            return result;
        }
```

Add `#include <iterator>` if needed.

The early `return result;` is deliberate and must stay: it bypasses the shared path-selection and limit block below. Face mode does not apply `--match`/`--exclude` in the current CLI either — its branch returns at `main.cpp:1070`, before that block — so returning here preserves behavior exactly. Do not add path selection to face mode.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ./build/phig-tests --gtest_filter='SearchEngine*'
```

Expected: all PASS (17 total).

- [ ] **Step 5: Prove the fix against the old behavior — required gate**

Reintroduce first-seen-wins inside `dedupe_face_matches` (replace the `else if` with `continue`), rebuild, and confirm `OneImageMatchingTwoQueryFacesIsKeptAtItsCLOSESTDistance` FAILS. This is the test that proves the extraction fixed a real defect rather than relocating it. Revert and confirm it passes. Report the exact failure output.

- [ ] **Step 6: Format and commit**

```bash
cd ~/devel/phig/phig
direnv exec . clang-format -i src/search_engine.cpp tests/test_search_engine.cpp
git add src/search_engine.cpp tests/test_search_engine.cpp
git commit -m "phig-core: implement SearchEngine face mode with closest-match dedup"
```

---

### Task 5: Switch the CLI over and prove output preservation

**Files:**
- Modify: `src/main.cpp` — `cmd_search` at `:919-1217` and `SearchOptions` at `:798`

**Interfaces:**
- Consumes: `SearchEngine`, `SearchQuery`, `SearchResult`, `SearchMode` from Tasks 2-4.
- Produces: nothing later depends on this.

- [ ] **Step 1: Read the whole function before changing it**

```bash
cd ~/devel/phig/phig
sed -n '798,830p' src/main.cpp    # SearchOptions
sed -n '919,1217p' src/main.cpp   # cmd_search
```

- [ ] **Step 2: Rewire `cmd_search`**

Keep in `main.cpp`, unchanged:
- `SearchOptions` and its argv parsing. It stays as-is; the CLI translates it into a `SearchQuery`.
- The stdin (`"-"`) handling and `TempCleanup` for `--face`/`--similar`.
- `preload_face_models()`, `decode_image`, `detect_faces` for `--face`, and `compute_phash` / `parse_hex_phash` for `--similar`. **The engine must not gain these.** The CLI decodes, detects, and passes `face_embeddings` or `phash` in.
- The `db.count() == 0` empty-database check and its message.
- Every output format (print0, porcelain, csv, json, human-readable) and `--count`.
- The "No faces detected in: ..." and "Cannot decode image: ..." errors — these concern the query file, which is the CLI's responsibility now.

Replace only the query dispatch: build a `SearchQuery`, construct a `SearchEngine`, call `run()`, then render from `SearchResult`.

The mode is chosen exactly as the original chose its branch: person if `--person`, else face if `--face`, else similar if `--similar` or `--similar-hash`, else filter. Preserve that precedence — it is observable when flags are combined.

Face mode renders from `result.matches` (which carries `distance` and the box), person mode from `result.paths`, the other two from `result.images`. That matches how the original had a separate output block for face search.

- [ ] **Step 3: Build and run the full suite**

```bash
cd ~/devel/phig/phig
direnv exec . cmake --build build -j
direnv exec . ctest --test-dir build 2>&1 | tail -6
```

Report the count.

- [ ] **Step 4: Prove every non-face mode is byte-identical**

Recreate the Task 1 collection with the NEW binary, then diff against the stored baselines. Use the same commands Task 1 used, writing to `/tmp/phig-search-verify`.

```bash
B=/tmp/phig-search-baseline; V=/tmp/phig-search-verify
rm -rf $V && mkdir -p $V/coll
cd ~/devel/phig/phig
cp tests/data/faces_small.jpg $V/coll/face_a.jpg
cp tests/data/faces_large.jpg $V/coll/face_b.jpg
cp tests/data/faces_small.jpg $V/coll/face_a_copy.jpg
cp tests/data/test_red.mov $V/coll/clip.mov
for i in 1 2 3; do cp tests/data/faces_large.jpg $V/coll/dup$i.jpg; done
DB=$V/base.db
./build/phig scan $V/coll --faces --db $DB > /dev/null 2>&1
./build/phig face rebuild --db $DB > /dev/null 2>&1
# Re-run each non-face capture from Task 1 against $DB, writing into $V,
# then diff. Paths differ between the two trees, so normalize before comparing:
for f in filter-text filter-csv filter-json filter-porcelain filter-media \
         filter-limit filter-limit-match filter-count similar-file similar-hash; do
  diff <(sed "s|$B/coll|COLL|g" $B/$f.txt) <(sed "s|$V/coll|COLL|g" $V/$f.txt) \
    && echo "$f: identical" || echo "$f: DIFFERS"
done
cmp $B/filter-print0.bin $V/filter-print0.bin && echo "print0: identical"
```

`print0` needs `cmp`, not `diff`, because of the NUL separators — and its paths differ between trees, so normalize it the same way or capture both from the same tree. Any `DIFFERS` on a non-face mode is a real regression: investigate rather than explaining it away.

- [ ] **Step 5: Show the face-mode change is exactly the intended one**

`--face` output changes by design. There are **two** face baselines, and they serve different purposes — diff both.

```bash
B=/tmp/phig-search-baseline; V=/tmp/phig-search-verify; DB=$V/base.db
# One query face: nothing to deduplicate, so this must stay identical.
./build/phig search --face $V/coll/face_a.jpg --db $DB > $V/face-text.txt 2>&1
diff <(sed "s|$B/coll|COLL|g" $B/face-text.txt) <(sed "s|$V/coll|COLL|g" $V/face-text.txt) \
  && echo "face (1 query face): identical — expected"
# Two query faces: this is where the fix shows.
./build/phig search --face $V/coll/face_b.jpg --db $DB > $V/face-2q-text.txt 2>&1
diff <(sed "s|$B/coll|COLL|g" $B/face-2q-text.txt) <(sed "s|$V/coll|COLL|g" $V/face-2q-text.txt)
```

`face_a.jpg` has one face; with a single query embedding there is nothing to dedupe, so **that output must be byte-identical**. A difference there is a regression.

`face_b.jpg` has **two** faces at distinct boxes (`184,76 24x32` and `165,7 16x20`, verified by SQL). Querying with it produces two embeddings, and every matched image also has two faces. In the old output all four matches report `distance: 0  face: 184,76 24x32` — the *same* box for every image, because first-seen dedup kept query face 1's result and discarded query face 2's. Half those boxes belong to the wrong face.

For that diff, report: the **set** of 4 paths (must be unchanged — the threshold did not move) and the reported **boxes** (may change to each image's closest-matching face). Distances tie at 0 in this fixture, so ordering may look unaffected; the box is the visible signal. If the set of paths changes, that is NOT the intended change — investigate.

- [ ] **Step 6: Clean up and commit**

```bash
cd ~/devel/phig/phig
rm -rf /tmp/phig-search-verify
pgrep -a phig || echo "no phig processes: clean"
direnv exec . clang-format -i src/main.cpp
git add src/main.cpp
git commit -m "search: render from SearchEngine instead of querying inline"
```

---

## Review Calibration

**Full-depth review:** Task 4 (the bug fix and the KNN merge) and Task 5 (the CLI switchover, where output preservation is the contract).

**Single-pass review:** Tasks 1, 2, 3.

**Skip re-review:** test-only or comment-only diffs verifiable in one command.

**Keep regardless:** both mutation questions ("what mutation makes my test fail?" and "what change to the implementation *survives* the whole suite?"), proving behavior preservation against the Task 1 baseline, and requiring implementers to state what they could **not** verify.

**Do not:** re-measure what the implementer already measured unless load-bearing; exceed two mutations per task; leave a `phig` process running.

**Known false positive:** subagents that commit may trip a "no explicit user approval" warning because the authorization is not in their own transcript. Per-task commits are authorized for this plan. **Pushing and merging are not.**

---

## Self-Review

**Spec coverage.** The design's interface split (query fields into the engine, the five presentation fields staying in the CLI) is Task 2's header plus Task 5's rewiring. Decision 1 (the engine never decodes) is enforced by Global Constraints and by Task 5 Step 2 listing the decode calls that stay. Decision 2 (fix the dedup bug) is Task 2's `dedupe_face_matches` plus Task 4's end-to-end test and its Step 5 gate. The Person-mode asymmetry and the limit interlock each have a named test.

**Placeholder scan.** No TBDs. Every code step carries real code. Four steps deliberately end in "verify and report" rather than a fixed answer, each naming what to check: the `--count` flag's real spelling, `hamming_distance`'s header and convention, `FaceInfo`'s field names, and whether the original applied `--match` to face results. Each is a fact in the tree the implementer must read, and each says what to do with the answer.

**Type consistency.** `SearchMode`/`SearchQuery`/`SearchResult`/`SearchEngine` and `dedupe_face_matches` are consistent across Tasks 2-5. `Database::SearchCriteria` is composed, not restated. `Database::FaceSearchResult` is used with its real fields (`path`, `distance`, `face_x/y/w/h`) — verified in `database.h`; there is no `FaceBox` type. `get_images_for_person` is used with its real `std::vector<std::string>` return type.

**One pre-existing oddity noted, not fixed.** `ImageInfo::phash` defaults to 0, so videos and unhashed rows carry 0. A `--similar-hash 0000000000000000` query therefore matches every such row at distance 0. This is current behavior, it is captured in the Task 1 baseline as `similar-hash.txt`, and this plan preserves it. Fixing it would change output and belongs in its own change.
