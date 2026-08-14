---
name: libphig Feature Parity
status: in-progress
created: 2026-08-08
closes_rules: [R62]
description: >
  The gap between what phig can do and what a client can reach. Answers the
  five cross-cutting questions the inventory shares — how a caller names its
  subject, whether a collection-wide read is a request or an operation, how a
  destructive operation is authorized, what a face is to a client, where a
  similarity query's phash comes from — then builds the selection surface those
  answers imply and closes the small gaps. The four large items are 062-065.
---

# 053 — libphig Feature Parity

## Goal

Make `libphig` reach all of phig's features. Root `CLAUDE.md`'s Interface
discipline states the rule: a capability that exists only in `core/` and `cli/`
is a gap with a schedule, not a boundary on what a client may do.

It is not a demand that every `core/` function become an entry point. The test
is whether a client can accomplish what phig can accomplish, through a surface
shaped for a client rather than for argv.

## Decisions

Five questions this plan held open while it was `designing`. Each is answered
below with what settled it. The reasoning lives here rather than in the plans
that consume it because 052 has to answer the same questions the same way, or
S2 learns two vocabularies.

### 1. A collection-wide read is an operation, not a request

`find_near_duplicates` compiled standalone against a synthetic corpus, threshold
5, on an M-series Mac:

| images | ms |
|---|---|
| 30,000 | 72 |
| 100,000 | 532 |
| 250,000 | 3,821 |
| 500,000 | 16,614 |
| 1,000,000 | 69,406 |

Above 100k it is roughly 4x per doubling. Measured twice — once on a corpus with
10% near-identical bursts, once all-unique — and the two agreed within 5%, so
this is the algorithm at these sizes rather than a property of the corpus. The
banded index splits a 64-bit phash into six bands at threshold 5, so around
10 bits per band; above ~100k images the buckets saturate and it degrades
toward all-pairs.

69 s cannot be a blocking ABI call or an HTTP request. Duplicates is a
`phig_op*` with progress and cancellation. It reads like a request today only
because 30k lands at 72 ms.

This also answers 052's open question 1 and gives 044's finding 8 the
measurement it asked for. Note what it does not do: an operation makes 69 s
tolerable, not fast. The algorithm's behaviour above 100k is 044's to fix.

### 2. Selection is ids everywhere; globs are query predicates

The ABI had three conventions: writes took `int64_t*` ids, operations took
`match`/`exclude`, and `phig_query` took neither, which is why there is no
filename search.

Globs earn a place in queries and nowhere else. A filename search cannot be
expressed as ids — resolving it to ids *is* the search. Everything whose
subject is existing rows takes ids:

```c
void        phig_query_add_match(phig_query* q, const char* glob);
void        phig_query_add_exclude(phig_query* q, const char* glob);
phig_status phig_query_ids(phig_ctx* ctx, const phig_query* q,
                           int64_t** out, size_t* n);
```

`phig_query_ids` resolves any query, not just a glob, so "every RAW from 2021
tagged keep" is one call and a glob joins the eleven filter setters already on
`phig_query` rather than becoming a parallel selection language.

Three things that were assumed and turn out not to hold:

- **Globs are not evaluated late.** `ThumbsEngine::run`
  (`core/thumbs_engine.cpp:24-32`) calls `get_all_images()` and filters in
  application memory — 044's finding 6. Ids and globs resolve at the same
  moment.
- **Ids are cheaper, not dearer.** 1M ids is 8 MB against the 111.6 MB 044
  measured `get_all_images()` already paying at 3.8 KB/row. An id list lets an
  engine load only selected rows, which moves toward 044's findings 5 and 6
  rather than against them.
- **A large id list needs no `IN (...)`.** `set_gallery_by_ids`
  (`core/database.cpp:1547`) steps one prepared statement per id inside a
  transaction, so SQLite's bound-parameter ceiling is not a constraint.

Index is the one surface that keeps a directory: it walks the filesystem, and
its subject is files that have no row yet.

Globs must be evaluated **in SQL**. `search_page` applies `LIMIT`/`OFFSET`, so
filtering afterwards returns short pages and skips rows. phig's `glob_match`
and SQLite's `GLOB` disagree — phig has backslash escapes, SQLite has `[...]`
character classes — so `glob_match` is registered as a custom SQLite function
rather than translating patterns and silently changing what `--match` means.

### 3. `images.id` has to be stable first

**Done — schema v9, R72 holds.** Decision 2 makes an id the way every caller
names a photo, and before v9 an id did not name one for long. `images.id` was
`INTEGER PRIMARY KEY` with no `AUTOINCREMENT`, so SQLite reused it. Against the
system SQLite 3.51.0 phig links:

```
before: 1 /a.jpg   2 /b.jpg   3 /c.jpg
DELETE /c.jpg, INSERT a different file
after:  1 /a.jpg   2 /b.jpg   3 /NEW-DIFFERENT-FILE.jpg
```

R51 gives `faces.id` this property and plan 045 migrated the column to get it.
Nothing gave it to `images.id`, so the ABI was handing ids out through
`phig_query_page` and taking them back in `phig_tag_add`, `phig_favorite_set`
and `phig_gallery_set` with nothing guaranteeing they still named the same
photo.

Schema v9 migrates the column. **It could not reuse 045's recipe**, and 045
said so at the time (`core/database.cpp:121`): that rebuild is safe with
`foreign_keys=ON` only because `faces` is a pure FK child. `image_tags.image_id`
and `faces.image_id` are both `REFERENCES images(id) ON DELETE CASCADE` and
`PRAGMA foreign_keys=ON` is set on every open, and SQLite's `DROP TABLE`
deletes every row before dropping — so the rebuild would have taken every
`image_tags` row and every `faces` row with it. Measured: `tags` and `people`
survive, every assignment to them does not, and the face rows go with their
embeddings, boxes and person labels.

`init_schema` therefore runs **every** migration with foreign keys off rather
than conditioning it on the version number, and calls `foreign_key_violations`
inside each migration's transaction before the commit. That check does not
cover the cascade case — deleted children leave no orphans, so it reports zero
violations against an emptied database — which is why the real-behavior test
required by root `CLAUDE.md` is the guard:
`ImagesMigrationV9.PreservesRowsIdsTagsFavoritesFacesAndLabels`, confirmed to
fail with the pragma re-enabled.

### 4. A destructive operation is authorized by plan/apply

Already settled by R66 and `libphig/CLAUDE.md`, with organize as the worked
example since 042. No entry point takes a confirmation callback: none may run
on an engine thread under handle-plus-poll, and none has meaning over 052's
wire protocol. `cmd_rm`'s `ConfirmFn` is a `cli/` affordance, not a model.

What blocks remove is not the shape but R61 — `cmd_rm` writes its preview to
`std::cout` as prose and returns an `int`, so there is nothing for a caller to
inspect. Turning that preview into a value is 064's first task.

### 5. A face is a box, and a phash comes from phig

The premise that these reopen the pixel-decode question does not hold.
`phig_thumb_generate` already decodes — its header says so: "Decodes pixels, so
this is slow relative to every other call in this header: hundreds of
milliseconds for a large RAW." What libphig lacks is any entry point that hands
back a *pixel buffer*; it decodes internally and delivers results as cache
files or values.

So a phash computed from a file fits the existing pattern, and no client is
asked to reimplement phig's phash bit-for-bit. Face crops fit it too, but boxes
are cheaper and `Database::FaceSearchResult` already carries `face_x/y/w/h`, so
the ABI returns boxes and a client crops.

## Scope

1. **Schema v9 and R72** — `images.id` to `AUTOINCREMENT`, per decision 3.
   Everything else depends on it.
2. **Query selection** — `phig_query_add_match`, `phig_query_add_exclude`,
   `phig_query_ids`, and `glob_match` registered as a SQLite function so
   selection is applied before `LIMIT`.
3. **Operations take ids** — `phig_thumbs_options` and `phig_faces_options`
   drop `match`/`exclude` for an id list, as does organize. The engines change
   with them, and `cli/` resolves globs to ids before calling in, which is the
   same work it does today with the resolution no longer hidden inside each
   engine.
4. **The other three search modes, paged natively.** Person, Similar and Face
   live in `SearchEngine`, which returns whole result sets, while `phig_query`
   pages through `search_page`. All three page in SQL rather than materializing:
   - **Person** — `get_images_for_person` gains a paged form with a stable
     `ORDER BY`.
   - **Similar** — `hamming` registered as a SQLite function, then
     `WHERE hamming(phash, ?) <= ? ORDER BY hamming(phash, ?), id`. This also
     removes the `get_all_images()` load at `core/search_engine.cpp:56`.
   - **Face** — the hard one. `dedupe_face_matches` keeps the smallest distance
     per path across per-face batches, so a rank cannot be computed from one
     page. Native paging means expressing dedup and rank in SQL: the per-face
     queries `UNION ALL`, then `GROUP BY image_id`, `MIN(distance)`,
     `ORDER BY`, `LIMIT`/`OFFSET`. Two candidates for the per-face part, and
     the choice is measured, not reasoned: `vec_faces` KNN with a `k`, which
     keeps 044's direction but retains the truncation weakness
     `core/search_engine.h:62-72` documents, or a full scan over the face rows,
     which is exact and removes it — 4,147 faces at 29,925 images, per 044.
5. **An image's tags, and the tag and gallery lists** — `get_tags_for_image`,
   `get_tag_counts`, `get_gallery_counts`. 047's sidebar sections and the
   viewer's tag chips need these, and everything above them in 047 depends on
   them.
6. **Face model download** — `models.h`'s `download_face_models`. Without it
   the ABI starts a faces operation that fails because ~150 MB of models are
   absent and offers no way to fetch them.
7. **`thumbs clean`** — `is_thumbnail_filename` is in `core/`; the loop is in
   `cli/main.cpp` and moves to `core/` first.
8. **R62** — the entry points that return something other than `phig_status`
   have no handler, and the ten `phig_query_set_*` functions are named in the
   record. This plan adds more setters of exactly that shape, so it closes the
   hole it would otherwise widen.

ABI versioning is not a design constraint: nothing outside the repo links
libphig, and `gtk/` — the only in-repo client — uses `phig_init`,
`phig_shutdown`, `phig_last_error`, `phig_query_{new,free,count,page,set_sort}`,
`phig_image*`, `phig_thumb_lookup` and `phig_thumb_generate`, and no operation
entry point at all. Signatures are reshaped in place rather than accreting
`_ids` variants. The version macros still have to agree with CMake's
`VERSION`/`SOVERSION`, because the rules gate checks that.

## Work order

1. Schema v9 + R72, with its real-behavior migration tests — done, executed via
   `docs/plans/2026-08-14-053-schema-v9.md`. R72 holds.
2. `phig_query_ids` and the glob predicates, on the SQLite function.
3. Operations converted to ids; `cli/` resolves ahead of them.
4. Items 5, 6, 7 — small, independent, unblocked once 2 lands.
5. The three search modes.
6. R62's handlers and its gate.

## Not in scope

- **Duplicates, people and faces, remove, the activity log** — 062, 063, 064,
  065. All four block on this plan's selection surface.
- **Organize (cp/mv)** — plan 042, shipped.
- **Scan's reconcile phase** — plan 041.
- **`ScanOptions::thumbs`/`faces`** — the standing exclusion. The ABI models
  index, thumbs and faces as independent operations and those flags refuse
  that.
- **Duplicate detection above 100k images** — decision 1 measured it; 044 owns
  the algorithm.
- **Handing a client a pixel buffer** — still unresolved, and decision 5 does
  not decide it.
- **Features phig does not have yet.** 009 (watch), 013 (geo search), 021
  (import) and 028 (`.phigignore`) each own their ABI exposure.

## Relationship to other plans

- **047** — the demand side, and the reason for the order above.
- **052** — needs every capability here over the wire and must answer the five
  decisions identically. Decision 1 answers its open question 1.
- **041, 042** — the two ABI-surface gaps already owned.
- **044** — owns pagination at scale, the `get_all_images` column trim that
  decision 2 leans on, and duplicate detection above 100k.
- **045** — the worked example decision 3 mirrors, and the record of why its
  recipe does not transfer to `images`.
- **031, 034** — the operation lifecycle, progress convention, and the
  ctx/error/threading rules everything here follows.
- **060** — owns what a faces rebuild may destroy, and R66's remaining gap.
- **062-065** — the four items this plan hands off.
