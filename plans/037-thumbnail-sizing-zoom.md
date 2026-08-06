---
name: Thumbnail Sizing, HiDPI & Grid Zoom
status: designing
created: 2026-08-05
description: >
  Fix the cause of M0 missing its 60 fps criterion: nothing scales a thumbnail
  to the cell, so a 512px cached thumb uploads into a 200px cell and the GPU
  downscales every frame. HiDPI and a zoomable grid pull the sizing decision in
  opposite directions, so all three are one problem — the cache key must become
  `(sha256, physical_size)` and the byte budget a target texture count. Three
  open decisions and an isolating measurement come before implementation.
  Outline in `docs/plans/2026-08-05-thumbnail-sizing-zoom-hidpi-outline.md`.
---

# 037 — Thumbnail Sizing, HiDPI & Grid Zoom

**Status is `designing`, not `planned`.** The diagnosis below is inferred from
code plus a frame-time pattern, not from an isolating experiment. The project
convention is that performance claims are measured, so the measurement in
"Before this becomes implementable" comes first. Spans `core/thumbs.cpp` and
`gtk/`.

Supersedes the M0 performance numbers in
`docs/plans/2026-08-04-phig-gtk-m0-ledger.md`: all of them — the 73.8 ms
startup median, the 83.3 ms worst frame, and the four tuned values including
the 256 MB cache budget — were measured at a single 200 px cell size and do not
generalise to a zoomable grid.

## The diagnosis

M0 measured 95% of frames over the 16 ms budget, worst frame 83 ms, and none of
the four tuned values (page radius, loader threads, texture cache bytes,
placeholder cap) moved it. From the code:

- phig's cache is a fixed **512 px** longest edge (`core/thumbs.cpp:13`,
  `THUMB_SIZE`, JPEG q80)
- the gtk cell is **200 px** logical (`gtk/src/main.c:10`, `PHIG_CELL_SIZE`)
- `thumb_loader_request` takes `max_dim` and **explicitly discards it**
  (`gtk/src/thumb-loader.c:389`, `(void)max_dim`)

So every cell uploads a 512 px texture into a 200 px frame and lets
`GTK_CONTENT_FIT_COVER` downscale on the GPU every frame. Supporting evidence
that this is per-frame fixed cost rather than jank or I/O: the recurring frame
times of 66.668 ms and 83.335 ms are exact 4× and 5× multiples of a 16.667 ms
refresh interval, and timing was invariant across all three swept values.

## Why these three are one problem

**The naive fix makes HiDPI worse.** Downscaling to the cell's *logical* size
looks correct at 1× and blurry on every Retina display. For a 200 px logical
cell: 1× needs 200 physical px (the 512 px cache is 6.6× waste — the bug), 2×
needs 400 (adequate), 3× needs 600 (**the 512 px cache is insufficient —
thumbnails go soft**). One number cannot satisfy both ends.

**Zoom makes `max_dim` a runtime range.** A file-browser-style zoom means the
cell size is user-controlled and changes while the grid is live, so the
cache-size question is not "what is the cell size" but **"what is the largest
zoom × scale we intend to support"** — a policy choice. A 384 px cell is
already soft at 2× against a 512 px cache.

Three consequences that are easy to miss:

1. **The texture cache key is wrong today.** `gtk/src/thumb-loader.c` keys by
   `sha256` alone. Correct for one fixed size; with zoom it serves a stale
   wrong-size texture or thrashes on every zoom step. The key must become
   `(sha256, physical_size)`, and eviction has to decide whether a zoom change
   discards the old generation or keeps both.
2. **A fixed byte budget means wildly different cache depth.** At 256 MB
   (`PHIG_THUMB_CACHE_BYTES`), RGBA at `w*h*4`: 1677 textures at 200 px, 419 at
   400 px, 64 at 1024 px, **28 at 1536 px** — roughly one screenful with no
   prefetch margin. A 60× swing from one budget, because resident depth shrinks
   with the *square* of the zoom. The budget likely needs expressing as a target
   texture count (or "N screenfuls") resolved to bytes at the current size.
3. **`PHIG_CELL_SIZE` is compile-time in four places** (`gtk/src/main.c:10`,
   used at `:185`, `:270`, `:366`). It becomes runtime state, so the size
   request, the `max_dim` passed to the loader, and the measurement harness all
   have to read it dynamically.

## The three open decisions

**1. Where does downscaling happen?** Today: GPU, every frame, from 512 px.
Candidate: on the loader's worker thread, once, into the texture cache
(`gdk_texture_downscale`, or decode then build a `GdkMemoryTexture` at the
target size), so the cost is paid once per distinct sha instead of once per
frame. This is the shape the seam was built for — `max_dim` is already in the
signature. Alternative: a `GdkPaintable` rendering at fixed size, less code but
leaves the full-size texture resident, so it does not cut upload bandwidth.
Which cost dominates is **not yet measured**.

**2. What size does the cache hold?** `THUMB_SIZE` is hard-coded 512
(`core/thumbs.cpp:13`). Two coherent positions:

- **Cache one master size large enough for max zoom × max scale** and derive
  every smaller display size in memory. Costs disk and a regeneration: measured
  from the current cache (24,529 thumbs, 947 MB, ~40 KB each), 768 px → ~2.1 GB,
  1024 px → ~3.7 GB. A full regeneration took **16 minutes** for 20.5k
  thumbnails here, HDD-seek-bound at 13 files/sec — a real cost for a large
  library on spinning disk.
- **Cap grid zoom at what the existing 512 px cache serves** for the scale
  factors we commit to, treating larger zooms as a viewer concern. Zero
  phig-side work; constrains the UI.

The second is cheaper and may be entirely adequate — a grid whose cells exceed
~256 px logical is arguably a viewer, not a grid. **Settle this before assuming
a regeneration is needed.** If a configurable size wins instead, phig already
has `phig_settings_*`, so a `thumbs.size` key is cheap — but the cache is keyed
by sha alone, so a size change invalidates everything unless the key includes
the size.

**3. How does the GUI learn the scale factor?** Currently it does not — zero
references to any scale API across the gtk sources. Available in GTK 4.22.4:
`gtk_widget_get_scale_factor` (integer, per-widget, the usual choice),
`gdk_surface_get_scale` (**fractional**, 4.12+, which matters because Wayland
supports 1.25/1.5 scaling the integer API rounds), `gdk_monitor_get_scale`.
Requirements: read the scale when a cell binds and pass `cell_size * scale` as
`max_dim`; react to change from **two independent sources** (dragging between
monitors, and the user zooming), which needs a `notify::scale-factor` handler
*and* a zoom handler feeding one policy; and decide the rounding rule for
fractional scales, since requesting 1.5× and letting GTK scale again
reintroduces the original problem in miniature.

**Design question worth settling early:** should zoom snap to discrete levels
(as most file browsers do) rather than being continuous? Discrete bounds the
number of cache generations, makes the `(sha, size)` key tractable, and lets a
regenerated phig cache target known sizes. Continuous means either re-scaling
from one master texture per sha or re-decoding constantly.

## Before this becomes implementable

In order, cheapest first:

1. **Set `PHIG_CELL_SIZE` to 512** so cells match the texture exactly and no
   scaling occurs. If frame time drops sharply the cost is scaling; if it barely
   moves, the cost is upload bandwidth. Discriminates the two hypotheses, needs
   no new code. **Do not skip this** — the whole plan rests on an inferred
   diagnosis.
2. **Downscale on the worker** to 200 px and re-measure. The candidate fix; if
   it does not move frame time, neither hypothesis was right.
3. If neither moves it, the cost is `GtkGridView` bind/rebind churn rather than
   pixels — profile the layout pass instead of the texture path.
4. **Re-measure at two or three zoom levels once zoom exists.** Rebinding every
   visible cell on a zoom step is itself an unmeasured cost; if it stutters, the
   answer may be rendering the old texture scaled for one frame while the
   correctly-sized one loads.

## Fold in from the M0 ledger

Two deferred items in the same code, better handled here than separately:

- **The loader probes its texture cache only after `phig_thumb_lookup` and the
  file decode**, so a repeat request for a cached sha pays ~0.5 ms it need not.
  A pre-dispatch probe removes it — directly relevant once textures are cached
  per size.
- **A permanently-failing row is re-requested on every rebind.**
  `mvi_2541-2.avi` retries roughly every 5 s because `THUMB_MISS`/`THUMB_FAILED`
  cells look identical to not-yet-loaded ones. A negative cache (sha →
  last-failure time, skip within N seconds) belongs in `ThumbLoader`: it caches
  a presentation decision, not phig logic.

## Constraints

**Thumbnail generation and any change to `THUMB_SIZE` belong in phig**, exposed
through the ABI (M0 spec, *Where functionality lives*, and the interface
discipline in the root `CLAUDE.md`). `gtk/` may downscale an already-decoded
texture for display — that is presentation — but must not decide how phig writes
its cache and must not reimplement generation. If the GUI needs a size phig does
not offer, the capability is added to phig.

**Do not add orientation handling anywhere in this work.** EXIF orientation is
applied by `cv::imread` at generation time and cached thumbnails are already
rotated — verified by pixel comparison, RMSE 0.0175 against the auto-oriented
source vs 0.378 against the raw.
