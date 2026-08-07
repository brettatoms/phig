---
name: vec_faces Delete Failure
status: designing
created: 2026-08-07
description: >
  Three DatabaseTest cases fail on master with `Failed to delete face vectors:
  SQL logic error`, thrown from `delete_faces_for_image`'s DELETE against the
  vec_faces vec0 virtual table. Reproducible from a clean Release build at
  3177320, so it is neither a stale artifact nor build configuration. The
  isolated SQL works — the same DELETE with a subquery, and the same DELETE
  after a KNN query, both succeed against a standalone vec0 table — so the cause
  is something in phig's own setup and is not yet identified. Every deletion path
  that clears face vectors is affected, which is user-visible data cleanup.
---

# 046 — vec_faces Delete Failure

## Symptom

```
DatabaseTest.UpdatePathOntoStaleDestClearsItsFaces
DatabaseTest.DeletePathsCascadesTagsAndFaces
DatabaseTest.PurgeSnapshotMarksOmittedFaces
```

All three throw:

```
C++ exception with description "Failed to delete face vectors: SQL logic error"
```

That message comes from `step_write_or_throw` in `Database::delete_faces_for_image`
(`core/database.cpp:1581`), on:

```sql
DELETE FROM vec_faces WHERE face_id IN (SELECT id FROM faces WHERE image_id = ?)
```

The statement **prepares** successfully and fails at `sqlite3_step`.

## What has been established

- **Pre-existing.** Reproduces from a clean `cmake -B` Release build at commit
  `3177320` (`Mark plan 037 completed`), with no working-tree changes. Not
  caused by plan 031's work, not a stale build artifact.
- **Not a vendored-dependency change.** `vendor/sqlite-vec` is v0.1.7 and has
  not been touched since `546cfa0`, the commit that added face recognition.
- **Not an inherent vec0 limitation.** Two hypotheses were tested against a
  standalone vec0 table and both were disproven:
  - `DELETE ... WHERE face_id IN (SELECT ...)` on a vec0 table — succeeds.
  - The same DELETE immediately after a stepped KNN (`MATCH ... AND k = ?`)
    query — succeeds.

So the failing ingredient is something in phig's own database setup that the
isolated repro does not have. `PRAGMA foreign_keys=ON` (`database.cpp:219`) is
the most obvious untested difference and is the next thing to try.

## Why it matters

`delete_faces_for_image` is called from every path that removes or repoints an
image row — `delete_paths`, `update_path_in_txn`, the purge snapshot path, and
`FacesEngine`'s per-image delete-then-reinsert. If it throws in real use, face
vectors are orphaned in `vec_faces`, which is a vec0 virtual table that FK
cascade cannot reach — exactly the cleanup plan 022 added this call to guarantee.

Whether it throws in real use is unestablished. The three failures are all
tests; no user-facing symptom has been reported, and `phig search --count`
against the real collection works. It is possible this only manifests under a
specific sequence the tests happen to hit. That is the first thing to determine,
because it decides the severity.

## Next steps

1. Add `PRAGMA foreign_keys=ON` to the isolated repro and re-test.
2. If that reproduces it, determine whether the fix is the pragma's interaction
   with a virtual table or the subquery shape, and whether splitting the DELETE
   into a select-then-delete-by-id avoids it.
3. Establish whether a real CLI path (`phig rm`, `phig scan` reconcile, `phig
   face rebuild` over a collection with existing faces) triggers it, or only the
   test sequence.
4. Fix with a real-behavior test per the project's data-safety rule.

## Relationship to other plans

- **Plan 022** — added the `vec_faces` orphan cleanup this call implements.
- **Plan 010** — introduced `vec_faces`.
- **Plan 045** — also touches the faces schema; if that migration rebuilds the
  table, sequencing the two matters.
