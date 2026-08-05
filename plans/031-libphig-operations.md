---
name: libphig Operations Surface
status: planned
created: 2026-08-05
description: Expose the long-running operations — scan, organize, remove, face detection — through libphig, so a GUI client can run them without shelling out to the CLI. The engines already exist in `core/` and are exercised by the CLI; this is ABI surface over working functionality plus a progress/cancellation convention that survives the C boundary. Supersedes the scoping decision in `docs/plans/2026-07-31-libphig-core.md` that kept these out of the ABI.
---

# 031 — libphig Operations Surface

libphig covers reads and small metadata writes: 42 symbols at ABI 1.2 spanning
query, records, settings, thumbnails, and tag/gallery/favorite mutation. The
long-running operations are absent — `phig_scan`, `phig_organize`,
`phig_remove`, and any face entry point do not exist.

`docs/plans/2026-07-31-libphig-core.md` describes that absence as a design
decision ("Long-running operations — scan, organize, remove, face detection —
are not in the ABI; run them through the CLI"). That framing no longer matches
the code. The engines are implemented in `core/` — `ScanEngine`,
`FacesEngine`, `organize_exec`, `remove_exec` — and `cli/main.cpp` includes
those headers directly and links `phig-core`. The CLI is a peer of libphig over
`core/`, not a wrapper around libphig.

So the gap is not a missing capability or a deliberate exclusion that still
holds. It is ABI surface that was never added, because the only client so far
(`gtk/` M0, a read-only thumbnail grid) never needed it.

## Why now

A GUI for non-CLI users cannot ship without this. A CLI user runs `phig scan`
in a terminal and watches progress on stdout; a general user has no terminal
and needs a button, a progress indicator, and a cancel. Today a GUI client's
only options are to shell out to the `phig` binary and scrape stdout, or to
link `phig-core` directly and violate the interface discipline in the root
`CLAUDE.md`.

## Scope

Expose through `phig.h`:

- **scan** — directory, recursive, force flags (`hash,thumbs,faces,all`), media
  filter, hidden, on-error mode
- **organize** — cp and mv, format string, match/exclude, conflict policy,
  dry-run
- **remove** — the `rm` modes (trash, hard, db-only, orphans), dry-run
- **face detection** — the `face rebuild` path, plus naming/identify if it
  falls out cheaply

Each needs three things the current ABI has no convention for:

1. **Progress reporting.** A C callback invoked with a counted position and a
   phase, or a pollable handle. Must not assume the caller's threading model.
2. **Cancellation.** `core/cancel.h` already backs the CLI's SIGINT handling
   with progress saved; the ABI needs a way to request it and observe that it
   took effect.
3. **Non-blocking execution.** `libphig/internal.h:133` already states the
   intent: "a GUI must not freeze behind a long-running CLI job." Whether that
   means the call runs on a caller-supplied thread, spawns one and returns a
   handle, or exposes a step-wise pump is the main design question below.

The writer lock and activity log must behave exactly as they do from the CLI.
Both live in `core/`, so this is a matter of routing the calls correctly, not
reimplementing them — but it needs tests, because "the GUI corrupted the
database because it skipped the lock" is the failure this project cannot ship.

## Open design questions

- **Threading model.** Handle-plus-poll is the most portable across FFI
  consumers (GTK's main loop, Elixir NIFs, a future mobile client) and avoids
  libphig calling back into caller code on a thread the caller did not create.
  A callback is simpler for a C client and worse for everyone else. Decide
  before writing the header; changing it later is an ABI break.
- **One handle type or one per operation.** A single opaque `phig_op*` with a
  kind tag keeps the surface small. Separate types give better compile-time
  safety. Leaning single, given the symbol count discipline in
  `libphig/CLAUDE.md`.
- **Error granularity.** Scan reports per-file failures and keeps going under
  `--on-error warn`. The ABI needs to convey "finished with 12 failures"
  without a message-per-file allocation storm.
- **Whether decode comes with it.** Separate question, tracked below.

## Out of scope

- **Pixel decoding.** Still absent from the ABI. `gtk/CLAUDE.md` anticipates
  decode moving out-of-process; measurement on 12,684 probes found 0 crashes,
  which argues in-process is defensible. That contradiction deserves its own
  plan and its own decision — it is not resolved by adding operation surface,
  and bundling it would make this plan's risk unbounded.
- **The GUI itself.** `gtk/` M0 is one milestone in and fails its own frame
  -rate bar. This plan unblocks a scan button; it does not build one.
- **A daemon or IPC layer.** Plan 006 (HTTP/JSON API) stays deferred. This is
  in-process ABI surface.
- **Changing any engine behavior in `core/`.** If an engine needs a
  cancellation or progress hook it does not have, adding the hook is in scope;
  changing what the engine does is not.

## Risk

The engines are proven through the CLI, so the functional risk is low. The real
risk is in the concurrency and lifetime story: a cancelled operation that
leaves a half-written transaction, a handle freed while its thread still runs,
or a client that starts a scan and a remove concurrently and deadlocks on the
writer lock. Those are the tests to write first.

Getting the threading model wrong is the expensive mistake — it is an ABI
break to fix, and `libphig/CLAUDE.md` is explicit that only an incompatible
change bumps `SOVERSION`.

## Relationship to other plans

- **Supersedes** the "not in the ABI" scoping in
  `docs/plans/2026-07-31-libphig-core.md`. That plan is completed and stays as
  written; this plan records the decision that replaced it.
- **Plan 006** (HTTP/JSON API, deferred) — remains deferred and separate.
- **Plan 007** (TUI/GUI, planned) — consumes this.
