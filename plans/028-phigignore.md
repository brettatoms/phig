---
name: .phigignore (gitignore-style ignore files)
status: planned
created: 2026-07-12
description: >
  Hierarchical, per-directory `.phigignore` files with full gitignore
  semantics, honored by `scan` to skip files and prune directories. Introduces
  a shared glob primitive (`glob.{h,cpp}`) supporting `*`, `?`, `[..]`, and
  `**`, which `--match`/`--exclude` also adopt. Scan stays additive (never
  removes rows or deletes files); a new `rm --ignored` mode explicitly drops DB
  rows for currently-ignored files.
---

# .phigignore

## Goal

Let a user keep files out of the phig catalog declaratively, the way
`.gitignore` keeps files out of git. Today the only ways to exclude files are
per-command `--exclude` globs (not persistent) and manual `rm`. A `.phigignore`
file lets ignore rules live next to the photos they apply to and take effect on
every scan.

`.phigignore` files are **hierarchical and per-directory**, matching gitignore:
a `.phigignore` in any directory applies to that directory's subtree, its
patterns anchor relative to that file's location, and nested files stack
(deeper files refine shallower ones, including re-includes via `!`).

Scope split, decided up front:

- **`scan`** honors `.phigignore` to skip new files and prune ignored
  directories. It stays purely additive: it never removes existing DB rows and
  never deletes files. Adding an ignore rule does not retroactively change the
  catalog on its own.
- **`rm --ignored`** (new mode on the `rm` command from plan 027) explicitly
  drops DB rows for files that are currently ignored. This is the deliberate
  "reconcile the catalog to my ignore rules" action, kept separate from scan so
  scan never surprises with removals.

## Non-goals

- **No file deletion.** Ignoring never touches files on disk. `rm --ignored`
  only removes DB rows (db-only semantics); the files stay put.
- **No global/central ignore file** in this plan. Only per-directory
  `.phigignore` discovered during the walk. A global ignore could be a later
  addition.
- **No `.phigignore` indexing.** The ignore files themselves are never indexed
  (they aren't media) and are not treated as sidecars.
- **No change to `--exclude`/`--match` semantics beyond the glob additions.**
  They gain `[..]` and `**` (see "Shared glob core"); anchoring, dir-only, and
  negation remain gitignore-only concepts, not applied to those flags.

## Gitignore semantics

Full gitignore-style matching per pattern line:

- `#` at line start → comment; blank lines ignored. A literal leading `#` or
  `!` is escaped with a backslash (`\#`, `\!`).
- Glob wildcards within a path segment: `*` (any run except `/`), `?` (one
  char except `/`), `[abc]` / `[a-z]` / `[!..]` character classes.
- `**` spans directory boundaries: `**/foo` (any depth), `foo/**` (everything
  under foo), `a/**/b` (zero or more dirs between).
- Leading `/` anchors the pattern to the directory containing the
  `.phigignore` (otherwise the pattern matches at any depth below it).
- Trailing `/` restricts the match to directories.
- Leading `!` negates: re-includes a path a previous pattern excluded. Order
  matters — the last matching pattern wins.
- A path is tested against the applicable stack of `.phigignore` files from the
  scan root down to the file's directory; deeper files' patterns are evaluated
  after (and can override) shallower ones, and negation cannot re-include a
  file if a parent **directory** is excluded (gitignore's rule — a pruned
  directory is never descended into, so its contents can't be re-included).

## Shared glob core

phig's current globber (`glob_match` in `selection.cpp`) supports only `*` and
`?`. gitignore needs `[..]` and `**` and segment-aware `*`. Rather than fork a
second matcher:

- **New module `src/glob.{h,cpp}`** holds the enhanced glob primitive:
  `*`, `?`, `[..]` (including ranges and `[!..]` negation), and `**` with
  gitignore's cross-segment semantics. It operates on a pattern and a candidate
  string and knows about `/` boundaries.
- **`selection.cpp`** (`glob_match` / `passes_selection`) is reimplemented on
  top of `glob.{h,cpp}`. As a result `--match` and `--exclude` gain `[..]` and
  `**` — pure additions; existing `*`/`?` patterns behave unchanged.
- **New module `src/phigignore.{h,cpp}`** builds the gitignore rule layer
  (parsing a `.phigignore`, anchoring, trailing-`/` dir-only, `!` negation,
  the per-directory stack) on top of `glob.{h,cpp}`.

This keeps one glob primitive, with the gitignore-only rules isolated in
`phigignore` and the selection flags isolated in `selection`.

## Scan integration

`scan` already walks with `fs::recursive_directory_iterator` and prunes hidden
directories via `disable_recursion_pending()` (see `scan_directory` in
`scanner.cpp`). The ignore layer hooks into the same walk:

- **Discovery**: on entering a directory, if it contains a `.phigignore`, parse
  it and push its patterns (anchored to that directory) onto the active stack;
  pop when leaving the subtree.
- **Directory pruning**: a directory matching an ignore rule (e.g.
  `node_modules/`, `.thumbnails/`) is not descended into — `disable_recursion_
  pending()`, same mechanism as hidden dirs. This is faster and matches
  gitignore.
- **File skipping**: a regular file matching the active ignore stack is not
  collected for indexing.
- The scan root's own `.phigignore` (if present) is honored; the root directory
  itself is always scanned even if a parent would ignore it (you asked to scan
  it).
- Because ignore evaluation happens inside the existing walk, it composes with
  the current hidden/temp-name skipping and media-extension filtering.

`scan_directory`'s signature grows an ignore-aware path (or a callback/predicate
the walk consults); exact shape is an implementation detail, but the pattern
stack lives with the iterator, not in global state.

Scan does **not** remove DB rows for files that are now ignored — that is
`rm --ignored`'s job.

## `rm --ignored` (plan 027 addition)

Add a fifth mode to the `rm` command (plan 027 defines trash / `--hard` /
`--db-only` / `--orphans`):

- **`--ignored`**: drop DB rows for files whose paths are currently ignored by
  the applicable `.phigignore` files. db-only semantics — the file is left on
  disk (ignore never deletes), only the row is removed, logged as `purge_row`.
- **Mode**: mutually exclusive with the other four. Any two mode flags is an
  error.
- **Selection**: like `--orphans`, `--match` is **optional**. No `--match`
  evaluates every DB row against its directory's ignore stack; `--match`
  narrows the check to a subtree. (The other file-removing modes still require
  `--match`.)
- **Evaluation**: for each candidate row, resolve the `.phigignore` stack for
  that file's directory and test the path. The DB holds absolute canonical
  paths from possibly several scan roots, so there is no single library root to
  anchor against: collect `.phigignore` files by walking the filesystem up from
  the file's directory (stopping at the filesystem root or the user's home,
  decided in implementation). This reuses the same `phigignore` module scan
  uses, so scan and `rm --ignored` agree on what "ignored" means.
- **`--dry-run`**, the broad-selection guard, and the activity-log operation
  (`rm`) all apply exactly as for the other modes.

This plan updates `plans/027-rm-command.md`: add `--ignored` to the mode list,
command surface, mode-exclusivity rule, optional-`--match` note (alongside
`--orphans`), and testing.

## Config

- A `[scan]`-level toggle (e.g. `respect_phigignore = true`, default true) so
  ignore handling can be disabled without deleting files. Follows the existing
  config pattern in `config.{h,cpp}`.
- Consider a `--no-ignore` scan flag to bypass ignore files for a single run
  (parallel to `--include-hidden`). Decide during implementation; default
  behavior is to respect ignore files.

## Module structure

- **New `src/glob.{h,cpp}`** — enhanced glob primitive (`*`, `?`, `[..]`,
  `**`), `/`-aware. No phig-specific logic.
- **New `src/phigignore.{h,cpp}`** — parse a `.phigignore`, represent a pattern
  with its anchor/dir-only/negation flags, maintain a per-directory pattern
  stack, and answer "is this path ignored?" using `glob`.
- **`src/selection.{h,cpp}`** — `glob_match` reimplemented on `glob`;
  `passes_selection` unchanged in behavior except `--match`/`--exclude` now
  accept `[..]`/`**`.
- **`src/scanner.{h,cpp}`** — `scan_directory` (and its recursive walk) consult
  the ignore stack for directory pruning and file skipping.
- **`src/main.cpp`** — `rm --ignored` mode wiring (in the `rm`/`remove_exec`
  path from plan 027); `--no-ignore` scan flag if adopted.
- **CMake** — add `glob.cpp`, `phigignore.cpp` to the source list.

## Testing (real-behavior)

Glob core (`glob.{h,cpp}`):

- `*`/`?` unchanged vs. today; `[abc]`, `[a-z]`, `[!..]` classes; `**` at start
  (`**/x`), end (`x/**`), and middle (`a/**/b`); `*` not crossing `/`.

Gitignore layer (`phigignore.{h,cpp}`):

- Comment (`#`) and blank-line handling; escaped `\#` / `\!`.
- Leading `/` anchors to the ignore file's dir; unanchored matches at any depth.
- Trailing `/` matches directories only, not a same-named file.
- `!` re-includes a previously excluded path; last match wins.
- A file under an excluded directory cannot be re-included by a later `!`.
- Nested `.phigignore`: a deeper file refines/overrides a shallower one.

Scan integration:

- New file matching an ignore rule is not indexed.
- Ignored directory is pruned (files beneath it not indexed), verified by a
  sentinel file deep in the tree.
- An already-indexed file that a new rule now ignores keeps its DB row after a
  rescan (scan is additive — no removal).
- `--no-ignore` (if adopted) / `respect_phigignore = false` indexes ignored
  files.
- `.phigignore` file itself is never indexed.

`rm --ignored`:

- Drops rows for currently-ignored files; leaves the files on disk; logs
  `purge_row` only.
- `--match` narrows to a subtree; without `--match` checks the whole DB.
- Mutually exclusive with the other four modes (error otherwise).
- `--dry-run` reports what would be removed, changes nothing.
- A row NOT ignored is left intact.

## Follow-up

- **Global ignore file** (e.g. `config_dir()/phigignore`) applied to every
  scan, layered beneath per-directory files.
- The `--path` cleanup on metadata commands and `phig undo`, both already noted
  in plan 027's follow-up, are unaffected by this plan.
