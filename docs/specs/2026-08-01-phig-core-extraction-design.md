# phig-core extraction — Design

Status: draft
Created: 2026-08-01

## What this is

Move phig's orchestration logic out of the CLI executable and into the shared
library, so the code that decides what happens to a user's files is testable,
reusable, and owns the qualities that matter: performance, safety, correctness.

Four defects found during the audit are fixed as part of the move, because each
lives in code being relocated anyway.

## Why

An audit of the current layering found the library is a strong *utility* library
and a hollow *application* library. Roughly **1,800 lines of domain logic sit in
`src/main.cpp`** versus ~4,200 across all core modules. The primitives
(`database.cpp`, `paths.cpp`, `hasher.cpp`) are solid and well covered. The
orchestration — the thread pools, the batching, the skip decisions, the
duplicate algorithm, the organize planning — lives in an executable that nothing
can link against and no test exercises.

Concretely, **zero of the 498 passing tests invoke the binary or cover
`cmd_scan`'s 415 lines**, the duplicate finder, organize planning, `cmd_thumbs`,
or `cmd_face`. The suite measures the primitives; the composition is untested.

## Requirements

1. **`main.cpp` becomes argv parsing and output rendering.** Domain logic moves
   to the core.
2. **The core performs no I/O and holds no global state.** No `std::cout`, no
   `std::cin`, no `g_interrupted`. Progress and cancellation are injected.
3. **Behavior is preserved** except where a defect is being fixed, and each such
   change is deliberate and tested.
4. **The four audited defects are fixed** (see "Defects").
5. **Extracted logic gains direct tests** that do not go through the CLI.

## Non-goals

- Growing the C ABI. `libphig.so` (728 lines, 41 symbols) is maintained and
  tracks the core, but gains no new surface here. Whether the GTK app links
  `phig-core` or `libphig.so` is deferred until the GUI reveals what it needs.
- Extracting every command. Commands that already delegate well
  (`tag`/`gallery`/`fav`) stay as they are.
- Changing the CLI's output, flags, or behavior as observed by a user.

## Naming

The CMake target `phig-lib` is renamed **`phig-core`**, producing
`libphig-core.a`. It is a static library — a convenience target compiled once
and linked into both the `phig` binary and `phig_abi`, with no runtime artifact
of its own. The current name collides confusingly with `libphig.so`, which is a
different thing entirely.

Final layering:

```
phig-core (static, C++, internal — the engine)
    ├── phig        (CLI: argv + rendering)
    └── libphig.so  (C ABI: thin adapter, optional)
```

## Defects fixed

All four were found with evidence during the audit.

### 1. `--fail-on-error` aborts the process

`main.cpp:602` rethrows inside a worker lambda; the joins at `:620` have no
catch, so `std::terminate` runs before the handler at `:2623`. Reproduced:
`terminate called recursively`, SIGABRT, exit 134, zero rows committed, WAL left
uncheckpointed.

Fix: the engine captures the first worker exception, signals cancellation to the
others, joins cleanly, and returns or rethrows on the calling thread once all
workers have stopped.

### 2. Near-duplicate detection is O(n²)

`main.cpp:783-795` is an all-pairs nested loop. Measured:

| images | time |
|---|---|
| 5,000 | 145 ms |
| 10,000 | 389 ms |
| 20,000 | 1.4 s |
| 40,000 | 5.2 s |

Clean quadratic; 100k ≈ 33 s, 1M ≈ 54 min. For a photo manager those are normal
collection sizes.

Fix: BK-tree over Hamming distance, built once and queried per image. Exact
results must be preserved — the same groups, in the same order — so the existing
CLI output is unchanged. This is verified by comparing old and new
implementations over a generated corpus, not by inspection.

### 3. SHA256 reads whole files into memory

`hasher.cpp:155` allocates a buffer the size of the file. Measured 1.85 GB peak
RSS on a 945 MB video — **per worker thread**, and scan defaults to
`hardware_concurrency()`. On a many-core machine scanning video this is an OOM.

Fix: stream in fixed-size blocks. SHA-256 is incremental, so this costs nothing.
Hashes must be byte-identical before and after, verified against the existing
corpus.

### 4. Tests cover a stale fork of `apply_format`

`main.cpp:1044` defines `apply_format` in an anonymous namespace, unreachable
from tests, so `tests/test_organize.cpp:55` defines its own copy. That copy is
missing `%gallery` and `%fav`. Seven `FormatTest` cases pass against code that is
not shipped, and the two tokens that decide destination paths during `mv`/`cp`
are covered by nothing. `json_get` is duplicated the same way
(`test_organize.cpp:45` shadows `dates.cpp:31`).

Fix: move `apply_format` into the core, delete both test forks, and point the
tests at the real functions. Add cases for `%gallery` and `%fav`.

## Engine interface

Every extracted engine follows one shape: a plain function or small class taking
its inputs, a cancellation token, and a progress callback. No I/O, no globals.

```cpp
// src/progress.h — shared by every engine
struct CancelToken {
    std::atomic<bool> cancelled{false};
    bool stop() const { return cancelled.load(std::memory_order_relaxed); }
};
```

```cpp
// src/scan_engine.h
struct ScanProgress {
    size_t done = 0, total = 0;
    size_t errors = 0, skipped = 0, relinked = 0;
    std::string path;      // file just completed
};

struct ScanResult {
    size_t scanned = 0, skipped = 0, errors = 0, relinked = 0;
    size_t thumbs = 0, faces = 0;
    bool cancelled = false;
};

ScanResult scan(const ScanOptions& opts, Database& db,
                const CancelToken& cancel,
                const std::function<void(const ScanProgress&)>& on_progress);
```

**The callback fires on worker threads.** That is stated in the header, and the
CLI serializes its own printing under a mutex. This is not a new hazard — today
`main.cpp` writes to `std::cout` from workers with no lock, which the audit
observed producing interleaved output (`[K[1/20] i18.png[K[2[K[/320/20]`).
Making the contract explicit is what lets the CLI fix it.

Cancellation replaces the `g_interrupted` file-scope global (`main.cpp:51`),
which cannot work if two engines run concurrently and cannot be set by a library
caller at all. The CLI's SIGINT handler sets a token instead.

The callback and observer patterns were considered equivalent here — both
reenter caller code from a worker thread — and the callback was chosen because
phig's progress is a single event with a struct payload, not several event
kinds, so an interface would be one virtual method with extra ceremony.

## What moves

| New unit | Owns | From |
|---|---|---|
| `src/scan_engine.{h,cpp}` | file walk, thread pool, batching, skip/relink decisions, thumb/face orchestration | `cmd_scan`, `main.cpp:323-735` (415 lines) |
| `src/duplicates.{h,cpp}` | exact + near duplicate grouping, BK-tree | `main.cpp:759-1024` (249 lines) |
| `src/organize_plan.{h,cpp}` | `apply_format`, destination planning, conflict resolution | `cmd_organize`, `main.cpp:1044-1236` |
| `src/thumbs_engine.{h,cpp}` | batch thumbnail generation, thread pool | `cmd_thumbs`, `main.cpp:1699-1921` (181 lines) |
| `src/faces_engine.{h,cpp}` | detect/identify/rebuild orchestration | `cmd_face`, `main.cpp:1925-2212` (230 lines) |

`src/organize_exec.cpp` already owns per-file execution; `organize_plan` gives it
the planning half that stayed behind.

**A shared `parallel_for_each`** replaces the three near-identical hand-rolled
thread pools in `cmd_scan`, `cmd_thumbs`, and `cmd_face`. All three use the same
`std::atomic<size_t>` work-index pattern; the audit assessed one abstraction as
sufficient for all three. It takes the item count, a worker count, a cancel
token, and a per-item function, and it propagates the first exception to the
caller after joining — which is defect 1's fix, in one place.

## What stays in `main.cpp`

Argv parsing, usage text, all output rendering (text/CSV/JSON/porcelain/print0),
interactive confirmation prompts, the SIGINT handler, and the progress printer.
The CLI keeps its exceptions and C++ types; it calls the core directly, not
through the C ABI.

## Known obstacles

The audit named these specifically; the plan must address each rather than
discover them.

- **`cmd_scan` is one closure.** `flush_batch` and `worker` are lambdas capturing
  eleven locals by reference. They become members of a `ScanEngine` type.
- **`rel_path` welds display into the work loop** (`main.cpp:333`) — it closes
  over `base_dir` purely to shorten paths for printing. The engine reports full
  paths; the CLI shortens them.
- **`cmd_face identify` writes raw SQL inline** (`main.cpp:2026-2076`) reaching
  through `db.db_ptr()`. This must become a `Database` method before the faces
  engine can move.
- **`remove_exec.cpp` is a cautionary precedent, not a template.** It is a
  complete extraction that took `std::cout` and an interactive
  `std::getline(std::cin)` prompt (`remove_exec.cpp:126`) *into* the core, making
  it unusable from a library. Its prompt moves back out to the CLI as a
  confirmation callback.
- **Unchecked 128-float blob reads** at `database.cpp:1811` and
  `main.cpp:2043/2064`: `std::copy(data, data+128, ...)` on
  `sqlite3_column_blob` with no `sqlite3_column_bytes` check. A truncated
  `faces.embedding` reads out of bounds. Fixed while moving that code.

### Found later, during Part 2 research (2026-08-03)

These were not in the original audit. Each is verified with a `file:line`
reference.

- **`flush_batch` has no transaction and no exception handling**
  (`main.cpp:405-434`). It calls `db.insert_batch(infos)` then loops inserting
  faces per row. If any `insert_face` throws mid-loop, some rows are committed
  and others are not, `batch.clear()` at `:433` never runs, and nothing rolls
  back. The `needs_faces_only` path (`:523-539`) wraps *the same kind of work* in
  an explicit transaction with rollback. **Decision: fix during the extraction** —
  wrap it in one transaction matching the other path.
- **`--fail-on-error` calls `std::terminate`** (`:601-603` rethrows from inside a
  raw `std::thread` lambda; the joins at `:620` have no catch). Reproduced: exit
  134, zero rows committed, WAL left uncheckpointed. **Decision: adopt
  `parallel_for_each`, accepting the behavior change** — the process will exit
  with a clear error and a consistent database rather than aborting mid-write.
  The old behavior is the defect, so this is a deliberate, documented break.
- **All three commands share `g_interrupted`** (`main.cpp:52-53`), each installing
  and restoring a `SIGINT` handler around its own pool (`:387-388/:629`,
  `:1654-1655/:1715`, `:2068-2069/:2078`). Extracting only scan would leave three
  call sites split across two mechanisms. **Decision: migrate all three** onto
  `parallel_for_each` + `CancelToken` and delete the global.
- **`format_time` is defined in `main.cpp`** (`:80-91`) and used by the scan
  worker at `:464`, but is not in any header. It must move into the core (its
  natural home is `dates.h`) before `ScanEngine` can compile outside `main.cpp`.
- **`sqlite3_last_insert_rowid(db.db_ptr())`** is used at `:426`, `:529-530`, and
  `:2049` to get a face's rowid immediately after `insert_face`. Correct only
  because `db_mutex` serializes access and no statement intervenes. Moving this
  across an abstraction boundary risks silently reading the wrong rowid — having
  `insert_face` return the rowid removes the hazard entirely.
- **`Database` has no internal locking** (`database.h:26-36`, one sqlite3
  connection). Today `db_mutex` is a local in `cmd_scan` and every call site is
  checked by hand. An engine taking `Database&` inherits an implicit contract the
  compiler cannot enforce; the engine should own the mutex and expose no
  unsynchronized path.
- **Check-then-act across a lock boundary.** The skip/`needs_faces_only`/
  `needs_thumbs_only` classification reads DB state under `db_mutex`
  (`:469-492`), then does the work outside it. One such race is defended
  explicitly (`:520-522` re-checks `has_faces`); the general pattern is easy to
  break when phases are reassembled at different granularity.
- **The three commands are not one engine.** Their loop mechanics are identical,
  but the per-item bodies genuinely differ: scan batches DB writes with three-way
  skip logic and a post-join cleanup phase; thumbs writes no DB rows at all and
  has a single flat skip check; faces rebuild has no skip logic and wraps every
  item in its own transaction. Scan walks the filesystem; the other two iterate
  `db.get_all_images()`. One shared loop primitive, three separate engines.

## Part 2b decisions and research (2026-08-03, after Part 2a merged)

Part 2a moved all three commands onto `parallel_for_each` + `CancelToken`, so the
code below differs from the original audit. Facts re-verified against
`main.cpp` at 2876 lines.

### Engine → caller communication: a small purpose-built progress queue

An inventory of every engine↔caller channel in phig:

| Direction | What | Sites |
|---|---|---|
| engine → caller | progress updates | 3 (scan, thumbs, face) |
| engine → caller | final summary | once per command, main thread |
| caller → engine | cancellation | already solved by `CancelToken` |
| caller → engine | confirmation | 2 (`remove_exec.cpp:126`, `main.cpp:1197`) |
| caller → engine | stdin data | 1 (`main.cpp:1304`, `search --similar -`) |

**Only progress is a genuine CSP fit** — high-frequency, unidirectional, crossing
a thread boundary. Confirmation is request/response and is better served by a
`ConfirmFn` callback; a channel pair there means two queues, two close protocols,
and a deadlock if nobody answers. Cancellation is one boolean. Summary is a
returned struct. Stdin input is a one-shot read.

So: **one channel, one direction, one message type.** A general-purpose CSP
library would be mostly unused surface, and phig's dependency posture is
deliberately austere (plan 018; `inih` vendored at ~400 lines; a TOML library
rejected during the settings work for being large relative to its benefit).

```cpp
// src/progress_queue.h — bounded, never blocks the producer
template <typename T>
class ProgressQueue {
   public:
    void push(T v);        // replaces the oldest when full; never blocks
    bool pop(T& out);      // blocks until an item arrives or the queue closes
    bool try_pop(T& out);  // non-blocking
    void close();          // wakes every waiter; pop() then drains and returns false
};
```

The producer never blocks and stale progress is dropped, because a progress
update nobody has read yet is worthless once a newer one exists. The consumer
pulls on its own thread, so **no engine code ever runs in caller context** —
which makes the lock hazard below structurally impossible rather than avoided by
discipline.

### The lock hazard this replaces

`main.cpp:511` prints per-file progress **while holding `db_mutex`** — the
`lock_guard` opens at `:491` and its scope runs to `:515`, so both the print at
`:511` and the early `return` at `:512` execute under the lock. With
`std::cout` that is merely poor practice. The moment it becomes a client
callback, libphig would invoke arbitrary GUI code while holding the database
lock. The extraction must emit progress outside every lock; the restructuring is
small (the early `return` becomes a flag, and the emit moves past `:515`).

The second progress print at `:630-631` is already outside any lock, as are
`cmd_thumbs`'s at `:1715`/`:1742` and `cmd_face`'s at `:2091`.

### What moves, and what stays

**Moves to the core:** `ScanOptions` (`main.cpp:248-263`), `ProcessResult`
(`:265-268`), `process_file` (`:270-337`), `ForceOptions` (`:215-246`).

**Stays in the CLI:** `parse_force` — it parses an argv string, and a library
caller never has one. Also `ScopedInterruptHandler`, `g_active_cancel`, and
`cli_sigint_handler` (`:53-82`): signal handling is a CLI concern by design, and
the comment at `:55-57` already says the core takes a `CancelToken` and knows
nothing about signals. Also `acquire_writer_lock` (`:86-95`), which calls
`std::exit(1)`.

### `remove_exec.cpp`'s prompt becomes a callback

`src/remove_exec.cpp:126` does `std::getline(std::cin, answer)` after printing
"Type 'yes' to proceed:". That file is **already compiled into the core**, so the
"core does no interactive I/O" rule is currently violated by shipped code — a
precedent rather than a standard. It becomes a `ConfirmFn` the caller supplies:
the CLI passes its stdin prompt, and a GUI passes a dialog. This also unblocks
`rm` for the GUI's destructive-operation review flow.

`cmd_trash_empty` (`main.cpp:1197`) has the same prompt but lives in `main.cpp`,
so it stays as-is.

### The three commands remain three engines

Confirmed again post-2a. `cmd_scan` (`:339-776`, 438 lines) batches ten rows per
transaction, has three-way skip branching, holds a `db_mutex` and per-worker
`FaceDetector` array, and owns a post-join cleanup phase that relinks, demotes,
and deletes. `cmd_thumbs rebuild` (`:1644-1761`) writes **no DB rows at all** —
thumbnails are filesystem cache artifacts — so it has no mutex, no transaction,
and one flat skip check. `cmd_face rebuild` (`:2002-2103`) has no skip logic and
wraps every single item in its own transaction. Scan walks the filesystem; the
other two iterate a `db.get_all_images()` snapshot taken before parallelism
starts.

One shared loop primitive (already built), one shared progress queue, three
separate engines.

### Additional prerequisites confirmed

- **`cmd_face identify` writes raw SQL through `db_ptr()`** at `main.cpp:1947-1997`
  — two prepared statements bypassing `Database` entirely. Must become `Database`
  methods before the faces engine moves.
- **Unchecked 128-float blob reads**, current lines: `database.cpp:1813`
  (`auto_label_face`), `main.cpp:1962`, `main.cpp:1985`. All three null-check the
  pointer but none calls `sqlite3_column_bytes`, so a truncated `faces.embedding`
  reads out of bounds.
- **`Database` has no internal locking** (confirmed: no `mutex` in
  `database.h`), and `db_ptr()` is public at `database.h:231`. `cmd_scan` and
  `cmd_face rebuild` each declare their own local `db_mutex` (`:418`, `:2054`).
  An engine should own its mutex and expose no unsynchronized path.
- **No test drives any of the three commands.** They are not linked into
  `phig-tests` at all. Coverage exists only for the primitives they call, so
  every behavior these engines encode is currently unverified.

## Testing

The point of the extraction is that these become testable at all.

| Unit | Coverage |
|---|---|
| `parallel_for_each` | cancellation mid-run, exception propagation from a worker, single-threaded and N-threaded equivalence |
| `scan_engine` | skip/rescan decisions, relink, batching boundaries, error accounting, cancellation leaves the DB consistent, `--fail-on-error` returns rather than aborts |
| `duplicates` | **old vs new implementation produce identical groups** over a generated corpus; plus a performance assertion that 40k images complete well under the current 5.2 s |
| `organize_plan` | the real `apply_format` including `%gallery`/`%fav`, conflict resolution, dry-run planning |
| `hasher` | streaming hash is byte-identical to the current implementation across the existing corpus; peak RSS stays bounded on a large file |
| `thumbs_engine`, `faces_engine` | orchestration with a stub progress callback; per-file failure does not abort the batch |

Existing tests must keep passing unchanged — they cover the primitives, which are
not being modified. Where a test asserted CLI-shaped behavior that has moved, it
moves with the code.

## Sequencing

Phased so each step is independently reviewable and the risky parts come after
the scaffolding exists.

1. Rename `phig-lib` → `phig-core`; add `CancelToken` and `parallel_for_each`
   with tests. No behavior change.
2. `apply_format` → `organize_plan`; delete the test forks; add `%gallery`/`%fav`
   cases. Fixes defect 4.
3. Streaming SHA256. Fixes defect 3.
4. `duplicates` with the BK-tree, verified equivalent to the current output.
   Fixes defect 2.
5. `scan_engine`, using `parallel_for_each`. Fixes defect 1.
6. `thumbs_engine`, `faces_engine`; `db_ptr()` SQL folded into `Database`;
   `remove_exec`'s prompt moved out.

Step 5 is the largest and depends on 1 and 3. Steps 2, 3, and 4 are independent
of each other.

## Cross-references

- The audit that motivated this (findings quoted above with `file:line`
  evidence) — run 2026-08-01, recorded in this document rather than separately.
- `../../../phig-gtk/docs/superpowers/specs/2026-07-31-phig-gtk-design.md` —
  phase 2 (`phig-decode`) builds on the extracted scan engine; the decode worker
  pool plugs into `scan_engine` rather than into `main.cpp`.
