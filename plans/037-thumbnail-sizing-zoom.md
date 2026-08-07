---
name: Thumbnail Sizing, HiDPI & Grid Zoom
status: completed
created: 2026-08-05
description: >
  HiDPI-correct thumbnail sizing and a zoomable grid. The plan began as a fix
  for M0 missing 60 fps, but measurement on 2026-08-06 closed that half: the
  inferred diagnosis (a 512px thumb GPU-downscaled into a 200px cell every
  frame) was disproven, and the frame-time "defect" turned out to be two
  measurement bugs — a threshold 0.667ms below the refresh interval, and
  compositor frame-callback throttling of an unfocused window read as
  multi-second stalls. There is no evidence the grid misses 60 fps while
  visible and focused. What remains is the feature: scale-aware sizing, a
  `(sha256, physical_size)` texture cache key, and a zoom ceiling set by what
  the 512px cache can serve.
  Outline in `docs/plans/2026-08-05-thumbnail-sizing-zoom-hidpi-outline.md`.
---

# 037 — Thumbnail Sizing, HiDPI & Grid Zoom

**Status is `completed`.** Shipped 2026-08-06/07. The performance half closed as
measurement artifacts (see the diagnosis); the feature half is implemented, with
two pieces deliberately deferred — see Outcome. The
performance half is closed: the original diagnosis was inferred from code plus a
frame-time pattern rather than an isolating experiment, and when the experiment
was run (2026-08-06, all of steps 1-3 below) it came back negative three times
over. The frame-time problem was measurement bugs, not the renderer — details in
the diagnosis section. Spans `core/thumbs.cpp` and `gtk/`.

This is the convention earning its keep. The inferred diagnosis was wrong in
direction, not degree: a fix built on it would have made things 37x worse, and
the "95% of frames over budget" it was meant to explain never existed.

Supersedes the frame-timing numbers in
`docs/plans/2026-08-04-phig-gtk-m0-ledger.md` and `gtk/README.md`'s original
table (both corrected 2026-08-06). The 73.8 ms startup median stands; the four
tuned values stand as RSS choices but were never frame-time findings, since the
thumbnail path measures as contributing approximately nothing.

## The diagnosis — DISPROVEN BY MEASUREMENT (2026-08-06)

**The texture path is not the bottleneck.** Steps 1 and 2 of "Before this
becomes implementable" have been run; both pixel-cost hypotheses are dead, and
so is the residency theory that step 1 suggested. Results below, then the
original (now-falsified) reasoning for the record.

Baseline first, to confirm the harness and machine still behave as M0 measured:
worst frame 66.7–83.3 ms, 153/162 frames over 16 ms — matching the ledger's
"66.7-83.3 ms, ~95%" exactly.

| Configuration | Worst frame | >16 ms |
|---|---|---|
| Baseline: 512 px texture → 200 px cell | 66.7–83.3 ms | 94% |
| Step 1: 512 px cell (no scaling at all) | 2767–3651 ms | ~90% |
| Step 2: 200 px texture → 200 px cell | 65.3–83.3 ms | ~98% |

**Step 1 falsifies "the GPU downscales every frame."** Making the cell match the
texture exactly — the one configuration where no scaling occurs — was **37×
worse**, not better. Controlled for scroll velocity: at 512 px the grid is 4.19×
taller (5,166,346 px vs 1,232,206 px), so a fixed 4000 px step traverses 7.8
cell-rows per tick instead of 20. The 512 px run scrolled *slower* in content
terms and was still 37× worse; re-running at 10240 px/tick to match rows-per-tick
gave the same answer.

**Step 2 falsifies "upload bandwidth."** Worker-side downscale to the cell size,
verified in effect by probing texture dimensions (200x150 / 150x200 / 200x133 —
long edge 200, aspect preserved, down from 512). It delivered exactly what it
promised: 768 KB → 117 KB per texture, resident cache depth 341 → 2,237 textures
in the same 256 MB budget. Frame time did not move.

So 6.6× smaller textures and a 6.6× deeper cache changed nothing, which also
kills the residency theory step 1 suggested (that a fixed byte budget holding
6.6× fewer large textures was thrashing).

**Where this leaves the cause: there was no performance defect.** Step 3 was run
too, and it found the frame-time problem was two measurement bugs.

1. **The 16 ms threshold was wrong.** `main.c` counted a frame late at a flat
   16.000 ms, but a correctly delivered 60 Hz frame is 16.667 ms — so every
   healthy frame was counted as a failure. On one run: 94% of frames exceed
   16.000 ms, only ~3% miss a whole refresh interval, and p50 lands on 16.666 ms.
   Fixed in `2962f7a` (threshold from `gdk_frame_clock_get_refresh_info`, plus
   p50/p90/p99 in the report).
2. **The multi-second stalls are compositor throttling of an unfocused or
   occluded window.** Wayland stops sending frame callbacks, GTK's frame clock
   pauses, and the harness reads the pause as one enormous frame. Evidence: during
   a run with 2830/3100/3317 ms worst frames the harness's own 8 ms scroll timeout
   had **zero** gaps, so the main thread was never blocked; and adding an idle
   10 ms timer removes the stalls entirely (10 runs with it, 0 stalls; 8 runs
   without, 3 stalls at 3020/1734/3630 ms) because it keeps the loop awake. A
   timer that only reads a clock cannot make rendering faster.

Also measured: with thumbnail loading stubbed out completely — no loader call, no
decode, no texture — frame timing is unchanged. The thumbnail path contributes
approximately nothing, which is why none of M0's four tuned values ever moved it.

**Conclusion: no evidence the grid misses 60 fps while visible and focused.** The
`GtkGridView` bind-churn hypothesis was never needed. The performance half of
this plan is closed; what remains is the HiDPI/zoom **feature**, below.

Two corrections that fall out of this:

- **`gdk_texture_downscale` does not exist** in GTK 4.22.4. Step 2 used
  `gdk_pixbuf_new_from_file_at_size` (decode + scale in one call, on the worker;
  gdk-pixbuf arrives transitively via GTK). Note `gdk_texture_new_for_pixbuf` is
  deprecated.
- **Step 1's "512 px cells are 37x worse" was itself the throttle artifact.**
  Large cells are not inherently catastrophic, so open decision 2 is not
  constrained by that result. It is settled on other grounds below.

Step 2's implementation is stashed, not committed — it changes nothing measurable
and introduces the consequence-1 `(sha, size)` staleness below, safe today only
because cell size is constant.

### The original reasoning, for the record

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

That last point is the weak link in hindsight: exact vsync multiples are equally
consistent with *waiting* — missing a deadline and idling to the next interval —
which fits "invariant across all three swept values" better than a fill-rate
story does. The multiples persisted unchanged through both steps above.

## Why sizing, HiDPI and zoom are one problem

**Downscaling to the wrong number makes HiDPI worse.** Scaling to the cell's
*logical* size looks correct at 1x and blurry on every HiDPI display. For a
200 px logical cell: 1x needs 200 physical px, 2x needs 400 (this machine —
adequate against the 512 px cache), 3x needs 600 (**insufficient — thumbnails go
soft**). One number cannot satisfy every scale, which is why `max_dim` has to be
`cell_size * scale` rather than a constant.

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

## The three decisions — all settled (2026-08-06)

**1. Where does downscaling happen? → SETTLED: it does not matter for
performance; do it on the worker anyway, for memory.**

The premise was that GPU-downscaling every frame was costing frame time. It is
not: step 2 moved the scale onto the worker (verified: textures came back
200x150 / 150x200, down from 512) and frame time did not change. So this is no
longer a performance decision.

It is still worth doing on the worker, for a reason that does hold up: a
cell-sized texture is 117 KB against 512 px's 768 KB, so the same 256 MB budget
holds 2,237 textures instead of 341. That is a memory and cache-depth win, not a
frame-time one — claim it as such.

Implementation note: `gdk_texture_downscale` **does not exist** in GTK 4.22.4.
Use `gdk_pixbuf_new_from_file_at_size` (decode + scale in one call, on the
worker), which also avoids decoding pixels that are then thrown away.
`gdk_texture_new_for_pixbuf` is deprecated, so prefer building a
`GdkMemoryTexture` from the pixbuf's buffer. A working implementation is in
`git stash` (`037 step 2: worker-side downscale`) — measured as
frame-time-neutral, so treat it as a starting point, not a validated fix.

**2. What size does the cache hold? → SETTLED: keep 512 px, cap grid zoom at 256
px logical.** No regeneration, no `THUMB_SIZE` change, no disk cost.

This machine runs `scale=2` (2560x1440 logical / 5120x2880 physical), so HiDPI is
the current environment rather than a future one. Against the existing 512 px
cache:

| Cell (logical) | Physical at 2x | vs 512 px cache |
|---|---|---|
| 150 px | 300 px | fine |
| 200 px | 400 px | fine (current default) |
| **256 px** | **512 px** | **exactly the cache — the natural ceiling** |
| 384 px | 768 px | soft |
| 512 px | 1024 px | soft |

256 px logical at 2x lands exactly on 512 px. So the cheap option the plan hoped
for is not merely adequate, it is well matched: a 256 px ceiling is generous for
a grid, and the plan's own argument is that anything larger is a viewer.

The expensive option (regenerate at 768/1024 px, ~2.1-3.7 GB, 16 min for 20.5k
thumbs at HDD-bound 13 files/sec) buys only cells above 256 px logical. Not
worth it now. Revisit only if a 3x display or a >256 px grid cell becomes a real
requirement.

**3. How does the GUI learn the scale factor? → SETTLED: integer
`gtk_widget_get_scale_factor`.**

Currently it does not: zero references to any scale API across `gtk/`.

**Fractional scaling is out of scope.** GTK will not ship on macOS — a native Mac
app is the plan there — so the fractional-scale case that motivated
`gdk_surface_get_scale` (Wayland 1.25/1.5, which the integer API rounds) is not a
case this client has to serve. Use `gtk_widget_get_scale_factor`: integer,
per-widget, correct for the displays this client targets. Revisit only if a
fractional-scaled Linux display becomes a real target.

Read the scale when a cell binds, pass `cell_size * scale` as `max_dim`, and
handle `notify::scale-factor` for the window moving between monitors. That and the
zoom handler feed one policy, not two code paths.

## The zoom model: fit-to-width with snap (settled 2026-08-06)

Modelled on iOS Photos, which is a better fit than stepping through preset cell
sizes: **the grid always divides the viewport width exactly, so there is never
horizontal scrolling and never a ragged right edge.**

The inversion that makes this work — zoom does not set the cell size:

```
zoom gesture -> target column count (integer) -> cell size = viewport_width / columns
```

Cell size is *derived*. Resizing the window changes cell size while the column
count holds; zooming changes the column count. `GtkGridView` already reflows
columns to fit the width (`min_columns=3`, no `max_columns`, `main.c:571`), so the
reflow half exists. What is missing is that the cell has a hard
`gtk_widget_set_size_request(frame, 200, 200)` (`main.c:309`), so cells stay 200 px
and the leftover width becomes gaps instead of being divided up.

### Snapping, with hysteresis

The gesture is continuous; the resting state is always a whole column count. Mid
gesture the column count is fractional (e.g. 4.6 columns). Which way it settles
depends on how much of the edge column is showing **and on the pinch direction**,
so a gesture hovering near a boundary does not oscillate between two counts:

```
pinching IN  (toward bigger cells):  commit when fraction < 0.4
pinching OUT (toward smaller cells): commit when fraction > 0.6
dead zone 0.4-0.6:                   hold the current count
```

Plain rounding at 0.5 was considered and rejected: under a slow pinch it
flip-flops exactly at the boundary.

### Texture requests follow the resting state, not the gesture

Requesting a texture per gesture frame would fire a new request every frame of a
pinch. Instead request for the **snap target** and let GTK scale the existing
texture during the gesture — which is also what makes the animation cheap, since
it scales textures already resident.

Sizes are then quantised to bound the cache: round the requested physical size up
to the next 64 px bucket, so a 187 px and a 195 px cell both request 256 px. This
keeps `(sha256, physical_size)` small without needing discrete cell sizes, and
means an ordinary window resize usually does not cross a bucket boundary — cached
textures stay valid and only layout changes.

### Bounds

- **Zoom-in limit is computed, not hardcoded.** The 512 px cache serves cells up
  to 256 px logical at scale=2, so the sharpness floor is about
  `min_columns = viewport_width / (256 * scale) * scale` — i.e. do not allow a
  column count whose derived cell size would need more than 512 physical px.
  Past that, thumbnails visibly soften.
- **`max_columns` must be set** (it currently is not). Without it a very wide
  window can reflow to 20+ unusably small columns.
- Note the interaction with wide windows: on this 2560 px display at scale=2, a
  3-column grid means 853 px cells needing 1706 physical px — far past the cache.
  The usable range on a wide monitor is therefore the *smaller-cell* end. This is
  a consequence of the fixed 512 px cache, not of the zoom model.

### Input

`GtkGestureZoom` (trackpad pinch) and `GtkEventControllerScroll` (Ctrl+scroll,
for mice) both exist in GTK 4.22.4; there is no gesture handling in `main.c`
today. Both feed the same column-count policy.

### Nice-to-have

The iOS transition animates cells to their new size rather than jumping. Not
required. It is cheap if texture requests follow the snap target as above, since
the animation then scales already-resident textures.

## The measurement record (steps 1-3 complete)

All three discriminating steps were run on 2026-08-06. Kept as a record of what
was tested and what each result actually meant.

1. ~~**Set `PHIG_CELL_SIZE` to 512**~~ — **DONE.** Reported 37x worse, which
   falsifies the GPU-downscale hypothesis either way. In hindsight the magnitude
   was mostly the compositor-throttle artifact found in step 3, so read this as
   "not better", not as "large cells cost 37x".
2. ~~**Downscale on the worker** to 200 px~~ — **DONE 2026-08-06.** No change in
   frame time despite 6.6× smaller textures and 6.6× deeper cache. Falsifies the
   upload-bandwidth hypothesis. Implementation stashed, not committed.
3. ~~**Profile the layout/bind path.**~~ — **DONE 2026-08-06.** Found two
   measurement bugs instead of a defect; see the diagnosis section. Stubbing out
   thumbnail loading entirely left frame timing unchanged, so the texture path was
   never implicated and the `GtkGridView` bind-churn hypothesis was never needed.
   The 512 px cliff from step 1 is the same compositor-throttle artifact, not a
   real 30-40x cost.
4. **Re-measure at two or three zoom levels once zoom exists.** Still worth doing
   — rebinding every visible cell on a zoom step is a genuinely unmeasured cost.
   Measure with the window focused and visible, using the corrected harness
   (`2962f7a`): report `late`% against the detected refresh interval, never a
   16 ms constant, and treat any multi-second frame as a throttle artifact until
   proven otherwise.


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
