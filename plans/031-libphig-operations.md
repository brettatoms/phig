---
name: libphig Operations Surface
status: completed
created: 2026-08-05
description: >
  Expose scan indexing, thumbnail generation, and face detection through libphig
  as three separate operations. One `phig_op*` lifecycle — create, poll, cancel,
  free — with typed per-operation result getters, a uniform progress record, and
  a bounded retained error list per result. Threading is handle-plus-poll. Also
  fixes two engine defects: thumbs collapses every failure into a bare bool and
  discards the reason at the catch site, and faces skips an undecodable file
  silently — no count, no reason, so an unreadable image is indistinguishable
  from one containing no faces. Scan's reconcile phase, organize (cp/mv), and the
  CLI's primitive split go to 041, 042, 043. Supersedes the scoping decision in
  `docs/plans/2026-07-31-libphig-core.md`.
---

# 031 — libphig Operations Surface

libphig covers reads and small metadata writes: 42 symbols at ABI 1.2 spanning
query, records, settings, thumbnails, and tag/gallery/favorite mutation. The
long-running operations are absent — `phig_scan`, a thumbnails entry point, and
any face entry point do not exist.

`docs/plans/2026-07-31-libphig-core.md` describes that absence as a design
decision ("Long-running operations — scan, organize, remove, face detection —
are not in the ABI; run them through the CLI"). That framing no longer matches
the code. The engines are implemented in `core/` — `ScanEngine`, `ThumbsEngine`,
`FacesEngine`, `organize_exec`, `remove_exec` — and `cli/main.cpp` includes
those headers directly and links `phig-core`. The CLI is a peer of libphig over
`core/`, not a wrapper around it.

So the gap is not a missing capability or a deliberate exclusion that still
holds. It is ABI surface that was never added, because the only client so far
(`gtk/` M0, a read-only thumbnail grid) never needed it.

## Why now

A GUI for non-CLI users cannot ship without this. A CLI user runs `phig scan` in
a terminal and watches progress on stdout; a general user has no terminal and
needs a button, a progress indicator, and a cancel. Today a GUI client's only
options are to shell out to the `phig` binary and scrape stdout, or to link
`phig-core` directly and violate the interface discipline in the root
`CLAUDE.md`.

## Operations are primitives, not one fused command

`phig scan --thumbs --faces` runs three separable jobs in one pass. The ABI
exposes them separately instead:

| Operation | Core entry point | Character |
|---|---|---|
| index | `ScanEngine::run_workers(cancel, progress)` | Additive, cancellable, restartable |
| thumbs | `ThumbsEngine::run(cancel, progress)` | Cache-only, cancellable, idempotent |
| faces | `FacesEngine::run(cancel, progress)` | Per-row, cancellable, idempotent |

All three already take `(const CancelToken&, ProgressQueue<T>*)` and return a
result struct, so this is a call into existing public methods — no extraction.

**`run_workers` is not already the index primitive.** With `opts_.thumbs` or
`opts_.faces` set it generates thumbnails inline (`scan_engine.cpp:395-410`) and
runs face detection in the same pass — that fusion is exactly what this plan
decomposes. The ABI's index operation must pass both false. The engine needs no
change; the ABI must not expose the flags.

**Cancellation semantics, verified.** `parallel_for_each` polls the token before
pulling each item, so workers stop pulling while an item already in flight runs
to completion. Threads join normally, `run_workers` falls through to its final
`flush_batch()`, and rows completed before the cancel are committed — batches are
10 items, "so cancellation loses less work". `result.completed` comes from a
completed-item count rather than re-reading the token, which the implementation
notes is deliberate: cancellation can flip after the last item finishes and would
otherwise misreport a full run as cancelled. Nothing destructive runs on this
path; `run_reconcile` is a separate call.

The cost is latency, not correctness: cancel is not instant. With faces enabled,
each in-flight item is a full detection pass, so up to `num_threads` images
finish after the button is pressed. The ABI should document cancel as
"stops promptly" rather than immediate.

Thumbnails do not need to be generated in bulk for a GUI to work.
`gtk/src/thumb-loader.c:262-277` already looks up a thumbnail and calls
`phig_thumb_generate` on a miss, on a worker thread. Bulk generation is a
warm-the-cache optimization, not a correctness requirement, and whether it beats
on-demand for the GUI's scroll pattern is measurable rather than arguable.

Face detection is not a background trickle. Each worker holds its own
`FaceDetector` at ~150MB of model state, because `cv::FaceDetectorYN` is not
thread-safe. A GUI must present it as an explicit operation with visible
progress, not something that starts silently.

### Why indexing is not decomposed further

`ScanEngine::run()` is `run_workers()` then `run_reconcile()`, with no shared
state beyond the result struct. `run_workers` is the additive half: walk, hash,
extract, upsert. That is what this plan exposes.

`run_reconcile` is the destructive half, and it is a barrier operation — it
cannot be streamed per file. The mount guard fires on "*every* row under this
directory is missing" (`scan_engine.cpp:517`), which is unknowable until the walk
completes; that guard is what stands between a user and losing an entire
external drive's metadata. RAW/JPEG demotion likewise needs to know whether a
same-basename RAW appeared anywhere in the walk. Both are batch decisions by
nature.

## Scope

### 1. Fix what makes the ABI's shape wrong — done

Two engine defects had to be fixed first, because an ABI designed around them
would encode them permanently. Neither engine was missing the *information* — the
reason exists at the failure site in both cases and is thrown away. `hasher.h`
already provides `decode_image_with_warning`, returning OpenCV's own error text
attributed per decode even under concurrency; both engines called the
`decode_image` wrapper, which logs that text and discards it.

**Thumbs collapsed every failure into a bare bool.** `generate_thumb` returns
`false` for an undecodable image, an empty sha, a bad size, or an `imencode`
failure alike, and the worker's `catch (...)` dropped any exception message. A
user with 40 failed thumbnails could not tell one corrupt file from a full disk.
The worker now decodes explicitly rather than through `generate_thumb(path, ...)`,
so decode failure, keyframe-extraction failure, encode failure, and exceptions
each keep their own reason.

**Faces skipped an undecodable file silently.** It did *not* abort the run —
`decode_image` returns an empty `cv::Mat` rather than throwing, and the worker
already handled that by skipping (the catch at `faces_engine.cpp:105` guards only
the per-image database transaction, where rethrowing is correct). The defect was
that the skip was invisible: no count, no path, no reason, and `FacesResult` had
no `errors` field, so an unreadable image was indistinguishable from one
containing no faces. A rebuild over a collection with unreadable files reported
total success. Now counted and published; the run still continues.

Both engines follow scan's existing convention: the engine does not log from a
worker thread, where another worker's progress line can overwrite the warning —
it routes the text to the caller, and logs directly only when the caller passed
no queue. `cli/main.cpp` renders both the way it already renders scan's, and
`face rebuild` gained an error count in its summary.

The three engines now report errors the same way: a count on the result and
formatted text on the progress record. The uniform ABI progress record below is
therefore honest rather than asserted.

### 2. ABI surface

One opaque `phig_op*` carrying the uniform lifecycle — create, poll, cancel,
result, free — with a **typed result getter per operation**. The lifecycle is
genuinely identical across the three; the payloads genuinely are not
(`ScanResult` has 13 fields, `ThumbsResult` 4, `FacesResult` 3 plus the new
count). A single flattened result would hand callers fields that are meaningless
for the operation they ran, with nothing catching the mistake. A kind mismatch on
a typed getter returns `PHIG_ERR_INVALID_ARG`.

The progress record is uniform across all three: stage, counts, path, error
text. That is honest only because of the fixes in part 1.

Each result carries a bounded list of retained failures plus a total count, so a
user who walks away from a long run still sees what failed. The progress stream
alone is insufficient: `ProgressQueue` drops the oldest item under back-pressure
by design, and `scan_engine.cpp:189-196` documents that hazard for the one-shot
`Walked` stage already.

That list has two producers, and needs both. Retaining only as the caller polls
would give a client that started an operation and waited an error *count* with no
way to learn which files failed — precisely the "walked away" case. So the worker
also sweeps whatever was never polled, through a non-consuming
`ProgressQueue::for_each` that leaves a polling caller's stream intact, and
`retain_error` suppresses the entry both routes see. A caller polling slowly
enough for updates to be *dropped* can still lose text, which is why the count on
the result stays the authority on how many failed.

### 3. Threading model

**Handle-plus-poll.** Not reopened — `core/progress_queue.h` already decided it,
with a rationale that only strengthens across an FFI boundary:

> Why a queue rather than a callback: a callback runs on the producer's thread,
> so an engine holding a database lock would invoke arbitrary caller code while
> holding it. Here the consumer pulls on its own thread and no engine code ever
> runs in caller context.

All four consumers in `cli/main.cpp` already drive it this way.

(`libphig/internal.h:133` is sometimes cited as the prior decision here. It is
not — it concerns the *writer lock* being non-blocking for small mutations.
Consistent in spirit, but it settles nothing about long-running operations.)

### 4. Tests

The functional risk is low: the engines are proven through the CLI. The risk is
concurrency and lifetime, and those are the tests to write first.

- Cancel mid-run, then verify the operation stopped and the result reports
  `completed = false`. Cancelling immediately after `_start` does not cover this:
  measured, the token is always seen before the parallel loop pulls its first
  item, so the test has to wait for real progress first.
- Handle freed while its thread is still running.
- Two operations started concurrently — both writing the database, not one
  writing only the thumbnail cache, or the contention being tested never happens.
- The activity log behaves as it does from the CLI. It lives in `core/`, so this
  is routing, not reimplementation.
- Per-file error capture in thumbs and faces, against real undecodable files
  rather than mocks, and retained both for a caller that polls and one that
  never does.
- Sanitizers, given the threads and the raw handle: ASan/UBSan over the whole
  suite, TSan over the operation tests with `OMP_NUM_THREADS=1`.

## Out of scope

- **Scan's reconcile phase** — plan 041. Needs `relink_moved_rows` and
  `demote_jpeg_rows` split into decide/apply halves so a GUI can show "12 files
  are gone — remove them?" before anything is deleted. That is surgery on the
  path that destroys user-authored metadata a rescan cannot rebuild, and it
  needs its own real-behavior tests. Deferring it is what makes index
  cancellable in this plan: the additive half polls the token throughout, while
  reconcile deliberately does not.
- **Organize (cp/mv)** — plan 042. The only one of the four operations with no
  engine in `core/`: `organize_plan.h` is one pure function and
  `organize_exec.h` is per-file executors, with the orchestration inline in
  `cli/main.cpp:720-744` and no progress queue at all.
- **The CLI's own primitive split** — plan 043. `scan --thumbs` / `--faces` are
  shipped flags; changing them is user-visible behavior and belongs with the
  decision about whether they stay as sugar.
- **`faces.id` rowid reuse** — plan 045. `faces.id` is `INTEGER PRIMARY KEY`
  without `AUTOINCREMENT`, so SQLite hands a deleted rowid back to the next
  insert. Any read-then-write that remembers a face id across a gap can therefore
  land on a *different* face. This plan's faces operation is lock-free, so it
  reaches that hazard from beside a CLI command the lock used to exclude —
  meaning the mitigation 045 was filed against does not survive this plan.
  Handled here only far enough to keep the operation safe to ship:
  `identify_person`'s write now matches on the embedding it compared rather than
  the row id, so it needs no lock, and `phig face name` keeps the writer lock it
  has always had, because it reads its face id from `find_closest_face` and has
  no stored embedding to match on. `name` therefore remains exposed to a
  concurrent ABI faces operation. Removing the reuse itself is 045's job.
  045 shipped: schema v8 made `faces.id` AUTOINCREMENT, so a deleted id is
  never reissued and the embedding match is no longer what makes the write
  safe. A face deleted in the gap now matches nothing on its id alone, and
  `person_id IS NULL` refuses one another labeler named first. Measured while
  writing rules R07 and R71, which record the mechanism as it stands: deleting
  `AND embedding = ?` from the predicate fails one test and leaves the
  concurrent-rebuild test green.
- **Pixel decoding.** Still absent from the ABI. `gtk/CLAUDE.md` anticipates
  decode moving out-of-process; measurement (0 crashes in 12,684 probes) argues
  in-process is defensible. That contradiction deserves its own plan.
- **The GUI itself.** This unblocks a scan button; it does not build one.
- **A daemon or IPC layer.** Plan 006 stays deferred. This is in-process ABI.

## Notes

**Versioning.** Adding symbols normally bumps `PHIG_ABI_VERSION_MINOR` and the
CMake `VERSION`, never `SOVERSION` (`libphig/CLAUDE.md`). But `phig.h:36-49` is
explicit that the version is deliberately not bumped pre-release: nothing outside
this repo links libphig, so signatures change in place and callers change with
them in the same commit. Keep 1.2 and make sure `libphig/CMakeLists.txt` still
matches — `tests/test_libphig_install` guards a stale `libphig.so.N` loading
silently.

**Lint.** `libphig/include/phig/phig.h` sits below CI's `-maxdepth 1`
clang-format glob and is not checked. Format it manually.

**New symbols** go in `phig.version` and `phig.exported_symbols` alongside the
header declaration, or they are hidden and unlinkable.

## Relationship to other plans

- **Supersedes** the "not in the ABI" scoping in
  `docs/plans/2026-07-31-libphig-core.md`. That plan is completed and stays as
  written; this records the decision that replaced it.
- **Plan 041** (scan reconcile plan/apply) — the destructive half of scan.
- **Plan 042** (organize engine + cp/mv ABI) — the fourth operation.
- **Plan 043** (CLI primitive split) — the same decomposition for `cli/`.
- **Plan 006** (HTTP/JSON API, deferred) — remains deferred and separate.
- **Plan 035** (GTK M0) — the client this unblocks.
