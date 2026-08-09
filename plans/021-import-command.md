---
name: Import Command (scan + scoped organize)
status: planned
created: 2026-07-11
description: >
  A `phig import <src> [<dest>] (--move|--copy)` command that scans a source
  directory and then organizes just the files under that source into the
  destination, in one invocation. Eliminates the error-prone manual
  `--match '<src>/**'` glob that today's `scan && mv` chain requires.
  `<dest>` falls back to a new `[import] destination` config key.
---

# Import Command

## Goal

Give phig a single command for the common "bring these files into my library"
workflow: scan a source directory, then file the files found there into the
destination layout. Today this is a two-command chain —

```
phig scan <src> --recursive
phig mv <dest> --match '<src>/**'
```

— and the value of a dedicated command is almost entirely the auto-injected
`--match '<src>/**'` scope. Without that glob, `mv`/`cp` operate on the *entire*
database, so filing "just what I scanned" by hand is an easy, destructive
mistake. `import` removes that footgun by scoping the organize phase to `<src>`
automatically.

`import` adds no new file-moving or scanning machinery. It composes `ScanEngine`
with plan 042's `OrganizeEngine`.

## Scope

In scope:
- New top-level `import` command dispatched from `cli/main.cpp`.
- Required `--move` / `--copy` selector (no default).
- Auto-scope of the organize phase to `canonical(<src>) + "/**"`.
- A curated flag surface (below); everything else comes from `config.ini`.
- One new config key, `[import] destination`, making `<dest>` optional.
- `--dry-run` that previews **both** phases without writing anything.

Out of scope:
- No config keys beyond `[import] destination`; otherwise `import` inherits
  `[scan]`, `[organize]` and `[log]` defaults.
- No new organize/scan behavior; both phases behave exactly as their standalone
  commands do, except for the auto-scope and the dry-run preview.
- No "what did this scan insert" tracking. Scope is "everything currently under
  `<src>`," implemented purely via `--match`.

## Behavior & data flow

`phig import <src> [<dest>] (--move | --copy) [options]`

1. **Scan phase.** Build a `ScanOptions` for `<src>`, seeded from config
   (`[scan]` defaults: faces, thumbs, media, parallel, on-error, recursive) and
   overridden by the curated CLI flags. Run the existing `cmd_scan` path, which
   drives `ScanEngine`.
2. **Organize phase.** Build a `core/organize_engine.h` `OrganizeOptions` for
   `<dest>`, seeded from config (`[organize] format`, `[log]` settings), with:
   - An auto-injected match of `canonical(<src>) + "/**"` prepended to
     `opts.matches`. Scan stores `fs::canonical` paths, so matching the
     canonical source prefix is robust to symlinks and trailing slashes.
   - Any user-supplied `--match` / `--exclude` appended, so they **compose on
     top of** (further narrow within) the auto-scope rather than replace it.
   - `on_conflict` from `--on-conflict`, as the run-level default.

   Then `OrganizeEngine::plan()` followed by `apply(plan, is_move, ...)`, with
   `is_move` from the required `--move`/`--copy` flag.

Plan 042's per-item `set_resolution` overrides exist on the plan `import`
computes, but `import` does not expose them. The CLI stays on the run-level
`--on-conflict` policy, per 042's decision 2 — per-item resolution is what a GUI
does with a plan it shows the user, and `import` has no way to ask.

Because organize already scopes via `--match`, files under `<src>` that were
already in the DB from a prior scan still get organized. That matches the chosen
scope: "file everything currently under `<src>`."

## Auto-scope detail

- The auto-match glob is `canonical(<src>) + "/**"`.
- `<src>` is canonicalized once (via `fs::canonical`) before use, so it matches
  the canonical paths scan writes to the DB.
- The auto-match is always present; user `--match`/`--exclude` are additional
  constraints layered on top. (`passes_filters` semantics unchanged: a file must
  pass the auto-scope match *and* whatever the user added.)

## Default destination

`<dest>` is optional. When omitted it comes from `[import] destination` in
`config.ini`, tilde-expanded like the other path keys. Omitting it with no
config key set is an error naming the key.

`core/config.cpp:89` refuses four keys outright — `force`, `dry_run`,
`on_conflict`, `ignore_mount_warning` — because a config file must not silently
change what a destructive command does. A defaulted destination looks like that
family and is not, because **phig's database stores real absolute paths.**
Changing the key changes where the *next* import lands and nothing else: files
already imported keep their rows, their paths stay correct, and nothing needs
migrating. The key is a default for one argument, not a pointer to a library
that can go stale.

What remains is smaller: a `--move` landing somewhere the user did not type. So
`import` prints the resolved destination before the organize phase runs
whenever it came from config rather than the command line. The plan phase
already computes destinations, so this costs nothing.

**One key, every importer.** The GUI's "add to library" (047), the macOS Photos
import (049), and device import (055) each need exactly one directory meaning
"where imported photos go", and it should be the same one the CLI uses. That is
why the section is `[import]` rather than `[organize]` — it names the operation,
not the caller.

**`scan_roots` is not this key.** Plan 052 needs an allowlist of directories
`POST /index` may resolve into. That is a list, it is a security boundary, and
it constrains *sources* — a destination default is none of the three. Deriving
one from the other would produce an allowlist that happens to be right once and
silently excludes the second import target someone adds.

`cp` and `mv` keep a required destination. They select across the whole database
by default, which makes an unstated destination a different proposition there.

## Removable sources

A card reader, a phone in mass-storage mode, and an external disk all mount as
ordinary directories (`/Volumes/…`, `/run/media/…`), so `import` already handles
them with no new mechanism. What they need is care that a local directory does
not:

- **`--move` off a card deletes the only copy.** Until the move completes, the
  card holds the sole copy of those files. `execute_organize_action` is already
  per-file transactional with on-failure compensation, so this is a matter of
  saying it in the docs rather than new code — but it is the reason `--move` and
  `--copy` have no default and never should.
- **`--dry-run` writes no rows.** Stated above; removable media is the reason.
- **A card is not a library.** After a `--move` import the source rows are gone
  from `<src>` and live under `<dest>`, which is the intended end state. After a
  `--copy` import the DB holds rows for files on a volume that will be ejected.
  That is `rm --orphans` and mount-guard territory (plan 032), not a defect, but
  the docs should say which of the two a user wants.

Mount discovery — enumerating what is plugged in and offering it — is a client
feature, not a CLI one, and belongs to 047. Nothing in this plan scans for
devices.

## Flag surface (curated)

Import-level:
- `<src> [<dest>]` — positional; `<dest>` falls back to `[import] destination`.
- `--move` | `--copy` — **required**; error if neither is given.
- `--dry-run` — preview both phases, write nothing (see below).
- `--db <path>` — shared by both phases.

Scan-phase pass-through:
- `--recursive`, `--media <image|video|all>`, `--hidden`, `--on-error <warn|fail>`,
  `--force [hash,thumbs,faces,all]`.

Organize-phase pass-through:
- `--format <fmt>`, `--on-conflict <skip|overwrite|rename>`.
- `--match <glob>`, `--exclude <glob>` — compose with the auto-scope.

From config only (no import flag): faces, thumbs, parallel. Configure once in
`config.ini`; `import` reads the `[scan]` defaults.

No flag names collide within this curated set, so no routing ambiguity.

## Dry-run (both phases)

`--dry-run` must write nothing — not the DB, not the disk.

- **Scan preview.** Enumerate with `scan_directory()` (already public in
  `core/scanner.h`) to list what *would* be scanned, without hashing, decoding,
  or DB writes. Print the count and (optionally) the paths. It stops at
  enumeration and never calls `process_file` or touches the DB.
- **Organize preview.** Plan 042 removes most of this problem.
  `OrganizeEngine::plan()` writes nothing by construction and returns
  destinations, conflicts, and each conflict's resolution — so on `cp`/`mv`,
  `--dry-run` *is* the plan phase and needs no preview machinery at all.

  It does not chain directly here, because `plan()` selects from the database
  and a dry-run scan has not written the `<src>` rows. Two ways out:

  - (a) Build in-memory `ImageInfo` stubs from the enumerated paths (filename,
    extension, mtime; no EXIF) and run the same planner over them. Dates fall
    back to mtime, so the layout is approximate for EXIF-dated files.
  - (b) Run the scan for real, then plan and stop. Indexing is additive — it
    writes rows, it does not move or delete files — so "writes nothing" could be
    read as "moves nothing", and the organize half would then be a genuine
    `OrganizePlan` with real dates and real conflicts.

**Resolved 2026-08-09: (a).** (b) gives the better preview, and that is the
whole of its case. Against it: import's subject is files from somewhere else,
and that somewhere is often removable — a card, a phone, an external disk.
Leaving rows pointing into `/Volumes/EOS_DIGITAL/DCIM` after the card is ejected
is the state plan 032's mount guard exists to be careful about, and a `--dry-run`
that leaves them behind has failed at the one thing its name promises. The
mtime approximation is the price, and it is bounded: label the output as a
directory-shape preview rather than a destination guarantee.

## Error handling

- Scan-phase hard failure (`--on-error fail` and an error occurs): stop; do not
  run the organize phase.
- Scan-phase soft errors (default `--on-error warn`): warn, skip the file,
  **continue to the organize phase.** Matches standalone `scan` + `mv` behavior.
- Organize phase: unchanged. Reuses the existing per-file transactional safety,
  conflict protection, and activity-log write path (plan 020). The log records
  the operation as `mv`/`cp` exactly as today (import is a thin wrapper, not a
  new log verb).

## Testing

Porcelain-style tests (see `tests/test_porcelain.cpp`):
- `import --copy`: fixture `<src>` → assert only `<src>` files land in `<dest>`;
  originals remain; a DB entry that predates the import and lives **outside**
  `<src>` is untouched.
- `import --move`: assert `<src>` originals are gone and appear under `<dest>`.
- Auto-scope composition: pre-seed the DB with files outside `<src>`, run
  `import`, assert only `<src>` files moved (the entire-DB footgun is gone).
- User `--match` composes: `import --copy --match '*.jpg'` under a mixed `<src>`
  moves only jpgs from `<src>`, nothing outside it.
- Missing selector: `import <src> <dest>` with neither `--move` nor `--copy`
  exits non-zero with a clear message.
- `--dry-run`: DB row count and on-disk `<dest>` unchanged after the run.

## Checklist

- [ ] `ImportOptions` struct and CLI parsing in `cli/main.cpp` (required
      selector, curated flags, `--dry-run`, `--db`).
- [ ] `[import] destination` in `core/config.{h,cpp}` with tilde expansion;
      `<dest>` optional; a resolved-from-config destination is echoed before
      the organize phase; omitting both is an error naming the key.
- [ ] Scan-phase construction from config + flags; run `cmd_scan`.
- [ ] Auto-scope glob (`canonical(<src>) + "/**"`) prepended; user
      match/exclude composed on top.
- [ ] Organize-phase `OrganizeOptions` from config + flags; `OrganizeEngine::
      plan()` then `apply()`.
- [ ] Scan-phase hard-fail aborts before organize; soft errors continue.
- [ ] `--dry-run` writes nothing; previews both phases (mtime-approx organize).
- [ ] Tests above pass.
- [ ] `print_usage` / README updated with the `import` command.

## File structure

- `cli/main.cpp` — `ImportOptions`, `cmd_import`, dispatch case, usage text.
- `core/config.h` / `core/config.cpp` — `import_destination`, parsed and
  tilde-expanded.
- `core/scanner.h` — no change: `scan_directory()` is already public, so the
  dry-run preview has its enumeration helper.
- `tests/test_porcelain.cpp` — import tests.
- `README.md` — document `import`.

## Open questions to resolve during implementation

1. Whether to print the full would-scan path list in dry-run or just counts
   (mirror `mv --dry-run`, which prints each planned action).

## Relationship to other plans

- **Depends on 042** — the organize phase is `OrganizeEngine::plan()` +
  `apply()`. `core/organize_engine.h` has that shape today; `cli/` is not on it
  yet, and `import` should not be built against the inline orchestration in
  `cmd_organize` that 042 replaces.
- **Plan 043** (CLI primitive split) — decides whether `cli/` moves onto the
  organize engine and how `--dry-run` is spelled across commands. `import`
  follows whatever it settles rather than inventing a second convention; 043
  already names import as the case where fusing thumbnail generation into the
  scan pass is genuinely wanted.
- **Plan 032** (mount guard) — the reason `--dry-run` writes no rows.
- **Plan 049** (macOS Photos import) is a different subject: a source phig
  cannot walk, exported by a TCC-authorized app. It shares the word, not the
  mechanism — but it reads the same `[import] destination`.
- **Plan 055** (device import) covers the sources this command cannot reach:
  PTP/MTP phones and cameras, which expose no filesystem path. A mounted card is
  this plan's; an unmounted device is 055's.
- **Plan 047** (GUI design) owns mount discovery and the import UI. This plan
  adds no device enumeration.
- **Plan 052** (phigd) — S1b is import over HTTP, and also waits on 042.
  `[server] scan_roots` is deliberately not this plan's destination key.
- **Plan 053** — lists this command as needing ABI exposure once it exists.
