---
name: Organize Keeps mtimes and Recognizes Identical Files
status: in-progress
created: 2026-11-14
description: >
  Two organize defects found while preparing a real collection for
  reorganization. A copy, or a move across filesystems, stamps the file with
  the time of the copy, which misdates every photo without an EXIF or sidecar
  date. A destination already holding the same bytes is treated as a conflict
  and renamed around, writing a second identical file as `_1`. Excludes
  resolving duplicates already in the collection, which is plan 077.
---

# 076 — Organize Keeps mtimes and Recognizes Identical Files

## Problem 1: a copy loses the file's mtime

`atomic_copy_file` (`core/paths.cpp`) and the cross-device branch of
`move_file` both copy with `fs::copy_file`, which gives the new file the current
time as its mtime. Every organize copy goes through one of them: `cp`, an `mv`
that crosses filesystems, and the sidecars, RAW JPEGs and Live Photo videos
carried with either. A trash move across filesystems does too.

phig dates a photo by EXIF `DateTimeOriginal`, then `DateTimeDigitized`, then a
sidecar date, then mtime (`get_best_date`). For a photo with none of the first
three, mtime is the date, so a copy files it under the day it was copied.

The database hides the damage until the next scan. `insert_copy` and
`update_path_in_txn` keep the source row's `modified_at`, so the row still holds
the old time while the file on disk has the new one. The next scan sees a
changed mtime, rehashes the file, and overwrites `modified_at` with the copy
time.

Measured on the real collection, 2026-11-14:

- One `phig mv` in the activity log crossed from an NVMe drive to an HDD.
  All 3,422 files it moved now carry the mtime of the day it ran.
- 3,415 rows hold a `modified_at` older than the file on disk. 104 of them have
  no EXIF date, so the database is the only remaining record of their real
  mtime, and the next scan of that directory will overwrite it.
- 523 files on the HDD already have an effective date in June or July
  2026. Most came through a copy made outside phig, but the mechanism is the
  same.

No rule states that phig keeps a file's mtime.

## Problem 2: an identical destination is copied again under a new name

`OrganizeEngine::plan` treats any occupied destination as a conflict, whatever
the occupant holds. Under `rename`, a destination that already holds the same
bytes gets a byte-identical second file named `_1`. Under `skip`, `mv` leaves
the source where it was, so the duplicate survives in its old location.

Measured: 879 exact-duplicate groups in the collection are a file plus a `_1`
copy of it, 1,070 redundant files in all, most from `mv --on-conflict rename`
runs in July. Merging the collection with a second copy of it kept on another
machine would produce thousands more.

No rule in `rules/conflict.toml` covers identical content at the destination.

## Decisions

### 1. Copies keep the source's mtime

`atomic_copy_file` and `move_file`'s cross-device branch set the temporary
file's mtime from the source before the rename publishes it, so the file never
appears at its final path with the wrong time. Only mtime: atime is not a date
source, and ctime cannot be set.

This covers every caller of the two functions, including attachment carries and
cross-device trash moves, without changing any call site.

New rule, concern `metadata`, protecting `original-file`: a file phig copies or
moves keeps its modification time.

### 2. An identical file at the destination is "already present", not a conflict

**What counts as identical: the same bytes.** Equal SHA-256 of the file
contents. The same pixels with different bytes is a near-duplicate, which only
a person decides on.

**Plan step.** When a planned destination is occupied on disk, and the
occupant is not one of this run's not-yet-moved sources, the planner compares
the occupant with the source: first by size, then by SHA-256. For a tracked
occupant it reads the row's hash; for an untracked one it hashes the file.
When they match, the item is marked `identical` and keeps its intended
destination. This is decided before the run's `on_conflict` policy and
overrides it: identical content is not a conflict, so `skip`, `rename` and
`overwrite` all reach the same result.

The same applies to an intra-run claim. If an earlier item in the same plan
claims the destination with a source whose row has the same SHA-256, the later
item is marked `identical` instead of being renamed. That is the case where two
folders' copies of one photo both plan to the same path.

**Apply step.** The plan is advisory, as R10 already makes it, so apply
re-verifies before acting. It hashes the source and the occupant from disk, not
from the database, because trashing the source depends on the answer.

- The bytes no longer match: the item is skipped and counted.
- The destination is now empty, because the earlier item it depended on was
  skipped or failed: the item is written normally.
- They match: the item is resolved as below.

**`mv` with an identical destination:**

1. Attachments are reconciled first. A source sidecar, RAW JPEG or Live Photo
   video that has no counterpart at the destination's name is carried as it is
   today. A counterpart with the same bytes is trashed with the source. A
   counterpart with different bytes means the files are not identical as a set,
   and the item falls back to the run's conflict policy. This is checked at plan
   time and re-checked at apply.
2. User metadata merges onto the destination row with the existing
   `transfer_user_metadata` (R37): tags and favorite unioned, gallery filled
   only when empty, faces moved only when the destination has none. Two cases
   would lose something under R37 and fall back to the conflict policy instead:
   - both rows have a gallery and they differ;
   - the source has named faces and the destination already has faces.
3. If the destination has no row, the source row is repointed at the
   destination path, because it now describes that file.
4. The source file goes to the trash through the existing trash action
   (R23), so it can be restored.
5. The source row is deleted with a restore snapshot (R04) unless step 3
   repointed it.
6. mtime: see decision 3.

The trash move happens first; the metadata transfer, row changes and log
entries then commit in one transaction (R18). If the transaction fails, the
source comes back from the trash. That mirrors how `execute_organize_action`
moves a file back after a failed path update.

**`cp` with an identical destination:** nothing is written to disk. If the
destination has no row, one is inserted as `insert_copy` does today, so the
result is the same as a real copy. If it has a row, nothing changes.

**Logging.** A `cp` that adopts an existing file is logged with its own entry
kind, not `copy`. A future undo (plan 065) must not delete a file phig did not
write. A `mv` logs a `trash` entry for the source and a `relink` entry for the
metadata merge.

**Results.** `OrganizeAction` gains an `identical` flag, and `OrganizeResult`
and `OrganizeProgress` gain an `identical` count. The CLI's dry run and summary
report "already present" separately from skipped. libphig's organize plan
exposes the flag as a new item accessor beside
`phig_organize_plan_conflicts_at`.

New rules, concern `conflict`:

- A destination holding the same bytes as the source is not a conflict under
  any policy: a copy writes nothing and a move trashes the source after merging
  its metadata.
- A source is trashed as identical only after its bytes and the destination's
  are compared on disk at apply time.

### 3. The surviving file takes the older mtime, within limits

When `mv` resolves an identical destination, the destination's mtime is
replaced with the source's only when all of these hold:

- the destination's date comes from mtime, because it has no EXIF date and no
  sidecar date (`get_best_date` reaches step 4);
- the source's mtime is older;
- the source's mtime is plausible: not before 1990-01-01, and not after the
  current time. That excludes the Unix epoch, the FAT default of 1980-01-01 and
  clock errors.

The old mtime is logged in a new `set_mtime` entry so it can be restored, and
`modified_at` and `effective_date` are updated in the same transaction. If
`modified_at` were left stale, the next scan would rehash the file.

This is one shared `core/` function, so plan 077 uses the same rule when it
keeps one copy of a duplicate group.

## Existing damage

Fixing the code does not repair rows whose only record of the real mtime is
the database, 104 in the measured collection. Restoring the disk mtime from
`modified_at` where the stored value is older and the file's SHA-256 still
matches the row, before anything rescans those files, is a one-off script
rather than phig code.

## Testing

Real-behavior tests against the engine and the built binary, each failing
before its change:

- `atomic_copy_file`, and `move_file` across filesystems, leave the destination
  with the source's mtime. The cross-device test follows
  `MoveFileTest.CrossDevice*` and skips the same way.
- `phig cp` and a cross-device `phig mv` of a photo with no EXIF date file it
  under its mtime's date, not today.
- `mv` onto an identical untracked file: the source is in the trash, its row
  points at the destination, and the activity log has the trash entry.
- `mv` onto an identical tracked file: the source row's tags, favorite and
  gallery are on the destination row, and the source row is deleted with a
  restore snapshot.
- `mv` onto an identical file whose row has a different gallery: falls back to
  the conflict policy.
- Two identical sources planned to one destination: the first moves, the second
  is `identical` and is trashed. When the first is skipped, the second moves
  normally.
- A source modified between plan and apply is skipped, not trashed.
- A sidecar with different bytes at the destination makes the item fall back
  to the conflict policy.
- `cp` onto an identical untracked file writes nothing and inserts a row, and
  its log entry is not a `copy`.
- The mtime rule: older and plausible is taken; an EXIF-dated destination is
  left alone; 1970 and 1980 source times are ignored.
- A failed transaction after the trash move brings the source back.

## Cost

Problem 2 adds hashing at apply time, of the source and the occupant, but only
for items whose destination is occupied. That cost is not known yet: measure a
`mv` of the real collection into an occupied tree on the HDD before calling it
acceptable.

## Relationship to other plans

- **077** reuses decision 3's mtime rule and decision 2's verify-then-trash
  sequence.
- **059** (`--on-conflict overwrite`) is unaffected. Identical content is
  decided before the policy.
- **065** (activity log reader and undo) needs to know the new `set_mtime`
  entry kind and the adopted-copy entry kind.
- **070** placed every destination inside the requested tree; this plan only
  changes what happens when that destination is occupied.

## Outcome

Built on branch `076-organize-mtimes-identical`, 2026-11-14, as decided above,
with these details settled in the code:

- `copy_mtime` in `core/paths.cpp` sets the temporary file's mtime before the
  fsync and rename, in `atomic_copy_file` and in `move_file`'s cross-device
  branch. A failure to set it fails the copy.
- `attachment_moves` (`core/organize_exec.cpp`) lists a photo's sidecars, RAW
  JPEG and Live Photo video with their destinations. `carry_sidecars` and the
  identical path both use it, so the naming lives in one place.
- The planner's check is `identical_occupant` (`core/organize_engine.cpp`); the
  executor is `execute_identical_action`. A copy or move that adopts an
  untracked file logs `adopt`; a changed mtime logs `set_mtime` with
  `{"previous_mtime": ...}`.
- One plan serves `cp` and `mv`, so a gallery or face conflict makes a copy fall
  back to the policy too, although a copy merges nothing.
- `Database::delete_paths_logged_in_txn` and `set_modified_at_in_txn` let the
  row deletion, metadata merge, mtime and log entries commit together.
- libphig ABI 1.5 adds `phig_organize_plan_identical_at` and an `identical`
  count at the end of `phig_organize_result`.
- `get_best_date` gains an overload that reports which source dated the file.

Rules R89–R94 record it. 22 new tests, each confirmed to fail with its piece of
the change removed.

Still to do before closing: measure the apply-time hashing on the real
collection (see Cost).
