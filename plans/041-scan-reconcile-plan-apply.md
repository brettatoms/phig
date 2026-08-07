---
name: Scan Reconcile Plan/Apply
status: designing
created: 2026-08-07
description: >
  Split scan's destructive reconcile phase into a decision that writes nothing
  and an application the caller authorizes, so a GUI can show "12 files are gone
  — remove them?" with the actual list before any row is deleted. Requires
  breaking `relink_moved_rows` and `demote_jpeg_rows` into decide/apply halves:
  today each decides and writes inside one transaction, returning only counts.
  The mount guard and RAW/JPEG demotion are batch decisions that need the
  completed walk, so this is a barrier operation, not a stream. Split out of 031,
  which exposed only scan's additive indexing half.
---

# 041 — Scan Reconcile Plan/Apply

Plan 031 exposed `ScanEngine::run_workers` — walk, hash, extract, upsert — as
the ABI's index operation. `run_reconcile` was deliberately left out. This plan
covers it.

Reconcile is the destructive half of scan: it looks at rows whose files are no
longer on disk and decides for each one to relink (transfer tags/gallery/faces
to a surviving row with the same sha256), demote (a JPEG that now has a
same-basename RAW), or delete. It adds nothing — the index pass is what adds
rows, and only for files that exist.

## Why a plan/apply split

A CLI user sees reconcile's outcome in a summary line after the fact. A GUI user
needs to authorize it beforehand, because the rows being deleted carry
user-authored metadata — tags, gallery membership, face labels — that a rescan
cannot rebuild.

The mount guard is the sharpest case. Today it fires as a `log_warn`
(`scan_engine.cpp:520`) telling the user to re-run with
`--ignore-mount-warning`. A GUI cannot act on a log line. It needs the guard's
finding as a value: "all 4,312 rows under this directory are missing — this
looks like an unmounted drive."

## What has to change in `core/`

The deletion decision is already reusable: `paths_safe_to_delete` is a free
function in `core/paths.h`, already called independently by
`remove_exec.cpp:90`. It partitions paths into deletable / parent_missing /
check_failed and writes nothing.

Relink and demote are not. `relink_moved_rows(db_, removed_paths, op_id)` and
`demote_jpeg_rows(db_, scan_prefix, op_id)` each decide *and* write, inside the
logged operation, returning only counts and the demoted path list. There is no
way to ask either what it *would* do. The `has_sibling_raw` peek at
`scan_engine.cpp:531` is explicitly non-authoritative — a cheap check to decide
whether opening an operation is worthwhile, with `demote_jpeg_rows` doing "the
real, authoritative work exactly once, inside that operation."

So both need splitting into a decide half returning candidates and an apply half
consuming them. This is surgery on the path that destroys metadata, which is
where the repo's "data-safety changes ship with real-behavior tests" convention
bites hardest.

## Open questions

- **Staleness.** Files can move between plan and apply. Apply must re-verify
  rather than trust the plan. `rm`'s guard is the precedent for how much
  re-checking is enough.
- **Plan lifetime across the ABI.** The payload is larger than anything
  currently in the ABI: three path vectors from `SafeToDeleteResult`, a demotion
  list, a relink list, a mount-warning flag. Does the caller own it and hand it
  back, or does a handle hold it server-side while the caller inspects?
- **Cancellation.** Reconcile currently polls no token and deletes inside one
  logged operation. Whether the apply half becomes cancellable, or stays atomic
  and is simply fast, needs deciding.
- **Whether the CLI adopts it.** `--dry-run` for scan would fall out of a plan
  phase almost for free. That may belong here or in plan 043.

## Relationship to other plans

- **Depends on 031** — the ABI operation lifecycle and progress convention.
- **Plan 043** (CLI primitive split) — may consume the plan phase as scan
  `--dry-run`.
- Related: 022 (logged scan removals with restore snapshots), 023 (scan
  re-link), 026 (RAW support / JPEG demotion), 032 (orphans mount guard).
