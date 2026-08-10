---
name: Large Collection Scale
status: planned
created: 2026-08-07
description: >
  Fix the places where phig materializes a whole collection in memory or does
  per-item work that scales with collection size, so it works on libraries of
  millions of items rather than tens of thousands. Audited 2026-08-07: the two
  worst offenders are the sibling-RAW check making scan quadratic per directory
  (measured) and `auto_label_face` full-scanning every labeled embedding per
  detected face — neither was in the original list. Ordered work items below.
---

# 044 — Large Collection Scale

phig is designed for media collections with potentially millions of items. Parts
of it are written as though a collection fits in memory, and parts do per-item
work whose cost grows with the size of the collection. That is not a correctness
bug today, and it is not a hypothetical either — it is a shape that stops
working somewhere between the tens of thousands and the millions.

Audited 2026-08-07 across `core/` and `libphig/`. Findings below, in the order
they should be fixed.

## Measurement baseline

Against the real collection (29,925 images, 4,147 faces, 34 MB of metadata JSON):

- Loading every row and discarding it — `thumbs rebuild` with a non-matching
  glob, which reaches `get_all_images()` and nothing else — peaks at **111.6 MB
  RSS**, about 3.8 KB per row. Linear extrapolation: ~3.7 GB at 1M images.
- `duplicates --type near` peaks at **148 MB** and runs in 1.3 s.

Against a synthetic directory of identical 32x32 JPEGs, timing a **no-op rescan**
(every file unchanged, so per-file work is a stat, a sibling-RAW check, and one
DB lookup):

| Layout | Time | Per file |
|---|---|---|
| 1,000 files, 1 directory | 0.52 s | 0.52 ms |
| 4,000 files, 1 directory | 7.33 s | 1.83 ms |
| 4,000 files, 4 directories | 2.03 s | 0.51 ms |

Same files, same work. The only variable is how many files share a directory.

A generator for a realistic large collection is still outstanding — the above is
enough to attribute finding 1, not enough to validate a fix at a million items.

## Findings

### 1. Scan is quadratic in the number of files per directory

`has_sibling_raw()` (`core/scan_engine.cpp:267`) runs for every file on every
scan, before the mtime skip check. It calls `sibling_raw_path()`
(`core/scanner.cpp:114`), which does a full `fs::directory_iterator` over the
parent directory. N JPEGs in one directory means N listings of N entries.

Reconcile pays it twice more: `run_reconcile`'s `any_of(has_sibling_raw)`
(`core/scan_engine.cpp:531`) and `demote_jpeg_rows` (`core/relink.cpp:52`) each
re-walk the same directories.

The measurement above attributes it exactly: 4,000 files cost 7.33 s in one
directory and 2.03 s split across four, with per-file cost flat at ~0.51 ms
whenever directory size is held at 1,000. Extrapolating, a single directory
holding 20,000 photos costs minutes to rescan when nothing has changed.

Direction: one directory listing per directory, keyed by stem, instead of one
listing per JPEG.

**This is not a free optimization.** The sibling-RAW answer decides whether a
JPEG's row is demoted and deleted, so a stale cache entry destroys user-authored
metadata. The scan walk is concurrent, and files can appear during it. Ships
with real-behavior tests covering a RAW arriving mid-scan and a cache entry
going stale.

### 2. `auto_label_face` full-scans every labeled embedding, per detected face

`core/database.cpp:1871` selects all labeled faces and compares in C++, with no
index and without `vec_faces`. It is called once per detected face from inside
the open write transaction at `core/scan_engine.cpp:241` and `:354`, and from
`core/faces_engine.cpp:173` — while holding `db_mutex`, so it serializes the
workers.

This is worse than `identify_person` (finding 4): identify is an explicit
command a user chooses to run, this runs on every scan with `--faces` and every
faces rebuild.

### 3. No index on `faces(person_id)`

Only `idx_faces_image` exists (`core/database.cpp:310`, `:409`). Four queries
full-scan the faces table for want of it: `get_unlabeled_face_embeddings`,
`get_face_embeddings_for_person`, `list_people` — which `identify_person` calls
only to turn a name into an id — and `auto_label_face`.

One forward migration, no behavior change. Cheapest item here and a prerequisite
for judging any of the face work by measurement.

### 4. Face identification loads everything and compares it pairwise

`get_unlabeled_face_embeddings()` (`core/database.cpp:1847`) pushes every
matching row into a `std::vector<FaceEmbeddingRow>` — 128 floats plus an id,
~520 bytes each. A few million unlabeled faces is over a gigabyte resident
before a single comparison runs. `get_face_embeddings_for_person`
(`core/database.cpp:1818`) has the same shape, bounded by how many faces one
person has.

`identify_person` (`core/faces_engine.cpp:39-46`) then compares them
O(candidates × references) with an early break on first match. At 2M candidates
and 50 references that is up to 100M distance computations over 128 floats each.
`vec_faces` has KNN search and `search_faces` already uses it; identify does not.

The cost falls on the command itself, not on the rest of the machine: `face
identify` takes no writer lock (`cmd_face`, `cli/main.cpp`), so a comparison
phase that scales with the collection delays no other mutating command. That
makes this a throughput problem for identify, not a contention one.

### 5. `get_all_images` carries a metadata blob none of its callers read

`core/database.cpp:1195` selects `metadata` for every row. Of the callers,
`ThumbsEngine` (`core/thumbs_engine.cpp:24`) needs path/sha256/media_type,
`FacesEngine` (`core/faces_engine.cpp:71`) needs path, `remove_exec`
(`core/remove_exec.cpp:76`) and `select_paths` (`cli/main.cpp:1609`) need path
alone. None read `metadata_json`. `SearchEngine`'s similar-mode
(`core/search_engine.cpp:56`) needs phash plus the row it returns.

Trimming the column list is independent of the streaming question in finding 6
and needs no semantic decision. It is most of the 3.8 KB/row measured above.

### 6. Selection filtering happens in application memory

`ThumbsEngine`, `FacesEngine`, `rm` and organize all pull every row and then
apply match/exclude globs. `passes_selection`'s glob semantics do not translate
directly to SQL, but a path-prefix push-down is available where a glob has a
literal prefix, and finding 5 reduces the cost of what remains.

### 7. libphig's exposure

The read surface is already correct: `phig_query_page`/`phig_query_count` page
properly and no ABI call returns "every X". Two real issues:

- `phig_op_thumbs_start` and `phig_op_faces_start` (`libphig/operations.cpp:125`,
  `:150`) run the engines from finding 5, so a foreign caller inherits the full
  materialization inside the library.
- `search_page` (`core/database.cpp:2102`) selects `metadata` for every row and
  `row_to_image_info` copies it into a `std::string`, but `phig_image` has no
  metadata field — the ABI serves metadata separately via
  `phig_image_metadata`. Every page reads and discards ~240 KB at a 200-row
  page. `search_page` has exactly one caller, so the fix is contained.

### 8. Near-duplicate brute-force fallback is O(n²)

`core/duplicates.cpp:110` and `:175` fall back to an all-pairs scan at threshold
> 7 or when candidate lists are dense. At 1M images that is ~5×10¹¹ distance
checks. The banded index itself is sound (measured in its own right — see the
comments there), but both paths take the whole collection by value.

Needs its own measurement before a direction is chosen.

### 9. Lower priority

- Every statement is re-prepared; there is no prepared-statement cache. Scan
  pays 3-5 `sqlite3_prepare_v2` calls per file. A constant factor, not a shape.
- `search_page` uses OFFSET, which is O(offset) per page, and no index backs
  `SortKey::Filename` or `SortKey::Size`. Matters to a libphig caller paging
  deeply; no such caller exists yet.

## Correctness constraints

Resource usage and performance do not come at the cost of data safety. Three
constraints bind the work above:

- **The sibling-RAW cache decides deletions.** See finding 1.
- **KNN is approximate.** Moving `auto_label_face` and `identify_person` onto
  `vec_faces` swaps an exact nearest neighbour for an approximate one. Labels
  are user-visible and awkward to unpick, so this ships with an equivalence test
  at the current threshold, not just a speed measurement.
- **Streaming changes snapshot semantics.** The engines currently read a stable
  snapshot and then mutate. `FacesEngine` and the remove executor mutate rows
  they are iterating, so naive paging can see a row twice or miss it. Reading
  ids first and processing by id avoids that; a plain `LIMIT`/`OFFSET` loop over
  a mutating table does not.

## Work order

1. Index on `faces(person_id)` — forward migration, no behavior change.
2. Column trim on `get_all_images`, plus a paths-only query for the callers that
   want paths (finding 5, and `select_paths`).
3. Sibling-RAW directory cache (finding 1) — biggest measured win, needs the
   data-safety tests.
4. `auto_label_face` and `identify_person` onto `vec_faces` KNN (findings 2, 4),
   with the equivalence test.
5. `search_page` column trim (finding 7).
6. Stream or page the engines (findings 5, 6) — needs the snapshot decision
   above resolved first.
7. Duplicates at scale (finding 8) — measure before designing.

Items 1, 2, 3 and 5 are mechanical and measurable against the current
collection. Items 4, 6 and 7 want the synthetic large collection first.

## Relationship to other plans

- **Plan 031** — `identify_person`'s two-phase transaction split landed there
  and is orthogonal to this: the transaction behavior does not change when the
  read path is paged. `core/faces_engine.h` carries a SCALE note pointing here.
- **Plan 010** (face recognition) — introduced `vec_faces` and the embedding
  storage findings 2-4 examine.
- **Plan 040** (multi-size thumbnail cache) — measured cache growth at
  collection scale; the same measure-first approach applies.
- **Plan 046** (vec_faces delete failure) — three `DatabaseTest` cases fail on
  master inside `delete_faces_for_image`. Any vec_faces work here runs against
  that known failure, not on top of a clean suite.
