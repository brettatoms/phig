# `rm --orphans` cannot remove orphans when a directory is deleted

**Status:** ready to plan
**Date:** 2026-08-05
**Found:** while diagnosing empty cells in phig-gtk's grid (see
`2026-08-04-phig-gtk-m0-ledger.md`)

## The problem

`phig rm --orphans` silently declined to remove 74 of 76 genuinely-orphaned rows, and
reported `No matching entries found.` while doing so.

The cause is `paths_safe_to_delete` (`src/paths.cpp:282-300`). For each row whose file is
missing it checks whether the **parent directory** still exists:

```cpp
// File is gone. Only delete if its parent directory still exists — a
// missing parent means the subtree is unmounted/unreachable, not that
// this file was individually removed.
fs::path parent = fs::path(p).parent_path();
bool parent_exists = fs::exists(parent, ec);
if (ec || !parent_exists) continue;  // parent gone/unknown -> keep
```

The heuristic is sound in intent — it stops a `rm --orphans` from wiping a collection when
an external drive is not mounted. But it cannot distinguish "drive unmounted" from
"directory genuinely deleted," and it fails toward silence.

**Observed case.** `/mnt/data/Pictures/Brett/2026/07/` was deleted (its parent `2026/`
still exists but is empty). `/mnt/data` was mounted and healthy the whole time. 74 rows
pointed into that directory. `rm --orphans --dry-run` found 2 orphans — the two whose
directory happened to survive — and gave no indication the other 74 existed. The workaround
was `rm --match '/mnt/data/Pictures/Brett/2026/07/*' --db-only`, which requires the user to
already know which paths are affected.

## Two defects, not one

**1. No way to override the guard.** There is no flag to say "I know the drive is mounted;
proceed." `scan` already has `--ignore-mount-warning` for its own version of this
situation (`src/scan_engine.h:31`, `src/main.cpp:1967`), so the gap is only in `rm`.

**2. The skip is silent, which is worse than the missing flag.** `paths_safe_to_delete`
returns a filtered vector and discards the reason each row was rejected, so
`remove_exec.cpp:85-93` cannot tell "there are no orphans" from "there are 74 orphans and I
skipped all of them." It prints `No matching entries found.` — success-shaped output for a
case where the tool declined to act. A user has no way to discover the rows exist.

The guard has three distinct `continue` branches and the caller can see none of them:

| Reason | Correct to skip? | Should the user be told? |
|---|---|---|
| Existence check errored (`EACCES`/`EIO`) | yes — unknown state | **yes**, this is a real problem |
| File still exists | yes — not an orphan | no, this is the normal case |
| Parent directory missing | **only sometimes** | **yes**, this is the surprising one |

## What to build

**A flag on `rm`, named for consistency with `scan`.** `--ignore-mount-warning` is the
existing precedent and should be reused rather than introducing `--force`, which is vaguer
and would collide conceptually with `--hard`. With it set, `--orphans` treats a missing
parent as a deleted directory and removes the row.

**Report what was skipped, always.** This is the more important half — it fixes the case
where the user does not know the flag exists. `paths_safe_to_delete` should return the
reason per path rather than a filtered list, so the caller can say something like:

```
Found 2 orphaned entries (mode: orphans)
Skipped 74 entries whose parent directory is missing — this usually means an
unmounted drive. If the drive is mounted and the directory was deleted, re-run
with --ignore-mount-warning.
```

Exact wording is the implementer's call, but it must state the count, the reason, and the
remedy. Note that changing the return type touches `scan_engine.cpp:496` as well; scan's
reconcile phase already has its own mount warning, so the two should end up consistent
rather than each reporting differently.

**Surface `EACCES`/`EIO` separately.** A row skipped because its existence could not be
determined is a different problem from one skipped by the mount heuristic, and today both
vanish identically. It needs its own count in the output.

## Scope notes

- **Do not remove or weaken the guard's default.** Defaulting to "skip when the parent is
  missing" is correct; the bug is the absence of an override and of any report.
- `scan`'s reconcile path (`src/scan_engine.cpp:496`) shares the same function. Deciding
  whether `--ignore-mount-warning` should also make scan's reconcile delete such rows is
  part of this work — today scan warns and skips wholesale when *every* file is missing,
  which is a coarser check than the per-row parent test.
- Data-safety change, so it ships with real-behaviour tests: a row whose parent directory
  is missing is skipped by default and removed with the flag; a row whose file exists is
  never removed either way; and the skip count is reported. The existing
  `tests/test_relink.cpp` and `tests/test_remove_exec.cpp` are the right homes.

## Not in scope

The date-fallback concern raised alongside this is **already fixed** and needs no work.
`src/dates.cpp:30-69` resolves a date as EXIF `DateTimeOriginal` → EXIF
`CreateDate`/`DateTimeDigitized` → sidecar → file mtime → invalid (`unsorted`). There is no
fallback to the current time; the two `now()` calls at `dates.cpp:98-99` are file-time epoch
conversion. The July 2026 `2026/07` misfiling was mtime resolving to the import date on
files that had been copied around, which is the documented tier-4 behaviour rather than a
defect.
