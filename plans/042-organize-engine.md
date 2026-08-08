---
name: Organize Engine & cp/mv ABI
status: planned
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
  Design settled 2026-08-08: adopts 041's plan-as-value convention, adds
  per-file conflict resolution because replacing one photo and keeping both of
  another is a genuine judgement call, and counts parents rather than files in
  progress.
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
ABI: copy is additive to the database, move mutates the collection's layout and
is undoable only through the activity log. `phig_op_copy_start` and
`phig_op_move_start` as distinct symbols mean a GUI cannot move a user's library
by passing the wrong boolean.

Fully separating them down to the planner would duplicate the shared logic. The
split is at the entry point, over one planner — and the plan itself is
action-agnostic, since destinations and conflicts are identical either way.

## Decisions

Settled 2026-08-08.

### 1. Adopts 041's plan-as-value convention

A plan is a caller-owned `phig_organize_plan*` the caller frees; the operation
that computed it may be freed immediately after. Apply snapshots the plan at
start and re-verifies each decision at execution time. This is 041's decision 1
and decision 4, unchanged, and it answers this plan's original open question
about whether to surface a plan as a value.

Adopting it also means `--dry-run` stops being a mode and becomes the plan phase
that every caller runs anyway.

### 2. Conflict resolution is per file

The run-level policy stays and remains the default, so `cli/` is unchanged and a
caller that overrides nothing behaves exactly as today. On top of it, a caller
may override the resolution for any individual conflicting item.

This is where organize departs from reconcile, and 041's decision 2 states the
general rule both follow: a plan carries per-item decisions, and whether one is
overridable is a property of the decision type. A conflict is a genuine
judgement call — replace this one, keep both of that one — with no
recoverable-later story, which is why it is overridable. Reconcile's deletions
are not, because 041's decision 6 removes the motive by completing the restore
snapshot.

**Mutation is safe because the plan is a caller-owned value.** Editing it is a
local operation on the caller's own memory: no locking, no coordination with a
running operation, no question about what a concurrent reader sees. Had the plan
been accessors on the operation handle, `set_resolution` would mutate an object
the engine also owns.

Setting a resolution recomputes that item's destination, so `dest_at` shows what
"keep both" will actually produce rather than leaving the caller to guess.

**Over the wire only the exceptions travel.** `phigd` sends the plan down and the
client posts back a sparse override list —
`{plan_operation: id, overrides: [{index: 7, resolution: "overwrite"}]}` — not
the whole plan.

**No `ConfirmFn` in the ABI.** `cmd_rm`'s `ConfirmFn` (`remove_exec.h:51-66`) is a
synchronous callback on the calling thread, and plan 031 forbids a callback on an
engine thread; it also has no meaning over HTTP, where nothing can call back into
a browser mid-request. The plan phase replaces it and carries strictly more
information than a composed warning string. The general rule, which plan 053
inherits when it exposes `rm`: **destructive ABI operations get plan/apply, not
callbacks.** `ConfirmFn` stays a `cli/` affordance for a command with no plan
phase.

Per-file resolution is the reason this plan does not simply mirror reconcile.
Not offering it was considered and rejected: Finder and Lightroom both resolve
per file on import, and retrofitting it later would mean two plan types with
different mutability rules.

### 3. Progress counts parents, not files

`done` and `total` count parents — the photos the user selected and the plan
listed. `processed` carries the running count of files actually written,
including sidecars and attached JPEGs.

`phig_op_progress` (`phig.h:445-454`) is a caller-allocated struct with no size
or version member, so adding a field is an ABI break requiring a `SOVERSION`
bump. Reusing `processed` needs no new field, and it follows the pattern the
header already documents — `processed` and `skipped` are index-and-thumbs only,
and thumbs redefines `processed` as thumbnails generated. Organize defines:

| Field | Meaning |
|---|---|
| `done` / `total` | parents |
| `processed` | files written, including sidecars and attached JPEGs |
| `skipped` | parents skipped by conflict policy |
| `errors` | failures |

A caller renders "412 of 1,204 photos — 1,238 files" from that.

Counting files in the denominator was rejected on cost and on reversibility.
The total is unknown until `find_sidecars` has run for every parent — a
directory listing per parent, the cost class plan 044 measured for
`has_sibling_raw` at 7.33s per 4,000 files in one directory — and it would be
paid twice, because `carry_sidecars` lists again at execution time and must,
since apply re-verifies. Changing what `total` counts later is a value change
with no soname bump, so neither direction is foreclosed.

### 4. Both cp and mv get the plan phase

Copy is additive to the *database*, not to the filesystem: `organize_exec.h:14-17`
records that on copy, "if `dest` already exists it is backed up first (to a
unique sibling) so a DB failure can restore it." So a copy can destroy a
destination file under `overwrite`, and wants the same preview as a move. Two
shapes for one engine would cost more than the symmetry saves.

### 5. Cancellation follows the existing transaction boundaries

`execute_organize_action` commits each file's disk action and its log entry in
one transaction, so the boundaries already exist per file. The token is polled
between files. This matches 041's decision 5 without needing its own argument.

## ABI shape

```c
typedef struct phig_organize_plan phig_organize_plan;

typedef enum {
    PHIG_CONFLICT_SKIP = 0,
    PHIG_CONFLICT_OVERWRITE = 1,
    PHIG_CONFLICT_RENAME = 2
} phig_conflict_resolution;

typedef struct {
    const char* destination;
    const char* format;
    const char* const* matches;
    size_t match_count;
    const char* const* excludes;
    size_t exclude_count;
    phig_conflict_resolution on_conflict;  /* run-level default */
} phig_organize_options;

typedef struct {
    uint64_t applied;       /* parents moved or copied */
    uint64_t skipped;
    uint64_t errors;
    uint64_t files_written; /* including sidecars and attached JPEGs */
    bool completed;         /* false when cancelled partway */
} phig_organize_result;

phig_status phig_op_organize_plan_start(phig_ctx*, const phig_organize_options*,
                                        phig_op** out);
phig_status phig_op_organize_plan_take(phig_op*, phig_organize_plan** out);
void        phig_organize_plan_free(phig_organize_plan*);

size_t      phig_organize_plan_count(const phig_organize_plan*);
const char* phig_organize_plan_source_at(const phig_organize_plan*, size_t i);
const char* phig_organize_plan_dest_at(const phig_organize_plan*, size_t i);
bool        phig_organize_plan_conflicts_at(const phig_organize_plan*, size_t i);
phig_conflict_resolution
            phig_organize_plan_resolution_at(const phig_organize_plan*, size_t i);

/* Override one item. Only meaningful where conflicts_at(i) is true; recomputes
 * that item's destination so dest_at reflects the choice. */
phig_status phig_organize_plan_set_resolution(phig_organize_plan*, size_t i,
                                              phig_conflict_resolution);

/* One plan, two applies. Both snapshot it at start; the caller may free the
 * plan on return. */
phig_status phig_op_copy_start(phig_ctx*, const phig_organize_plan*, phig_op** out);
phig_status phig_op_move_start(phig_ctx*, const phig_organize_plan*, phig_op** out);
phig_status phig_op_organize_result(phig_op*, phig_organize_result* out);
```

Every symbol is additive, so `PHIG_ABI_VERSION_MINOR` and the CMake `VERSION`
bump while `SOVERSION` stays, per `libphig/CLAUDE.md`.

## Core shape

```cpp
struct OrganizeAction {
    std::string source;
    std::string dest;
    bool conflicts = false;
    ConflictResolution resolution;  // the run policy unless overridden
};

struct OrganizePlan {
    std::vector<OrganizeAction> actions;
};

class OrganizeEngine {
   public:
    OrganizeEngine(const OrganizeOptions&, Database&);

    // Writes nothing.
    OrganizePlan plan(const CancelToken&, ProgressQueue<OrganizeProgress>*);
    OrganizeResult apply(const OrganizePlan&, bool is_move, const CancelToken&,
                         ProgressQueue<OrganizeProgress>*);
};
```

`is_move` stays a bool in `core/` and is split into two symbols at the ABI. That
is the boundary this plan's title names: shared underneath, separate where a
caller could get it wrong.

## Testing

- The plan writes nothing: compare rows and the destination tree before and
  after a plan run.
- An override changes both the destination `dest_at` reports and the outcome on
  disk.
- An override on a non-conflicting item is rejected rather than silently
  ignored.
- Apply re-verifies: a destination occupied between plan and apply falls back to
  the run policy, and one that became free is simply written.
- Sidecars and an attached JPEG travel with their parent, count in
  `files_written`, and do not move `done`/`total`.
- Copy and move both honor per-item overrides.
- The plan freed before apply starts still applies correctly.
- Cancel between files leaves the logged operation `interrupted` and disk and
  database consistent with each other.
- A copy over an existing destination under `overwrite` restores the backup when
  the database step fails, which is `organize_exec.h`'s stated contract and is
  now reachable through the ABI.

## Relationship to other plans

- **Depends on 031** — the ABI operation lifecycle and progress convention.
- **Depends on 041** for the plan/apply convention, which this plan adopts
  rather than re-deciding. 041's decision 2 carries the shared rule about which
  decisions are overridable.
- **Plan 043** (CLI primitive split) — `cli/` moves onto this engine, and
  `--dry-run` becomes the plan phase.
- **Plan 052** (phigd) — S1b import needs this; the sparse override list is the
  wire form of decision 2.
- **Plan 053** (libphig parity) — inherits decision 2's rule that destructive ABI
  operations get plan/apply rather than a confirmation callback, which is the
  answer to how `rm` gets exposed.
- Related: 003 (copy & move), 020 (activity log), 025 (sidecar files), 026 (RAW
  support), 044 (`has_sibling_raw` cost).
