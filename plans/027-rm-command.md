---
name: Remove Command (phig rm, replaces purge)
status: planned
created: 2026-07-12
description: >
  A `phig rm` command that removes selected images, with modes: trash
  (default, recoverable), `--hard` (permanent delete), `--db-only` (drop the DB
  row, leave the file — today's `purge`), `--orphans` (drop rows whose files
  are missing from disk, mount-safe), and `--ignored` (drop rows for
  `.phigignore`-ignored files; added by plan 028). Trashes/deletes the photo's sidecar
  files alongside it, leaves same-stem RAW/JPEG siblings intact, and records
  every action in the activity log. Replaces the `purge` command.
---

# Remove Command

## Goal

phig can move, copy, and purge, but it has no command that removes a photo from
disk *and* the database in one step. Today `mv` does disk + DB + logging, and
`purge` drops the DB row while leaving the file on disk. Deleting a photo for
real means running an OS `rm` and then `scan` to reconcile the database — two
steps, and the file is gone with no recovery.

`phig rm` is the single removal command. It selects images with
`--match`/`--exclude` like `mv`/`cp`, and removes them in one of five modes:

- **trash** (default) — move the file to a phig-managed trash and drop the DB
  row. Recoverable.
- **`--hard`** — permanently delete the file and drop the DB row. Irreversible.
- **`--db-only`** — drop the DB row, leave the file untouched. This is exactly
  what `purge` does today. Use it to stop tracking files you still keep on disk.
- **`--orphans`** — drop rows whose files are already missing from disk. Never
  touches disk (nothing to delete). Mount-safe: a row is not dropped when its
  parent directory is missing (an unmounted/unreachable drive is not treated as
  deletion). Use it to prune stale catalog entries.
- **`--ignored`** — drop rows for files currently ignored by `.phigignore`
  (plan 028). db-only semantics: the file is left on disk (ignore never
  deletes), only the row is removed. Use it to reconcile the catalog to the
  ignore rules. Defined by plan 028.

The trash, `--hard`, and `--db-only` modes require at least one `--match`.
`--orphans` and `--ignored` make `--match` optional: with no `--match` they
sweep the whole database (for orphaned / ignored rows respectively); with
`--match` they restrict the sweep to that subtree.

Because db-only removal is now one mode of `rm`, **the `purge` command is
removed** (see "Replacing purge"). Sidecar files travel with the photo in trash
and hard modes. Every action is recorded in the activity log so a future
`phig undo` can reverse it.

## Non-goals

- **No restore command in v1.** `rm` is log-only: it records enough to reverse
  later, but ships no `phig undo` / `phig trash restore`. Recovery today is
  manual (move files out of the trash dir, rescan). A separate follow-up plan
  covers the general `undo` command.
- **No same-shot sibling deletion.** Deleting a JPEG never touches its
  same-stem RAW (or vice versa). Only the selected media file and its sidecars
  are removed.
- **No auto-expiry / retention policy** on the trash in v1. The only trash
  maintenance command is `phig trash empty`.
- **No native OS trash** (freedesktop `.Trash-$uid` / macOS `~/.Trash`). See
  "Why a phig-managed trash" below.

## Command surface

```
phig rm [--match <glob>] [--exclude <glob>]
        [--hard | --db-only | --orphans | --ignored] [--dry-run] [--yes] [--db <path>]

phig trash empty [--yes] [--db <path>]
```

- **Mode** (mutually exclusive; default trash): none = trash file + drop row;
  `--hard` = permanently delete file + drop row; `--db-only` = drop row, leave
  file; `--orphans` = drop rows whose files are missing (mount-safe), disk
  untouched; `--ignored` = drop rows for `.phigignore`-ignored files (plan 028),
  disk untouched. Any two mode flags together is an error.
- **Selection**: `--match` / `--exclude`, same as the other file commands
  `mv`, `cp` (`--exclude` excludes and wins over `--match`). Trash, `--hard`,
  and `--db-only` require at least one `--match` (like the old `purge`).
  `--orphans` and `--ignored` make `--match` **optional** — no `--match` sweeps
  the whole DB; `--match` narrows the sweep. Note `--path` is a metadata-command flag
  (`tag`/`gallery`/`fav`); the file commands don't take it, and `rm` follows the
  file-command convention. (Whether `--path` should stay on the metadata
  commands at all is flagged as a separate cleanup — see "Follow-up".)
- **`--dry-run`**: print every file and sidecar that would be removed and the
  mode (trash / hard-delete / db-only); touch nothing (no disk change, no DB
  write, no log entry).
- **`--yes` / `-y`**: confirm. Required for broad selections in every mode,
  reusing the old `purge` guard (see "Confirmation guard"). **`--hard` always
  requires `--yes`**, regardless of selection size, because it is irreversible.
- **`--db <path>`**: override the database path, as elsewhere.

Default mode: trash. Trashed files go to `data_dir() + "/trash"`
(`~/.local/share/phig/trash` under the default XDG data dir). `--db-only`
never touches the trash or the file.

## Why a phig-managed trash

phig must run on both desktop and headless/server machines. Native OS trash
degrades or fails headless:

- CLI shims (`gio trash` on Linux, Finder scripting on macOS) are usually
  absent on minimal servers and need a user session; unreliable over SSH.
- Native trash is two different, largely-undocumented formats
  (freedesktop `.Trash-$uid` on Linux, `~/.Trash` + per-volume `.Trashes` on
  macOS, the latter reliably reachable only via the Cocoa
  `NSFileManager trashItemAtURL:` API, i.e. Objective-C++). Two platform
  backends, most code, most edge cases.

An app-owned recoverable-delete area is a common pattern for tools that own a
collection (Lightroom / macOS Photos "Recently Deleted", git's unreferenced
objects, mail Trash folders). One code path works identically on desktop,
macOS, and headless servers, and it plugs into phig's existing activity-log
model.

The one hard problem native trash solves that we still handle ourselves is
**cross-mount deletes**: `fs::rename` fails with `EXDEV` across filesystems, so
we fall back to copy-then-delete. Native trash has the same problem (it keeps a
trash dir per mount); we accept a slower copy for the rare cross-drive case
rather than manage a trash dir per volume in v1. Permissions are not extra
burden — an unreadable/unmovable file fails identically under any scheme.

## Trash mechanics

- **Location**: a single directory `data_dir()/trash`, created on first use.
- **Move**: same-filesystem → `fs::rename` (fast, atomic). Cross-mount
  (`fs::rename` throws / `EXDEV`) → copy the bytes then `fs::remove` the
  original.
- **Name collisions inside the trash**: suffix with a counter —
  `IMG_1234.jpg`, then `IMG_1234.jpg.1`, `IMG_1234.jpg.2`, … The chosen trash
  path is what gets recorded in the log entry, so restore is unambiguous.
- **`--hard`**: `fs::remove` the file directly; no trash copy.
- **`phig trash empty`**: delete every entry under `data_dir()/trash`. Requires
  `--yes` for the same reason `--hard` does — it is a permanent delete. Does
  not touch the database or activity log (trashed rows are already removed).
- No auto-expiry in v1.

## Sidecars & siblings

- Locate the photo's sidecars with the existing `find_sidecars()` (`.xmp`,
  `.json`, `.pp3`, `.dop`, `.on1`, `.aae`, `.thm`, `.lrv`, in both stem-style
  and append-style naming).
- Each sidecar is trashed (or hard-deleted, matching the photo's mode)
  **alongside** the photo, and logged as its own entry.
- Same-stem RAW/JPEG **siblings are never touched**. Deleting `IMG_1234.jpg`
  leaves `IMG_1234.CR2` on disk. This matches how `mv` carries sidecars but not
  siblings.
- `find_sidecars()` already cedes a stem-style sidecar to a same-stem sibling
  only when that sibling media file exists on disk, so deleting one member of a
  RAW+JPEG pair will not strand or wrongly claim the other's sidecar.

## Activity log & reversibility

The activity log already supports entries with differing reversibility —
undoability is a function of what a kind records (its `restore_json` / `dst`),
not a separate flag. `rm` adds two new `OperationEntry` kinds (`trash`,
`delete_file`) for the disk action, and reuses `purge_row` for the row.

Two entry roles combine to describe a full removal: the **row deletion** and
the **disk action**.

- **Row deletion**: reuse `delete_paths_logged()`, which already writes a
  `purge_row` entry per removed row carrying the images-row `restore_json`
  snapshot (faces / embeddings are not serialized in this slice; the future
  `undo` defines that). This is unchanged from `purge` and is what makes the
  DB row restorable.
- **Disk action**: in trash / hard modes, one new entry per file recording what
  happened to the bytes. `--db-only` and `--orphans` record no disk entry (the
  file is untouched / already gone), exactly as `purge` does today.

| Kind          | Records                                    | Future-undo capability                          |
|---------------|--------------------------------------------|-------------------------------------------------|
| `trash`       | origin path (`src`), trash location (`dst`) | reversible: move the file back from `dst`       |
| `delete_file` | origin path (`src`); `dst` empty            | not reversible: bytes are gone (honest audit)   |

- One operation per `rm` invocation: `log_begin_operation("rm", argv, ts)`,
  finished `completed` (or `interrupted` on failure), like `mv`. (The operation
  is named `rm`, not `purge` — see "Replacing purge" for the log-history note.)
- Per media file (and per sidecar) removed: one `purge_row` entry from the row
  deletion, plus — in trash / hard modes — one `trash` / `delete_file` entry
  for the disk action. `--db-only` and `--orphans` write only the `purge_row`
  entry. A future `undo` pairs disk and row entries by `src` path to restore
  both.
- **v1 is log-only.** No command consumes these entries yet. The kinds are
  designed so the deferred `phig undo` can reverse a `trash` disk entry
  (move the file back) plus its `purge_row` (re-insert the row), and for a
  `delete_file` entry restore the row while reporting the file as permanently
  deleted.

## Ordering & error handling (per file)

0. Determine the target paths. Trash / `--hard` / `--db-only`: the match/filter
   selection. `--orphans`: of the match/filter selection (or the whole DB when
   no `--match`), keep only rows whose files are missing, using the existing
   `paths_safe_to_delete()` helper — the same one `scan` uses, which is
   mount-safe (a row whose parent directory is missing is treated as
   unreachable, not orphaned, and is kept).
1. Trash / hard modes: move the media file (and its sidecars) to trash, or
   `fs::remove` them for `--hard`, appending a `trash` / `delete_file` entry per
   file. `--db-only` and `--orphans` skip this step entirely.
2. Delete the DB rows via `delete_paths_logged()` (writes the `purge_row`
   entries) in the same operation.

Rationale: the disk change happens first and is captured in the log before the
row is dropped, so a mid-run failure never leaves a deleted-on-disk file with a
live DB row that a later `scan` would then reconcile silently. If the disk move
fails, that file is skipped and reported; the run continues with the rest.
`--db-only` and `--orphans` are row-only, matching today's `purge`/`scan`
behavior exactly.

- **File present in DB but missing on disk**: delete the row anyway (as `purge`
  does); log a `delete_file` disk entry with `dst` empty (nothing to trash)
  alongside its `purge_row`.
- **`--dry-run`**: no disk, DB, or log writes at all.
- Errors are per-file: a failed move/delete is reported and skipped; other
  selected files still process. The operation's terminal status reflects
  whether any file failed.

## Confirmation guard

Carry over the old `purge` broad-selection guard verbatim: a selection is
"broad" when it covers at least half the database **and** at least 20 entries
(`matched * 2 >= total && matched >= 20`). A broad `rm` without `--yes` prompts
for a typed `yes` on an interactive terminal, or refuses outright when stdin is
not a tty. A narrow (non-broad) trash / `--db-only` / `--orphans` / `--ignored`
`rm` proceeds without prompting. (A whole-DB `--orphans` sweep counts all missing-file rows as
the selection for the guard, so a large cleanup still prompts.)

`--hard` additionally requires `--yes` **always**, even for a single narrow
selection, because it cannot be undone. Without `--yes`, `--hard` refuses
(prompting on a tty, refusing otherwise) regardless of selection size.

## Replacing purge

Option C: `rm --db-only` subsumes `purge`, and the `purge` command is removed.

- **Dispatch**: delete the `command == "purge"` branch in `main.cpp`; add
  `command == "rm"` and `command == "trash"`. `rm --db-only` runs the db-only
  path that `cmd_purge` runs today.
- **`cmd_purge`**: its db-only logic (match/filter selection, broad guard,
  `delete_paths_logged`) moves into the shared `rm` path. The DB method
  `Database::purge()` may stay (internal helper) or be inlined; not user-facing.
- **Operation name**: the logged operation is `rm` for all modes.
  Existing activity-log rows named `purge` from prior runs remain untouched and
  readable; only new operations use `rm`.
- **Docs**: remove `phig purge` from `README.md` and `AGENTS.md`, replace with
  `phig rm`. (Note: `AGENTS.md` currently documents `purge ... [--path <dir>]`,
  but the code never parsed `--path` for purge — a pre-existing doc bug; the
  replacement docs omit `--path`, consistent with the code.)
- **Tests**: update tests referencing the `purge` command / `"purge"`
  operation name to `rm --db-only`. The DB-level `Database::purge()` unit tests
  can stay if that method is kept.
- **No deprecated alias**: `phig purge` is removed outright; invoking it errors
  with the usual unknown-command message.

## Module structure

- New `src/remove_exec.{h,cpp}` (mirrors `organize_exec`), holding mode
  handling, the trash move, cross-mount fallback, collision suffixing, sidecar
  handling, DB-row removal, and logging. `main.cpp` keeps only arg parsing and
  dispatch for `rm` and `trash`.
- Reuse: `find_sidecars()` (sidecar.h), `passes_filters()` for match/filter
  selection (the same helper `mv` uses), `paths_safe_to_delete()` (paths.cpp)
  for the mount-safe `--orphans` sweep (the same helper `scan` uses), the
  activity-log API (`log_begin_operation` / `log_add_entry` /
  `log_finish_operation`), and `delete_paths_logged()` — the existing logged
  row-deletion helper that produces the `purge_row` `restore_json` snapshot.
  `rm` records `trash` / `delete_file` entries for the disk action separately
  from the row-deletion entry.
- Extend the `OperationEntry` kind comment in `database.h` to list `trash` and
  `delete_file`.
- Add `rm` and `trash` to `print_usage()`; remove the `purge` usage line.

## Testing (real-behavior)

Per the standing rule that every phig data-safety change ships with
comprehensive real-behavior tests:

- Trash move on the same filesystem: file lands in `data_dir()/trash`, original
  gone, DB row removed, `trash` entry logged with correct `src`/`dst`.
- Cross-mount trash: copy-then-delete fallback produces the file in trash and
  removes the original (exercised via a temp path on a different filesystem, or
  by forcing the `EXDEV` fallback path).
- Trash name collision: second same-name file gets a suffixed trash path, and
  that path is what the log records.
- `--hard`: file permanently removed (not in trash), `delete_file` entry logged
  with empty `dst`, DB row removed.
- Sidecars: a photo's `.xmp` / `.json` are trashed (and hard-deleted) with it,
  each logged as its own entry.
- Siblings: deleting `IMG_1234.jpg` leaves `IMG_1234.CR2` on disk and its DB
  row intact.
- DB row removed for the deleted media and its sidecars only.
- `--db-only`: DB row dropped, file **and** sidecars left on disk, only a
  `purge_row` entry logged (no disk entry) — matching prior `purge` behavior.
- `--orphans` (no `--match`): rows whose files are missing are dropped across
  the whole DB; rows whose files still exist are kept; only `purge_row` entries
  logged; disk untouched.
- `--orphans --match '<subtree>/**'`: only orphaned rows under that subtree are
  dropped; orphans elsewhere are kept.
- `--orphans` mount-safety: a row whose **parent directory** is missing (e.g. an
  unmounted drive) is NOT dropped, even though the file itself is absent —
  verifies reuse of `paths_safe_to_delete()`.
- Mode exclusivity: any two of `--hard` / `--db-only` / `--orphans` /
  `--ignored` together are rejected with an error, no changes.
- Required `--match`: trash / `--hard` / `--db-only` without any `--match` is
  rejected; `--orphans` and `--ignored` without `--match` are allowed.
- (`--ignored` behavior is specified and tested in plan 028.)
- `--dry-run`: nothing on disk changes, no DB row removed, no log entry written;
  output names the mode.
- Confirmation guard: broad selection without `--yes` refuses (in every mode);
  `--hard` without `--yes` refuses even for a single narrow selection.
- Missing-on-disk file selected (trash/hard mode): DB row removed,
  `delete_file` entry logged, no error.
- `phig trash empty`: clears the trash dir; refuses without `--yes`.
- `phig purge` is no longer a valid command (errors as unknown).

## Follow-up

Separate plans, in rough priority order:

- **`phig undo`** — reverse a whole logged operation (default most recent),
  restoring `move`/`copy`/`trash`/`purge_row` entries and reporting
  `delete_file` entries as bytes-unrecoverable. Turns `rm`'s log-only records
  into an actual restore path.
- **`.phigignore`** — specified in plan 028. Adds the `--ignored` mode to this
  command (drop rows for `.phigignore`-ignored files) and a shared glob core
  that gives `--match`/`--exclude` `[..]` and `**`. Related to `rm --orphans`
  and `rm --db-only`: all are about keeping the catalog free of files the user
  doesn't want tracked; `.phigignore` does it declaratively at scan time.
- **`--path` cleanup on metadata commands** — `--path` on `tag`/`gallery`/`fav`
  appears to have been added in error (and `AGENTS.md` wrongly documents it on
  `purge`). Evaluate removing it in favor of the `--match '<prefix>/**'` idiom
  the file commands use, for a single consistent selection model across
  commands. Out of scope for this plan; noted so it isn't lost.
