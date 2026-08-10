---
name: Scan Reconcile Plan/Apply
status: planned
created: 2026-08-07
description: >
  Split scan's destructive reconcile phase into a decision that writes nothing
  and an application the caller authorizes, so a GUI can show "12 files are gone
  — remove them?" with the actual list before any row is deleted. Requires
  breaking `relink_moved_rows` and `demote_jpeg_rows` into decide/apply halves:
  today each decides and writes inside one transaction, returning only counts.
  The mount guard and RAW/JPEG demotion are batch decisions that need the
  completed walk, so this is a barrier operation, not a stream. Design settled
  2026-08-08: a plan is a caller-owned value rather than an operation's borrowed
  output, authorization is all-or-nothing with the mount override as an apply
  flag, and apply re-verifies every decision so the plan is advisory only. Split
  out of 031, which exposed only scan's additive indexing half.
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

## Decisions

Settled 2026-08-08. These replace this plan's original open questions and are
the convention plan 042 inherits.

### 1. A plan is a caller-owned value

`phig_op_reconcile_plan_take` extracts the plan into a `phig_reconcile_plan*`
the caller owns and frees. The operation that computed it may be freed
immediately after; the plan outlives it.

The alternative — reading the plan through accessors on the operation handle,
the way `phig_op_error_at` works — was rejected because it makes the apply
handle depend on the plan handle staying alive. That lifetime rule is one a
foreign caller can violate while deciding which rows to delete, and the callers
this ABI exists for are the ones most likely to violate it: `gtk/` has its own
object lifetimes, and the Elixir client named in root `CLAUDE.md` is a GC'd
runtime that would hold two opaque handles with an ordering constraint between
them.

Separating the plan from the operation removes the rule rather than documenting
it. `phig_op_reconcile_apply_start` takes a `const phig_reconcile_plan*` and does
not retain it, which is what a C caller already assumes about a const pointer
argument.

This is not a new pattern in the ABI. `phig_query` (`phig_query_new` /
`phig_query_free`, `phig.h:171-172`) and `phig_settings` (`phig_settings_free`,
`phig.h:306`) are already caller-owned opaque values with lifecycles independent
of any operation.

Two consequences fall out. Whether apply copies or moves the plan's contents at
start is invisible to callers and can change later without touching the header.
And a plan becomes storable — a client may plan, show the user, and apply a
minute later, which decision 4 already makes safe.

### 2. Reconcile's decisions are not overridable

The caller inspects the plan and either applies it whole or discards it. There is
no per-file selection and no per-category toggle, so nothing travels back across
the boundary except the plan itself.

This is narrower than "plans are immutable", which is what this decision
originally said. Plan 042 needs per-file conflict resolution for organize —
replacing one photo but keeping both of another is a genuine judgement call — so
the general convention is:

> A plan carries per-item decisions. Whether an item's decision is overridable
> is a property of the decision type, not of the plan mechanism.

Organize's conflict resolutions are overridable. Reconcile's are not, and the
reason is decision 6 rather than taste: the only thing keeping a doomed row buys
is metadata that a delete destroys irrecoverably, and completing the restore
snapshot removes that motive. Overriding a deletion would leave a row pointing
at a file that does not exist, which is a broken state rather than a choice.

The one case with real value — two or more on-disk files sharing a sha256, where
relink deliberately does nothing (`relink.h:12`) and the row is deleted with its
tags and faces — is not an override but a choice of target. It is a distinct
feature and is not in this plan.

### 3. The mount-guard override is an apply flag

`phig_op_reconcile_apply_start` takes `bool include_parent_missing`. The plan
always reports `deletable`, `parent_missing` and `check_failed` as separate
lists, so a caller passing the flag is acting on a list it was shown rather than
on something it was not.

**This diverges from plan 042's apply signature deliberately.** There, apply
takes the plan and nothing else, because a per-item override already carries the
caller's answer. Reconcile has no per-item overrides (decision 2), so the one
choice a caller still has to make has nowhere else to live. Two apply shapes is
the consequence of that difference, not an inconsistency to tidy away.

The override was not put in the plan options, which would have kept apply
argument-free at the cost of a full re-plan to flip it. Planning demotion runs
`has_sibling_raw` per row, and plan 044 measured that doing a full
parent-directory listing per file — 4,000 files cost 7.33s in one directory. A
re-plan is not cheap enough to spend on a boolean.

`check_failed` paths are never deleted, flag or not. That is plan 032's rule: a
missing parent is a heuristic judgement a caller may override, while a failed
existence check is an unknown state that must not be.

### 4. Apply re-verifies; the plan is advisory

Apply re-runs each decision's predicate before acting on it. A plan can never
cause a deletion that apply would not independently justify, which is what makes
a stale plan a non-event rather than a race.

- Delete: re-check `fs::exists`. A file that came back is skipped.
- Relink: re-resolve by sha256 inside the transaction, which
  `relink_moved_rows` already does.
- Demote: re-check `has_sibling_raw`, which `demote_jpeg_rows` already treats as
  authoritative.

Two of the three re-checks exist already and move to the apply half unchanged.
The result carries a `skipped_stale` count so a caller learns the plan aged
instead of silently getting a different outcome.

### 5. Cancellation follows the existing transaction boundaries

Reconcile is not one transaction. Its three phases commit differently, and the
cancellation points are wherever they already commit:

| Phase | Transaction structure | Cancellable |
|---|---|---|
| `relink_moved_rows` | one transaction per row (`relink.cpp:35-41`) | between rows |
| `demote_jpeg_rows` | one transaction per row (`relink.cpp:68-74`) | between rows |
| `delete_paths_logged` | all deletes + log writes in ONE transaction (`database.h:97`) | before it starts |

So the token is polled between rows in relink and demote, and once before the
delete transaction. Delete stays atomic. On cancel the result reports
`completed = false` and the logged operation is marked `interrupted`, reusing the
path `scan_engine.cpp:549-559` already takes when an exception unwinds.

This adds no new class of partial state. Relink and demote can already complete
partially before an exception today, which is why that `interrupted` marking
exists. Cancellation makes a reachable state deliberate rather than error-only.

The expensive phase is the plan, which writes nothing and is therefore trivially
safe to cancel. Chunking the delete transaction to make it interruptible was
rejected: it would trade the delete phase's atomicity for interruptibility of
the fast half.

### 6. Every path that drops a row writes a restore snapshot

`delete_paths_logged` snapshots 16 columns of the `images` row into
`restore_json` (`database.cpp:977-1015`): path, filename, extension, file_size,
created_at, modified_at, sha256, phash, width, height, metadata, media_type,
duration_ms, gallery, blur_score, effective_date.

Tags live in `image_tags` and faces in `faces`/`vec_faces`. Neither is captured,
and both are cascade-deleted. The code says so at `database.cpp:1017-1020`, where
it records a face *count* "so a future undo can honestly report the row is only
partially restorable rather than silently dropping face data."

So gallery survives a delete and tags and face labels do not. That asymmetry is
what makes an unrecoverable loss out of reconcile, and it is the actual defect
behind the pressure to let a caller keep individual rows. Fixing the snapshot
addresses it at the source, and it fixes `phig rm` at the same time, since both
go through this function.

**Tags are captured.** A list of tag names, added to `restore_json`. They are
small, they cannot be regenerated once the file is gone, and there is no
ambiguity about what restoring them means.

**Faces need a measurement before the shape is fixed.** A face's value is its
128-float embedding, which is what makes it searchable; a person label with no
vector restores nothing useful. Embeddings can only be regenerated if the file
comes back. Capturing them is therefore the complete fix, at roughly 1.5 KB of
JSON per face against a `restore_json` that already carries the EXIF blob —
plan 044 measured that at ~3.8 KB/row — under a default `log_max_entries` of
100,000. Estimated, not measured. Implementation measures the real distribution
against the collection and either captures embeddings or falls back to labels
plus bounding boxes, recording which and why.

**Organize drops rows through two other functions, and neither records
anything.** `delete_paths_logged` is not the only path that destroys a row.
When a move or a copy lands on a path some row already names,
`Database::update_path_in_txn` (`database.cpp:831-876`) deletes that row with a
plain `DELETE FROM images WHERE path = ?` so repointing the source does not hit
`UNIQUE(path)`, and `Database::insert_copy` (`database.cpp:630-663`) replaces it
with `INSERT OR REPLACE`. Both clear the occupant's faces first through
`delete_faces_for_image`, because the FK cascade cannot reach `vec_faces`; the
`image_tags` rows cascade and the gallery goes with the column. Neither calls
`delete_paths_logged`, so there is no `purge_row` entry and no `restore_json`.
The only log entry `execute_organize_action` writes is the `move` or `copy` for
the file that arrived (`organize_exec.cpp:57` and `:102`), which names the
source and the destination and says nothing about what it displaced.

This is reachable from the CLI with the default `--on-conflict skip` and no
override. `rules/metadata.toml`'s R40 measured it against the built binary: a
stale row carrying the tag `precious` and the gallery `wedding`, a `mv` of
another file onto its path, `Done: 1 Skipped: 0 Errors: 0` and exit 0, with the
row, the tag association and the gallery gone. `cp` replaced the row the same
way. R40 is `status = "violated"` with this plan as `owner`, and its `test_gap`
names the reason this decision is worded as it is: 041's acceptance criteria
were written against `delete_paths_logged`, which neither of those two paths
calls, so closing 041 as written would leave the loss open.

So the decision is the general one — every path that drops a row writes the same
snapshot — and the two organize paths are in scope here rather than deferred.
Each writes a `purge_row` entry inside the transaction `execute_organize_action`
has already opened for the move or copy and its log entry, so disk, row and log
stay in step. They cannot call `delete_paths_logged` as it stands: it opens its
own `BEGIN IMMEDIATE` (`database.cpp:971`) and both callers are already inside a
transaction, so the snapshot-and-log body has to come out as a function that
assumes an open transaction, the way `update_path_in_txn` already does.

Restoring is not in scope. There is no reader for the activity log anywhere in
`core/` and no undo command — plan 020 deferred it and plan 027 shipped the log
write-only, with plan 053 owning the reader. This decision makes the record
complete so a future undo has something to restore; it does not build the undo.

## The guards on the delete path

Deletion is gated in more than one place, and they are worth listing before the
work starts rather than discovering one mid-implementation. Plan 042 found a
second guard — `move_file`'s refusal to overwrite — only after an authorized
action failed at the syscall, having already been approved by the guard the
design knew about. Reconcile is more layered, not less.

| Guard | Where | What it stops |
|---|---|---|
| Mount warning | `scan_engine.cpp:517-523` | Skips the entire destructive phase when every row under the prefix is missing |
| `paths_safe_to_delete` | `core/paths.cpp` | Partitions into deletable / parent_missing / check_failed |
| `delete_paths_logged` | `database.cpp:996-999` | Skips any path with no row, so a stale plan entry deletes nothing |

The plan/apply split must not weaken any of them, and apply's re-check
(decision 4) is a fourth layer rather than a replacement for these. Plan 054
owns writing the full set down, including the reconcile side; this table is the
inventory this plan's implementer needs in hand.

## ABI shape

```c
typedef struct phig_reconcile_plan phig_reconcile_plan;

typedef struct {
    const char* directory;
} phig_reconcile_options;

typedef struct {
    size_t removed;
    size_t relinked;
    size_t demoted;
    size_t skipped_stale;  /* decision no longer justified at apply time */
    bool completed;        /* false when cancelled partway */
} phig_reconcile_result;

phig_status phig_op_reconcile_plan_start(phig_ctx*, const phig_reconcile_options*,
                                         phig_op** out);

/* Extracts the plan into a caller-owned object. The plan operation may be freed
 * immediately after. */
phig_status phig_op_reconcile_plan_take(phig_op*, phig_reconcile_plan** out);
void        phig_reconcile_plan_free(phig_reconcile_plan*);

size_t      phig_reconcile_plan_deletable_count(const phig_reconcile_plan*);
const char* phig_reconcile_plan_deletable_at(const phig_reconcile_plan*, size_t i);
/* …the same pair for parent_missing, check_failed and demote; relink exposes
 * source and target; mount_warning is a bool getter. */

/* Snapshots the plan at start. The caller may free the plan on return. */
phig_status phig_op_reconcile_apply_start(phig_ctx*, const phig_reconcile_plan*,
                                          bool include_parent_missing, phig_op** out);
phig_status phig_op_reconcile_apply_result(phig_op*, phig_reconcile_result* out);
```

Every symbol is additive, so `PHIG_ABI_VERSION_MINOR` and the CMake `VERSION`
bump while `SOVERSION` stays, per `libphig/CLAUDE.md`.

**`phig_op_poll` must gain a case for the new kinds.** Its switch in
`libphig/operations.cpp` has no `default:`, so adding a `phig_op::Kind` without
extending it is a `-Wswitch` error under `-DPHIG_WERROR=ON` — which is what CI
builds with, and what a default local build does not. Plan 042 shipped exactly
this omission: the kinds were added, the case was not, polling an organize
operation returned `false` forever, and no test caught it because every ABI test
called `phig_op_wait` rather than `phig_op_poll`. Do not repeat it.

## Core shape

```cpp
// Writes nothing. Takes the token and queue because this is the slow half —
// it runs has_sibling_raw per row — and cancelling it can never leave state
// behind.
ReconcilePlan ScanEngine::plan_reconcile(const CancelToken&,
                                         ProgressQueue<ScanProgress>*);
void ScanEngine::apply_reconcile(const ReconcilePlan&, bool include_parent_missing,
                                 const CancelToken&, ScanResult&);

std::vector<RelinkDecision>   plan_relink(Database&, const std::vector<std::string>&);
void                          apply_relink(Database&, const std::vector<RelinkDecision>&,
                                           int64_t op_id, const CancelToken&);
std::vector<DemotionDecision> plan_demotions(Database&, const std::string& prefix);
void                          apply_demotions(Database&, const std::vector<DemotionDecision>&,
                                              int64_t op_id, const CancelToken&);
```

`run_reconcile` stays as a convenience calling both halves, so `cli/` is
unchanged and plan 043 gets `scan --dry-run` from the plan half.

## Testing

Real-behavior tests, per the repo convention for data-safety changes.

- The plan writes nothing: compare rows before and after a plan run.
- A file that reappears between plan and apply is skipped and counted.
- A demotion whose RAW sibling vanished between plan and apply is skipped.
- `parent_missing` rows survive apply without the flag and are deleted with it.
- `check_failed` rows survive apply *with* the flag set.
- The plan operation freed before apply starts still applies correctly, which is
  what decision 1 buys.
- **An ABI test that polls, not waits.** `phig_op_poll` on both a reconcile plan
  and a reconcile apply returns progress. This exists because its absence is what
  hid a broken switch in plan 042 through an entire implementation.
- Cancel mid-apply leaves the logged operation `interrupted` and the database
  consistent.
- **Cancel mid-relink is idempotent**: relink, cancel partway, re-run, and assert
  no duplicate tags and no lost gallery or faces. `transfer_user_metadata` is
  assumed insert-or-ignore shaped; this test is what establishes it, since a
  cancelled relink is re-planned and re-applied on the next scan.

Decision 6 adds five more:

- A deleted row's tags appear in its `restore_json`, with the tag names intact.
- A deleted row's faces are recorded in whichever shape the size measurement
  settles on, and the existing face *count* still matches what was deleted.
- `phig rm` produces the same complete snapshot, since it shares
  `delete_paths_logged`. This is the regression test for fixing one caller and
  not the other.
- **`mv` onto an occupied path snapshots the row it displaces.** A tracked
  destination whose row carries a tag and a gallery, moved onto with the default
  `--on-conflict skip` and no override, leaves a `purge_row` entry whose
  `restore_json` names both, alongside the `move` entry for the file that
  arrived. Today there is no entry for the displaced row at all.
- **`cp` onto an occupied path does the same**, through `insert_copy` rather
  than `update_path_in_txn`. Both branches need their own test: they are
  separate functions with separate SQL, and the two existing tests
  (`DatabaseTest.UpdatePathOntoStaleDestClearsItsFaces` and
  `DatabaseTest.InsertCopyOntoStaleDestClearsItsFaces`) put neither a tag nor a
  gallery on the row they destroy, so they will keep passing while the loss
  stays reachable.

## Open — progress reporting

Two decisions this plan does not yet make, raised by reviewing it against plan
042's implementation. Both must be settled before implementation, because 042
demonstrated what happens when a progress denominator is invented at the
keyboard: it counted plan entries rather than actionable ones, so a run with
skips reported progress toward a total it could never reach.

1. **What the `phig_op_progress` fields mean for reconcile.** Plan 042 defines
   its mapping explicitly — `done`/`total` are parents, `processed` is files
   written. This plan defines nothing. Reconcile needs it more, not less: the
   *plan* phase is the slow half, running `has_sibling_raw` per row, so a caller
   watching it needs a denominator. Open: is `total` the row count under the
   prefix? Does `done` count rows across relink, demote and delete together or
   per phase? Does `processed` carry anything, or stay 0 the way faces leaves it?
2. **Which progress type the core side uses.** The Core shape below reuses
   `ProgressQueue<ScanProgress>`, but `ScanStage` is
   `{FileDone, Walked, LoadingFaceModels}` and none of those means "planning
   finished" or "relinking". Either `ScanStage` gains values or reconcile gets
   its own progress struct. At the ABI level plan 042 reused
   `PHIG_STAGE_SELECTED` for the same problem, which is available here too, but
   the core-side type still needs deciding.

## Unmeasured

- The plan payload's size. Estimated at a few MB for the 29,925-image collection
  assuming ~80 bytes per path, but not measured. It matters only if plan 044's
  million-item target makes copying at apply-start expensive, and decision 1
  keeps that a private implementation choice.
- A full re-plan's cost. Inferred from 044's `has_sibling_raw` measurement rather
  than timed end to end.
- How much `restore_json` grows if decision 6 captures face embeddings. Estimated
  at ~1.5 KB per face against an entry already carrying the EXIF blob, which 044
  measured at ~3.8 KB/row, under a 100,000-entry default cap. The real
  distribution of faces per image in the collection is what settles it, and
  nothing about the shape should be fixed before that is measured.

## Relationship to other plans

- **Depends on 031** — the ABI operation lifecycle and progress convention.
- **Plan 042** (organize engine) inherits the convention above: a plan is a
  caller-owned value; apply consumes it at start and re-verifies at execution.
  Its own open question about whether to surface a plan is answered by adopting
  this shape. Decision 6 also reaches into the two row-dropping functions
  organize goes through; both predate 042 and it did not change them, so the
  work is in `core/database.cpp` and `core/organize_exec.cpp`, not in 042's
  plan file.
- **Plan 043** (CLI primitive split) — consumes the plan phase as scan
  `--dry-run`.
- **Plan 052** (phigd) — a plan is a document over the wire, which is the same
  model rather than a second one; `POST /reconcile/plan` then
  `POST /reconcile/apply`.
- Related: 022 (logged scan removals with restore snapshots), 023 (scan
  re-link), 026 (RAW support / JPEG demotion), 032 (orphans mount guard),
  044 (`has_sibling_raw` cost).
