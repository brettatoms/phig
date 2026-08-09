---
name: Uniform Operation Status
status: designing
created: 2026-08-09
description: >
  A client can only learn an operation's progress by draining its event queue.
  `phig_op_status` returns the state enum and no counts; the typed results are
  valid only once the run has finished; so every client reimplements the same
  loop — poll repeatedly, keep the last update, render that. Adds a
  non-consuming `phig_op_snapshot` returning the current absolute counters, and
  a `phig_op_get_type` accessor so a caller managing several operations can
  dispatch without having remembered what it started. Both are uniform across
  every operation type and need no per-type knowledge, which is what makes a
  generic "operations in flight" view possible. The typed result getters stay
  as they are, deliberately.
---

# 056 — Uniform Operation Status

## What is already uniform

`phig_op_poll`, `phig_op_status`, `phig_op_cancel`, `phig_op_wait`,
`phig_op_free`, `phig_op_error_count` and `phig_op_error_at` are one call each
for every operation. The lifecycle plan 031 established is genuinely shared, and
plan 042's organize operations joined it without adding a symbol.

## What is missing

### 1. There is no non-consuming way to ask how far along a run is

`phig_op_status` returns `phig_op_state` — RUNNING, DONE, CANCELLED, FAILED —
and nothing else. The typed results (`phig_op_index_result` and friends) are
only valid once the operation has left RUNNING. So while a run is in flight, the
sole source of progress is `phig_op_poll`, which **pops** one update off a
bounded queue that drops its oldest entry under back-pressure
(`core/progress_queue.h`).

Every client therefore writes the same loop: poll until empty, keep the last
update, render that, repeat on a timer. A GUI repainting at a fixed rate does not
want an event stream; it wants what is true now.

Nothing is lost to a dropped update, because every counter is absolute rather
than a delta (`phig.h:440`). That is exactly what makes a snapshot cheap: the
operation already knows its current totals, and handing them back requires no
new bookkeeping.

### 2. A caller cannot ask what kind of operation it holds

`phig_op_index_result` on a thumbs operation returns `PHIG_ERR_INVALID_ARG`, and
nothing lets a caller ask which getter is the right one. A caller that started
the operation knows; a generic panel rendering several does not, and neither
does code that received a handle from elsewhere in an application.

### 3. Field meanings drift, documented only in prose

`processed` means files processed for index, thumbnails generated for thumbs,
nothing at all for faces (it stays 0), and files written including carried
sidecars for organize. Each is documented in a different comment in `phig.h`.

## What this adds

```c
typedef enum {
    PHIG_OP_TYPE_INDEX = 0,
    PHIG_OP_TYPE_THUMBS = 1,
    PHIG_OP_TYPE_FACES = 2,
    PHIG_OP_TYPE_ORGANIZE_PLAN = 3,
    PHIG_OP_TYPE_COPY = 4,
    PHIG_OP_TYPE_MOVE = 5
    /* Published ABI: append only, never renumber. */
} phig_op_type;

PHIG_API phig_op_type phig_op_get_type(phig_op* op);

/* The operation's current absolute counters, without consuming anything from
 * the update stream. Safe from any thread at any time — before the operation
 * has published anything, while it runs, and after it finishes. `path` and
 * `error` are empty: a snapshot reports totals, not the most recent file.
 *
 * This is what a caller repainting on a timer wants. phig_op_poll stays for a
 * caller that needs every update, including per-file paths and per-file error
 * text. */
PHIG_API phig_status phig_op_snapshot(phig_op* op, phig_op_progress* out);
```

Both are additive: `PHIG_ABI_VERSION_MINOR` and the CMake `VERSION` bump,
`SOVERSION` stays.

Together they make a generic operations view possible with no per-type code:
type, state, `done`/`total`, `errors` — uniform for every operation phig adds
later, including 041's reconcile pair.

## What this deliberately does not change

**The typed result getters stay.** `phig.h:456-459` already argues the case: a
scan reports images, videos, faces, thumbs, removed, relinked and demoted, while
organize reports applied and files written, and flattening those hands every
caller a struct where most fields are meaningless. A caller wanting specifics
knows what it started, or now asks with `phig_op_get_type`.

**Field-semantics drift is a documentation fix, not an API change.** One table
in `libphig/CLAUDE.md` naming what each field means per operation type replaces
the comments currently scattered through the header. Renaming the fields would
be a break for no functional gain.

## Open questions

1. **Where the snapshot's counters come from.** The engines currently publish
   progress and keep no running total the ABI can read; `phig_op` holds only the
   queue and the final result. Either the operation accumulates as it publishes —
   cheap, but it means writing counters from the engine thread that the caller's
   thread reads, so they need to be atomic or mutex-guarded — or the ABI keeps
   the last update it saw, which is wrong for a caller that never polls. The
   first is correct; the synchronization needs specifying.
2. **Whether operations can be enumerated.** A client that lost a handle has no
   recovery, and `phigd` will need `GET /operations` regardless. A
   `phig_op_list`-shaped accessor raises real questions — who owns the list, what
   a freed operation does to it, whether a handle obtained that way may be freed
   twice — so it may want its own plan rather than riding along here.
3. **What `stage` reports in a snapshot.** The most recently entered stage is the
   obvious answer, but a snapshot taken before anything is published has no stage
   to report. `PHIG_STAGE_FILE_DONE` is 0 and would be a misleading default.

## Relationship to other plans

- **Plan 031** established the operation lifecycle this extends.
- **Plan 041** has two open progress questions of its own — what the fields mean
  for reconcile, and which core-side progress type it uses. Those stay 041's;
  this plan settles the mechanism, not the meanings.
- **Plan 042** is where the drift became visible: organize is the fourth
  operation to redefine `processed`, and its `phig_op_poll` case was missing
  entirely at first because nothing polled.
- **Plan 052** faces the same question over HTTP, where a polled snapshot is the
  only option — there is no event stream to drain. `GET /operations/{id}`
  returning current counters is this plan's shape, arrived at independently.
- **Plan 053** owns ABI parity generally; this is a usability gap in what already
  exists rather than a missing capability, which is why it is separate.
- `libphig/CLAUDE.md` records that `phig_op_progress` cannot gain a field without
  a `SOVERSION` bump. This plan reuses the struct unchanged and does not consume
  that budget.
