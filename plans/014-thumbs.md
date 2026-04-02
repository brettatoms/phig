---
name: Thumbnails
status: completed
completed: 2026-04-02
created: 2026-03-29
description: >
  Generate 512px JPEG thumbnails for scanned images. Store on filesystem
  in cache directory, keyed by SHA256 content hash. Separate `phig thumbs`
  command plus `--thumbs` flag on scan. Includes `--force` granularity
  rework for scan command.
---

# Thumbnails

## Goal

Generate and cache thumbnail images for scanned photos. Thumbnails enable fast previews in a future TUI/GUI and reduce bandwidth for an API layer.

## Design Decisions

### Storage: Filesystem Cache

Thumbnails are derived data stored in `~/.cache/phig/thumbs/`, following XDG conventions. The cache is disposable — thumbnails can be regenerated from originals at any time. This keeps the SQLite database lean and avoids BLOB-related performance issues.

### Naming: SHA256 Content Hash

Thumbnails are named by the image's SHA256 hash (already computed during scan):

```
~/.cache/phig/thumbs/ab/ab3f7c9e...d4e2.jpg
```

Two-character prefix subdirectories prevent large flat directories. Benefits:
- Stable — doesn't depend on row IDs or file paths
- Deduplicates naturally — identical images share one thumbnail
- Already computed — no extra work

### Size: 512px

512px on the longest edge, aspect ratio preserved, JPEG at 80% quality. Roughly 20–50 KB per thumbnail (~30 KB typical). Good for HiDPI grids and preview panels. A 512px thumbnail can be downscaled for smaller grid views without re-reading the original.

## CLI

### `phig thumbs` Command

Subcommand-based, following the same pattern as `phig face`:

```
phig thumbs rebuild [--match <glob>] [--filter <glob>]
                    [--force] [--parallel N] [--db <path>]
phig thumbs clean [--db <path>]
```

**rebuild:**
- Generates thumbnails for all DB entries that don't already have a cached thumbnail
- `--force` regenerates even if thumbnail file exists
- `--parallel N` for concurrent generation (OpenCV decode + resize)
- `--match/--filter` to narrow scope
- Skips entries with no SHA256 hash (shouldn't happen, but defensive)

**clean:**
- Walks the thumbnail cache directory
- Removes any `.jpg` file whose stem (SHA256) doesn't match a hash in the DB
- Removes empty subdirectories afterward

### `--thumbs` Flag on Scan

```
phig scan <directory> [--thumbs] [--faces] [--recursive] [--force [...]] [--db <path>]
```

When `--thumbs` is passed, thumbnail generation happens as part of the scan pipeline. After an image is hashed and inserted/updated, its thumbnail is generated if not already cached.

### `--force` Granularity (Scan Command)

Currently `--force` on scan is a boolean that means "re-hash everything." This rework makes it granular:

```
phig scan <directory> --force hash,thumbs,faces
phig scan <directory> --force hash --force thumbs
phig scan <directory> --force              # bare --force = all
```

| Value   | Meaning |
|---------|---------|
| `hash`  | Re-hash + re-extract EXIF even if path+mtime unchanged (current `--force` behavior) |
| `thumbs`| Regenerate thumbnails even if cached file exists |
| `faces` | Re-detect faces even if face data exists |
| `all`   | All of the above |

Syntax: comma-separated values and/or repeated `--force` flags. Both are supported and combined.

Bare `--force` (no value) defaults to `all` — "redo everything."

**Note:** `--force` without `--thumbs`/`--faces` still only forces what's enabled. E.g., `--force thumbs` without `--thumbs` is a no-op for thumbnails. The `--force` values control *what gets reprocessed*, while `--thumbs`/`--faces` control *what's enabled*.

**Safety guard:** The existing unmounted-drive safety guard (warns when all DB entries for a directory are missing from disk) is separated from `--force` into its own flag: `--ignore-mount-warning`. When all files in a scanned directory are missing, phig warns and stops — this flag overrides that warning. A few missing files are still cleaned up automatically as before. `--force` is now purely about reprocessing.

Warning message becomes:
```
Warning: All 1,234 files in /mnt/photos are missing from disk.
This may indicate an unmounted drive.
Use --ignore-mount-warning to remove these entries from the database.
```

## Pipeline

### Thumbnail Generation

```
image path → OpenCV imread → resize (512px longest edge, aspect preserved)
           → JPEG encode (80% quality) → write to cache path
```

Cache path derivation:
```
sha256 = image's SHA256 from DB
cache_dir = ~/.cache/phig/thumbs/
path = cache_dir / sha256[0:2] / sha256 + ".jpg"
```

### During `phig thumbs`

1. Query DB for matching images (respecting `--match`/`--filter`/`--path`)
2. For each image:
   - Derive cache path from SHA256
   - Skip if file exists (unless `--force`)
   - Read original image, resize, encode, write
3. Report progress (count, skipped, errors)

### During `phig scan --thumbs`

After each image is processed (hashed + inserted):
- Derive cache path from SHA256
- Skip if file exists (unless `--force thumbs` or `--force all`)
- Resize the already-decoded image (avoid re-reading from disk)
- Write thumbnail to cache

## Search Output

All machine-readable search output formats include the thumbnail cache path as a standard column:

- **Porcelain:** appended as an additional tab-separated column: `path \t width \t height \t file_size \t date \t camera \t thumb_path`
- **CSV:** added as a `thumb_path` column
- **JSON:** added as a `thumb_path` field

The path is derived from the SHA256 hash at output time — no extra DB query needed. If the thumbnail file doesn't exist (not yet generated), the path is still included — consumers can check for existence themselves.

## Cache Management

Orphan thumbnails (from purged DB entries or changed files) can be cleaned up with `phig thumbs clean`, which cross-references the cache against DB hashes and removes orphans.

## Implementation

### New Files

- `src/thumbs.h` / `src/thumbs.cpp` — thumbnail generation logic:
  - `get_thumb_path(const std::string& sha256)` → filesystem path
  - `generate_thumb(const cv::Mat& img, const std::string& sha256)` → bool
  - `generate_thumb(const std::string& image_path, const std::string& sha256)` → bool (reads from disk)
  - `thumb_exists(const std::string& sha256)` → bool
  - Cache directory creation (mkdir -p equivalent)

### Modified Files

- `src/main.cpp`:
  - Add `--thumbs` flag to scan options
  - Rework `--force` parsing: accept optional comma-separated values, support repeated flag
  - Add `phig thumbs` command with match/filter/path/force/parallel
  - Generate thumbnails in scan pipeline when `--thumbs` enabled
  - Update help text
- `CMakeLists.txt` — add thumbs.cpp to build

### Force Options Struct

```cpp
struct ForceOptions {
    bool hash = false;
    bool thumbs = false;
    bool faces = false;

    bool any() const { return hash || thumbs || faces; }
    void set_all() { hash = thumbs = faces = true; }
};
```

Replace `bool force` in scan options with `ForceOptions force`.

## Checklist

- [x] `src/thumbs.h/cpp` — thumbnail generation module
- [x] `get_thumb_path()` — derive cache path from SHA256
- [x] `generate_thumb()` — resize + encode + write
- [x] `thumb_exists()` — check cache
- [x] `phig thumbs` command (match/filter/path/force/parallel)
- [x] `--thumbs` flag on scan
- [x] `--force` granularity rework (ForceOptions struct, parsing)
- [x] Progress reporting for thumbs command
- [x] Parallel thumbnail generation
- [x] Update help text
- [x] Tests for thumbnail path derivation
- [x] Tests for thumbnail generation (size, format, aspect ratio)
- [x] Add thumb_path to porcelain output (search)
- [x] Add thumb_path to CSV output (search)
- [x] Add thumb_path to JSON output (search)
- [x] Separate safety guard into `--ignore-mount-warning` flag
- [x] Update AGENTS.md project description
