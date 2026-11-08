---
name: Orphan Face Rows
status: designing
created: 2026-11-07
description: >
  A backup of the real collection holds 36 face rows on 21 image ids that have
  no images row, although faces.image_id cascades on delete. Establish how they
  got there, whether current code can still produce one, and whether a
  collection carrying them needs repair. Excludes the vec_faces delete failure,
  which plan 046 owns.
---

# 066 — Orphan Face Rows

## Symptom

Found 2026-11-07 while measuring plan 060's R60 fix, by reading
`~/.local/share/phig/phig.db.bak-20260805-091416` read-only: 36 rows in `faces`
whose `image_id` matches no row in `images`, spread over 21 image ids.

`faces.image_id` is declared `REFERENCES images(id) ON DELETE CASCADE`
(`core/database.cpp:45`, and in the v8 rebuild at `:514`). With foreign keys
enforced, deleting an image deletes its faces, so these rows should not exist.

## What is known

- **Foreign keys were not always enforced.** `Database`'s constructor sets
  `PRAGMA foreign_keys=ON` on every connection (`core/database.cpp:307`). That
  line arrived in `df37b47` on 2026-07-05. Before it, SQLite's per-connection
  default was off, and deleting an image left its faces behind. The backup was
  taken on 2026-08-05, a month after, so rows orphaned before 2026-07-05 would
  still be in it.
- **Migrations run with foreign keys off.** `643e4b2` (2026-08-14) added a
  `foreign_key_check` before each migration's commit. That is after the backup,
  so a migration that ran before it could have orphaned rows without failing.
- **Nothing checks an existing database.** `Database::foreign_key_violations`
  runs only inside a migration's transaction. A database already at the current
  schema is never checked, so orphans from either cause above persist.

## Questions

1. **When were these rows orphaned?** Compare the 21 image ids against the
   activity log in the same backup, and against the ids assigned around
   2026-07-05. If every orphan predates `df37b47`, current code is probably not
   producing them.
2. **Can current code produce one?** List every path that deletes from `images`
   and confirm each runs on a connection with foreign keys on. That includes
   raw SQL in tests, `scripts/`, and anything else that opens the file without
   `Database`.
3. **Do the orphans carry person labels or `vec_faces` rows?** A labelled
   orphan with a vector can be returned by `find_closest_face` and feed
   `auto_label_face` a person from a photo that is gone. That would make the
   orphans a correctness problem, not just leftover rows.
4. **Does the current collection have them?** Check the live database and any
   newer backup, read-only.
5. **Does repair belong in `init_schema` or a command?** A one-time
   `foreign_key_check` at open and deletion of orphans is cheap, but it deletes
   rows from a user's file on open. A rule should state which.

## Constraints

Never write to `~/.local/share/phig/` while investigating. Open backups with
`SQLITE_OPEN_READONLY`, or copy them to a temp directory first.

Opening a WAL-mode backup read-only still creates `-shm` and `-wal` files beside
it. The R60 measurement left
`phig.db.bak-20260805-091416-shm` and `-wal` in `~/.local/share/phig/`. Copy the
file first.

## Relationship to other plans

- **Plan 046** — the `vec_faces` delete failure. A failed vector delete leaves
  an orphaned `vec_faces` row, a different orphan from the ones here.
- **Plan 060** — the measurement that found these.
- **Plan 045** — rebuilt `faces` for schema v8, the migration at
  `core/database.cpp:514`.
