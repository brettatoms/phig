---
name: Blur Detection
status: planned
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

## Open Questions

- What default threshold for `--blurry`? Needs testing on real photo collections.
- **Resolution normalization:** A 4K image will naturally produce higher Laplacian variance than a 640x480 image of the same scene. Options to make scores comparable across resolutions:
  - *Downscale first:* Resize to a fixed size (e.g., 512x512) before computing. Scores are directly comparable.
  - *Normalize at query time:* Store the raw score and scale by dimensions (e.g., `blur_score / (width * height)`) during search. Simpler storage, dimensions already in DB.
  - *Store raw, ignore the issue:* Most photo collections are from similar devices, so resolution variance may be small in practice.
  - Not yet decided which approach to use.
- Should there be a `--sort sharpness` flag on search to rank results?

## Checklist

- [ ] Add `blur_score` column to schema (with migration for existing DBs)
- [ ] Compute Laplacian variance in `hasher.cpp` alongside phash (already-decoded image)
- [ ] Store in database during scan
- [ ] Add `blur_score` to `ImageInfo` struct
- [ ] Search integration (`--blurry`, `--min-sharpness`, `--max-sharpness`)
- [ ] Tests for blur score computation
