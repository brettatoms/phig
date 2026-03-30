---
name: Thumbnails
status: planned
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

Standalone command to generate thumbnails, following the same pattern as `phig faces` / `phig face rebuild`:

```
phig thumbs [--match <glob>] [--filter <glob>] [--path <dir>]
            [--force] [--parallel N] [--db <path>]
```

- Generates thumbnails for all matching DB entries that don't already have a cached thumbnail
- `--force` regenerates even if thumbnail file exists
- `--parallel N` for concurrent generation (OpenCV decode + resize)
- Skips entries with no SHA256 hash (shouldn't happen, but defensive)

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

**Safety guard exception:** The existing unmounted-drive safety guard (`--force` to proceed when all DB entries are missing) remains. This is orthogonal — it's about allowing stale entry cleanup, not about reprocessing. This will need a `--force` value or a separate flag (e.g. `--force purge` or keeping the bare `--force` behavior for this case). TBD during implementation — may warrant a separate `--yes` or `--confirm` flag to avoid overloading `--force`.

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

## Cache Management

Orphan thumbnails (from purged DB entries or changed files) are harmless — they're just unused files in the cache. No automatic cleanup for now. A future `phig cache clean` command could prune orphans by cross-referencing the DB.

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

- [ ] `src/thumbs.h/cpp` — thumbnail generation module
- [ ] `get_thumb_path()` — derive cache path from SHA256
- [ ] `generate_thumb()` — resize + encode + write
- [ ] `thumb_exists()` — check cache
- [ ] `phig thumbs` command (match/filter/path/force/parallel)
- [ ] `--thumbs` flag on scan
- [ ] `--force` granularity rework (ForceOptions struct, parsing)
- [ ] Progress reporting for thumbs command
- [ ] Parallel thumbnail generation
- [ ] Update help text
- [ ] Tests for thumbnail path derivation
- [ ] Tests for thumbnail generation (size, format, aspect ratio)
- [ ] Update AGENTS.md project description
