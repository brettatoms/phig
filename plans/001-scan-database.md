---
name: Scan & Database
status: completed
created: 2026-03-28
description: >
  Directory scanning, image decoding, phash + SHA256 hashing, EXIF extraction,
  SQLite storage with WAL mode, mtime-based skip, stale entry cleanup with
  unmount safety guard.
---

# Scan & Database

## Goal

Scan directories of image files, extract metadata and hashes, store everything in a SQLite database that serves as the central source of truth for the image collection.

## CLI

```
phig scan <directory> [flags]
  --recursive         Scan subdirectories (default: non-recursive)
  --on-error warn|fail  Behavior for unreadable files (default: warn)
  --force             Re-hash all files, ignoring path+mtime skip
  --db <path>         Override DB location (default: ~/.local/share/phig/phig.db)
```

## Schema

```sql
CREATE TABLE images (
    id          INTEGER PRIMARY KEY,
    path        TEXT NOT NULL UNIQUE,
    filename    TEXT NOT NULL,
    extension   TEXT NOT NULL,
    file_size   INTEGER NOT NULL,
    created_at  TEXT,
    modified_at TEXT,
    sha256      TEXT NOT NULL,
    phash       INTEGER NOT NULL,
    width       INTEGER,
    height      INTEGER,
    exif        TEXT              -- JSON blob, all EXIF tags
);
```

## Checklist

- [x] Directory traversal (recursive/non-recursive)
- [x] Image extension filtering (known formats only, skip `._` files, `.DS_Store`, etc.)
- [x] SHA256 computation
- [x] OpenCV image decode + phash
- [x] EXIF extraction to JSON via libexif
- [x] SQLite storage with WAL mode
- [x] Re-scan skip by path + mtime
- [x] `--force` flag to reprocess everything
- [x] Thread pool parallelism for decode/hash/EXIF
- [x] Single writer thread with batched inserts
- [x] Stale entry cleanup (files deleted from disk → removed from DB)
- [x] Unmount safety guard (all entries missing → warn, require `--force`)
- [x] OpenCV warning capture with filename context
- [x] Progress output

## Design Decisions

- **Database location:** `~/.local/share/phig/phig.db` (XDG_DATA_HOME). Single DB accumulates across multiple scan runs of different directories.
- **EXIF as JSON:** Avoids schema migrations. Queryable via `json_extract()`.
- **Try all files:** Not filtered by extension for decode — uses known image extensions for file discovery, then attempts OpenCV decode. Warns/fails per `--on-error` flag.
- **Canonical paths:** All paths stored as `fs::canonical()` to avoid duplicates from symlinks or relative paths.
