# phig

An image management power tool. CLI-first tool to scan image directories, build a metadata/hash database, detect duplicates, and organize files into a structured folder layout. Designed for a future API layer and TUI/GUI.

## Tech Stack

- **Language:** C++20
- **Build:** CMake
- **Dev environment:** devenv + direnv (Nix)
- **Dependencies:** OpenCV (image decode + phash), libexif (EXIF extraction), SQLite (database), GoogleTest (tests)

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

116 tests across 13 suites: scanner, hasher (SHA256 + phash), exif, database, glob, filters (match/filter precedence), date parsing, JSON helpers, format strings, face DB operations, embedding distance.

## CLI Commands

```bash
# Scan a directory into the database
phig scan <directory> [--recursive] [--force [hash,thumbs,faces,all]] [--thumbs] [--faces] [--ignore-mount-warning] [--on-error warn|fail] [--db <path>]

# Generate thumbnails
phig thumbs rebuild [--match <glob>] [--filter <glob>] [--force] [--parallel N] [--db <path>]
phig thumbs clean [--db <path>]

# Find duplicate images
phig duplicates [--type exact|near|all] [--threshold 0-64] [--format text|csv|json] [--output <path>] [--match <glob>] [--filter <glob>] [--path <dir>] [--db <path>]

# Copy files into YYYY/MM structure
phig cp <destination> [--format <string>] [--match <glob>] [--filter <glob>] [--path <dir>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Move files into YYYY/MM structure
phig mv <destination> [--format <string>] [--match <glob>] [--filter <glob>] [--path <dir>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Remove entries from the database
phig purge [--match <glob>] [--filter <glob>] [--path <dir>] [--dry-run] [--db <path>]
```

Default database location: `~/.local/share/phig/phig.db`

## Project Structure

```
src/
├── main.cpp          CLI parsing, orchestration, organize/purge/duplicates commands
├── scanner.h/cpp     Directory traversal, image extension filtering
├── hasher.h/cpp      SHA256 computation, OpenCV phash
├── exif.h/cpp        EXIF extraction to JSON via libexif
├── database.h/cpp    SQLite schema, CRUD, queries
├── filters.h/cpp     Glob matching, match/filter logic
├── thumbs.h/cpp      Thumbnail generation (512px JPEG, SHA256-keyed cache)
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
- **Thumbnails** — 512px JPEG cached in `~/.cache/phig/thumbs/` keyed by SHA256. Deduplicates naturally.
- **Separate cp/mv commands** — cp is non-destructive, mv updates DB paths.
- **Match/filter system** — `--match` includes (repeatable), `--filter` excludes (repeatable), filter wins on overlap.
- **Duplicates compare against full DB** — `--match` filters which groups to show, not which files to compare.

## Plans

See `plans/000-INDEX.md` for the plan index and `plans/` for detailed plans.

**Plan rules:**
- Completed plans should not be edited unless the user explicitly asks. It's fine to add cross-references to other plans.
- Changes to completed features get a new plan rather than editing the original.
- Always write a plan and get approval before implementing.
