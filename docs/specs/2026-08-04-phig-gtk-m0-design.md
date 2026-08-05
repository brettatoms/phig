# phig-gtk M0 — Design

**Status:** approved, ready to plan
**Date:** 2026-08-04
**Supersedes for M0:** the phase-2/phase-3 ordering in `2026-07-31-phig-gtk-design.md`.
That document remains the reference for everything beyond M0 — its architecture, threading
rule, and grid design are unchanged here.

## Why this document exists

The 2026-07-31 design predates libphig's settings API, phig-core's five engines, and the
41-symbol ABI. Reviewing it against what shipped found four drifts, three of which change
what to build first.

### What the older design got right and this keeps

The component split, the "GUI calls happen only on the main thread" rule, the custom
`GListModel` that holds pages rather than the collection, `get_item` never blocking,
byte-sized texture caching, cancel-on-unbind, and non-modal error handling. None of that
needed revision.

### What has drifted

**1. Its "Required phig-side changes" list is done except one item.**

| Required by the old design | Status 2026-08-04 |
|---|---|
| Stored, indexed `effective_date` on `images` | shipped (`idx_effective_date`) |
| Pagination in the query layer | shipped (`search_page`, `count_matching`) |
| Shared criteria-to-SQL between CLI and ABI | shipped |
| `libphig` shared-library target + installed header | shipped (ABI 1.1, 41 symbols) |
| Machine-readable job progress | **not done** — `--porcelain` covers `search` and the `list` subcommands, not `scan`. Only blocks the Jobs phase, not M0. |

**2. Phase 1 (libphig core) is complete.** The ABI already exposes everything a read-only
gallery needs: `phig_init`, `phig_shutdown`, `phig_last_error`, `phig_query_new`,
`phig_query_count`, `phig_query_page`, `phig_image_metadata`, `phig_thumb_lookup`, 11
filter setters, and `phig_query_set_sort`.

**3. Ten functions the old design specified do not exist**, but every one belongs to a
later phase, not to M0: `phig_query_add_match`, `phig_query_add_exclude`,
`phig_query_set_text`, `phig_query_set_favorite`, `phig_query_set_person`,
`phig_tag_list`, `phig_gallery_list`, `phig_person_list`, `phig_log_page`,
`phig_duplicates_page`.

**4. Phase 2 (`phig-decode`) is deferred, and its ordering inverted.** Crash isolation was
its primary justification; measured 2026-08-04, zero crashes in 12,684 probes including
all 177 real CR2 files through the unprotected pre-fallback path. See
`2026-08-04-decode-crash-investigation.md`. `max_dim`-aware decoding remains a real
requirement, but it is a request parameter rather than a reason for a process boundary —
and since M0 has no full-image viewer, M0 exercises no `max_dim` path and would prove
nothing about it either way.

### One blocker the old design does not mention

`gtk4` and `libadwaita-1` are **not in phig's devenv** (`phig/devenv.nix` provides cmake,
gcc, opencv4, libexif, sqlite, pkg-config, gtest, curl, ffmpeg-headless, spdlog, libraw).
Nothing GUI compiles until an environment provides them. `phig-gtk/` is also an empty
directory outside the phig git repo, so it needs its own repo, devenv, and build.

## Measurements this design rests on

Taken 2026-08-04 against the real default database (`~/.local/share/phig/phig.db`,
**29,953 images**, all hashed), through the *installed* ABI from a foreign C client that
included only `<phig/phig.h>`:

| call | warm |
|---|---|
| `phig_init` | **0.4 ms** |
| `phig_query_count` (29,953 rows) | **1.3 ms** |
| `phig_query_page` offset 0, limit 200 | **0.85 ms** |
| `phig_query_page` offset 29,000, limit 200 | **2.6 ms** |

Deep paging stays flat, so `idx_effective_date` is doing its job. A cold first page cost
19 ms (page cache), which is the number that matters for startup budget.

**Consequence for the design: the ABI is not the bottleneck.** A 200-row page at under
3 ms is roughly three orders of magnitude inside a 16 ms frame budget. Scroll performance
will be decided by thumbnail file loading and texture upload, so that is what M0 must
measure and what the resident-page window can afford to be generous about.

Thumbnail cache state at design time: **4,097 of 29,953** cached (~14%), 165 MB. The grid
will therefore be mostly placeholders on a first run, which stresses the page LRU and
cancel-on-unbind harder than a warm cache. `phig thumbs rebuild` converts it to the
warm-cache case when that measurement is wanted.

## Where functionality lives

**Non-GUI functionality belongs in libphig, not in phig-gtk.** The older design states the
negative rule — phig-gtk must not read phig's schema, table names, or cache layout. This is
the positive one, and it governs every task: anything that manipulates images or the
database, or that a non-GUI client would also need, is implemented in phig and exposed
through the C ABI. phig-gtk implements presentation and interaction only.

Concretely, phig-gtk may contain: widget construction, list-model paging and eviction,
texture caching, cancellation on unbind, rendering, and input handling. It must not contain:
image decoding, thumbnail generation, EXIF or orientation logic, phash or hashing, SQL,
date normalisation, path or selection semantics, or anything else that duplicates phig-core.

When a task needs a capability the ABI lacks, **add it to libphig** rather than
reimplementing it GUI-side. That is a phig-side task with its own tests, and it is the
correct cost — `phig-web` and any future client inherit the capability for free, and the
behaviour stays identical across clients because there is one implementation.

Two consequences already visible:

- **Thumbnail generation on a cache miss** needs `phig_thumb_generate`. Note the code is
  *already inside* `libphig.so`: `generate_thumb` is compiled in (via `phig-core`) but is a
  local symbol, and `libphig.so` already links 9 OpenCV libraries. So this is a one-function
  export, not a new dependency.
- **EXIF orientation** is already handled correctly and must stay in phig. `cv::imread`
  applies EXIF orientation by default (OpenCV ≥ 3.4) and phig never passes
  `IMREAD_IGNORE_ORIENTATION`, so cached thumbnails are written already-rotated. Verified by
  comparing a `Right-top` image's thumbnail against both renderings: RMSE 0.017 against the
  auto-oriented source versus 0.378 against the raw one. The GUI therefore needs no
  orientation code, and must not add any — rotating at display time would double-apply it.

## Scope

**M0 is one window: a virtualized grid of thumbnails over the real collection, where
clicking an image opens the system image viewer.**

In scope:

- `phig-gtk/` as its own git repo with its own devenv providing gtk4 and libadwaita-1
- Consumption of libphig through `pkg-config`, which requires adding `phig.pc` to phig
- `AdwApplicationWindow` containing a `GtkGridView`
- `PhigGalleryModel` — a custom `GListModel` holding a page LRU, not the collection
- `ThumbLoader` — an async request queue for thumbnail **file loads**
- A texture cache bounded in bytes
- `gtk_file_launcher_launch` on cell activation

Out of scope, each with its reason:

| Deferred | Why |
|---|---|
| Sidebar filters | The ABI's 11 filter setters are ready; the UI is additive |
| An in-app image viewer | The system viewer replaces it. Revisit only if inadequate |
| Detail pane | `phig_image_metadata` exists; additive |
| Any decoding inside the GUI | A cache miss shows a placeholder. No decode entry point is reachable through the ABI |
| `phig-decode` | No crash justification (measured), and M0 exercises no `max_dim` path |
| Mutations, jobs, destructive ops, duplicates review | Later phases; several need ABI functions that do not exist yet |

### Success criterion

A measurement, not a feature list: **the window appears in under 500 ms, and scrolling the
full 29,953-row grid holds 60 fps with no stalls.** The old design calls this phase "the
checkpoint" — the first point where the performance requirement is verifiable end to end
on a real collection. That is M0's purpose.

## Architecture

```
┌─────────────────────────────────────────────┐
│ main thread (GTK only)                      │
│   AdwApplicationWindow → GtkGridView        │
│         │ binds cells                       │
│         ▼                                   │
│   PhigGalleryModel (custom GListModel)      │
│     - total from phig_query_count           │
│     - LRU of fetched pages (~200 rows each) │
│     - get_item never blocks                 │
└───────┬─────────────────────┬───────────────┘
        │ page fetch          │ enqueue(sha256)
        ▼                     ▼
┌───────────────┐   ┌─────────────────────────┐
│ query worker  │   │ ThumbLoader             │
│ 1 thread      │   │ N threads + bounded q   │
│ libphig calls │   │ file → GdkTexture       │
└───────┬───────┘   └──────────┬──────────────┘
        │ g_idle_add           │ g_idle_add
        └──────────┬───────────┘
                   ▼
            main thread applies
```

### `PhigGalleryModel`

Owns the total and an LRU of fetched pages. `get_item(n)` returns a lightweight row object
immediately; when the containing page is not resident it returns a placeholder row and
schedules a fetch. It never blocks and never calls libphig on the main thread.

Page size 200 rows. Because a page costs under 3 ms, eviction can be aggressive and
refetching is cheap.

**Three values are deliberately left to measurement, not chosen here:** how many pages stay
resident either side of the viewport, how many `ThumbLoader` threads to run, and the texture
cache's byte budget. Picking them by reasoning would violate the project's own rule that
performance claims are measured. The implementation should start with defensible starting
points — 2 pages either side, 4 loader threads, 256 MB — and the plan must include a step
that measures scroll behaviour on the real 29,953-image collection and records the values
that actually hold 60 fps. If the starting points turn out fine, record that; the point is
that the number in the code is one somebody watched.

### `ThumbLoader` — the seam

This is the component that makes the later `phig-decode` question a backend swap rather
than a rewrite. Its entire public surface:

```c
/* Request a thumbnail texture for `sha256`.
 *
 * Asynchronous and cancellable: the callback runs on the main thread via
 * g_idle_add, or never runs if cancelled before dispatch. `max_dim` is the
 * largest edge the caller can use.
 *
 * M0's backend resolves the path with phig_thumb_lookup and loads the file on a
 * worker thread. A cache miss replies THUMB_MISS; M0 never decodes. A later
 * backend may generate the thumbnail in-process or hand the request to a
 * phig-decode subprocess without changing this signature or any call site.
 */
void thumb_loader_request(ThumbLoader*, const char* sha256, guint32 max_dim,
                          GCancellable*, ThumbCallback, gpointer user_data);
```

`max_dim` is in the signature from day one even though M0 always passes the cache
thumbnail size. It is the one parameter whose absence would force a signature change
later, and it costs nothing now.

Two rules make the eventual swap cheap, and both are structurally enforced rather than
merely documented:

- **No decode function is reachable from the GUI.** Note the precise mechanism:
  `libphig.so` *does* link OpenCV and LibRaw transitively, because it embeds `phig-core`.
  But `phig_abi` is built with `C_VISIBILITY_PRESET hidden` plus a linker version script,
  and the public header declares **zero** decode entry points — `phig_thumb_lookup`
  returns a path, not pixels. So the GUI cannot call a decoder even accidentally, and the
  guarantee rests on symbol visibility rather than on discipline.
- **No `cv::Mat` reaches GUI code.** It cannot cross a process boundary, so anything
  holding one becomes a rewrite when the backend changes. `ThumbLoader`'s callback
  delivers a `GdkTexture`, which is the GUI's own type.

### Texture cache

LRU bounded in **bytes**, not entries: a 512 px JPEG decodes to roughly 1 MB of texture,
so an entry count lets memory wander by an order of magnitude depending on thumbnail size.
Cells drop their reference on unbind; the cache holds everything else.

## Data flow

### Startup

The window must map before any query completes:

1. Present the window with an empty grid of skeleton cells.
2. `phig_init` on the query worker (0.4 ms measured).
3. `phig_query_count` sets the model's total (1.3 ms measured).
4. Fetch the first page (0.85 ms warm, 19 ms cold).

Nothing eager runs before the window maps: no counting on the main thread, no cache
warming, no prefetch beyond the resident window.

### Scroll

`GtkGridView` binds a cell → `get_item(n)` → page resident ? return row : return
placeholder and schedule a fetch → the cell calls `thumb_loader_request` → a hit loads the
file off-thread and posts a `GdkTexture` back through `g_idle_add`; a miss posts
`THUMB_MISS` and the cell keeps its placeholder. On unbind the cell cancels its pending
request.

### Activation

Cell activation calls `gtk_file_launcher_launch` (GTK 4.10+) on the image's path, which
routes through the desktop portal and so works under Flatpak. Verified available on this
machine: `xdg-open` and `gio` present, `org.freedesktop.impl.portal.desktop.gnome` running
on the session bus.

## Error handling

Non-modal throughout. A modal dialog in a gallery is a bug, not a safety measure.

| Failure | Behaviour |
|---|---|
| `phig_init` fails (missing or locked DB, schema newer than the ABI) | An error page inside the window carrying `phig_last_error` text. The app stays open |
| Page fetch fails | Affected cells keep placeholders; one toast, not one per row |
| Thumbnail cache miss | Grey placeholder. Expected, not an error — 86% of the collection on a first run |
| Thumbnail file unreadable or corrupt | Broken-image placeholder, counted |
| `gtk_file_launcher_launch` fails (no registered handler) | `AdwToast` |

Counted failures matter because a silently blank grid is indistinguishable from a bug.

## Testing

The two components holding the logic are headless-testable, which is deliberate — the same
split that makes phig-core's engines testable while `main.cpp`'s rendering is not.

**`PhigGalleryModel`, headless, against a real database fixture.** Its logic is page
arithmetic, LRU eviction, and the placeholder-versus-row decision; none of it needs a
display.

- `get_item` returns without blocking for a non-resident page
- out-of-range indices return null
- eviction keeps the resident page set bounded while scrolling the full range
- a failed page fetch leaves placeholders rather than gaps or a shifted model

**`ThumbLoader`, headless, against real files on disk.**

- a cache hit yields a texture
- a cache miss yields `THUMB_MISS` and never decodes
- cancelling before dispatch never invokes the callback
- cancelling after dispatch discards the result rather than delivering it late
- the texture cache respects its byte budget under a load that exceeds it

**Startup latency, measured, asserted.** A test that fails when window-mapped-to-first-page
exceeds the budget on the real collection, so a regression fails rather than merely feeling
slow.

**Not unit tested:** widget rendering and scroll smoothness. Those are verified by running
against the real collection and watching. A CI assertion of "60 fps" would measure the CI
machine, not the code.

**One real-run check is mandatory, because the unit suite provably cannot see it.** GTK
reports contract violations as runtime warnings that no headless test detects. This bit
hard: a shared-placeholder-object bug made `GtkGridView` render **every cell empty** while
all 6 model tests passed green, and it was a human looking at the window who caught it.
So any change touching the model or the grid must run:

```bash
timeout 12 direnv exec . ./build/phig-gtk ~/.local/share/phig/phig.db 2>&1 \
  | grep -cE "Gtk-WARNING|Gtk-CRITICAL|GLib-GObject"
```

That count must be **0**. Treat a GTK warning as a failure rather than noise — this one
was 223 warnings and a blank grid that the test suite happily called working.

## Build

`phig-gtk/` is its own git repo with its own `devenv.nix` providing `gtk4` and
`libadwaita-1` (and cmake, gcc, pkg-config, gtest). CMake with `pkg-config`, matching
phig's build shape:

```cmake
pkg_check_modules(PHIG REQUIRED phig)
pkg_check_modules(GTK REQUIRED gtk4 libadwaita-1)
```

### The one change to phig

libphig installs `libphig.so.1.1.0` and `include/phig/phig.h` but ships no `.pc` file and
no CMake package config, so a consumer has nothing to discover it with. Verified by
building a foreign C client, which required hand-passed `-I` and `-L`.

Add a generated `phig.pc` to phig's CMake, installed to `lib/pkgconfig`. This is the only
phig-side change M0 requires. It also makes libphig discoverable for phig-web later, which
a CMake-only package config would not.

**The repo boundary is the point.** phig-gtk consuming installed libphig through
`pkg-config` makes it impossible to reach past the ABI into `phig-core` headers, which is
the interface discipline the workspace `CLAUDE.md` requires of every client. A same-repo
target would make that violation merely discouraged.

## What M0 deliberately does not answer

- **Whether `phig-decode` is ever needed.** With no full-image viewer there is no
  `max_dim` path, so M0 produces no evidence either way. That question reopens when a
  viewer exists, or stays closed if the system viewer proves sufficient.
- **Whether in-process thumbnail generation is acceptable.** M0 never generates one. The
  cache-miss backend is where that decision lands, behind an interface that already exists.
- **Whether the system viewer is good enough.** M0 is how we find out.
