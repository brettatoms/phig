---
name: vec_faces Delete Failure
status: designing
created: 2026-08-07
description: >
  Three DatabaseTest cases fail on master with `Failed to delete face vectors:
  SQL logic error`, thrown from `delete_faces_for_image`'s DELETE against the
  vec_faces vec0 virtual table. Reproducible from a clean Release build at
  3177320, so it is neither a stale artifact nor build configuration. It is
  however platform-dependent: all three pass on macOS, and a repro reproducing
  phig's whole connection setup and DDL succeeds there against two SQLite
  versions, with and without a transaction. That rules out phig's own setup as
  the sole cause and makes the SQLite the platform provides the prime suspect.
  The failing machine's sqlite3_libversion() is the missing fact. Every deletion
  path that clears face vectors is affected, which is user-visible data cleanup.
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

## What macOS rules out

Measured 2026-08-07 at `7c59d2d` on macOS arm64. Repro checked in at
`docs/plans/2026-08-07-046-vec-delete-repro.cpp` (build command in its header).

- **The three tests pass here.** Full suite: 690 pass, 12 skipped. So a clean
  run does not demonstrate a fix — it may only demonstrate the machine.
- **`PRAGMA foreign_keys=ON` does not reproduce it.** That was this plan's
  leading hypothesis. The repro reproduces phig's entire open sequence in order
  — `sqlite3_vec_init`, `PRAGMA journal_mode=WAL`, `sqlite3_busy_timeout(30000)`,
  `PRAGMA foreign_keys=ON` — on phig's exact `images`/`people`/`faces`/`vec_faces`
  DDL, then runs the failing DELETE verbatim. It succeeds.
- **Nor does WAL, nor a surrounding transaction.** The repro is file-backed, so
  WAL is really engaged (it is a no-op on `:memory:`, which is why an in-memory
  repro does not count). Tested with the DELETE outside any transaction and
  inside `BEGIN IMMEDIATE`, matching the real call sites. All four combinations:
  prepare `rc=0`, step `rc=101 DONE`, zero rows left.
- **Nor is it the SQLite version, across the two available here.** Apple's
  system 3.51.0 (what phig links on macOS) and Homebrew's 3.53.4 both succeed.

phig's setup is identical on both platforms, and it reproduces nothing here.
That weakens "something in phig's own database setup" as the sole explanation
and leaves what the platform supplies: the SQLite library and the toolchain
that built it. Two SQLite versions have been cleared; the failing machine's is
not among them and is not recorded anywhere.

**The missing fact is the failing machine's `sqlite3_libversion()`** and its
`PRAGMA compile_options`. Neither is recorded anywhere, and without them there
is nothing left to vary here — every ingredient this machine can supply has
been tried.

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

1. On the machine where the three tests fail, record `sqlite3_libversion()` and
   `PRAGMA compile_options`, then run
   `docs/plans/2026-08-07-046-vec-delete-repro.cpp` there. That either
   reproduces the failure in ~100 lines or proves the trigger is something the
   tests do that the repro does not — and either answer is progress.
2. If it reproduces, bisect the SQLite version against the same vendored
   sqlite-vec v0.1.7 to find where the behavior changes, and check whether
   splitting the DELETE into a select-then-delete-by-id avoids it.
3. If it does not reproduce, the trigger is in the surrounding test sequence
   rather than the statement. `DatabaseTest` runs through `init_schema`'s
   migration path, which the repro skips.
4. Establish whether a real CLI path (`phig rm`, `phig scan` reconcile, `phig
   face rebuild` over a collection with existing faces) triggers it, or only the
   test sequence.
5. Fix with a real-behavior test per the project's data-safety rule.

Until step 1 is done, treat a passing run as inconclusive: verify fixes on the
platform that fails, not on macOS.

## Relationship to other plans

- **Plan 022** — added the `vec_faces` orphan cleanup this call implements.
- **Plan 010** — introduced `vec_faces`.
- **Plan 045** — rebuilds the `faces` table for schema v8, so sequencing the two
  matters: 045 should land after this is understood. 045 also measured what an
  orphaned `vec_faces` row costs once this failure leaks one — a reused
  `faces.id` then collides, `UNIQUE constraint failed on vec_faces primary key`,
  and before that `find_closest_face` returns the id of a face that no longer
  exists. 045's AUTOINCREMENT removes the collision but not the dead-id read.
