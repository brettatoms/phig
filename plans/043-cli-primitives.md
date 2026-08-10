---
name: CLI Primitive Split
status: designing
created: 2026-08-07
description: >
  Apply the same decomposition plan 031 gave the ABI to `cli/`: indexing,
  thumbnail generation, and face detection as primitives rather than jobs fused
  into one `scan` pass. `phig thumbs rebuild` and `phig face rebuild` already
  exist as standalone forms, so the question is what happens to `scan --thumbs`
  and `--faces` — keep as sugar, deprecate, or drop. Shipped flags, so this is
  user-visible behavior change and is deliberately not part of 031.
---

# 043 — CLI Primitive Split

Plan 031 exposed indexing, thumbs, and faces through libphig as separate
operations rather than one fused `scan`. The reasoning applies to the CLI too:
primitives benefit every future `core/`/`libphig` consumer, not just the GUI.

The CLI already has most of it. `phig thumbs rebuild` and `phig face rebuild`
are standalone commands over `ThumbsEngine` and `FacesEngine`. What remains is
`phig scan --thumbs` and `--faces`, which fuse all three into one pass.

## The question

Fusing has a real advantage the ABI does not care about: one directory walk, one
set of decode results, no second pass over the collection. A user scanning a new
import genuinely wants thumbnails generated in the same pass. Splitting for
purity would make the common CLI case slower and more typing.

So the options are:

- **Keep as sugar.** `scan --thumbs` stays, documented as running the thumbs
  primitive in the same pass. No user-visible change.
- **Deprecate.** Warn, point at `thumbs rebuild`, remove in a later version.
- **Drop.** Cleanest surface, most disruption, and loses the single-pass
  advantage.

Leaning "keep as sugar": the fusion is a genuine optimization at the CLI layer,
and the primitives exist underneath either way. But it needs deciding rather
than defaulting.

## Also in scope

- **`thumbs rebuild --size N` does not exist.** Measured: `phig thumbs rebuild
  --size 768` prints `Unknown option: --size` and exits 1. It was documented in
  root `CLAUDE.md` until that reference was corrected, is still advertised by
  phig's own help text (`cli/main.cpp:129`), and is
  supported by the engine — `ThumbsOptions::size` is a real field
  (`core/thumbs_engine.h:18`) that `ThumbsEngine` honours — but `cmd_thumbs`'s
  `rebuild` branch parses only `--match`, `--exclude`, `--force`, `--parallel`
  and `--db`. Plan 040 shipped the size-partitioned cache and the engine
  parameter and never wired the CLI to it: its decision 3 says "`phig thumbs
  rebuild --size N` populates a size upfront", and its Deferred note names only
  `thumbs clean --size N`, so this was not a recorded deferral — it is a gap the
  plan believed it had closed. The one surface a user has for filling a second
  size is the one that cannot. A parser branch, not a design question; it lands
  here because it is `cli/` argv parsing and the flag belongs to one of the
  three primitives.
- **Every CLI command opens the collection read-write and migrates it, including
  the ones that only read** (R70, `rules/boundary.toml`). All twelve `Database`
  constructions in `cli/main.cpp` pass one argument, so all twelve take the
  default `read_only = false`, and `cmd_search` and `cmd_duplicates` call
  `init_schema` on their first two lines (`cli/main.cpp:855-856` and
  `:500-501`). `Database`'s `read_only` flag exists and works — libphig's
  `phig_init` passes it from the context mode and skips `init_schema` for a
  read-only context (`libphig/context.cpp:31-45`) — and `read_only` appears zero
  times in `cli/main.cpp`. Measured: `phig search --count` against a path
  holding no database left a 122880-byte v8 database there and exited 1 with
  `Database is empty.`; `phig duplicates` did the same; against copies of a
  collection with `meta.schema_version` set back to 7, both left it at 8;
  `phig help` and `phig --help` created nothing. So a read migrates a collection
  forward irreversibly — `init_schema` refuses a database newer than the build,
  so a `search` on one machine can make the collection unopenable by an older
  phig on another — and a mistyped `--db` leaves a stray empty collection behind
  instead of an error. R20 and R21 record the dry-run corners of this for `rm`
  and organize. It lands here because the fix is a `cli/` change: pass
  `read_only = true` on the commands that only read, and decide what those
  commands do with a database older than the build, where libphig returns
  `PHIG_ERR_SCHEMA`.
- Whether `cli/` moves onto plan 042's organize engine once it exists, replacing
  the inline orchestration at `cli/main.cpp:720-744`.
- Whether scan gains `--dry-run` from plan 041's reconcile plan phase.
- Whether the CLI keeps linking `phig-core` directly or moves onto libphig. Today
  it is a peer of libphig over `core/`, not a wrapper — that is deliberate and
  documented in `libphig/CLAUDE.md`, and this plan should not change it
  casually.

## Relationship to other plans

- **Depends on 031** — establishes the primitive boundaries this mirrors.
- **Plan 041** (reconcile plan/apply) — a source of `scan --dry-run`.
- **Plan 042** (organize engine) — the CLI would move onto it.
