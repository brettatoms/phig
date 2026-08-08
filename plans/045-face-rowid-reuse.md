---
name: faces.id Rowid Reuse
status: completed
created: 2026-08-07
description: >
  `faces.id` is `INTEGER PRIMARY KEY` without `AUTOINCREMENT`, so SQLite hands a
  deleted rowid back to the next insert. Any read-then-write that remembers a
  face id across a gap can land on a different face than the one it read —
  silently attaching a person label to a face that was never compared. Schema v8
  migrates the column to AUTOINCREMENT, which removes the hazard at the source
  and lets `face name`, `face identify` and `face rebuild` drop the writer lock;
  the lock-free rebuild then has to tolerate an image deleted underneath it.
---

# 045 — faces.id Rowid Reuse

## The defect

`core/database.cpp` declares faces as:

```sql
CREATE TABLE IF NOT EXISTS faces (
    id          INTEGER PRIMARY KEY,
    ...
)
```

`INTEGER PRIMARY KEY` without `AUTOINCREMENT` is an alias for the rowid, and
SQLite assigns `max(rowid) + 1` on insert. Delete the highest row and the next
insert takes its id back.

That makes a face id unstable as a reference across any window where another
writer can delete and reinsert. A full `face rebuild` does exactly that, per
image, for the whole collection — deleting and reinserting at the tail of the
table is its steady state.

## Why it matters

`identify_person` (`core/faces_engine.cpp`) is two-phase by design: it compares
embeddings with no transaction open, then commits labels in one. The gap between
the phases is where a remembered id can be reused. Phase 2 would then write a
person label onto a face phase 1 never examined — wrong user-authored metadata,
written silently, with no error and nothing to detect it afterward.

`tests/test_faces_identify.cpp` demonstrates the reuse directly in
`RowidsAreReusedAfterADelete`. That test asserts today's behavior; when this plan
lands it should start failing and be inverted.

## What plan 031's review already changed here

Plan 031 exposes a faces operation through libphig that deliberately takes no
writer lock. That broke the mitigation this plan was filed against: the CLI's
lock across `face identify` and `face rebuild` only excludes other lock-takers,
so an ABI caller running a rebuild beside a CLI `face identify` reached the
hazard the lock was supposed to hide. Two changes landed with 031 in response:

- **`identify_person` defends itself.** Phase 2 writes through
  `Database::set_face_person_if_unchanged`, whose `UPDATE` matches on
  `person_id IS NULL` and on the embedding blob phase 1 compared. A recycled
  rowid carrying a different face fails the predicate and updates zero rows,
  which `labeled` already counts correctly. Covered by
  `AReusedRowidIsNotLabeledBecauseItsEmbeddingChanged` and, end to end against a
  concurrent rebuild, `ConcurrentRebuildNeverGetsAFaceLabeledItDidNotMatch`
  (measured: detects the defect in 9 of 10 runs when the guard is removed).
- **`phig face name` kept its lock**, which plan 031 had initially dropped on
  the incorrect grounds that it is a single atomic UPDATE. It is not — it reads
  a face id from `find_closest_face`, then creates a person, then updates. That
  is the same read-then-write window, and unlike identify it has no embedding
  from the stored row to match on, so it cannot defend itself the same way.

So the remaining exposure is `face name` against a lock-free ABI faces
operation. That is what this plan removes at the source.

## Measurements

Harness at `docs/plans/2026-08-07-045-rowid-reuse-bench.cpp` (build command in
its header), built against the same Apple SQLite 3.51.0 and vendored sqlite-vec
the build links, using phig's `faces`/`vec_faces`/`images`/`people` DDL and
phig's connection setup (WAL, 30s busy timeout, `foreign_keys=ON`). macOS arm64,
2026-08-07. Every number below is re-derivable by running it.

**The migration is cheap and linear.** 2.0 µs/row — 2.01s for 1,000,000 faces,
1.9 µs/row at 50,000. It preserves row count, `max(id)`, `person_id`, and the
`vec_faces` row count; leaves zero orphaned vectors; seeds `sqlite_sequence` to
`max(id)` without being told to; KNN still returns a row afterward; and a
delete-then-insert at the tail no longer reuses.

**AUTOINCREMENT costs about 5% on the insert path**, which is what `face
rebuild` pays forever after (200,000 inserts into an empty table, 5 reps, min,
configurations alternated rep by rep):

| | plain rowid | AUTOINCREMENT | delta |
|---|---|---|---|
| faces only | 0.662s | 0.711s | +7.5% |
| faces + vec_faces | 1.566s | 1.646s | +5.1% |

**`PRAGMA foreign_keys=OFF` is ignored inside the migration transaction**, as
expected — `Database::init_schema` opens `BEGIN IMMEDIATE` around every
migration step and the pragma is a no-op in a transaction. The recipe below
runs to completion anyway, because `faces` is a pure FK child.

**Test state at `7c59d2d`:** 690 tests pass (`ctest --parallel`, 12.9s), 12
skipped (face models absent, cross-device move). The three `DatabaseTest`
failures in CLAUDE.md's Known issues do not reproduce on macOS against Apple
SQLite 3.51.0, so that failure is platform- or version-specific — see 046.

## Scope

### 1. Schema v8 — `faces.id` to AUTOINCREMENT

Change the fresh-schema DDL, bump `CURRENT_SCHEMA_VERSION` to 8, and add the
migration as a plain string in `migrations()`. No C++ special-casing (as v6
needed) — the recipe works with `foreign_keys` on:

```sql
CREATE TABLE faces_new (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
  embedding BLOB NOT NULL, x INTEGER, y INTEGER, width INTEGER, height INTEGER,
  person_id INTEGER REFERENCES people(id));
INSERT INTO faces_new(id, image_id, embedding, x, y, width, height, person_id)
  SELECT id, image_id, embedding, x, y, width, height, person_id FROM faces;
DROP TABLE faces;
ALTER TABLE faces_new RENAME TO faces;
CREATE INDEX idx_faces_image ON faces(image_id);
```

Columns are listed explicitly rather than `SELECT *` because the two origins
differ in column order: the pre-migration path creates `faces` without
`person_id` and adds it by `ALTER TABLE`. `vec_faces` is not touched — ids are
preserved, so every `face_id` it holds stays valid.

### 2. Drop the writer lock from `face name`, `identify` and `rebuild`

`cli/main.cpp` takes it for all three. After v8 none of them needs it:

- `name`'s only reason was the reuse hazard, which is gone.
- `identify` already documents the lock as throughput, not correctness — its
  write re-checks the embedding, proven by
  `ConcurrentRebuildNeverGetsAFaceLabeledItDidNotMatch`.
- `rebuild`'s delete-then-reinsert is already atomic per image under
  `BEGIN IMMEDIATE` (`core/faces_engine.cpp`), so two concurrent rebuilds
  serialize per image and each converges on a complete face set.

The lock never gave full exclusion regardless: libphig's faces operation takes
none, so a CLI rebuild and an ABI faces op already run together.

### 3. A lock-free rebuild must tolerate a vanishing image

This is what makes 2 safe, not a nicety. Today the lock is the only thing
stopping `phig rm --hard` from deleting image rows during a rebuild. Without it,
the worker reads `image_id` outside its transaction, `rm` deletes the row, and
the next `insert_face` violates the foreign key — measured, `rc=19 FOREIGN KEY
constraint failed`. `parallel_for_each` stores the first exception and sets
`failed`, which every other worker checks at the top of its loop, so **one
deleted image abandons the entire rebuild**.

Move the image-existence check inside the per-image transaction. `BEGIN
IMMEDIATE` holds the write lock, so a re-check inside it cannot be invalidated
by another writer. If the image is gone: roll back, skip it, and count and
report it the way the undecodable-file path already does. Do not throw.

### 4. `face name` must not report success it did not achieve

It discards `set_face_person`'s return and prints `Named face as "X"`
unconditionally. `database.h` documents that return as "false when no row
matched — the face was deleted since the caller looked it up", and names this
exact misuse. Check it and report honestly.

## Tests

- Invert `RowidsAreReusedAfterADelete`: the next insert must get a fresh id.
- Re-target `AReusedRowidIsNotLabeledBecauseItsEmbeddingChanged` — its
  `ASSERT_EQ(reused, doomed)` premise dies with v8. The embedding guard stays:
  `AnUnchangedFaceIsStillLabeled` shows it is also a compare-and-set against
  another labeler, not only a reuse guard.
- New: a v7 database with faces, person labels and vectors migrates to v8 with
  ids, labels and `vec_faces` rows intact, and inserts after it do not reuse.
- New: `face name` concurrent with a rebuild does not name a face it did not
  match.
- New: a rebuild skips, rather than aborts, when an image is deleted mid-run.

## Resolved questions

- **Migration cost.** 2.0 µs/row, linear to 1M. Not a concern at any collection
  size phig targets, and the ongoing cost is ~5% per face insert.
- **Does `vec_faces` need the same treatment?** No. `insert_face` always
  supplies `face_id` explicitly from `faces.id`, so vec0 never generates one and
  has no independent reuse behavior — it inherits `faces.id`'s. Its shadow
  tables (`vec_faces_rowids`, `vec_faces_chunks`) use AUTOINCREMENT for their
  own bookkeeping, which is not the id phig hands around.
- **Any other table with the same shape?** `images.id`, `people.id`, `tags.id`,
  `operations.id` and `operation_entries.id` are all bare `INTEGER PRIMARY KEY`.
  They are deliberately out of scope, because the recipe above is **not** safe
  on them: `faces` is the only one that is a pure FK child.
  - `images`, `tags` and `operations` have children with `ON DELETE CASCADE`.
    `DROP TABLE images` succeeds and the implicit `DELETE FROM` cascades —
    measured: 5 faces and 1 image_tag silently deleted, 5 orphaned `vec_faces`
    rows left behind, no error.
  - `people` is referenced by `faces.person_id` with no `ON DELETE` action, so
    the drop aborts: `rc=19 FOREIGN KEY constraint failed`.

  The 12-step ALTER procedure avoids both by requiring `foreign_keys=OFF`, which
  cannot be set inside the transaction the migration loop opens. Migrating those
  four means restructuring that loop to toggle the pragma outside the
  transaction — machinery every migration runs through. That is its own plan,
  and it wants a demonstrated hazard first.

## Relationship to other plans

- **Plan 031** — its ABI operations are lock-free, which is what makes this
  reachable outside the CLI. `core/faces_engine.h` points here.
- **Plan 044** — the comparison phase this lock is held across is the same one
  044 wants on `vec_faces` KNN.
- **Plan 046** — an orphaned `vec_faces` row plus a reused id gives `UNIQUE
  constraint failed on vec_faces primary key` from `insert_face`, and
  `find_closest_face` returns the id of a face that no longer exists.
  AUTOINCREMENT removes the collision but not the dead-id read, which stays 046's.
- **Plan 047** — its People/Faces screen names unnamed faces from a GUI, which
  requires libphig to export face ids held across human-scale time. A client
  cannot use the embedding-match defense, because it has no embedding. This plan
  is the prerequisite.
- **Plan 010** — introduced the faces schema and `vec_faces`.
