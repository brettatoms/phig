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

82 tests across 9 suites: scanner, hasher (SHA256 + phash), exif, database, glob, filters (match/filter precedence), date parsing, JSON helpers, format strings.

## CLI Commands

```bash
# Scan a directory into the database
phig scan <directory> [--recursive] [--force] [--on-error warn|fail] [--db <path>]

# Find duplicate images
phig duplicates [--type exact|near|all] [--threshold 0-64] [--format text|csv|json] [--output <path>] [--match <glob>] [--filter <glob>] [--path <dir>] [--db <path>]

# Organize files into YYYY/MM structure
phig organize <destination> [--format <string>] [--match <glob>] [--filter <glob>] [--path <dir>] [--action copy|move] [--dry-run] [--duplicates skip|keep] [--on-conflict skip|overwrite|rename] [--db <path>]

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
- **Safety guard on scan** — warns if all DB entries for a directory are missing (possible unmounted drive). Requires `--force` to proceed with deletion.
- **Organize with copy is default** — non-destructive. DB paths only updated on move, not copy.
- **Match/filter system** — `--match` includes (repeatable), `--filter` excludes (repeatable), filter wins on overlap.
- **Duplicates compare against full DB** — `--match` filters which groups to show, not which files to compare.

## Plans

See `plans/000-INDEX.md` for the plan index and `plans/` for detailed plans.
