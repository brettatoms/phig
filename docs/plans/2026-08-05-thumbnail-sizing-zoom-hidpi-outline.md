# Thumbnail sizing: texture downscaling, HiDPI, and grid zoom — outline

**Status:** outline only, not ready to implement. Deliberately parked; needs a design pass
and measurement before it becomes a plan.
**Date:** 2026-08-05
**Context:** `2026-08-04-phig-gtk-m0-design.md` (M0 missed its 60 fps criterion),
`2026-08-04-phig-gtk-m0-ledger.md` (measurements and the final review's diagnosis)

## Why this is one problem, not two

M0 measured 95% of frames over the 16 ms budget with a worst frame of 83 ms, and none of the
four tuned values (page radius, loader threads, texture cache bytes, placeholder cap) moved
it. The final review's diagnosis from the code: **nothing scales a thumbnail to the cell.**

- phig's cache is a fixed **512 px** longest edge (`src/thumbs.cpp:13`, `THUMB_SIZE`, JPEG q80)
- phig-gtk's cell is **200 px** logical (`src/main.c:10`, `PHIG_CELL_SIZE`)
- `thumb_loader_request` takes `max_dim` and **explicitly discards it** (`(void)max_dim`)
- so every cell uploads a 512 px texture into a 200 px frame and lets
  `GTK_CONTENT_FIT_COVER` downscale on the GPU every frame

Corroborating evidence that this is per-frame fixed cost rather than jank or I/O: the
recurring frame times of 66.668 ms and 83.335 ms are exact 4× and 5× multiples of a
16.667 ms refresh interval, and timing was invariant across all three swept values.

**The naive fix makes HiDPI worse, which is why these must be designed together.** Measured
requirements for a 200 px logical cell:

| Display | Physical px needed | 512 px cache |
|---|---|---|
| 1× | 200 | 6.6× more pixels than needed — the performance bug |
| 2× (Retina) | 400 | adequate, 1.6× waste |
| 3× | 600 | **insufficient — thumbnails go soft** |

Downscaling to the cell's *logical* size would look correct at 1× and blurry on every
Retina display. The two concerns pull in opposite directions and one number cannot satisfy
both.

### And the grid will be zoomable, which makes `max_dim` a runtime range

A file-browser-style zoom means the cell size is **not a constant** — it is a user-controlled
value that changes while the grid is live. That is not an extra feature bolted onto this
work; it changes the shape of all three decisions below.

Physical pixels needed across plausible zoom levels:

| Zoom (logical cell) | 1× | 2× | 3× | 512 px cache |
|---|---|---|---|---|
| 96 | 96 | 192 | 288 | fine everywhere |
| 128 | 128 | 256 | 384 | fine everywhere |
| 200 | 200 | 400 | 600 | fine to 2×, soft at 3× |
| 256 | 256 | 512 | 768 | fine to 2×, soft at 3× |
| 384 | 384 | 768 | 1152 | **soft at 2× already** |
| 512 | 512 | 1024 | 1536 | **soft at 2× already** |

So the cache-size question is not "what is the cell size" but **"what is the largest
zoom × scale we intend to support"** — a single product that has to be chosen as policy.

Three consequences that are easy to miss:

**1. The texture cache key is wrong today.** `thumb-loader.c:112` keys the cache by `sha256`
alone. With one fixed size that is correct; with zoom it serves a stale wrong-size texture or
thrashes on every zoom step. The key must become `(sha256, physical_size)`, and the eviction
policy has to decide whether a zoom change discards the old generation or keeps both.

**2. A fixed byte budget means wildly different cache depth at different zooms.** At the
current 256 MB (`PHIG_THUMB_CACHE_BYTES`), RGBA at `w*h*4`:

| Physical px (square) | Per texture | Textures held in 256 MB |
|---|---|---|
| 200 | 0.15 MB | 1677 |
| 400 | 0.61 MB | 419 |
| 600 | 1.37 MB | 186 |
| 1024 | 4.00 MB | 64 |
| 1536 | 9.00 MB | 28 |

A 60× swing from the same budget: zooming in shrinks the resident window by the *square* of
the zoom. **28 textures at max zoom on a 3× display is roughly one screenful with no
prefetch margin.** So the budget likely needs expressing as a target texture *count* (or
"N screenfuls") that resolves to bytes at the current size, rather than a fixed byte number
— and Task 7's measured 256 MB was measured at exactly one zoom level, so it does not
generalise.

**3. `PHIG_CELL_SIZE` is compile-time in four places** (`main.c:10`, and used at `:185`,
`:270`, `:373`). It becomes runtime state on the app, which means the size request, the
`max_dim` passed to the loader, and the measurement harness's assumptions all have to read it
dynamically.

**Design question worth settling early:** should zoom snap to a small set of discrete levels
(as most file browsers do) rather than being continuous? Discrete levels bound the number of
cache generations, make the `(sha, size)` key tractable, and let a regenerated phig cache
target known sizes. Continuous zoom means either re-scaling from one master texture per sha
or re-decoding constantly.

## The three decisions to make

### 1. Where does downscaling happen?

Today: GPU, every frame, from 512 px. Options:

- **On the loader's worker thread, once, into the texture cache.** `gdk_texture_downscale`
  exists, or decode then build a `GdkMemoryTexture` at the target size. Cache the
  *display-sized* texture rather than the file-sized one, so the cost is paid once per
  distinct sha instead of once per frame. This is the shape the seam was built for —
  `max_dim` is already in the signature.
- **Ask GTK to do it once** via a `GdkPaintable` that renders at a fixed size. Less code,
  but leaves the full-size texture resident, so it does not reduce upload bandwidth.

The first is the likely answer, but which cost actually dominates — upload bandwidth or
per-frame scaling — is **not yet measured**. See the measurement list below.

### 2. What size does the cache hold?

`THUMB_SIZE` is a hard-coded 512 in `src/thumbs.cpp`. Choices:

- **Keep 512 and accept softness above 2×.** Zero work. Fine until someone runs it on a 3×
  display.
- **Raise it.** Measured cost from the current cache (24,529 thumbs, 947 MB, ~40 KB each):
  768 px → ~2.1 GB (2.2×), 1024 px → ~3.7 GB (4.0×). Also a full regeneration, which took
  **16 minutes** for 20.5k thumbnails on this collection — and that was HDD-seek-bound at
  13 files/sec, so it is a real cost for a user with a large library on spinning disk.
- **Make it configurable**, with the GUI requesting what it needs. phig already has a
  settings API (`phig_settings_*`), so a `thumbs.size` key is cheap. Interacts with the cache
  layout: thumbnails are keyed by sha alone, so a size change invalidates everything unless
  the key includes the size.
- **Two tiers** — a small one for grids, a larger one for the viewer. Doubles cache size but
  makes each request cheap. Worth considering only if one size cannot serve both.

**Open question that should be settled first:** is a bigger cache even needed, or should the
GUI generate a display-sized texture on demand from the 512 px cache and keep *that* in
memory? At 2× the 512 px cache is already adequate; only 3× is short.

**With zoom, this question gets sharper rather than going away.** The largest
zoom × scale sets the requirement, and the table above shows a 384 px cell is already soft at
2× against a 512 px cache. Two coherent positions:

- **Cache one master size large enough for max zoom × max scale**, and have the GUI scale
  down from it for every smaller zoom. One cache generation on disk, all display sizes derived
  in memory. Costs the disk space and a regeneration; the size must be chosen once and lived
  with.
- **Cap the maximum zoom at what the existing 512 px cache can serve** for the scale factors
  we commit to supporting, and treat larger zooms as a viewer concern rather than a grid one.
  Zero phig-side work; constrains the UI.

The second is cheaper and may be entirely adequate — a grid whose cells exceed ~256 px
logical is arguably a viewer, not a grid. Worth deciding before assuming a regeneration is
needed.

### 3. How does the GUI learn the scale factor?

Currently it does not — `grep` finds **zero** references to any scale API across all three
phig-gtk source files. Available and confirmed present in GTK 4.22.4:

- `gtk_widget_get_scale_factor` — integer, per-widget, the usual choice
- `gdk_surface_get_scale` — **fractional** (GTK 4.12+), which matters because Wayland
  supports 1.25/1.5 scaling that the integer API rounds
- `gdk_monitor_get_scale`, `gdk_surface_get_scale_factor`

Requirements a design must handle:

- Read the scale when a cell binds and pass `cell_size * scale` as `max_dim`, where
  `cell_size` is now the **current zoom level**, not a constant.
- **React to change, from two independent sources.** Dragging a window between a 1× and a 2×
  monitor changes the scale; the user zooming changes the cell size. Either invalidates cached
  textures at the old size. Needs a `notify::scale-factor` handler *and* a zoom handler,
  feeding one policy — which is why the `(sha, size)` cache key matters more than it first
  appears.
- Decide the rounding rule for fractional scales. Requesting 1.5× and getting a texture
  scaled again by GTK reintroduces the original problem in miniature. If zoom is discrete,
  consider quantising `zoom × scale` to the same small set so the number of live cache
  generations stays bounded.
- **Rebinding every visible cell on a zoom step is itself a cost** that has not been measured.
  A zoom is a size-request change across the whole grid plus a fresh texture per visible cell;
  if that stutters, the answer may be to render the old texture scaled for one frame while the
  correctly-sized one loads — the same thumbnail-then-full-image trick the viewer path uses.

## What to measure before designing

The M0 measurements narrowed the cause but did not isolate it. In order, cheapest first:

1. **Set `PHIG_CELL_SIZE` to 512** so cells match the texture exactly and no scaling occurs.
   If frame time drops sharply the cost is scaling; if it barely moves, the cost is upload
   bandwidth. This one change discriminates the two hypotheses and needs no new code.
2. **Downscale on the worker** to 200 px and re-measure. This is the candidate fix; if it
   does not move frame time, neither hypothesis was right.
3. If neither moves it, the cost is `GtkGridView` bind/rebind churn rather than pixels —
   profile the layout pass rather than the texture path.

4. **Re-measure frame time at two or three zoom levels once zoom exists.** Every number in
   the M0 ledger — the 73.8 ms startup median, the 83.3 ms worst frame, and the four tuned
   values including the 256 MB cache budget — was measured at a single 200 px cell size. None
   of them generalises to a zoomable grid, and the cache-depth table above shows why: the same
   byte budget holds 1677 textures at one zoom and 28 at another. Treat the M0 values as valid
   only for the zoom level they were taken at.

Do not skip step 1. The project's convention is that performance claims are measured, and
this whole outline rests on a diagnosis inferred from code plus a frame-time pattern, not on
an isolating experiment.

## Related work this should pick up

Two deferred items from the M0 ledger live in the same code and should be handled here
rather than separately:

- **The loader consults its texture cache only *after* `phig_thumb_lookup` and the file
  decode**, so a repeat request for an already-cached sha pays ~0.5 ms it need not. A
  pre-dispatch cache probe removes it. Directly relevant once textures are cached per size.
- **A permanently-failing row is re-requested on every rebind.** `mvi_2541-2.avi` retries
  roughly every 5 s because `THUMB_MISS`/`THUMB_FAILED` cells look identical to
  not-yet-loaded ones and nothing remembers the failure. A negative cache (sha →
  last-failure time, skip within N seconds) belongs in `ThumbLoader`; it is caching a
  presentation decision, not reimplementing phig logic.

## Constraint that governs the whole design

Per the M0 spec's *Where functionality lives*: **thumbnail generation and any change to
`THUMB_SIZE` belong in phig**, exposed through the ABI. phig-gtk may downscale an
already-decoded texture for display — that is presentation — but must not decide how phig
writes its cache, and must not reimplement generation. If the GUI needs a size phig does not
offer, the capability is added to phig.

Also: do not add orientation handling anywhere in this work. EXIF orientation is applied by
`cv::imread` at generation time and cached thumbnails are already rotated (verified by pixel
comparison, RMSE 0.0175 against the auto-oriented source vs 0.378 against the raw).
