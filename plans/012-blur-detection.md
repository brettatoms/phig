---
name: Blur Detection
status: completed
created: 2026-03-28
description: >
  Compute a blur/sharpness score for each image during scan using Laplacian
  variance. Store in the database. Searchable to find and filter blurry images.
---

# Blur Detection

## Goal

Score each image's sharpness during scan. Store the score in the database so users can find and clean up blurry photos.

## Method

Laplacian variance — a well-established single-number sharpness metric:

1. Convert decoded image to grayscale
2. Apply `cv::Laplacian` (second derivative)
3. Compute variance of the result

Higher variance = sharper image. Blurry images have low variance because pixel intensity changes are gradual.

This is computed on the already-decoded `cv::Mat` during the scan pipeline, so the cost is negligible (~1-2ms on top of the existing decode).

## Schema

```sql
ALTER TABLE images ADD COLUMN blur_score REAL;
```

Single float. Typical values:
- < 100: very blurry
- 100-500: somewhat soft
- 500+: sharp

Exact thresholds depend on image content and resolution, so these are rough guidelines.

## CLI

### Scan

No new flags needed — blur score is always computed during scan (it's essentially free). Existing images without a blur score get computed on next `--force` scan or could be backfilled with a lightweight pass.

### Search (integration with plan 008)

```bash
phig search --blurry              # images below a default sharpness threshold
phig search --min-sharpness 500   # only sharp images
phig search --max-sharpness 100   # only blurry images
```

### Practical workflow

```bash
# Find blurry photos
phig search --blurry --path ~/Photos/Vacation

# Review them, then purge
phig purge --match <blurry files>
```

## Decisions (resolved during implementation)

- **Resolution normalization:** downscale (only shrink) to a 512px longest edge
  before computing the Laplacian variance, so scores are comparable across image
  sizes without fabricating detail on small images by upscaling.
- **`--blurry` default:** `blur_score <= 100` — a rough guideline (the plan's
  "< 100 very blurry"). `--min-sharpness`/`--max-sharpness` give precise control.
- **Unscored images:** `blur_score` is nullable; images scanned before this feature
  read back as `-1` and are excluded from sharpness filters until re-scanned
  (`phig scan --force hash`).

## Still open (future)

- A `--sort sharpness` flag to rank search results was not added; deferred.

## Checklist

- [x] Add `blur_score` column to schema (v4 migration + fresh path)
- [x] Compute Laplacian variance in `hasher.cpp` alongside phash (already-decoded image)
- [x] Store in database during scan (insert + insert_copy carry it)
- [x] Add `blur_score` to `ImageInfo` struct
- [x] Search integration (`--blurry`, `--min-sharpness`, `--max-sharpness`)
- [x] Tests for blur score computation + sharpness search + round-trip
