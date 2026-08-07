---
name: faces.id Rowid Reuse
status: designing
created: 2026-08-07
description: >
  `faces.id` is `INTEGER PRIMARY KEY` without `AUTOINCREMENT`, so SQLite hands a
  deleted rowid back to the next insert. Any read-then-write that remembers a
  face id across a gap can land on a different face than the one it read —
  silently attaching a person label to a face that was never compared.
  `identify_person` no longer depends on the writer lock for this — its write
  matches on the embedding it compared rather than the row id (plan 031) — but
  that is one caller defending itself, not the reuse being fixed. `phig face
  name` still relies on the lock and is therefore unprotected against plan 031's
  lock-free ABI operations. Schema v8 migration to AUTOINCREMENT removes the
  hazard at the source; deciding whether the lock can then come off follows.
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

**So the remaining exposure is `face name` against a lock-free ABI faces
operation.** That is narrow — the window is a SELECT plus an INSERT, widened
only when `get_or_create_person` waits on the busy timeout — and it is the one
this plan removes at the source. Fixing it before then would mean either giving
`find_closest_face` a row-returning variant so `name` can match on the embedding
too, or making the ABI operation take the lock, which reverses 031's design.

## Scope

- Schema v8: migrate `faces.id` to `INTEGER PRIMARY KEY AUTOINCREMENT`, which
  makes SQLite track the high-water mark in `sqlite_sequence` and never reuse.
  Forward-only, like every other phig migration.
- Decide whether `face identify` and `face rebuild` can then drop the writer
  lock. AUTOINCREMENT removes the *wrong-face* hazard; a deleted-and-not-reused
  id is already harmless (`set_face_person` returns false and identify does not
  count it). That likely makes the lock unnecessary, but it is a data-safety
  claim that needs its own test rather than an inference.
- Invert `RowidReuseMeansAStaleIdCanHitADifferentFace`, and add a test that a
  delete-then-reinsert in the phase gap no longer collides.

## Open questions

- **Migration cost on a large table.** `AUTOINCREMENT` cannot be added with
  `ALTER TABLE`; it needs a new table plus a copy. On a collection with millions
  of faces that is a long one-time migration, and `vec_faces` references face
  ids, so it has to be kept consistent through the copy.
- **Does `vec_faces` need the same treatment?** It is a vec0 virtual table keyed
  on `face_id`; whether it has its own reuse behavior needs checking rather than
  assuming.
- **Any other table with the same shape.** `images.id` and `people.id` are
  plausibly identical declarations. Worth an audit in the same pass — the same
  argument applies wherever an id is remembered across a gap.

## Relationship to other plans

- **Plan 031** — its ABI operations are lock-free, which is what makes this
  reachable outside the CLI. `core/faces_engine.h` points here.
- **Plan 044** — the comparison phase this lock is held across is the same one
  044 wants on `vec_faces` KNN. Fixing either shortens the window.
- **Plan 010** — introduced the faces schema and `vec_faces`.
