# phig

An image management power tool. CLI-first tool to scan image directories, build a metadata/hash database, detect duplicates, and organize files into a structured folder layout. Designed for a future API layer and TUI/GUI.

## Tech Stack

- **Language:** C++20
- **Build:** CMake
- **Dev environment:** devenv + direnv (Nix)
- **Dependencies:** OpenCV (image decode + phash + video keyframes), libexif (EXIF extraction), FFmpeg/libavformat (video metadata), SQLite (database), spdlog (diagnostic logging), GoogleTest (tests)

## Building

Requires Nix with devenv installed. direnv auto-activates the environment on `cd`.

```bash
cmake -B build          # configure (only needed once or after CMakeLists.txt changes)
cmake --build build     # compile
```

Binary: `./build/phig`

## Running Tests

```bash
cmake --build build && ./build/phig-tests
```

Coverage: scanner, hasher (SHA256 + phash), exif, database, glob, selection (match/exclude precedence), date parsing, JSON helpers, format strings, face DB operations, embedding distance, thumbnails, video extensions, video phash, video metadata, migrations, video search.

### ThreadSanitizer: always set `OMP_NUM_THREADS=1`

```bash
OMP_NUM_THREADS=1 ./build/phig-tests   # TSan build
```

OpenCV's parallel framework here is OpenMP, and `libgomp` is not TSan-instrumented, so TSan
cannot see its barriers. Every OpenCV parallel region inside a `detect()` or decode call
then looks like unsynchronised access, reported against whichever phig function invoked it.

Measured: `*Face*` 77-86 reports and `*Scan*` 42 at the default thread count, **0 for both**
with `OMP_NUM_THREADS=1` and no code change. They are false positives, not races — if they
were real, serialising OpenCV's internal pool could not silence them. Run TSan without this
and ~120 false positives will bury any genuine finding.

## Linting & CI

- **Formatting:** clang-format, Google style / 4-space / 100-col (`.clang-format`).
  Applies to `src/` and `tests/`; `vendor/` is excluded. Check with
  `clang-format --dry-run --Werror <files>`.
- **Warnings:** first-party code builds with `-Wall -Wextra`; configure with
  `-DPHIG_WERROR=ON` to make them fatal (CI does). Vendored code (`vendor/`) is a
  separate `phig-vendor` target without these flags.
- **clang-tidy:** high-signal set in `.clang-tidy` (bugprone/performance/analyzer,
  noisy checks disabled). Run locally via `./scripts/clang-tidy.sh` — the wrapper
  injects g++'s libstdc++ header dirs so the Nix clang-tidy can find `<string>` etc.
  CI runs `run-clang-tidy` directly on its system toolchain.
- **CI:** `.github/workflows/ci.yml` builds + tests on ubuntu-latest and
  macos-latest (with `-DPHIG_WERROR=ON`) and runs the lint gate. GitHub Actions are
  pinned to full commit SHAs.

## CLI Commands

```bash
# Scan a directory into the database
phig scan <directory> [--recursive] [--force [hash,thumbs,faces,all]] [--thumbs] [--faces] [--media image|video|all] [--hidden] [--ignore-mount-warning] [--on-error warn|fail] [--db <path>]

# Generate thumbnails
phig thumbs rebuild [--match <glob>] [--exclude <glob>] [--force] [--parallel N] [--db <path>]
phig thumbs clean [--db <path>]

# Find duplicate images
phig duplicates [--type exact|near|all] [--threshold 0-64] [--format text|csv|json] [--output <path>] [--match <glob>] [--exclude <glob>] [--path <dir>] [--db <path>]

# Copy files into YYYY/MM structure
phig cp <destination> [--format <string>] [--match <glob>] [--exclude <glob>] [--path <dir>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Move files into YYYY/MM structure
phig mv <destination> [--format <string>] [--match <glob>] [--exclude <glob>] [--path <dir>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Remove images from the database and/or disk
phig rm [--match <glob>] [--exclude <glob>] [--hard|--db-only|--orphans] [--dry-run] [--yes] [--db <path>]
phig trash empty [--yes]
```

Paths follow XDG conventions (all resolved in `src/paths.cpp`): database at
`~/.local/share/phig/phig.db`, models at `~/.cache/phig/models`, thumbnails at
`~/.cache/phig/thumbs`, diagnostic log at `~/.local/state/phig/phig.log`, config
at `~/.config/phig/config.ini`. An optional INI
config file (parsed in `src/config.cpp` via vendored inih) sets defaults; a global
`--config <path>` flag overrides its location. Precedence: CLI flag > config > default.

## Project Structure

```
src/
├── main.cpp          CLI parsing, orchestration, organize/duplicates commands
├── scanner.h/cpp     Directory traversal, image extension filtering
├── hasher.h/cpp      SHA256 computation, OpenCV phash
├── exif.h/cpp        EXIF extraction to JSON via libexif
├── database.h/cpp    SQLite schema, CRUD, queries
├── selection.h/cpp   Glob matching, match/exclude logic
├── thumbs.h/cpp      Thumbnail generation (512px JPEG, SHA256-keyed cache)
├── metadata.h/cpp    Video metadata extraction via libavformat
├── remove_exec.h/cpp rm command: trash/hard-delete/db-only/orphans, trash empty
└── types.h           ImageInfo struct
tests/
├── test_scanner.cpp
├── test_hasher.cpp
├── test_exif.cpp
├── test_database.cpp
└── test_organize.cpp  Glob, date parsing, format string tests
```

## Key Design Decisions

- **Database is source of truth** — always reflects current file locations. Designed to be reusable by other tools (e.g., a future API/GUI).
- **EXIF stored as JSON blob** — no schema migrations needed, queryable via `json_extract()`.
- **Re-scan skips unchanged files** — matches on path + mtime. Use `--force` to reprocess.
- **Granular --force** — `--force` accepts `hash,thumbs,faces,all` (comma-separated or repeated). Bare `--force` = all.
- **Safety guard on scan** — warns if all DB entries for a directory are missing (possible unmounted drive). Requires `--ignore-mount-warning` to proceed with deletion.
- **Scan re-link** — when scan removes a stale row (its file is gone) and exactly one other tracked file with the identical sha256 exists on disk, it transfers the stale row's user-authored metadata (tags/favorites, gallery if the target has none, faces if the target has none) to that surviving row before deleting the stale one, logged as a `relink` entry. So moving/renaming files outside phig and rescanning preserves metadata. Ambiguous (two+ identical-content files) or no on-disk match: no transfer, row still deleted with a restore snapshot.
- **Thumbnails** — 512px JPEG cached in `~/.cache/phig/thumbs/` keyed by SHA256. Deduplicates naturally.
- **Video support** — scans video files (MP4/MOV/MKV/AVI/WebM etc.), extracts keyframe phash via OpenCV VideoCapture, metadata via libavformat. Videos stored with `media_type = 'video'`, searchable/filterable.
- **Schema migrations** — `meta` table tracks schema version. Forward-only migrations. Version check on startup bails if DB is newer than code.
- **`exif` column renamed to `metadata`** — stores JSON blob for any media type (EXIF for images, container metadata for videos).
- **Separate cp/mv commands** — cp is non-destructive, mv updates DB paths.
- **Match/exclude system** — `--match` includes (repeatable), `--exclude` excludes (repeatable), exclude wins on overlap.
- **Duplicates compare against full DB** — `--match` selects which groups to show, not which files to compare.
- **Activity log** — `mv`/`cp`/`rm` record an operation plus per-file entries in
  `phig.db` (append-only; undo is a future logged operation). Each entry commits in
  the same transaction as the DB change it describes. Disable with `[log] enabled =
  false`; cap size with `[log] max_entries` (default 100000, `0` = unlimited, oldest
  operations pruned first). `--dry-run` writes nothing.
- **Diagnostic log** — one spdlog logger, two sinks: stderr (warnings/errors,
  `[warning]`/`[error]` prefix) and a rotating file at
  `~/.local/state/phig/phig.log` (UTC-timestamped; one line per entry with
  newlines escaped; 5 MB rotation to `phig.1.log`). Command invocations and
  scan summaries log at INFO (file only). `[log] file = false` drops the file
  sink; `[log] file_level = info|warn|error` sets its threshold. All logging
  goes through `src/log.h` — no spdlog includes elsewhere.
- **Sidecar files** — `mv`/`cp` carry a media file's metadata sidecars (xmp,
  pp3, dop, on1, aae, json, thm, lrv; both `IMG_1234.xmp` and `IMG_1234.CR2.xmp`
  naming) to the destination beside their image, renaming them to match a
  renamed parent, logged as `move_sidecar`/`copy_sidecar` entries linked to the
  parent image id. Sidecars are never indexed as media. Organize and search
  display also read a sidecar date (XMP `CreateDate`/`DateTimeOriginal`, Google
  Takeout `photoTakenTime`) as a date source after EXIF and before mtime, so
  EXIF-stripped photos file (and display) under their real date, not the copy
  date. RAW↔JPEG pairing is plan 026, not here.
- **Concurrency** — SQLite is WAL with a 30s busy_timeout; write transactions
  use `BEGIN IMMEDIATE` so read-then-write paths don't hit BUSY_SNAPSHOT, and
  schema migrations re-check the version inside an IMMEDIATE transaction so
  concurrent first-runs don't re-apply DDL. Mutating commands additionally take
  a process-wide advisory `flock` on `<db>.lock` (`src/lockfile.cpp`) so whole
  operations serialize; reads never take it. Per-machine only.
- **RAW + JPEG pairing** — a RAW file (CR2, NEF, ARW, etc.) is the indexed
  shot; a same-basename JPEG beside it is not indexed as its own row. Instead
  it's an attachment: the RAW row's pixel source (for phash/faces/thumbs) and
  a file carried alongside the RAW on `mv`/`cp`, like a sidecar. Pixels come
  from a 3-tier LibRaw-backed source — sidecar JPEG, then embedded preview,
  then full RAW decode — replacing OpenCV's `cv::imread`, which fails on many
  RAW formats. On scan, a JPEG with a same-basename RAW on disk is demoted:
  skipped as its own row, and if it was already indexed from an earlier scan,
  its tags/faces/gallery transfer to the RAW row at cleanup (reusing plan
  023's metadata transfer) before its row is dropped. A lone JPEG with no
  sibling RAW is indexed normally. Carry-on-move reuses plan 025's sidecar
  machinery.

## Plans

See `plans/000-INDEX.md` for the plan index and `plans/` for detailed plans.

**Plan rules:**
- All plans, specs, and designs live in `plans/` as numbered `NNN-name.md` files with the
  established frontmatter (`name`, `status`, `created`, `description`). Do NOT create a
  `docs/`, `docs/superpowers/specs/`, or any other parallel plan/spec layout — this
  `plans/` directory is the single source of truth.
- When writing a new plan, use the next number in sequence and add a row to `plans/000-INDEX.md`.
- Completed plans should not be edited unless the user explicitly asks. It's fine to add cross-references to other plans.
- Changes to completed features get a new plan rather than editing the original.
- Always write a plan and get approval before implementing.
