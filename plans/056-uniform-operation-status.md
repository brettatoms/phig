---
name: Uniform Operation Status
status: planned
created: 2026-08-09
description: >
  A client can only learn an operation's progress by draining its event queue.
  `phig_op_status` returns the state enum and no counts; the typed results are
  valid only once the run has finished; so every client reimplements the same
  loop — poll repeatedly, keep the last update, render that. Adds a
  non-consuming `phig_op_snapshot` returning the current absolute counters, a
  `phig_op_get_type` accessor so a caller managing several operations can
  dispatch without having remembered what it started, and enumeration of the
  operations a context has in flight. All three are uniform across every
  operation type and need no per-type knowledge, which is what makes a generic
  "operations in flight" view possible. Design settled 2026-08-09: a snapshot is
  the newest published record rather than an accumulated total, because every
  engine's per-item record already carries `total` and the queue drops its
  oldest entry rather than its newest; and enumeration hands out ids, never
  handles, which is what removes the ownership and double-free questions instead
  of documenting them. The typed result getters stay as they are, deliberately.
---

# 056 — Uniform Operation Status

**Execution script:** `docs/plans/2026-08-09-056-uniform-operation-status.md`

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

### 4. A client that lost a handle has no recovery

`phig_ctx` holds `db`, `mode`, `db_path` and `thumbs_dir` and nothing else.
`make_op` hands the handle to the caller and the context never sees it again, so
an operation is reachable only through the pointer that started it. A panel
rendering "what is running now" cannot be built from what the ABI exposes.

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

/* Appended to phig_op_stage: the operation has published nothing yet. Distinct
 * from SELECTED with total = 0, which is a run that has started and has no work
 * to do. */
PHIG_STAGE_STARTING = 4

PHIG_API phig_status phig_op_get_type(phig_op* op, phig_op_type* out);

/* The operation's current absolute counters, without consuming anything from
 * the update stream. Safe from any thread at any time — before the operation
 * has published anything, while it runs, and after it finishes.
 *
 * `path` and `error` point at static empty strings: a snapshot reports totals,
 * not the most recent file. Unlike phig_op_poll's output they carry no validity
 * window, which is what lets phig_op_info embed this struct.
 *
 * This is what a caller repainting on a timer wants. phig_op_poll stays for a
 * caller that needs every update, including per-file paths and per-file error
 * text. */
PHIG_API phig_status phig_op_snapshot(phig_op* op, phig_op_progress* out);

/* Stable for the operation's lifetime and never reused. 0 is never valid. */
PHIG_API uint64_t phig_op_id(phig_op* op);

/* One operation, by value. May gain fields: both calls below take the caller's
 * sizeof explicitly, so an older client gets only the prefix it allocated. */
typedef struct {
    uint64_t id;
    phig_op_type type;
    phig_op_state state;
    phig_op_progress progress;
} phig_op_info;

/* Fills up to `cap` entries and writes the true count to *n_total, so a caller
 * sizes with cap = 0 and then calls again. The caller owns the buffer; no
 * operation handle is returned.
 *
 * `struct_size` is the caller's sizeof(phig_op_info). It is both the array
 * stride and the write limit, which is why it is a parameter rather than a
 * member: the library has to know the element size before it can address the
 * second element, so it cannot read the size out of the elements themselves. */
PHIG_API phig_status phig_op_list(phig_ctx* ctx, phig_op_info* out, size_t struct_size,
                                  size_t cap, size_t* n_total);

/* PHIG_ERR_NOT_FOUND once the operation has been freed. */
PHIG_API phig_status phig_op_info_by_id(phig_ctx* ctx, uint64_t id, phig_op_info* out,
                                        size_t struct_size);

/* Cancel is reachable by id because it only sets an atomic flag. Nothing else
 * mutating is. */
PHIG_API phig_status phig_op_cancel_by_id(phig_ctx* ctx, uint64_t id);
```

Every symbol is additive: `PHIG_ABI_VERSION_MINOR` and the CMake `VERSION` bump,
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

## Decisions

Settled 2026-08-09. These replace this plan's original open questions.

### 1. A snapshot is the newest published record, not an accumulated total

The original question assumed a binary: either the operation accumulates
counters as it publishes, or the ABI caches the last update it saw — and
rejected the second because it is wrong for a caller that never polls. Three
facts make a composite of the second strictly simpler than the first.

- **Every per-item record is already a complete snapshot.** All four engines set
  `total` on the per-file record, not only on the one-shot stage record —
  `scan_engine.cpp:191-192`, `thumbs_engine.cpp:56-57`, `faces_engine.cpp:129-130`,
  `organize_engine.cpp:168` and `:190`. Nothing needs accumulating because no
  record is a fragment.
- **The queue drops its oldest entry, never its newest** (`progress_queue.h:34-37`).
  So the tail is the true latest published value however far behind the caller
  has fallen, which is exactly the never-polls case.
- **Reading without consuming already exists.** `ProgressQueue::for_each`
  (`progress_queue.h:67`) walks items under the lock, and libphig uses it today
  to retain error text (`operations.cpp:36-40`).

So a snapshot reads, in order: the tail of the operation's queue; failing that, a
cached copy of the last record `phig_op_poll` consumed; failing that,
`PHIG_STAGE_STARTING` with zeroed counters. No engine changes, no accumulation,
no new atomics.

Two supporting changes. `ProgressQueue` gains `bool peek_back(T&) const`,
mirroring `for_each`'s locking. And `phig_op_poll`'s per-kind mapping — currently
inline in its four-arm switch — moves into a shared `map_progress` that poll and
snapshot both call.

**That refactor is the point, not a side effect.** Plan 041 records that plan 042
shipped a `phig_op_poll` switch missing its new cases entirely, undetected
because every ABI test called `phig_op_wait` rather than `phig_op_poll`. One
mapping site cannot drift from itself.

### 2. Enumeration hands out ids, never handles

The original question named three hazards — who owns the list, what a freed
operation does to it, whether a handle obtained that way may be freed twice.
Returning ids rather than handles means none of them exists.

`phig_op_list` fills a caller-allocated buffer, so the caller owns it. Ids come
from a monotonic counter starting at 1 and are never reused, so an id never names
a different operation. A freed operation is gone from the registry and
`phig_op_info_by_id` returns `PHIG_ERR_NOT_FOUND`. `phig_op_free` remains the
only way to free, and only for the handle the caller already holds — enumeration
confers no ownership.

This is the same move decision 1 of plan 041 made for reconcile plans: remove the
lifetime rule rather than document it.

### 3. The registry uses a shared_ptr so no ordering rule appears

`phig_ctx` gains a `std::shared_ptr<OpRegistry>` — a mutex plus
`unordered_map<uint64_t, phig_op*>`. `make_op` assigns an id and registers.
`phig_op_free` deregisters **under the registry mutex before it joins or destroys
anything**, so a lookup that succeeds is guaranteed its target stays alive for
the whole read. Every by-id read holds the registry mutex across the read for
that reason. Lock order is registry then queue, and nothing takes them the other
way.

The `shared_ptr` is load-bearing rather than decorative. `phig_shutdown` is
`delete ctx` and `make_op` never stores the ctx, so today an operation and a
context are fully independent and a caller may free the context with operations
still running. A plain back-pointer would break that and add a "free every
operation before the context" rule. The operation instead holds a `weak_ptr` to
the registry, so a deregistration after the context is gone is a no-op.

### 4. A snapshot before anything is published reports `PHIG_STAGE_STARTING`

Appended to `phig_op_stage`, which `libphig/CLAUDE.md` records as carrying the
"append only, never renumber" provision the progress record lacks. `PHIG_STAGE_FILE_DONE`
is 0 and would claim a file finished when none has.

This keeps `phig_op_snapshot` an always-succeeds call and keeps two states
apart that a caller cares about: an operation that has not started, and one that
started with nothing to do. They are genuinely distinct — a thumbs run with an
empty selection publishes `Selected` with `total = 0` (`thumbs_engine.cpp:38`).

The alternative, reporting "nothing yet" through the return code, was rejected
on two counts: no existing `phig_status` fits, so it grows that enum instead; and
it makes a caller repainting on a timer handle a normal transient state as an
error.

### 5. `phig_op_get_type` returns `phig_status`, not `phig_op_type`

This plan originally proposed the direct return, and `phig_op_status` sets a
precedent by returning `PHIG_OP_FAILED` for a null handle. But there is no
honest sentinel for a type: `PHIG_OP_TYPE_INDEX` is 0, so returning it for a null
handle repeats the problem decision 4 rejects.

### 6. `phig_op_info` is versioned by an explicit size parameter

`libphig/CLAUDE.md` records that `phig_op_progress` can never gain a field —
caller-allocated, no size member, so a client built against an older header would
have a newer `libphig.so` write past the end of its buffer. It calls a size
member the cleaner fix, "free only while nothing outside the repo links libphig",
and nothing outside the repo does yet. A new record costs nothing to get right,
and `phig_op_info` is the one most likely to want a field later.

**The size is a parameter, not a member.** `phig_op_list` fills a caller-allocated
array, so the library must know the element size before it can address the second
element — it cannot recover that from a member inside elements it has not reached.
One rule for both calls is also simpler than a member on one and a parameter on
the other, so `phig_op_info_by_id` takes it too. The library writes
`min(struct_size, sizeof its own)` bytes per element and strides by
`struct_size`.

The embedded `phig_op_progress` stays frozen; growth happens after it.

## Testing

Against fixture data, not the collection.

- A snapshot before anything publishes reports `PHIG_STAGE_STARTING` and zeroed
  counters, and a thumbs run with an empty selection reports `PHIG_STAGE_SELECTED`
  with `total = 0`. This is the pair decision 4 exists to keep apart.
- A snapshot is non-consuming: snapshot, then poll, and the poll still returns
  the update.
- With the queue drained, a snapshot reports the last polled record, not zeros.
- With the caller never polling, a snapshot reports the newest published record
  rather than the oldest — the drop-oldest property made observable.
- A snapshot after `PHIG_OP_DONE` still reports the final record.
- **`phig_op_snapshot` and `phig_op_get_type` for all six types.** This is the
  conformance shape whose absence hid plan 042's missing poll cases.
- `phig_op_list` with `cap = 0` reports `n_total` and writes nothing; with a
  short buffer it fills `cap` and still reports the true total.
- `phig_op_list` with a `struct_size` smaller than the library's own writes only
  that prefix and strides by it, leaving the byte after each element untouched.
  This is decision 6's whole point and is the one test that fails if the size is
  ever read from the record instead of the parameter.
- An id returns `PHIG_ERR_NOT_FOUND` after its operation is freed, and ids are
  not reused across a free-then-start.
- Freeing the context with an operation still running, then freeing the
  operation, is clean under ASan and TSan. This is the case decision 3's
  `weak_ptr` exists for.
- Concurrent `phig_op_list` against `phig_op_free` on another thread, under TSan.

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
  returning current counters is this plan's shape, arrived at independently, and
  `GET /operations` is decision 2's list. Ids rather than handles is what makes
  the two surfaces the same vocabulary: an id survives a wire boundary, a pointer
  does not.
- **Plan 053** owns ABI parity generally; this is a usability gap in what already
  exists rather than a missing capability, which is why it is separate.
- **Plan 054** classifies every ABI symbol in `rules/operations.toml`, and its
  check 4 fails a build when one is neither an operation surface nor on the
  read-only allowlist. Five of the six new entry points are read-only and belong
  on that allowlist, with `phig_op_cancel_by_id` beside `phig_op_cancel`, which
  is already there. Land them in the same change rather than leaving CI to
  discover them.
- `libphig/CLAUDE.md` records that `phig_op_progress` cannot gain a field without
  a `SOVERSION` bump. This plan reuses that struct unchanged and does not consume
  the budget. `phig_op_info` is a new record and takes the size-member fix the
  same note calls cleaner and still free — see decision 6.
