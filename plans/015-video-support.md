---
name: Video Support
status: planned
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

## New Dependency: FFmpeg

**Why:** `libexif` only handles JPEG/TIFF EXIF. Video metadata (creation date,
GPS, duration, codec, rotation) lives in container atoms (MP4/MOV) or Matroska
tags. FFmpeg's `libavformat` is the standard library for this and is likely
already an indirect dependency via OpenCV's VideoCapture backend.

**Libraries needed:** `libavformat`, `libavcodec`, `libavutil`, `libswscale`

**devenv.nix:** Add `pkgs.ffmpeg` (or `pkgs.ffmpeg-headless`) to packages and
`CMAKE_PREFIX_PATH`.

## Schema Changes

Add video-specific columns to the `images` table (or rename to `media`):

```sql
ALTER TABLE images ADD COLUMN media_type TEXT NOT NULL DEFAULT 'image';
  -- 'image' or 'video'
ALTER TABLE images ADD COLUMN duration_ms INTEGER;
  -- video duration in milliseconds, NULL for images
ALTER TABLE images ADD COLUMN video_codec TEXT;
  -- e.g. 'h264', 'hevc', 'av1', NULL for images
ALTER TABLE images ADD COLUMN audio_codec TEXT;
  -- e.g. 'aac', 'opus', NULL for images or silent videos
ALTER TABLE images ADD COLUMN fps REAL;
  -- frames per second, NULL for images
```

Migration: existing rows get `media_type = 'image'`, other columns NULL.
The table stays named `images` for backward compatibility (renaming is cosmetic
and would break every query).

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
  - Returns metadata as a struct; creation date/GPS get folded into the
    existing `exif_json` field as a JSON blob for consistency

**`src/exif.h/cpp`:**

- No changes. Only called for image files.

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
- `--images-only` / `--videos-only` flags for selective scanning (optional,
  could defer)

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

### `phig purge`

- Works as-is — no changes needed

## Implementation Phases

### Phase 1: Foundation

1. Add FFmpeg to `devenv.nix` and `CMakeLists.txt`
2. Schema migration (add columns)
3. Add video extensions to scanner
4. Implement `compute_video_phash()` using OpenCV VideoCapture
5. Implement `extract_video_metadata()` using libavformat
6. Branch `process_file()` on media type

### Phase 2: Commands

7. Update scan progress reporting
8. Add `--media` filter to duplicates/search
9. Add duration search to `phig search`
10. Tests: video scan, video phash, video metadata, duplicates with mixed media

### Phase 3: Polish (optional)

11. `--images-only` / `--videos-only` scan flags
12. Video thumbnail extraction for `phig thumbs` (see plan 014)
13. Face detection on video keyframes

## Testing

- Test videos: small MP4/MOV/MKV files in `tests/data/` (a few seconds each,
  generated with FFmpeg during test setup or committed as tiny fixtures)
- Test cases:
  - Video extension recognition
  - Video phash extraction (non-zero, deterministic)
  - Video metadata extraction (duration, codec, dimensions)
  - Mixed image+video duplicate detection
  - Schema migration from image-only DB
  - Organize with mixed media types

## Risks & Open Questions

- **Large video files and SHA256:** Hashing a multi-GB video is slow. Could
  offer a fast-hash mode (first + last N MB) as a future optimization, but
  start with full-file SHA256 for correctness.
- **phash quality for videos:** A single keyframe may not represent the video
  well. Could compute phash from multiple frames and store the "most
  representative" one. Start simple (single frame at 10%), iterate later.
- **Table rename:** Keeping `images` as the table name with video rows is
  slightly misleading. Could rename to `media` in a future migration, but
  it's not worth the churn now.
- **FFmpeg linking:** Need to verify that Nix's `pkgs.ffmpeg` provides the
  dev headers and pkg-config files. May need `ffmpeg.dev` in
  `CMAKE_PREFIX_PATH`.

## Cross-References

- [001-scan-database.md](001-scan-database.md) — scan pipeline, schema
- [002-duplicate-detection.md](002-duplicate-detection.md) — duplicate logic
- [008-search.md](008-search.md) — search command
- [014-thumbs.md](014-thumbs.md) — thumbnail generation (extend for video)
