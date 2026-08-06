# phig

phig is an image-management engine: it scans image and video directories,
builds a metadata/hash database, detects duplicates, and organizes files into
a structured folder layout. Three consumers sit on top of the engine: the
`cli/` command-line tool, the `gtk/` desktop client (M0 — a working thumbnail
grid, not a finished app), and `libphig/`, the versioned C ABI for foreign
callers such as a future Elixir web client.

This file is the single root guidance file for the monorepo. Each directory
below has its own `CLAUDE.md` with directory-specific rules; this file covers
what's shared. `AGENTS.md` is a pointer to this file, kept only so agent
tools that look for that filename resolve to something.

## Layout

| Path | What it is | Stability |
|---|---|---|
| `core/` | C++ engine: scanning, hashing, EXIF, database, the five engines. | Internal, no promise |
| `libphig/` | Versioned C ABI (`libphig.so`) for foreign callers. | Versioned boundary |
| `cli/` | `phig` command-line tool: argv parsing, output rendering. | — |
| `gtk/` | GTK4/libadwaita desktop client. | M0, in progress |
| `tests/` | GoogleTest suite (fast + perf) covering `core/`, `libphig/`. | — |
| `vendor/` | Vendored third-party code, built without `-Wall -Wextra`. | — |
| `scripts/` | Dev tooling (e.g. `clang-tidy.sh`). | — |
| `docs/` | Cross-project specs/ledgers from when phig, phig-gtk, and phig-web were separate repos, plus execution artifacts owned by a numbered plan. Date-stamped files. | Historical record |
| `plans/` | Numbered implementation plans (`NNN-name.md`) for the whole monorepo — the single index of what's outstanding. | Working index |
| `MARKET.md` | Competitive analysis: the field, its recurring weaknesses, phig's differentiators. Dated, additive. | Working document |

See each directory's `CLAUDE.md` for what it is, its stability contract, and
directory-specific rules. Command reference, tech stack, and phig's key
design decisions (RAW+JPEG pairing, activity log, concurrency, etc.) are
below in this file.

## Building and testing

Native libraries (OpenCV, LibRaw, FFmpeg, libexif, SQLite3, spdlog, gtest,
CURL) come from the system package manager — see `README.md` for per-distro
package names. No prefix is needed; a plain `cmake` works:

```bash
cmake -B build && cmake --build build
./build/phig-tests        # fast suite (~1s), 636 tests
./build/phig-perf-tests   # performance suite (~3s), 4 tests
ctest --test-dir build --parallel   # both — 640, what CI runs (~7s; 48s without --parallel)
clang-format -i <files>
```

Optionally install [mise](https://mise.jdx.dev/) and run `mise install`
(`mise.toml`) to get the exact `cmake`/`clang-format`/`clang-tidy` versions
CI pins — not required for a build, only for the lint gate to agree with CI.

Performance tests live in a separate target so the everyday suite stays
fast. One test is a pre-existing, unrelated skip:
`ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing`. If a single
unattributed failure shows up instead, check
`DatabaseConcurrency.ConcurrentMigrationIsSafe` first — see Known issues
below before assuming your change caused it.

`--parallel` has to be passed explicitly: CMake cannot make a bare `ctest`
parallel (`CTestConfig.cmake`'s `CTEST_PARALLEL_LEVEL` is ignored by a direct
`ctest` invocation — measured, still 48s). Tests get a unique temp directory
from `tests/temp_dir.h`, which is what makes parallel safe; a new fixture that
hardcodes a temp path instead will pass alone and fail under `--parallel`.

`gtk/` has its own build (`cmake -B build && cmake --build build -j` from
inside `gtk/`) and 13 tests across two binaries — see `gtk/CLAUDE.md`.

### ThreadSanitizer: always set `OMP_NUM_THREADS=1`

```bash
OMP_NUM_THREADS=1 ./build/phig-tests   # TSan build
```

OpenCV's parallel framework here is OpenMP, and `libgomp` is not
TSan-instrumented, so TSan cannot see its barriers — every OpenCV parallel
region inside a `detect()` or decode call then looks like unsynchronized
access. Measured: `*Face*` 77-86 reports and `*Scan*` 42 at the default
thread count, 0 for both with `OMP_NUM_THREADS=1` and no code change. Run
TSan without this and roughly 120 false positives will bury any genuine
finding.

### Real-run checks

Two commands that must keep producing these outputs against the real
collection:

```bash
./build/phig search --count --db ~/.local/share/phig/phig.db   # 29925
./build/phig --version                                          # phig 0.3.0
```

### Linting & CI

- **Formatting:** clang-format, Google style / 4-space / 100-col
  (`.clang-format`). CI's lint step globs `core libphig cli tests -maxdepth
  1` (commit `6a13144` — `src/` no longer exists, this replaced a glob on
  it). `vendor/` is excluded. Check with `clang-format --dry-run --Werror
  <files>`. **`-maxdepth 1` means nested files are not covered** —
  `libphig/include/phig/phig.h` in particular is not linted by CI; check it
  manually.
- **Warnings:** first-party code builds with `-Wall -Wextra`; configure with
  `-DPHIG_WERROR=ON` to make them fatal (CI does). Vendored code (`vendor/`)
  is a separate `phig-vendor` target without these flags.
- **clang-tidy:** high-signal set in `.clang-tidy` (bugprone/performance/
  analyzer, noisy checks disabled). Run locally via
  `./scripts/clang-tidy.sh` — it injects g++'s own libstdc++ header dirs at
  runtime (discovered via `g++ -E -x c++ - -v`, never hardcoded) so
  clang-tidy's clang frontend can find `<string>` etc. regardless of which
  toolchain installed it. CI runs `run-clang-tidy` directly on its system
  toolchain.
- **CI:** `.github/workflows/ci.yml` builds + tests on ubuntu-latest and
  macos-latest (with `-DPHIG_WERROR=ON`) and runs the lint gate. GitHub
  Actions are pinned to full commit SHAs.

## Tech stack

- **Language:** C++20
- **Build:** CMake
- **Dev environment:** system-installed native libraries; `mise` (optional)
  pins `cmake`/`clang-format`/`clang-tidy` to the versions CI uses
- **Dependencies:** OpenCV (image decode + phash + video keyframes), libexif
  (EXIF extraction), FFmpeg/libavformat (video metadata), SQLite (database),
  spdlog (diagnostic logging), GoogleTest (tests)

## Interface discipline

**Clients go through phig's defined interfaces only.** `gtk/` and any future
client read phig only through `libphig`'s C ABI (`<phig/phig.h>`). No client
reads phig's SQLite schema, table names, or thumbnail cache layout directly
— those are private and change. When a client needs something the interface
lacks, the capability is added to libphig and tracked in `plans/`.

**Non-GUI functionality belongs in libphig, not the clients.** Logic that
isn't specific to rendering a UI — filtering, formatting, business rules —
goes in `core/` behind a `libphig` entry point, not duplicated in `gtk/` or
any other client.

## CLI command reference

```bash
# Scan a directory into the database
phig scan <directory> [--recursive] [--force [hash,thumbs,faces,all]] [--thumbs] [--faces] [--media image|video|all] [--hidden] [--ignore-mount-warning] [--on-error warn|fail] [--db <path>]

# Generate thumbnails
phig thumbs rebuild [--match <glob>] [--exclude <glob>] [--force] [--size N] [--parallel N] [--db <path>]
phig thumbs clean [--db <path>]

# Find duplicate images
phig duplicates [--type exact|near|all] [--threshold 0-64] [--format text|csv|json] [--output <path>] [--match <glob>] [--exclude <glob>] [--path <dir>] [--db <path>]

# Copy files into YYYY/MM structure
phig cp <destination> [--format <string>] [--match <glob>] [--exclude <glob>] [--path <dir>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Move files into YYYY/MM structure
phig mv <destination> [--format <string>] [--match <glob>] [--exclude <glob>] [--path <dir>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Search the collection
phig search [--count] [--db <path>] ...

# Remove images from the database and/or disk
phig rm [--match <glob>] [--exclude <glob>] [--hard|--db-only|--orphans] [--dry-run] [--yes] [--db <path>]
phig trash empty [--yes]
```

Paths follow XDG conventions (all resolved in `core/paths.cpp`): database at
`~/.local/share/phig/phig.db`, models at `~/.cache/phig/models`, thumbnails
at `~/.cache/phig/thumbs`, diagnostic log at `~/.local/state/phig/phig.log`,
config at `~/.config/phig/config.ini`. An optional INI config file (parsed
in `core/config.cpp` via vendored inih) sets defaults; a global `--config
<path>` flag overrides its location. Precedence: CLI flag > config >
default.

## Project structure

```
core/       C++ engine (internal, no stability promise). scanner, hasher,
            exif, database, selection, dates, metadata, models, raw,
            relink, sidecar, settings, parallel, log, lockfile, paths,
            config, and the five engines: ScanEngine, ThumbsEngine,
            FacesEngine, SearchEngine, plus the remove/organize executors.
libphig/    C ABI (see libphig/CLAUDE.md) — context, query, mutate,
            records, settings, thumbs entry points behind phig.h.
cli/        phig binary: main.cpp (argv parsing, usage text, output
            rendering, SIGINT handler, progress printer).
gtk/        GTK4/libadwaita desktop client (see gtk/CLAUDE.md).
tests/      GoogleTest suite: one file per core/libphig unit, plus
            perf_duplicates.cpp for the perf target.
vendor/     Vendored third-party code.
scripts/    Dev tooling.
docs/       Cross-project specs + execution artifacts (see Layout above).
plans/      Numbered plans, monorepo-wide (see Layout above).
```

## Key design decisions

- **Database is source of truth** — always reflects current file locations.
  Designed to be reusable by other tools (the GTK client, libphig callers).
- **EXIF stored as JSON blob** — no schema migrations needed, queryable via
  `json_extract()`.
- **Re-scan skips unchanged files** — matches on path + mtime. Use
  `--force` to reprocess.
- **Granular --force** — accepts `hash,thumbs,faces,all` (comma-separated
  or repeated). Bare `--force` = all.
- **Safety guard on scan** — warns if all DB entries for a directory are
  missing (possible unmounted drive). Requires `--ignore-mount-warning` to
  proceed with deletion.
- **Scan re-link** — when scan removes a stale row (its file is gone) and
  exactly one other tracked file with the identical sha256 exists on disk,
  it transfers the stale row's user-authored metadata (tags/favorites,
  gallery if the target has none, faces if the target has none) to that
  surviving row before deleting the stale one, logged as a `relink` entry.
  Ambiguous (two+ identical-content files) or no on-disk match: no
  transfer, row still deleted with a restore snapshot.
- **Thumbnails** — JPEGs cached in `~/.cache/phig/thumbs/<size>/ab/<sha>.jpg`,
  keyed by (SHA256, size). Deduplicates naturally, and sizes are independent:
  generating 768px never replaces or invalidates 512px, so a client can ask for
  what it needs without a cache-wide regeneration. 512 is the default that
  `scan --thumbs` and `thumbs rebuild` write; `thumbs rebuild --size N` fills
  another, and libphig generates one on demand when a lookup misses.
- **Video support** — scans video files (MP4/MOV/MKV/AVI/WebM etc.),
  extracts keyframe phash via OpenCV VideoCapture, metadata via
  libavformat. Videos stored with `media_type = 'video'`,
  searchable/filterable.
- **Schema migrations** — `meta` table tracks schema version. Forward-only.
  Version check on startup bails if DB is newer than code. Current: v7.
- **`exif` column renamed to `metadata`** — stores JSON blob for any media
  type (EXIF for images, container metadata for videos).
- **Separate cp/mv commands** — cp is non-destructive, mv updates DB paths.
- **Match/exclude system** — `--match` includes (repeatable), `--exclude`
  excludes (repeatable), exclude wins on overlap.
- **Duplicates compare against full DB** — `--match` selects which groups
  to show, not which files to compare.
- **Activity log** — `mv`/`cp`/`rm` record an operation plus per-file
  entries in `phig.db` (append-only; undo is a future logged operation).
  Each entry commits in the same transaction as the DB change it
  describes. Disable with `[log] enabled = false`; cap size with `[log]
  max_entries` (default 100000, `0` = unlimited, oldest operations pruned
  first). `--dry-run` writes nothing.
- **Diagnostic log** — one spdlog logger, two sinks: stderr
  (warnings/errors, `[warning]`/`[error]` prefix) and a rotating file at
  `~/.local/state/phig/phig.log` (UTC-timestamped; one line per entry with
  newlines escaped; 5 MB rotation to `phig.1.log`). Command invocations and
  scan summaries log at INFO (file only). `[log] file = false` drops the
  file sink; `[log] file_level = info|warn|error` sets its threshold. All
  logging goes through `core/log.h` — no spdlog includes elsewhere.
- **Sidecar files** — `mv`/`cp` carry a media file's metadata sidecars
  (xmp, pp3, dop, on1, aae, json, thm, lrv; both `IMG_1234.xmp` and
  `IMG_1234.CR2.xmp` naming) to the destination beside their image,
  renaming them to match a renamed parent, logged as
  `move_sidecar`/`copy_sidecar` entries linked to the parent image id.
  Sidecars are never indexed as media. Organize and search display also
  read a sidecar date (XMP `CreateDate`/`DateTimeOriginal`, Google Takeout
  `photoTakenTime`) as a date source after EXIF and before mtime, so
  EXIF-stripped photos file (and display) under their real date, not the
  copy date.
- **Concurrency** — SQLite is WAL with a 30s busy_timeout; write
  transactions use `BEGIN IMMEDIATE` so read-then-write paths don't hit
  BUSY_SNAPSHOT, and schema migrations re-check the version inside an
  IMMEDIATE transaction so concurrent first-runs don't re-apply DDL.
  Mutating commands additionally take a process-wide advisory `flock` on
  `<db>.lock` (`core/lockfile.cpp`) so whole operations serialize; reads
  never take it. Per-machine only.
- **RAW + JPEG pairing** — a RAW file (CR2, NEF, ARW, etc.) is the indexed
  shot; a same-basename JPEG beside it is not indexed as its own row.
  Instead it's an attachment: the RAW row's pixel source (for
  phash/faces/thumbs) and a file carried alongside the RAW on `mv`/`cp`,
  like a sidecar. Pixels come from a 3-tier LibRaw-backed source — sidecar
  JPEG, then embedded preview, then full RAW decode — replacing OpenCV's
  `cv::imread`, which fails on many RAW formats. On scan, a JPEG with a
  same-basename RAW on disk is demoted: skipped as its own row, and if it
  was already indexed from an earlier scan, its tags/faces/gallery
  transfer to the RAW row at cleanup before its row is dropped. A lone
  JPEG with no sibling RAW is indexed normally.

## Plans and specs

`plans/` is the single index of phig's work — one sequence across the whole
monorepo, covering `core/`, `libphig/`, `cli/`, `gtk/`, and anything added
later. There are no per-directory plan folders, and a plan may span several
directories. Every plan is `plans/NNN-name.md` with frontmatter (`name`,
`status`, `created`, `description`) and a row in `plans/000-INDEX.md`. If it
isn't in the index, it isn't tracked — the index is the only place to look
for what's outstanding.

`docs/` holds two things, neither of them tracked plans: cross-project specs
and ledgers from when phig, phig-gtk, and phig-web were separate repos, and
execution artifacts (superpowers plans, SDD ledgers, diagnoses) belonging to
a numbered plan. An artifact in `docs/` may be linked from a numbered plan;
it never carries the status itself.

**Plan location overrides superpowers.** The `writing-plans` skill defaults
to `docs/superpowers/plans/YYYY-MM-DD-name.md` and yields to project
preference. In this repo a plan goes to `plans/NNN-name.md` with an index
row. If the plan is an execution script — bite-sized TDD steps with
checkboxes — write it to `docs/plans/` as an artifact and open or update the
numbered plan that owns it.

Status is one of: `designing` (open questions remain), `planned` (designed,
not started), `in-progress` (started; the index row says what's left),
`completed` (shipped, including pieces explicitly deferred to a later plan),
`deferred` (deliberately not now), `superseded` (replaced — the row and the
file's frontmatter both name the replacement).

New cross-project or monorepo-wide design docs go in `docs/`. Completed
plans should not be edited unless the user explicitly asks — it's fine to
add cross-references to other plans. Changes to completed features get a new
plan rather than editing the original. Always write a plan and get approval
before implementing.

## Conventions

- **Comments describe the present.** What the code does and why it is that
  way now — not what it used to do, or which change introduced it. History
  belongs in the commit message.
- **Prose is plain.** No marketing tone, no three-part flourishes, no
  padding phrases. State the thing and stop. This applies to commit
  messages, specs, plans, and READMEs.
- **Performance claims are measured, not reasoned.** This has been
  load-bearing: a BK-tree specified for duplicate detection turned out 40x
  slower than its target, and three rounds of guard calibration each found
  a regression the previous round could not have predicted. Correctness
  questions yield to reasoning; performance questions have only yielded to
  measurement.
- **Data-safety changes ship with real-behavior tests**, not mocks.

## Known issues

`DatabaseConcurrency.ConcurrentMigrationIsSafe` was a genuine 10% flake
caused by `sqlite3_busy_timeout` being set after `PRAGMA
journal_mode=WAL`; fixed in `73d1e1a`. If a single unattributed test
failure appears, check whether it is that test before investigating a
change.
