---
name: Organize Engine & cp/mv ABI
status: designing
created: 2026-08-07
description: >
  Extract an organize engine into `core/` matching the shape of ScanEngine,
  ThumbsEngine, and FacesEngine — `run(CancelToken, ProgressQueue*)` — then
  expose copy and move as separate ABI entry points over it. Organize is the
  only one of the four long-running operations with no engine: `organize_plan.h`
  is one pure function, `organize_exec.h` is per-file executors, and the
  orchestration sits inline in `cli/main.cpp:720-744` with no progress queue.
  Separate at the boundary because copy is additive and move mutates the
  collection's layout; shared underneath because planning is ~80% common.
---

# 042 — Organize Engine & cp/mv ABI

`phig cp` and `phig mv` copy or move files into the structured folder layout
(`%Y/%m/%d/%camera/%original`), with match/exclude globs, conflict policy
(skip/overwrite/rename), dry-run, and DB path updates on move. Sidecars and
attached JPEGs travel with their parent.

Plan 031 covered index, thumbs, and faces because each already had an engine
with a `run(CancelToken, ProgressQueue*)` shape. Organize did not, so it was
deferred here.

## What exists

- `core/organize_plan.h` — `apply_format`, one pure function expanding a format
  string against an image.
- `core/organize_exec.h` — `execute_organize_action` (one file, move or copy)
  and `carry_sidecars`.
- `cli/main.cpp:720-744` — the orchestration: selection, conflict resolution,
  the loop, the operation lifecycle, a local `CancelToken`, and no progress
  queue at all.

## Separate at the boundary, shared underneath

Copy and move share their planning phase entirely: selection by match/exclude,
expanding the format string to a destination, conflict resolution. What differs
is the executor branch — move renames and updates the DB path, copy duplicates
and inserts a new row — and the rollback: move restores by moving the file back,
copy by removing the copy and restoring a backup.

So one command with an `is_move` flag is not an accident of the CLI; it reflects
that most of the work is common. But the *destructive* difference matters at the
ABI: copy is additive and safe, move mutates the collection's layout and is
undoable only through the activity log. `phig_copy` and `phig_move` as distinct
symbols mean a GUI cannot move a user's library by passing the wrong boolean.

Fully separating them down to the planner would duplicate the shared logic. The
split is at the entry point, over one planner.

## Open questions

- **Plan/apply.** `--dry-run` already implies a plan phase. Whether the engine
  surfaces it as a value (as plan 041 proposes for reconcile) or keeps it
  internal.
- **Conflict policy in a GUI.** skip/overwrite/rename is a flag on the CLI. A
  GUI may want per-file resolution, which is a different interaction model and
  may need a callback or a pause-and-ask, echoing `cmd_rm`'s `ConfirmFn`.
- **Progress granularity.** A move carries sidecars and attached JPEGs with the
  parent. Whether progress counts parents or all files affects a progress bar's
  total.

## Relationship to other plans

- **Depends on 031** — the ABI operation lifecycle and progress convention.
- **Plan 043** (CLI primitive split) — the CLI would move onto this engine.
- Related: 003 (copy & move), 025 (sidecar files), 026 (RAW support), 020
  (activity log).
