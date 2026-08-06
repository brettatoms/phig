---
name: libphig C ABI
status: completed
created: 2026-07-31
description: >
  Build `libphig`, the versioned C ABI over phig's engine, so native clients
  read a collection without touching its SQLite schema — query, records,
  thumbnails, tag/gallery/favorite mutation, and a comment-preserving settings
  API. Opaque handles, flat structs, explicit free functions, no exceptions
  crossing the boundary. Executed as two superpowers plans in `docs/plans/`
  (libphig-core, libphig-settings). Long-running operations were left out; plan
  031 adds them. Retroactively indexed 2026-08-06.
---

# 034 — libphig C ABI

Retroactive index entry for shipped work. The execution plans are the detailed
record; this file exists so the index shows the change.

`libphig` is the versioned boundary between phig's engine and foreign callers.
Without it a client's only options were shelling out to the CLI and scraping
stdout, or reading phig's SQLite schema directly — which the root `CLAUDE.md`
forbids, because that schema is private and changes.

Depends on plan 033: the ABI wraps `Database` and the extracted engines.

## What shipped

Currently ABI 1.2 (`PHIG_ABI_VERSION_MAJOR`/`MINOR` in
`libphig/include/phig/phig.h`), spanning context, query, mutate, records,
settings, and thumbs entry points. C-only at the boundary — opaque handles,
flat structs, explicit free functions, no exceptions escaping — with C++
internals.

Paginated queries are pushed down into SQL against an indexed `effective_date`
column rather than filtered in the caller.

The settings API needed a new line-oriented config parser in `core/` that
retains every source line, so writing one key leaves the rest of the user's
file byte-identical, comments included. `inih` could not back it: its callback
interface cannot report line positions.

Also shipped: `phig.pc` for pkg-config consumers, thread-local `last_error`,
and `phig_thumb_generate`.

## Execution artifacts

- `docs/plans/2026-07-31-libphig-core.md` — the ABI over `Database`
- `docs/plans/2026-08-01-libphig-settings.md` — settings API + config parser
- `docs/specs/2026-08-01-libphig-settings-design.md` — settings design
- `docs/specs/2026-07-31-phig-gtk-design.md` — sections "libphig C ABI" and
  "Required phig-side changes"

## Scope left open

Long-running operations — scan, organize, remove, face detection — are absent
from the ABI. `docs/plans/2026-07-31-libphig-core.md` framed that as a design
decision, but the engines exist in `core/` and the CLI links them directly, so
the real reason is that the only client so far (`gtk/` M0) never needed them.
**Plan 031 supersedes that scoping decision** and adds them, along with the
progress/cancellation/non-blocking convention the current ABI has no
convention for. Pixel decoding stays out.

CI's lint glob is `-maxdepth 1`, so `libphig/include/phig/phig.h` is **not**
covered by the formatting gate — check it manually. See root `CLAUDE.md`.
