---
name: Search
status: planned
created: 2026-03-28
description: >
  General-purpose query command for searching the image database by filename,
  date, EXIF data, file size, and visual similarity. Human-readable default
  output with a --porcelain mode for machine consumption.
---

# Search

## Goal

Search the image database with flexible criteria. Returns matching images with metadata. Supports both human-readable output and a stable machine-parseable format (porcelain) for piping to other tools (fzf, scripts, future TUI).

## CLI

```
phig search [flags]
  --name <glob>            Match filename glob
  --ext <extension>        Match file extension (e.g., jpg, png, cr2)
  --after <date>           Files dated after YYYY-MM-DD
  --before <date>          Files dated before YYYY-MM-DD
  --camera <string>        Substring match on EXIF camera model
  --make <string>          Substring match on EXIF camera make
  --min-size <bytes>       Minimum file size
  --max-size <bytes>       Maximum file size
  --similar <image-path>   Find images visually similar to this image
  --threshold [0-64]       Hamming distance for --similar (default: 5)
  --path <directory>       Only files under this directory
  --match <glob>           Include filter (repeatable)
  --filter <glob>          Exclude filter (repeatable, wins over --match)
  --porcelain              Machine-parseable output (stable format)
  --format text|csv|json   Output format (default: text, ignored with --porcelain)
  --output <path>          Write to file
  --db <path>              Database path
```

## Output Modes

### Human (default)

Nicely formatted, readable:
```
/photos/vacation/IMG_1234.jpg
  4032x3024  5.2MB  2023-08-15  Canon EOS R5

/photos/vacation/IMG_1235.jpg
  4032x3024  5.1MB  2023-08-15  Canon EOS R5

Found 2 images
```

### Porcelain (`--porcelain`)

Stable, tab-separated, one record per line. Suitable for `cut`, `awk`, `fzf`, scripts:
```
/photos/vacation/IMG_1234.jpg\t4032\t3024\t5242880\t2023-08-15\tCanon EOS R5
/photos/vacation/IMG_1235.jpg\t4032\t3024\t5201920\t2023-08-15\tCanon EOS R5
```

Format: `path\twidth\theight\tfile_size\tdate\tcamera`

This format is a contract — once defined, it won't change without a version bump.

### CSV / JSON

Same as other commands, via `--format`.

## Similar Image Search (`--similar`)

1. Decode the provided image and compute its phash (image does NOT need to be in the DB)
2. Compare against all phashes in the database
3. Return matches within `--threshold` hamming distance, sorted by distance (closest first)
4. Human output shows hamming distance per result

Use cases:
- "Is this image already in my collection?"
- "Find all variants of this photo"

## Date Matching

Uses the same fallback chain as organize:
1. EXIF DateTimeOriginal
2. EXIF DateTimeDigitized
3. File modified time

`--after` and `--before` check against whichever date is available.

## Open Questions

- Should `--min-size` / `--max-size` accept human-friendly units (e.g., `5MB`, `100KB`)?
- Should there be a `--limit N` flag for large result sets?
- Should `--similar` support providing a phash directly (hex string) instead of an image path?
- Porcelain format: is tab-separated the right choice, or should it be null-separated (like `find -print0`) for paths with special characters?

## Checklist

- [ ] Database search with criteria (path, name, extension, date, EXIF, size)
- [ ] `--similar` image search via phash comparison
- [ ] Human-readable default output
- [ ] Porcelain output mode
- [ ] CSV/JSON output formats
- [ ] `--match` / `--filter` glob support
- [ ] `--output` file redirect
- [ ] Tests for search criteria, similar search, output formats
