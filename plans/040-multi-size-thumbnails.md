---
name: Multi-Size Thumbnail Cache
status: completed
created: 2026-08-06
description: >
  Make the thumbnail size an explicit parameter instead of a hardcoded 512px,
  and partition the cache by size (`thumbs/<size>/ab/<sha>.jpg`) so several
  sizes coexist rather than one invalidating another. Unblocks plan 037's zoom
  ceiling, which currently caps cells at ~250px logical because nothing larger
  can be served without upscaling. `phig_thumb_lookup` and
  `phig_thumb_generate` gain a size argument — a break, but pre-release with one
  in-tree consumer, so it is made in place and the version bumped as a signal
  rather than a migration.
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

### ABI: change in place, bump to 2.0

`phig_thumb_lookup` and `phig_thumb_generate` gain a size argument, changing
their signatures. That is a break, and it costs nothing here: phig is
pre-release, `libphig.so` is not installed or linked by anything outside this
repo, and its only consumer is `gtk/`, updated in the same commit.

So: change them in place, no `_sized` variant and no compatibility shim. Bump
`PHIG_ABI_VERSION_MAJOR` to 2 as an honest signal, not because anything depends
on it.

The versioned boundary means breaks are *expressible*, not that every change
needs a migration story. Revisit that once something outside this repo links
against libphig.

### What phig decides vs what the client decides

Per the root `CLAUDE.md` interface-discipline rule: **phig owns generation and
the cache layout**; a client asks for a size and gets a path. The client does
not write into the cache, does not compute cache paths itself, and does not
decide the directory scheme. `gtk/` may still downscale an already-decoded
texture for display — that is presentation.

## Decisions

1. **ABI.** Change in place, bump to 2.0. See above.
2. **Migration.** Move the existing flat `thumbs/ab/<sha>.jpg` into
   `thumbs/512/ab/<sha>.jpg` on first run. They are all 512px, so it is a
   rename with no re-encode — seconds, and all 24,529 stay usable. Skipping it
   would strand 947 MB and re-do work already done.
3. **Generation is on demand.** The slow part is producing new sizes: ~20 min
   for 768px across 24,529 images, HDD-bound at ~13 files/sec. A cell that
   requests a size phig does not have generates it then, so bigger thumbnails
   appear where the user is actually looking rather than after a batch wait.
   `phig thumbs rebuild --size N` populates a size upfront for anyone who wants
   that instead.

4. **`scan --thumbs` writes one size, the default (512).** Generating every
   size on scan would multiply its cost for sizes nobody may look at. Other
   sizes are filled on demand.
5. **`thumbs clean` gains `--size N`**, scoping the orphan sweep to one size
   tree. Without it, it sweeps every size — which is what "remove thumbnails
   whose rows are gone" should mean.
6. **The GTK clamp becomes a configured maximum, not a cache probe.** With
   on-demand generation the question is not "what is cached" but "what will
   phig generate", so `PHIG_CACHE_THUMB_PX` (`gtk/src/main.c:41`, used at :386
   and :426) becomes the largest size phig is willing to serve. Set it to 1024
   so the zoom range is genuinely usable, and let generation fill in.

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

## Outcome (2026-08-06)

Implemented as designed. `THUMB_SIZE` is gone; `get_thumb_path`, `thumb_exists`
and `generate_thumb` all take a size, and the cache is
`thumbs/<size>/ab/<sha>.jpg`. `phig_thumb_lookup`/`phig_thumb_generate` gained
the argument, so ABI is 2.0 — with `libphig/CMakeLists.txt`'s `VERSION`/
`SOVERSION` bumped alongside the header, which is easy to miss: a stale
`libphig.so.1` on the library path silently kept the old layout and rewrote flat
directories until it was removed.

Migration ran against the real cache: 24,529 thumbnails moved into `512/` in
0.6s, byte-identical (938,210,582 bytes before and after), and a second run is a
0.13s no-op. On-demand generation verified end to end through the ABI — a 768px
lookup misses, generates, and lands at `thumbs/768/…` with the 512px tree
untouched.

The GTK ceiling (`PHIG_CACHE_THUMB_PX`) went 512 → 1024, which is what the zoom
work needed: on a 1494px viewport at scale=2 the grid previously collapsed at 6
columns / 249px cells and now continues to 3 columns / 498px cells.

Deferred: `thumbs clean --size N` (decision 5) is not implemented — `clean`
still sweeps the whole cache, which is correct but coarse.

## Depends on / relates to

- **037** (thumbnail sizing, HiDPI & grid zoom) — this unblocks its zoom
  ceiling. 037's `PHIG_CACHE_THUMB_PX 512` constant in `gtk/src/main.c` becomes
  a query against what phig actually has.
- **014** (thumbnails) — the original plan that established the 512px cache and
  the sha-keyed layout this changes.
