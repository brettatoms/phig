---
name: Multi-Size Thumbnail Cache
status: designing
created: 2026-08-06
description: >
  Make the thumbnail size an explicit parameter instead of a hardcoded 512px,
  and partition the cache by size (`thumbs/<size>/ab/<sha>.jpg`) so several
  sizes coexist rather than one invalidating another. Unblocks plan 037's zoom
  ceiling, which currently caps cells at ~250px logical because nothing larger
  can be served without upscaling. Breaking ABI change: `phig_thumb_lookup`
  and `phig_thumb_generate` gain a size argument, so this is libphig 2.0.
---

# 040 — Multi-Size Thumbnail Cache

`THUMB_SIZE` is `static const int = 512` in `core/thumbs.cpp:13`. Every
thumbnail is that size, and the on-disk cache is keyed by sha alone
(`thumbs/ab/<sha>.jpg`), so the size is not part of the identity of a cached
file.

Two consequences:

- **Zoom is capped.** Plan 037's grid derives the physical pixels a cell needs
  (`cell_size * scale_factor`) and refuses to request more than the cache holds,
  because a larger request would be upscaled and visibly soft. On a 1494px
  viewport at scale=2 that caps cells at 249px logical: every zoom level above a
  ~244px target collapses onto the same 6 columns.
- **Changing the size throws the cache away.** Because size is not in the key,
  raising `THUMB_SIZE` silently invalidates all 24,529 existing thumbnails and
  forces a full regeneration.

## Measured costs

Re-encoded with phig's own pipeline (longest edge, `INTER_AREA`, JPEG q80) over
a 34-image sample of real collection photos over 500 KB. **Plan 037's estimates
were roughly 2x too pessimistic** — they scaled from average file size rather
than re-encoding.

| Size | Mean per thumb | Projected for 24,529 | vs 512px |
|---|---|---|---|
| 512px (current) | 27.1 KB | 0.63 GB | — |
| 768px | 51.8 KB | 1.21 GB | +0.58 GB |
| 1024px | 80.1 KB | 1.87 GB | +1.24 GB |

(Plan 037 predicted 2.1 GB and 3.7 GB. The actual on-disk cache today is 947 MB
over 24,529 files, larger than the 0.63 GB projection because it includes
thumbnails for sources smaller than the sample's.)

What each size unlocks, as the largest cell that can be served without
upscaling:

| Cache | @1494px viewport, scale=2 | @2560px viewport, scale=2 |
|---|---|---|
| 512px | 249px logical (6 cols) | 256px logical (10 cols) |
| 768px | 373px logical (4 cols) | 365px logical (7 cols) |
| 1024px | 498px logical (3 cols) | 512px logical (5 cols) |

768px buys the two extra zoom levels plan 037 wants, for +580 MB.

## Design

### Cache layout

```
~/.cache/phig/thumbs/512/ab/ab3f7c9e...d4e2.jpg
~/.cache/phig/thumbs/768/ab/ab3f7c9e...d4e2.jpg
```

The size directory sits above the existing two-hex-char fanout, so sizes are
independent trees. Adding a size costs nothing to the sizes already there, and
the GTK client's quantised requests (plan 037, `PHIG_TEXTURE_SIZE_QUANTUM`) map
onto a small set of directories rather than one per pixel value.

### API

Size becomes an explicit parameter — the caller names what it wants, and there
is no ambient default to get wrong:

```cpp
std::string get_thumb_path(const std::string& sha256, int size);
bool thumb_exists(const std::string& sha256, int size);
bool generate_thumb(const cv::Mat& img, const std::string& sha256, int size);
bool generate_thumb(const std::string& image_path, const std::string& sha256, int size);
```

Callers to update: `core/scan_engine.cpp` (5 sites), `core/thumbs_engine.cpp`
(3), `libphig/thumbs.cpp` (5), `cli/main.cpp` (4).

### ABI: this is 2.0, not 1.3

`phig_thumb_lookup(ctx, sha256, out_path, out_len, exists)` gains a size
argument, which **breaks** the existing signature. Current ABI is 1.2
(`PHIG_ABI_VERSION_MAJOR 1`, `MINOR 2`).

Two options, to settle before implementing:

- **Break to 2.0.** Cleanest surface. libphig's only consumer today is `gtk/`,
  in this repo, so the blast radius is one caller we control.
- **Add `phig_thumb_lookup_sized` alongside**, keeping the old entry point at
  the default size. 1.3, no break, but the surface carries a legacy call
  forever, and the "default size" ambiguity this plan exists to remove comes
  straight back.

Leaning **2.0**: the versioned boundary exists so breaks are expressible, and
the sole consumer is in-tree. Confirm before implementing.

### What phig decides vs what the client decides

Per the root `CLAUDE.md` interface-discipline rule: **phig owns generation and
the cache layout**; a client asks for a size and gets a path. The client does
not write into the cache, does not compute cache paths itself, and does not
decide the directory scheme. `gtk/` may still downscale an already-decoded
texture for display — that is presentation.

## Open questions

1. **ABI 2.0 vs an additive `_sized` entry point.** See above; leaning 2.0.
2. **Which sizes does phig generate by default?** A `--size` on
   `phig thumbs rebuild` is the obvious surface, but scan also generates
   thumbnails (`--thumbs`), and it needs to know which size(s) to write. A
   single configured default with explicit opt-in for others is probably right,
   but the config key interacts with question 3.
3. **Does `thumbs clean` become size-aware?** Today it deletes orphaned
   thumbnails whose rows are gone. With several sizes it also needs to answer
   "delete a whole size tree" (e.g. after deciding 512px is no longer wanted).
   Probably `phig thumbs clean [--size N]`.
4. **Migration of the existing 947 MB cache.** The current flat
   `thumbs/ab/<sha>.jpg` files are all 512px. Moving them into `thumbs/512/`
   preserves them for free and avoids a regeneration; leaving them stranded
   wastes 947 MB until someone deletes it. A one-time move on first run is
   cheap (rename, no re-encode) but is a migration step that has to be right.
5. **Does the GTK client request 768px, or keep 512px until asked?** Plan 037's
   clamp reads the cache size; if that becomes a set of sizes, the clamp needs
   to know which are actually populated rather than assuming the largest.

## Tests

Data-adjacent (it moves cached files), so real-behaviour tests:

- `get_thumb_path` includes the size and differs between sizes for one sha.
- Generating at two sizes leaves both on disk, neither clobbering the other.
- `thumb_exists(sha, 512)` is false when only 768px exists.
- The existing `tests/test_thumbs.cpp` dimension assertions (four hardcoded
  512/307 values at :72, :73, :85, :86) become size-parameterised.
- Migration: a pre-existing flat `thumbs/ab/<sha>.jpg` ends up at
  `thumbs/512/ab/<sha>.jpg` with identical bytes, and is not re-encoded.
- `thumbs clean` removes orphans within a size tree without touching others.

Gate: `ctest --test-dir build --parallel` green, plus the real-run checks
(`phig search --count` = 29925, `phig --version` = 0.3.0).

## Depends on / relates to

- **037** (thumbnail sizing, HiDPI & grid zoom) — this unblocks its zoom
  ceiling. 037's `PHIG_CACHE_THUMB_PX 512` constant in `gtk/src/main.c` becomes
  a query against what phig actually has.
- **014** (thumbnails) — the original plan that established the 512px cache and
  the sha-keyed layout this changes.
