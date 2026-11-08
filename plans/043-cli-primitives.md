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
- **Reads that create and migrate (R70)** moved to plan 067 on 2026-11-08. 067
  changes how every command in `cli/main.cpp` opens its database, so the
  read-only open is part of the same change.
- **Nine numeric flags abort the process on a non-numeric value.**
  `std::stoi`, `std::stof`, `std::stod` and `parse_size` throw
  `std::invalid_argument` and nothing catches it. Measured:
  `scan --parallel abc` (`cli/main.cpp:1924`), `duplicates --threshold abc`
  (`:1967`) and `search`'s `--min-size`, `--max-size`, `--threshold`,
  `--face-threshold`, `--min-sharpness`, `--max-sharpness` and `--limit`
  (`:2232` to `:2264`) each give `libc++abi: terminating due to uncaught
  exception of type std::invalid_argument` and exit 134. The split is the same
  one that makes the flag inventory unscriptable: those three commands parse
  inline in `main()`, outside every `try`, while `thumbs rebuild --parallel`
  (`:1135`) and `face rebuild --parallel` (`:1428`) sit inside `cmd_*` calls
  wrapped at `:2158` and `:2189` and exit 1 with `[error] thumbs: stoi: no
  conversion`. `--min-duration` and `--max-duration` already carry their own
  `try`/`catch` (`:2285`, `:2292`), so the defect was noticed once and fixed at
  one call site rather than at the parser.

  **Nothing is at risk.** Every one of the nine converts during argv parsing,
  before any `Database` is constructed — measured, `phig scan <dir> --parallel
  abc --db <new path>` aborts and leaves no database file at that path. So this
  is a robustness defect, not a data-safety one: exit 134 is a code no script
  can tell from a crash, and the message is a C++ runtime's rather than phig's.

  It is deliberately **not** a rule. It was written as one (R82) and removed
  after review: a `cli/`-anchored rule has to name a user asset a command can
  destroy, because the anchor's whole point is that some plan moves the guard
  into `core/` where every surface inherits it. There is no asset here and
  nothing to move — `core/` never sees argv. `rules/README.md` now states that
  bar under "Anchors outside `core/` and `libphig/`", with this as the worked
  example of what fails it. The fix is one `try`/`catch` per site, or one
  helper; it lands here because this is the open plan that owns `cli/`.
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
