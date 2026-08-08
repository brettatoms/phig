---
name: macOS App M0 — Grid & Viewer
status: designing
created: 2026-08-08
description: >
  First browsing milestone for the macOS client: sidebar, virtualized thumbnail
  grid over the whole collection, and a single-image viewer with a metadata
  inspector. Read-only — no tags, no favourites, no destructive actions. Builds
  on the app target, XcodeGen project and Swift-to-libphig interop created by
  049. The grid rendering choice (SwiftUI `LazyVGrid` versus AppKit
  `NSCollectionView`) is deliberately left open and decided by measurement
  against the real imported collection, because no number for either exists at
  30k items and GTK M0's do not transfer.
---

# 050 — macOS App M0 — Grid & Viewer

## Scope

A read-only browsing app: sidebar, thumbnail grid, viewer with metadata.

Read-only is a deliberate boundary, not a shortcut. It means M0 cannot damage a
collection, so the whole milestone can be exercised against real photos without
a restore plan. Tags, favourites, galleries, organize and remove come later,
each with its own confirmation surface.

Deliberately excluded from M0: search, filter chips, duplicates, people,
activity log, settings. Plan 047 specifies all of them; none belongs in the
first browsing milestone.

## What 049 already provides

The app target, `macos/project.yml`, the bundle, and the Swift-to-libphig
interop layer — handle lifetimes in `deinit`, `phig_status` converted to thrown
Swift errors, `phig_last_error` read on the failing thread. This plan adds
screens to an app that already runs and already talks to libphig.

It also provides the collection: after 049 the imported Photos library *is* the
corpus, which is what makes the measurement below possible on real data. This
is a real benefit of doing the import first — the development machine has no
phig collection at all today (measured: `phig.db` is 77 KB of empty schema, no
thumbnail cache), and the 29,925-image collection in the root `CLAUDE.md` is on
the Linux machine.

## The grid decision is deferred to measurement

Plan 047 leaves the macOS rendering path open ("SwiftUI is preferred; AppKit may
be necessary"). This plan does not close it by preference. There is no measured
number for either option at collection scale, and the root `CLAUDE.md` is
explicit that performance questions have only ever yielded to measurement here —
the BK-tree that came in 40x off its target, and three rounds of guard
calibration that each found a regression the previous round could not predict.

So: **build the smallest honest harness, measure both, then choose.**

### Harness

One window, one grid, the real collection, both implementations behind the same
protocol:

```swift
protocol ThumbnailGrid {
    func setItemCount(_ n: Int)
    func setCellSize(_ physicalPixels: Int)
}
```

- **A** — SwiftUI `LazyVGrid` in a `ScrollView`.
- **B** — `NSCollectionView` wrapped in `NSViewRepresentable`.

Both fed by the same paging model over `phig_query_page` and the same
thumbnail loader, so the only variable is the rendering path.

### What to measure

- Frame interval during a sustained full-range fast scroll, as a distribution —
  median, 95th, 99th, worst — not a mean.
- Memory high-water mark across the same scroll.
- Time from launch to first painted row.

### Measurement traps, carried over from plan 037

That plan's frame-time "defect" turned out to be two measurement bugs, not a
rendering problem. Both apply here:

- **Read the refresh interval at runtime.** 037's late-frame threshold sat
  0.667 ms under the actual refresh interval and manufactured late frames. Take
  the interval from `CADisplayLink`/`NSScreen.maximumFramesPerSecond` rather
  than hardcoding 16.67 ms — and note the number varies by machine and by
  external display.
- **Measure only a focused, visible window.** 037 read compositor throttling of
  an unfocused window as multi-second stalls. Discard samples taken while the
  window is not key and on screen.

A result is reportable only with the machine, display refresh rate, scale
factor, collection size and thumbnail size recorded beside it.

### Decision rule, fixed in advance

If SwiftUI holds the frame budget at the 99th percentile on the real
collection, take SwiftUI — it is less code for every subsequent screen. If it
does not, take `NSCollectionView`. Writing the rule down before seeing the
numbers is what stops the answer being chosen and then justified.

## Screens

**Sidebar** — All Photos and Favorites only. Galleries, People and Tags are
sidebar items in plan 047's design but need query surfaces and screens M0 does
not have; adding empty disclosure triangles would be worse than omitting them.

**Grid** — virtualized, page-fetched through `phig_query_page`, sorted by date
descending. Zoom changes the column count; cell size is derived by dividing the
viewport width, so the grid always fills the width exactly with no horizontal
scroll. That policy is inherited from plan 037, where it was settled with
direction-dependent hysteresis (commit in below 0.4, out above 0.6) to stop a
hovering pinch oscillating.

**Viewer** — full-resolution image scaled to fit, arrow-key and swipe
navigation, prev/next prefetch. The upscaled thumbnail shows while the full
decode is in flight, so there is never a blank frame. A metadata inspector as
the third column of a `NavigationSplitView`: filename, date, camera, exposure,
dimensions, and the blur score.

The viewer needs full-resolution pixels, which libphig does not provide —
`libphig/CLAUDE.md` records that pixel decoding is absent from the ABI. On
macOS the viewer can decode through ImageIO directly, since after 048 every
format phig indexes is one ImageIO reads, except RAW and the OpenCV-only
formats. RAW in the viewer is an open question below.

## Thumbnail loading

Workers call `phig_thumb_lookup`, falling back to `phig_thumb_generate` on a
miss, then hand results back to the main actor. Three rules carried directly
from `gtk/CLAUDE.md`, each of which was learned there the hard way:

- **Cancel on recycle.** A cell reused for a different row must cancel its
  outstanding request, or it flashes the previous row's thumbnail.
- **Size in physical pixels, quantized.** The loader's target is the largest
  edge in physical pixels — cell size times the backing scale factor (2 on the
  development Mac). Quantize to buckets before requesting, or the texture cache
  grows a generation per distinct pixel size.
- **No orientation handling in the client.** Orientation is applied at
  thumbnail generation time in `core/`. After 048 that includes HEIC. A client
  that rotates would double-apply.

## Testing

- **Headless model tests**: paging arithmetic, LRU page eviction, cell-to-index
  mapping, zoom column/hysteresis arithmetic, cancel-on-recycle. GTK M0 got 13
  headless tests over the same logic; this is the same surface in Swift.
- **Interop tests**: handle lifetimes released in `deinit`, error propagation
  from a bad database path, `phig_last_error` read on the failing thread.
- **Harness measurement**, recorded with its conditions, as above.
- **Real-run check**: a full-range fast-drag scroll over the whole collection
  produces no dropped-frame cluster and no thumbnail flashing.

## Open questions

1. **Grid implementation** — settled by the measurement, not by argument.
2. **RAW in the viewer.** ImageIO reads some RAW formats, but phig decodes RAW
   through its LibRaw 3-tier source (plan 026), which is unavailable to the app
   because it lives in `core/` behind no ABI. Use ImageIO's RAW support and
   accept different pixels than phig hashed, upscale the thumbnail, or add a
   full-decode entry point to libphig?
3. **Quick Look.** Plan 047 wants spacebar Quick Look in the grid. Free via
   `QLPreviewPanel` when the file exists at the indexed path — cheap enough that
   it may belong in M0.
4. **Window restoration.** Whether scroll position and selection survive a
   relaunch.
5. **The collection has no `libphig` search surface for "Favorites" yet** —
   `phig_query` has tag filtering, and favourite is a protected built-in tag
   (plan 011), so this is expected to work through `phig_query_add_tag`.
   Unverified against the ABI.

## Cross-references

- `plans/049-photos-import.md` — creates the app target and interop layer, and
  supplies the collection.
- `plans/047-gui-functional-design.md` — the full screen inventory; this is its
  first two screens for macOS.
- `plans/037-thumbnail-sizing-zoom.md` — zoom policy, HiDPI sizing, and the two
  measurement bugs to avoid repeating.
- `plans/035-gtk-m0.md` — the equivalent milestone on GTK, and the source of the
  loader discipline.
- `plans/031-libphig-operations.md` / `plans/034-libphig-abi.md` — the ABI this
  client reads through.
