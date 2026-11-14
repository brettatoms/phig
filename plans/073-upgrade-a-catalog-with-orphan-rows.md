---
name: Upgrade a Catalog With Orphan Rows
status: completed
created: 2026-11-13
completed: 2026-11-13
closes_rules: [R29]
description: >
  A catalog whose rows already break a foreign key cannot be upgraded: each
  migration step checks the whole database before committing and fails on any
  violation, including ones it did not create. Fail only on violations a step
  adds, back the database up before the first step, and show the GTK client's
  user that an upgrade is running. Excludes repairing the orphan rows, which
  plan 066 owns.
---

# 073 — Upgrade a Catalog With Orphan Rows

## Problem

Opening the user's collection in the GTK client (plan 071) showed "Could Not
Open Collection: Migration to schema v8 left 36 foreign key violations". The
catalog is at schema v7 and this build is at v10.

`Database::init_schema` (`core/database.cpp:600-650`) runs each migration step
in an IMMEDIATE transaction with foreign keys off, then calls
`foreign_key_violations()`, which runs `PRAGMA foreign_key_check` over the whole
database, and rolls back if the count is above zero. The check exists to catch
a step that strands child rows (R84). It cannot tell those rows from rows that
were already orphaned before the step ran, so a catalog carrying any orphan
fails every upgrade, and no version of phig can upgrade it.

The 36 rows are almost certainly the orphan face rows plan 066 found in a
backup of the same collection: 36 `faces` rows on 21 image ids with no `images`
row, most likely left by deletes made before foreign keys were enforced on
2026-07-05. The rollback worked as designed and the catalog is unchanged.

The effect reaches every client. `phig scan` and every other CLI command that
writes opens with `CatalogAccess::Write` and fails the same way, and reads
refuse an outdated catalog (R70). M0's client opened read-only, which is why
this did not surface before.

The review of this plan's core change found a second way an upgrade can strand
a catalog. The pre-migration path, for a database from before the `meta`
table, reads version 0 outside a transaction and does not re-read it inside its
IMMEDIATE one. If another process creates a fresh current schema between the
two, this one finds an `images` table, stamps the current schema v1, commits,
and fails its v2 step, leaving a database stamped v1 that no later open can
upgrade. Two Create opens reach it without the catalog lock, as two libphig
read-write contexts on a missing catalog do. R29 states that every migration
re-reads the version inside its transaction, so it is recorded as partial, and
this plan fixes it.

## Decisions

Decided 2026-11-13.

1. **Upgrades stay automatic and forward-only.** Opening for writing upgrades
   the catalog, as now, with no prompt. phig never downgrades: migrations are
   forward-only and a catalog newer than the build is refused (R70's
   rationale). The user's condition for an automatic upgrade.
2. **A step fails only on violations it adds.** Inside each step's
   transaction, the set of violations is read before the step and again after
   it, each identified by child table, child rowid and parent table. The step
   fails only if the after set holds an entry the before set does not.
   Existing orphans are carried through unchanged. Nothing is deleted to make
   an upgrade succeed. The foreign key index is left out of the identity
   because a table rebuild can renumber a table's foreign keys.
3. **The database is backed up before the first step.** When at least one
   step is pending, `VACUUM INTO` writes a copy beside the database, named
   with the version it held and the time, such as
   `phig.db.v7-backup-20261113T201500Z`. If the copy cannot be written the
   upgrade does not start and the open fails with the reason, so nothing is
   changed. The backup is never deleted by phig. It costs one database's worth
   of disk once per upgrade; the thumbnail cache is not copied.
4. **The user is told.** Core logs the upgrade and the backup path at warning
   level, which the CLI prints on stderr. The GTK client shows an "Upgrading
   your collection…" page while the upgrade runs, then a toast naming the
   backup.
5. **The GTK client always shows the page when an upgrade is pending**, rather
   than only when it is expected to be slow. It knows one is pending from a
   read-only open, which refuses an outdated catalog with
   `CatalogError::Kind::Outdated`; it then shows the page and opens for
   writing on its worker, as it opens now. No time threshold to guess at. The
   upgrade's duration on a large catalog is measured and recorded here, since
   the v8 and v9 steps copy whole tables and the backup copies the whole file.
6. **Repair is not part of this plan.** Whether orphan rows are deleted, and
   what authorizes it, stays with plan 066. A repair is destructive and goes
   through a plan the user reviews before it applies.

## Rules

- **New rule (R88):** a schema migration step fails only on foreign key
  violations it creates, and no step runs until a backup of the database has
  been written. Anchored at `init_schema` and `foreign_key_violations`; asset:
  database rows. Binds the operations R84 and R70 bind for a write open.
- **R84's** rationale describes the check as failing on any violation; it is
  updated to the before-and-after comparison.

## Testing

Real-behaviour tests against databases built at an older schema, the way
`FacesMigrationV8.PreservesIdsLabelsAndVectorsAndStopsReuse` and the v9 test
build theirs:

- A v7 database with orphan rows in `faces`, `image_tags` and
  `operation_entries`, inserted with foreign keys off, upgrades to the current
  version. Every orphan is still present with its values, and every other row,
  label and vector survives as the existing migration tests require.
- A step that adds a violation still fails and rolls back, with the orphan
  rows already present. This needs a way to run a step that strands a row,
  through a test-only migration or by testing the comparison on its own.
- The backup is written before the first step, opens at the old version, and
  holds the same rows, `vec_faces` included. No backup is written when no step
  is pending. When the backup cannot be written, as when the catalog
  directory is read-only, the open fails and the database is unchanged.
- The CLI: `phig scan` against such a catalog upgrades it and indexes.
- The GTK seam: `open` on such a catalog succeeds; a read-only check reports
  an upgrade pending before it, and nothing pending after.
- Measured: upgrade time and backup size on a synthetic catalog of 30,000
  images with faces and embeddings.

## Measured

2026-11-13, on an AMD Ryzen 5 5600X (6 cores, 12 threads), Linux 6.18, system
SQLite 3.54.1, release build of `phig-tests`. A synthetic v7 catalog of 30,000
images, each with one face and its embedding, every tenth image tagged and
favorited, and 21 images deleted with foreign keys off so 21 face rows are
orphans. The database was 67 MB after a WAL checkpoint. Opening it with
`CatalogAccess::Write` upgraded it from v7 to v10, backup included, in:

- 340-346 ms with the catalog on tmpfs (three runs), of which the backup's
  `VACUUM INTO` was about 50 ms;
- 415-424 ms on ext4 on NVMe (three runs), of which the backup was 70-76 ms.

The backup was 50 MB. It is smaller than the 67 MB database because building
the fixture rebuilt and dropped the images and faces tables, which left free
pages, and `VACUUM INTO` writes none. How a real catalog's backup compares with
its own size was not measured. One `PRAGMA foreign_key_check` over the upgraded
catalog took 10-11 ms; each of the three steps runs two, so the
before-and-after comparison costs about 60 ms of the upgrade.

## Outcome

Built as decided, 2026-11-13, in `core/` on master and in the GTK client on the
`071-gtk-m1` branch.

- A migration step compares the foreign key violations before and after it
  and fails only on new ones, so existing orphans are carried through
  unchanged. A step that rebuilds a table without an INTEGER PRIMARY KEY must
  keep its rowids, or its existing orphans would read as new; R88 says so.
- Before the first pending step, `VACUUM INTO` writes a copy beside the
  database and flushes it; its directory is flushed best effort. A copy is
  kept only when a step committed, so a failed attempt and a process that loses
  a concurrent upgrade leave none. The CLI prints the copy's path; the GTK
  client shows "Upgrading Your Collection" while it runs and a toast naming
  the copy that stays until dismissed.
- Two races in the same code were fixed with the user's agreement. The
  pre-migration and fresh-schema paths now re-read the version inside their
  transactions, so a connection that read version 0 no longer stamps a
  current schema v1 (R29, recorded partial and then fixed). And an open that
  another build upgrades past this one during the open is refused as newer
  (R70).
- New rule R88; R29, R70 and R84 updated.

Known gap: when a step after the first commits and a later one fails, the copy
is kept and logged, but the GTK client's error page does not name it, because
the open's error carries no path.

## Relationship to other plans

- **066** found the orphan rows and owns whether and how to repair them.
- **071** (GTK M1) found this; its by-hand checks on the user's own collection
  wait for this plan.
- **054** owns the rules mechanism R88 is added through.
