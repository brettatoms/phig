---
name: Video Support
status: completed
completed: 2026-04-02
created: 2026-04-01
description: >
  Extend phig to scan, hash, deduplicate, and organize video files alongside
  images. Add FFmpeg (libavformat/libavcodec) for video metadata extraction
  and keyframe decoding. Reuse existing SHA256, phash, and organize pipelines.
---

# Video Support

## Goal

Support video files as first-class citizens in phig. Videos should be scannable,
deduplicated (exact and near), searchable, and organizable just like images.

## Design Decisions

### Metadata storage

- **Rename `exif` column to `metadata`** — the column stores a JSON blob of
  descriptive metadata for any media type. "exif" is misleading since PNG has
  text chunks, WebP has XMP, and videos have container metadata (not EXIF).
  Since we haven't released, this is a rename in the base schema, not a
  migration.
- **Video metadata goes in the JSON blob** — codec, fps, creation date, GPS,
  rotation, audio codec all live in the `metadata` JSON column, same pattern
  as image EXIF. SQLite expression indexes (`json_extract`) can be added if
  query performance requires it.
- **`media_type` column** — `'image'` or `'video'`, stored as a dedicated
  column (not in JSON) since it's used in nearly every query for filtering.
- **`duration_ms` column** — video duration in milliseconds as a dedicated
  column for indexed search (`--min-duration`/`--max-duration`).
- No other dedicated columns for video-specific data.

### Schema migration system

- **`meta` table** with key/value rows, stores `schema_version`.
- **Forward-only migrations** — no down migrations. During development, manually
  fix or delete the DB if needed.
- **Version check on startup** — if DB schema version > code version, bail with
  an error message telling the user to update phig.
- **Migration array** in code — `migrations[N]` upgrades from version N-1 to N.
  Fresh DBs create the full schema at latest version directly (no replaying).
- **Version 1** = current schema (images + faces + people + vec_faces) with
  `exif` renamed to `metadata`.
- **Version 2** = video support columns (`media_type`, `duration_ms`).

### Video metadata extraction

- **libavformat** for video metadata (creation date, GPS, duration, codec, fps,
  rotation). OpenCV VideoCapture doesn't expose container metadata.
- **OpenCV VideoCapture** for keyframe decoding (phash extraction).
- **libexif** remains for image EXIF only — no changes.

### Test fixtures

- Small video files (MP4/MOV/MKV, ~5-15KB each) committed to `tests/data/`.
  Generated with FFmpeg (1 second, solid color, low resolution).

## New Dependency: FFmpeg

**Libraries needed:** `libavformat`, `libavcodec`, `libavutil`, `libswscale`

**devenv.nix:** Add `pkgs.ffmpeg` (or `pkgs.ffmpeg-headless`) to packages and
`CMAKE_PREFIX_PATH`.

## Schema Changes

Rename `exif` → `metadata` in base schema (v1).

Add video columns (v2 migration):

```sql
ALTER TABLE images ADD COLUMN media_type TEXT NOT NULL DEFAULT 'image';
ALTER TABLE images ADD COLUMN duration_ms INTEGER;
```

All other video metadata (codec, fps, audio codec, rotation, creation date,
GPS) stored in the `metadata` JSON column via `json_extract()`.

## Scanner Changes

**`src/scanner.h/cpp`:**

- Add `is_known_video_extension()` — recognizes: `mp4`, `mov`, `avi`, `mkv`,
  `webm`, `m4v`, `wmv`, `flv`, `3gp`, `mts`, `m2ts`, `ts`, `mpg`, `mpeg`
- Add `is_known_media_extension()` that combines image + video checks
- `scan_directory()` uses `is_known_media_extension()` instead of
  `is_known_image_extension()`
- Existing image-only function kept for backward compat / filtering

## Processing Pipeline Changes

**`src/hasher.h/cpp`:**

- **SHA256:** No change — works on any file.
- **phash for video:** New function `compute_video_phash(path)` that:
  1. Opens video with `cv::VideoCapture`
  2. Seeks to ~10% of duration (avoids black intro frames)
  3. Reads that frame
  4. Passes it to existing `compute_phash(cv::Mat)` overload
  - Fallback: if seek fails, read the first non-black frame (scan first N
    frames, pick first with mean brightness > threshold)
  - Returns `PhashResult` (hash + width + height from the frame)

**`src/metadata.h/cpp` (new file):**

- `extract_video_metadata(path) -> VideoMetadata` using libavformat:
  - `duration_ms`, `width`, `height`, `video_codec`, `audio_codec`, `fps`,
    `rotation`
  - Creation date, GPS (if present in container metadata)
  - Returns metadata as a struct
  - Caller serializes to JSON for the `metadata` column

**`src/exif.h/cpp`:**

- No changes. Only called for image files.
- Code references to `exif_json` renamed to `metadata_json` in ImageInfo.

**`src/main.cpp` scan pipeline:**

- After `scan_directory()`, check extension to determine media type
- **Image path:** existing pipeline (decode → phash → faces → EXIF)
- **Video path:** SHA256 → `compute_video_phash()` → `extract_video_metadata()`
  → skip face detection → store with `media_type = 'video'`
- The `process_file()` function gets a branch based on media type

## Command Changes

### `phig scan`

- Picks up video files automatically (no flag needed)
- Progress output distinguishes images/videos: `Scanned 42 images, 7 videos`
- `--images-only` / `--videos-only` flags for selective scanning

### `phig duplicates`

- Works as-is for exact (SHA256) duplicates
- Near duplicates (phash) work across media types — a video's keyframe phash
  could match an image's phash (this is a feature: finds thumbnails of videos)
- Add `--media image|video|all` filter (default: `all`)

### `phig cp` / `phig mv`

- Work as-is — format strings use dates from metadata
- Videos use creation date from container metadata (same field in DB)

### `phig search`

- Add `--media image|video|all` filter
- `--similar` works: compares phash of video keyframe
- Duration-based search: `--min-duration 10s`, `--max-duration 5m` (new)

### `phig thumbs rebuild`

- Generate thumbnails for videos using keyframe extraction (same frame as phash)

### `phig purge`

- Works as-is — no changes needed

## Implementation Steps

1. Migration system (`meta` table, version check, migration array)
2. Rename `exif` → `metadata` in schema, `ImageInfo` struct, all code
3. Add FFmpeg to `devenv.nix` and `CMakeLists.txt`
4. Video extensions in scanner
5. `compute_video_phash()` via OpenCV VideoCapture
6. `extract_video_metadata()` via libavformat (new `src/metadata.h/cpp`)
7. Schema v2 migration (`media_type`, `duration_ms`)
8. Update `ImageInfo`/types with video fields + `media_type`
9. Branch `process_file()` on media type
10. Command updates (`--media` filter, `--min-duration`/`--max-duration`,
    progress reporting, `--images-only`/`--videos-only`)
11. Video thumbnail support in `phig thumbs rebuild`
12. Test fixtures + tests

## Testing

- Test videos: small MP4/MOV/MKV files (~5-15KB each) committed to
  `tests/data/`.
- Test cases:
  - Video extension recognition
  - Video phash extraction (non-zero, deterministic)
  - Video metadata extraction (duration, codec, dimensions)
  - Mixed image+video duplicate detection
  - Schema migration (version check, forward migration)
  - Organize with mixed media types

## Risks & Open Questions

- **Large video files and SHA256:** Hashing a multi-GB video is slow. Could
  offer a fast-hash mode (first + last N MB) as a future optimization, but
  start with full-file SHA256 for correctness.
- **phash quality for videos:** A single keyframe may not represent the video
  well. Could compute phash from multiple frames and store the "most
  representative" one. Start simple (single frame at 10%), iterate later.
- **FFmpeg linking:** Need to verify that Nix's `pkgs.ffmpeg` provides the
  dev headers and pkg-config files. May need `ffmpeg.dev` in
  `CMAKE_PREFIX_PATH`.

## Checklist

- [x] Migration system (meta table, version check, migration array)
- [x] Rename `exif` → `metadata` in schema and all code
- [x] Add FFmpeg to devenv.nix and CMakeLists.txt
- [x] Video extensions in scanner (`is_known_video_extension()`)
- [x] `compute_video_phash()` via OpenCV VideoCapture
- [x] `extract_video_metadata()` via libavformat
- [x] Schema v2 migration (media_type, duration_ms columns)
- [x] Update ImageInfo/types with media_type, duration_ms
- [x] Branch process_file() on media type
- [x] `--media image|video|all` filter on duplicates/search
- [x] `--min-duration`/`--max-duration` on search
- [x] `--images-only`/`--videos-only` on scan
- [x] Scan progress reporting (images vs videos)
- [x] Video thumbnail support in thumbs rebuild
- [x] Test fixtures (small MP4/MOV/MKV)
- [x] Tests for video scan, phash, metadata, migration, mixed duplicates
- [x] Update help text
- [x] Update AGENTS.md

## Cross-References

- [001-scan-database.md](001-scan-database.md) — scan pipeline, schema
- [002-duplicate-detection.md](002-duplicate-detection.md) — duplicate logic
- [008-search.md](008-search.md) — search command
- [014-thumbs.md](014-thumbs.md) — thumbnail generation (extend for video)
