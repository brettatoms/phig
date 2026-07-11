---
name: Import Command (scan + scoped organize)
status: planned
created: 2026-07-11
description: >
  A `phig import <src> <dest> (--move|--copy)` command that scans a source
  directory and then organizes just the files under that source into the
  destination, in one invocation. Eliminates the error-prone manual
  `--match '<src>/**'` glob that today's `scan && mv` chain requires.
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

`import` adds no new file-moving or scanning machinery. It composes the existing
`cmd_scan` and `cmd_organize` code paths.

## Scope

In scope:
- New top-level `import` command dispatched from `main.cpp`.
- Required `--move` / `--copy` selector (no default).
- Auto-scope of the organize phase to `canonical(<src>) + "/**"`.
- A curated flag surface (below); everything else comes from `config.ini`.
- `--dry-run` that previews **both** phases without writing anything.

Out of scope:
- No config keys specific to `import` (it inherits `[scan]`, `[organize]`,
  `[log]` defaults).
- No new organize/scan behavior; both phases behave exactly as their standalone
  commands do, except for the auto-scope and the dry-run preview.
- No "what did this scan insert" tracking. Scope is "everything currently under
  `<src>`," implemented purely via `--match`.

## Behavior & data flow

`phig import <src> <dest> (--move | --copy) [options]`

1. **Scan phase.** Build a `ScanOptions` for `<src>`, seeded from config
   (`[scan]` defaults: faces, thumbs, media, parallel, on-error, recursive) and
   overridden by the curated CLI flags. Run the existing `cmd_scan` path.
2. **Organize phase.** Build an `OrganizeOptions` for `<dest>`, seeded from
   config (`[organize]` format, `[log]` settings), with:
   - `is_move` set from the required `--move`/`--copy` flag.
   - An auto-injected match of `canonical(<src>) + "/**"` prepended to
     `opts.matches`. Scan stores `fs::canonical` paths, so matching the
     canonical source prefix is robust to symlinks and trailing slashes.
   - Any user-supplied `--match` / `--filter` appended, so they **compose on
     top of** (further narrow within) the auto-scope rather than replace it.
   Run the existing `cmd_organize` path.

Because organize already scopes via `--match`, files under `<src>` that were
already in the DB from a prior scan still get organized. That matches the chosen
scope: "file everything currently under `<src>`."

## Auto-scope detail

- The auto-match glob is `canonical(<src>) + "/**"`.
- `<src>` is canonicalized once (via `fs::canonical`) before use, so it matches
  the canonical paths scan writes to the DB.
- The auto-match is always present; user `--match`/`--filter` are additional
  constraints layered on top. (`passes_filters` semantics unchanged: a file must
  pass the auto-scope match *and* whatever the user added.)

## Flag surface (curated)

Import-level:
- `<src> <dest>` — positional.
- `--move` | `--copy` — **required**; error if neither is given.
- `--dry-run` — preview both phases, write nothing (see below).
- `--db <path>` — shared by both phases.

Scan-phase pass-through:
- `--recursive`, `--media <image|video|all>`, `--hidden`, `--on-error <warn|fail>`,
  `--force [hash,thumbs,faces,all]`.

Organize-phase pass-through:
- `--format <fmt>`, `--on-conflict <skip|overwrite|rename>`.
- `--match <glob>`, `--filter <glob>` — compose with the auto-scope.

From config only (no import flag): faces, thumbs, parallel. Configure once in
`config.ini`; `import` reads the `[scan]` defaults.

No flag names collide within this curated set, so no routing ambiguity.

## Dry-run (both phases)

`--dry-run` must write nothing — not the DB, not the disk.

- **Scan preview.** Reuse the scanner's file-enumeration helper (the
  `collect`-based walk in `scanner.cpp` that returns the candidate file list) to
  list what *would* be scanned, without hashing, decoding, or DB writes. Print
  the count and (optionally) the paths. This is a new, thin preview path: it
  stops at enumeration and does not call `process_file` or touch the DB.
- **Organize preview.** The scan preview wrote nothing to the DB, so the DB does
  not yet contain the `<src>` entries. To preview the organize step we cannot
  rely on `db.get_all_images()`. Two options for the organize preview, decide in
  Task 3:
  - (a) Build in-memory `ImageInfo` stubs from the enumerated paths (filename,
    extension, mtime; no EXIF) and run the same planning logic. Dates fall back
    to mtime, so the preview layout is approximate for EXIF-dated files.
  - (b) Print only the scan preview plus a note that the organize layout can't be
    previewed without scanning, and suggest running a real scan then
    `mv --dry-run`.
  Recommended: (a), clearly labeled as an mtime-based approximation, so
  `import --dry-run` gives an end-to-end picture in one command.

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

- [ ] `ImportOptions` struct and CLI parsing in `main.cpp` (required selector,
      curated flags, `--dry-run`, `--db`).
- [ ] Scan-phase construction from config + flags; run `cmd_scan`.
- [ ] Auto-scope glob (`canonical(<src>) + "/**"`) prepended; user match/filter
      composed on top.
- [ ] Organize-phase construction from config + flags; run `cmd_organize`.
- [ ] Scan-phase hard-fail aborts before organize; soft errors continue.
- [ ] `--dry-run` writes nothing; previews both phases (mtime-approx organize).
- [ ] Expose the scanner file-enumeration helper for the dry-run preview.
- [ ] Tests above pass.
- [ ] `print_usage` / README updated with the `import` command.

## File structure

- `src/main.cpp` — `ImportOptions`, `cmd_import`, dispatch case, usage text.
- `src/scanner.h` / `src/scanner.cpp` — expose the enumeration helper (currently
  internal) for the dry-run preview.
- `tests/test_porcelain.cpp` — import tests.
- `README.md` — document `import`.

## Open questions to resolve during implementation

1. Dry-run organize preview: option (a) mtime-approx stubs vs (b) scan-only
   preview with a note. Plan recommends (a).
2. Whether to print the full would-scan path list in dry-run or just counts
   (mirror `mv --dry-run`, which prints each planned action).
