---
name: Organize Engine & cp/mv ABI
status: completed
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

**A destination occupied since planning is skipped, not re-resolved.** An earlier
draft of this plan said apply would fall back to the run policy. That would
re-run `resolve_conflict` at execution time and could file the photo under a name
the caller never saw, which contradicts the point of authorizing a plan.
Skipping keeps apply's outcome a subset of what was shown; the item lands in
`skipped`, so a caller can re-plan and see the new conflict. An item carrying an
explicit override is exempt, since that override *is* the authority to replace
what is there.

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

Not because copy destroys anything today. `cmd_organize`'s `is_protected`
predicate (`cli/main.cpp:671-676`) returns true for any path that exists on
disk, and `resolve_conflict` turns `overwrite` into `rename` for a protected
destination (`paths.h:97-99`). So through the CLI, `overwrite` differs from
`rename` only on an intra-run collision — two sources resolving to one
destination that nothing occupies yet. `execute_organize_action`'s
backup-and-restore is a net that predicate keeps from ever firing.

Copy gets the plan phase for two other reasons. A conflict changes what copy
does — the file lands under a name the user did not choose — and that is worth
seeing before authorizing. And after decision 6 an explicit override makes copy
genuinely able to destroy bytes, so the preview stops being a convenience.

Two shapes for one engine would cost more than the symmetry saves.

### 5. Cancellation follows the existing transaction boundaries

`execute_organize_action` commits each file's disk action and its log entry in
one transaction, so the boundaries already exist per file. The token is polled
between files. This matches 041's decision 5 without needing its own argument.

### 6. An explicit override bypasses the on-disk guard, never the batch guard

Per-file resolution does not work at all without this. A user who sees the plan,
sees that `IMG_1234.CR2` already occupies the destination, and chooses Replace
would have `is_protected` turn that into a rename — their choice silently
discarded.

So an item carrying an explicit override skips the `fs::exists` arm of the
guard. That arm exists to prevent *silent* data loss (audit findings #1 and #4),
and an override is the opposite of silent: the plan showed the conflict and the
user answered it.

The `batch_sources` arm is never bypassable. That path is another image's
not-yet-moved source file — a tracked original about to be moved — and
overwriting it destroys bytes phig is in the middle of relocating. No user
intent makes that correct, so an override targeting a batch source is rejected
rather than honored.

The plan reports whether the occupying file is tracked by phig, so a client can
warn harder when it is not. An untracked file is one phig cannot restore from
its own records, which is exactly when a confirmation should be loudest.

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

/* Whether the file occupying this item's destination is tracked by phig.
 * Meaningless unless conflicts_at(i). False means phig cannot restore it from
 * its own records, which is when a client should warn hardest. */
bool        phig_organize_plan_occupant_tracked_at(const phig_organize_plan*, size_t i);

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
- Without an override, `overwrite` still falls through to `rename` for a
  destination that exists on disk. This is the regression test for today's
  behavior, which the extraction must not change.
- With an explicit override to `overwrite`, an existing untracked file at the
  destination *is* replaced — decision 6's bypass actually working.
- An override targeting another image's not-yet-moved source is rejected, and
  that source still exists afterwards.
- `occupant_tracked_at` is false for a file phig has no row for and true for one
  it does.
- Apply re-verifies: a destination occupied between plan and apply is **skipped**,
  and one that became free is written where it was planned.
- Sidecars and an attached JPEG travel with their parent, count in
  `files_written`, and do not move `done`/`total`.
- Copy and move both honor per-item overrides.
- The plan freed before apply starts still applies correctly.
- Cancel between files leaves the logged operation `interrupted` and disk and
  database consistent with each other.
- A copy over an existing destination under `overwrite` restores the backup when
  the database step fails, which is `organize_exec.h`'s stated contract and is
  now reachable through the ABI.

## Outcome

Shipped 2026-08-09. 13 new ABI symbols (69 total, ABI 1.3, soname unchanged),
36 new tests, 765 passing. Clean under `-DPHIG_WERROR=ON`, ASan/UBSan and TSan.

`cli/`'s `cmd_organize` is now argv parsing, option filling and rendering; the
selection, planning and execution it used to hold live in `OrganizeEngine`.
Output is unchanged.

### What the design got wrong about the code

Three things, all found by reading what was being extracted rather than by a
failing test. They are the reason plan 054 exists.

1. **`--on-conflict overwrite` never replaced a file that exists on disk.**
   `cmd_organize`'s `is_protected` covers any existing path and
   `resolve_conflict` renames around a protected destination, so the policy
   differed from `rename` only on an intra-run collision. Decision 4 originally
   claimed the opposite and was corrected before implementation.
2. **`move_file` is a second guard**, independent of the planner's. It refuses to
   overwrite (plan 022's no-clobber rename), so an authorized override still
   failed at the syscall. `execute_organize_action` gained `allow_overwrite`,
   which renames the occupant aside and restores it if anything downstream
   fails — the same dance the copy branch already did unconditionally.
3. **Progress denominators**: counting plan entries rather than actionable ones
   made a run with skips report progress toward a total it could never reach.

### Marked completed prematurely, then reverted

Worth recording because it explains why certain tests exist. The plan was marked
`completed` with two defects outstanding, both in paths nothing exercised:

- `phig_op_poll` had no case for the `Plan`, `Copy` and `Move` kinds, so polling
  an organize operation returned `false` forever and decision 3's field mapping
  was specified but unimplemented. Every ABI test used `phig_op_wait`, so nothing
  touched the poll path.
- That same missing case was a `-Wswitch` error under `-DPHIG_WERROR=ON`, which
  is what CI builds with. Local builds default to warnings-off, and build output
  was being filtered for `error:` only, so it was hidden twice over.

Five tests this plan's own Testing section names had also not been written. Both
defects and all five gaps are closed.
`AbiOrganizeTest.PollReportsPlannedThenOneUpdatePerParent` exists specifically
because its absence is what hid the switch.

### Not verified locally

Two CI gates this machine cannot reproduce: the **gtk job** (Linux-only; the ABI
change is additive with the soname unchanged, so `gtk/` should link untouched,
but that is reasoning rather than a run) and the **ubuntu `-Werror` build**
(same config, different compiler).

Real-run check: `phig cp --dry-run` listed three planned copies and created
nothing; the real run created all three under `out/2026/08/`.

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
