# SearchEngine extraction — design

**Status:** approved, ready to plan
**Date:** 2026-08-04
**Follows:** `2026-08-01-phig-core-extraction-design.md` (Parts 1, 2a, 2b-i, 2b-ii all merged)

## Why

`cmd_search` is the last command in `main.cpp` holding real domain logic, and at
298 lines it is the file's largest function. It is also the query path every
future client needs first: a GTK gallery view is a search with filters, and
`phig-web`'s M0 is search plus thumbnails.

Extracting it finishes what Parts 1 through 2b-ii started. After this, what
remains in `main.cpp` is argv parsing, usage text, and output rendering — which
the extraction design says stays there.

## What makes this different from the previous engines

`SearchEngine` is structurally the **simplest** engine so far, despite
`cmd_search` being the largest function. Verified by reading the code:

| | ScanEngine | ThumbsEngine | FacesEngine | SearchEngine |
|---|---|---|---|---|
| `parallel_for_each` | yes | yes | yes | **no** |
| `ProgressQueue` | yes | yes | yes | **no** |
| `CancelToken` | yes | yes | yes | **no** |
| Atomic counters | yes | yes | yes | **no** |
| Writes to the DB | yes | no | yes | **no** |
| Decodes pixels | yes | yes | yes | **no** (see below) |

`cmd_search` is single-threaded with no cancellation and no progress reporting.
It performs no database writes. So `SearchEngine` needs none of the concurrency
machinery the other three required, and its result is a plain value.

37 of the 298 lines write output. Those stay in the CLI.

## Two decisions taken before planning

### 1. The engine never decodes pixels

`cmd_search` decodes exactly **one** file — the user's query image — at two
sites: `decode_image(face_path)` for `--face` and `compute_phash(similar_path)`
for `--similar`. Everything else is read from the database: `search_faces`
queries stored embeddings, and `--similar` compares against `get_all_images()`.
**No collection files are decoded.**

Rather than move that decode into the engine, the **caller decodes and passes
data in**. `SearchQuery` carries an already-decoded `cv::Mat` for face search and
an already-computed `uint64_t` phash for similarity search. The engine is then a
pure database-query component with no OpenCV decode path at all.

This is the cleanest boundary of any engine so far, and it means a GUI or
`libphig` caller chooses its own isolation strategy for the query file — which is
what the out-of-process rule is protecting. It also makes the engine trivially
testable: a test supplies an embedding or a phash directly, with no fixture file
and no models on disk.

Consequence the plan must handle: `preload_face_models()` and `detect_faces()`
stay on the CLI side of the boundary, because they operate on the decoded query
image before the engine is called.

### 2. The face-match dedup bug is fixed during the extraction

`main.cpp:1009` deduplicates face matches by path **before** sorting by distance:

```cpp
// Deduplicate by path — keep the closest match per image
if (seen_paths.insert(m.path).second) {   // keeps FIRST-seen, not closest
```

`Database::search_faces` returns vec0 KNN results sorted by distance **per
call**, but the loop calls it once per detected query face, so the dedup spans
separate sorted lists. The comment states an invariant the code does not hold.

Failure case: a query photo containing two people, and a collection photo
containing both. If query face A matches it at distance 0.8 and query face B at
0.3, the 0.8 match is kept because face A was processed first. The image is
ranked by detection order rather than match quality, and under `--limit` it can
be dropped entirely.

The fix is to keep the minimum distance per path, then sort. **This changes
`--face` result ordering**, so the extraction cannot claim byte-identical output
for that mode. The acceptance bar for `--face` is "identical except where the old
ranking was wrong, proven by a test that fails against the old behavior."

All other modes keep byte-identical output.

## Interface

`SearchOptions` in `main.cpp:798` mixes query criteria with presentation. The
split is the core of this design.

**Moves into the engine** (query): the 14 filter fields that
`Database::SearchCriteria` already declares — `extension`, `after`, `before`,
`camera`, `make`, `min_size`, `max_size`, `media`, `min_duration_ms`,
`max_duration_ms`, `tags`, `gallery`, `min_sharpness`, `max_sharpness` — plus
`threshold`, `face_threshold`, `person`, `matches`, `excludes`, and `limit`.

**Stays in the CLI** (presentation): `count_only`, `porcelain`, `print0`,
`format`, `output`. Also `similar`, `similar_hash`, and `face` — these are
*paths and hex strings to be resolved*, and resolving them is the caller's job
per decision 1.

```cpp
// Which query the engine runs. Exactly one mode is active per call.
enum class SearchMode { Filter, Person, Face, Similar };

struct SearchQuery {
    SearchMode mode = SearchMode::Filter;

    // Reused, not restated. Database::SearchCriteria already holds every filter
    // field cmd_search populates — extension, after, before, camera, make,
    // min_size, max_size, media, min/max_duration_ms, tags, gallery,
    // min/max_sharpness, limit (verified at database.h:128-144). Declaring a
    // parallel copy here would be two structs to keep in sync.
    Database::SearchCriteria filter;

    // Path selection and the post-filter limit apply to Filter and Similar
    // ONLY. Verified against main.cpp: the shared selection-and-limit block is
    // at :1130-1143, but Person returns at :984 and Face at :1070, so neither
    // reaches it. Person ignores --match/--exclude/--limit entirely; Face has
    // its own per_face_limit and applies no selection.
    //
    // `filter.limit` is the SQL-level limit and is only set when no
    // match/exclude is pending — see the limit interlock below.
    std::vector<std::string> matches, excludes;
    int limit = -1;

    // Person mode.
    std::string person;

    // Face mode: embeddings the CALLER extracted from the query image. The
    // engine neither decodes nor detects.
    std::vector<std::array<float, 128>> face_embeddings;
    float face_threshold = 0.6f;

    // Similar mode: phash the CALLER computed (from a file) or parsed (from
    // --similar-hash).
    uint64_t phash = 0;
    int phash_threshold = 5;
};

// Face mode returns distances and face boxes; the other modes do not. One
// result type carries both rather than three near-identical ones.
//
// `matches` reuses Database::FaceSearchResult (path, distance, face_x, face_y,
// face_w, face_h — verified in database.h) rather than declaring a parallel
// type. There is no `FaceBox` type in this codebase.
struct SearchResult {
    // Populated for Filter and Similar modes.
    std::vector<ImageInfo> images;
    // Populated for Face mode only, already deduplicated by path (keeping the
    // minimum distance) and sorted ascending by distance.
    std::vector<Database::FaceSearchResult> matches;
    // Populated for Person mode only — see the asymmetry note below.
    std::vector<std::string> paths;
};

class SearchEngine {
   public:
    SearchEngine(const SearchQuery& query, Database& db);
    SearchResult run();

   private:
    SearchQuery query_;   // by value; a const& member dangles from a temporary
    Database& db_;
};
```

### The Person-mode asymmetry

`Database::get_images_for_person(const std::string&)` returns
`std::vector<std::string>` — bare paths, not `ImageInfo` (verified at
`database.h:237`). So `--person` cannot today emit the same columns as the other
modes, and `SearchResult` has three populated-by-mode vectors instead of one.

**Decision: keep the asymmetry for this extraction.** Widening
`get_images_for_person` to return `ImageInfo` would change what `--format json`
and `--porcelain` emit for `--person`, which is a user-visible output change
unrelated to moving code. It is worth doing — a GUI needs more than a path to
render a gallery row — but it belongs in its own change with its own output
comparison, not folded into an extraction whose acceptance bar is unchanged
output.

The plan should state this explicitly so a reviewer does not read the three-vector
`SearchResult` as sloppiness. If it grates, the follow-up is a one-line
`Database` method returning `ImageInfo` and a JSON/porcelain output diff.

## The limit interlock

`cmd_search` applies `limit` in two places, and the interaction is easy to break:

```cpp
// Don't apply limit in DB if we also need match/filter (apply after)
if (opts.matches.empty() && opts.excludes.empty()) {
    criteria.limit = opts.limit;
}
...
// after passes_selection filtering
if (opts.limit > 0 && results.size() > limit) results.resize(limit);
```

Pushing `limit` into SQL while a `--match` filter is pending would truncate
before filtering and return fewer rows than asked for. This needs an explicit
test: a limit combined with a match pattern that excludes some of the first N
rows.

## Out of scope

- No `ProgressQueue`, `CancelToken`, or threading. Search is fast and
  single-shot; adding them would be speculative.
- No new `libphig` ABI surface. The C ABI already has query entry points; whether
  they should route through `SearchEngine` is a later question, and the extraction
  design's own rule is to defer that until a client asks.
- `cmd_search`'s argv parsing, usage text, and all five output formats stay in
  `main.cpp`.
