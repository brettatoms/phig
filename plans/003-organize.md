---
name: Organize
status: completed
created: 2026-03-28
description: >
  Copy/move files into structured folder layout using format strings.
  Glob match/filter. Conflict resolution. Dry-run mode.
---

# Organize

## Goal

Copy or move images from their current locations into a structured folder layout. Deduplication is a separate concern handled by the `duplicates` and `purge` commands.

## CLI

```
phig organize <destination> [flags]
  --format <string>            Output path format (default: %Y/%m/%original)
  --match <glob>               Only organize files matching glob (repeatable)
  --filter <glob>              Exclude files matching glob (repeatable, wins over --match)
  --path <directory>           Only organize files from this source directory
  --action copy|move           Copy or move files (default: copy)
  --dry-run                    Show what would happen without doing it
  --on-conflict skip|overwrite|rename  (default: skip)
  --db <path>                  Database path
```

## Format Tokens

- `%Y` — year
- `%m` — month
- `%d` — day
- `%camera` — EXIF camera model
- `%make` — EXIF camera make
- `%original` — original filename

## Checklist

- [x] Format string expansion with date and EXIF tokens
- [x] Date fallback chain: EXIF DateTimeOriginal → DateTimeDigitized → file mtime → `unsorted/`
- [x] Epoch/zero date rejection (1970-01-01, 0000-00-00)
- [x] Copy and move actions
- [x] Dry-run mode
- [x] Filename conflict resolution (skip/overwrite/rename with numeric suffix)
- [x] DB path update on move (not on copy)
- [x] Match/filter glob support (repeatable, filter wins)
- [x] `--path` prefix filtering
- [x] Re-run safety (skip on conflict by default)

## Design Decisions

- **Copy is default:** Non-destructive. Source files remain in place.
- **DB updated on move only:** On copy, the source still exists at its original path, so DB stays correct. On move, the file has relocated, so DB must be updated.
- **No dedup:** Organize purely reorganizes files. Deduplication is a separate workflow via `phig duplicates` + `phig purge`.
