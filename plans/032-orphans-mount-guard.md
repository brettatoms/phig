---
name: rm --orphans Mount Guard
status: completed
created: 2026-08-06
description: >
  Make `rm --orphans` able to remove orphaned rows whose parent directory was
  deleted, and stop it reporting "No matching entries found." when it silently
  skipped rows. `paths_safe_to_delete` returns a per-path reason instead of a
  filtered list so both callers can report what they declined to touch;
  `--ignore-mount-warning` (scan's existing flag name) overrides the
  missing-parent heuristic on `rm`. The guard's default stays as-is.
  Diagnosis in `docs/plans/2026-08-05-orphans-mount-guard.md`.
---

# 032 — `rm --orphans` Mount Guard

`phig rm --orphans` declined to remove 74 of 76 genuinely-orphaned rows and
printed `No matching entries found.` while doing it. `/mnt/data` was mounted and
healthy throughout; the affected directory
`/mnt/data/Pictures/Brett/2026/07/` had been deleted. The 74 stuck paths are
recorded in `docs/plans/2026-08-05-dangling-rows.txt`.

The cause is `paths_safe_to_delete` (`core/paths.cpp:282-301`). For each row
whose file is missing it requires the **parent directory** to still exist:

```cpp
fs::path parent = fs::path(p).parent_path();
bool parent_exists = fs::exists(parent, ec);
if (ec || !parent_exists) continue;  // parent gone/unknown -> keep
```

The intent is right — it stops `rm --orphans` wiping a collection when an
external drive is not mounted. But it cannot distinguish "drive unmounted" from
"directory genuinely deleted", and it fails toward silence.

The workaround was `rm --match '/mnt/data/Pictures/Brett/2026/07/*' --db-only`,
which requires already knowing which paths are affected — the one thing the
output withheld.

## Two defects

**1. No override.** There is no way to say "the drive is mounted, proceed."
`scan` already has `--ignore-mount-warning` for its own version of this
situation (`core/scan_engine.h`, `cli/main.cpp:1967`); the gap is only in `rm`.

**2. The skip is silent, and that is the worse half.** `paths_safe_to_delete`
returns a filtered `vector<string>` and discards why each row was rejected, so
`core/remove_exec.cpp:86-93` cannot tell "there are no orphans" from "there are
74 orphans and I skipped every one." It prints success-shaped output for a case
where the tool declined to act.

The guard has three `continue` branches and the caller can see none of them:

| Reason | Correct to skip? | Tell the user? |
|---|---|---|
| Existence check errored (`EACCES`/`EIO`) | yes — unknown state | **yes**, a real problem |
| File still exists | yes — not an orphan | no, the normal case |
| Parent directory missing | **only sometimes** | **yes**, the surprising one |

Fixing defect 2 matters more than defect 1: it is what rescues the user who
does not know the flag exists.

## Scope

- `paths_safe_to_delete` returns a reason per path rather than a filtered list.
- `rm` gains `--ignore-mount-warning`, which makes `--orphans` treat a missing
  parent as a deleted directory and remove the row.
- `rm --orphans` reports skipped counts by reason, always — including when
  nothing is deletable, which is the reported bug.
- `EACCES`/`EIO` skips get their own count, separate from the mount heuristic.

## Not in scope

- **Weakening the guard's default.** Skipping when the parent is missing stays
  the default behaviour. The bug is the missing override and the missing report.
- **The date-fallback concern raised alongside this.** Already correct:
  `core/dates.cpp` resolves EXIF `DateTimeOriginal` → EXIF
  `CreateDate`/`DateTimeDigitized` → sidecar → mtime → `unsorted`, with no
  fallback to the current time. The July 2026 misfiling was mtime resolving to
  the import date on copied files — documented tier-4 behaviour, not a defect.

## Design

### The return type

`paths_safe_to_delete` (`core/paths.h:72`) becomes reason-carrying. Both call
sites — `core/remove_exec.cpp:87` and `core/scan_engine.cpp:496` — change with
it. Following `core/CLAUDE.md`, the result carries no user-facing prose; wording
is the caller's job.

```cpp
enum class KeepReason {
    Deletable,       // file gone, parent present -> safe to remove the row
    FilePresent,     // not an orphan
    ParentMissing,   // possible unmount; overridable
    CheckFailed,     // fs::exists errored (EACCES/EIO)
};

struct SafeToDeleteResult {
    std::vector<std::string> deletable;
    std::vector<std::string> parent_missing;
    std::vector<std::string> check_failed;
    // FilePresent paths are not collected — the normal case, nothing to report.
};

SafeToDeleteResult paths_safe_to_delete(const std::vector<std::string>& db_paths);
```

`parent_missing` and `check_failed` hold paths, not just counts, so `--dry-run`
can list them and a future `--verbose` needs no signature change.

### The override

`--ignore-mount-warning` on `rm`, reusing scan's name rather than introducing
`--force` (vaguer, and it would collide conceptually with `--hard`). With it
set, `--orphans` unions `parent_missing` into the deletable set. `check_failed`
is **never** overridden — an unreadable path is an unknown state, not a
heuristic judgement.

### The report

Printed before the `matched.empty()` check in `core/remove_exec.cpp`, so it
appears in the reported-bug case. Shape:

```
Found 2 orphaned entries (mode: orphans)
Skipped 74 entries whose parent directory is missing — this usually means an
unmounted drive. If the drive is mounted and the directory was deleted, re-run
with --ignore-mount-warning.
```

Exact wording is the implementer's call; it must state the count, the reason,
and the remedy. When `deletable` is empty and `parent_missing` is not, the
skip report replaces `No matching entries found.` rather than accompanying it.

### scan's reconcile

`core/scan_engine.cpp:496` shares the function. Scan has its own coarser mount
warning — it warns and skips wholesale when *every* file under a directory is
missing, versus this per-row parent test. Two decisions belong to this plan:

- Scan's reconcile reports `parent_missing` and `check_failed` counts in its
  summary, so the two commands stop reporting the same condition differently.
- Scan's `--ignore-mount-warning` **also** makes reconcile delete missing-parent
  rows (decided 2026-08-06). One flag, one meaning across both commands: it
  means "proceed past the mount heuristic". This widens what an existing flag
  deletes, which is a behaviour change for current users of the flag, not only
  new surface. `check_failed` is never included either way.

## Tests

Data-safety change, so real-behaviour tests, not mocks. Homes:
`tests/test_paths.cpp` (`SafeToDeleteTest`, which already covers the three
branches at lines 444-509) and `tests/test_remove_exec.cpp` (which has
`RmOrphansDropsMissingKeepsPresent` at :406).

New fixtures use `make_temp_dir` from `tests/temp_dir.h` — a hardcoded temp
path passes alone and fails under `ctest --parallel` (plan 030).

- A missing-parent row is skipped by default and its path lands in
  `parent_missing`.
- The same row **is** removed with `--ignore-mount-warning`.
- A row whose file still exists is never removed, with or without the flag.
- A `check_failed` path is not removed even with `--ignore-mount-warning`.
- `rm --orphans` with only missing-parent rows reports the skip count and does
  **not** print `No matching entries found.` — the reported bug.
- The existing four `SafeToDeleteTest` cases keep passing against the new
  return type (they assert the classification, which does not change).
- Scan's reconcile still preserves an unmounted subtree.

Gate: `ctest --test-dir build --parallel` green, and `./build/phig search
--count --db ~/.local/share/phig/phig.db` still reports 29925.

## Verifying against the real case

The 74 paths in `docs/plans/2026-08-05-dangling-rows.txt` are the original
evidence, but they may already have been cleared by the `--db-only` workaround.
Check with `rm --orphans --dry-run` before treating the collection as a
reproduction; if the rows are gone, the tests above are the reproduction and no
real-run check is possible for the missing-parent path.

**Outcome (2026-08-06):** the rows were already cleared — `rm --orphans
--dry-run` on the real collection reports nothing, so the tests are the
reproduction. The bug was also reproduced end-to-end on a scratch collection
(3 indexed files, their directory deleted, drive mounted): before the fix
`No matching entries found.`, after it a skip count naming
`--ignore-mount-warning`, and the flag then removes exactly those 3 rows.
