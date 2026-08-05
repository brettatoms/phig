# phig-gtk — Design

Status: draft
Created: 2026-07-31

## What this is

A GTK4/libadwaita desktop application for browsing and managing a
[phig](../../../../phig) image collection. It presents phig's functionality —
gallery browsing, search, tags/galleries/favorites, faces, duplicates,
scan/organize/remove — as a native GNOME app.

Written in C++ against the raw GTK4 and libadwaita C APIs. Talks to phig
through a new `libphig` C ABI for queries and metadata, a dedicated
`phig-decode` worker pool for all pixel decoding, and the existing phig CLI for
long-running mutating jobs.

## Requirements

1. **Feels like a natural GNOME app.** Current libadwaita widgets and patterns —
   adaptive layout, navigation stack, toasts, banners.
2. **Performance is a primary requirement**, specifically: time to first window,
   gallery scrolling on large collections, and opening an individual image.
3. **Covers most phig tasks in the UI**, not only browsing.
4. **A malformed image never crashes the app.** phig's decode path (OpenCV,
   FFmpeg, LibRaw) can segfault on user files; undecodable Canon CR2 files are
   known to exist in real collections.

## Scope

The **design** covers phig's full task surface so the architecture does not need
retrofitting. **Implementation is phased** (see "Phasing"); early phases are
read-only and the app relies on the CLI to populate the collection.

Non-goals: multi-collection support, remote/networked databases, editing image
pixels, replacing the CLI.

## Interface discipline

phig-gtk talks to phig **only** through defined interfaces: the `libphig` C ABI,
the `phig-decode` protocol, and the CLI's structured output. It must **not** read
phig's SQLite schema, table or column names, or the thumbnail cache file layout
directly. Those are phig's private implementation.

This is the same rule phig-web adopted, and it is why `libphig` exposes a C ABI
even though phig-gtk is C++ and could link phig's C++ core directly: the C ABI is
the stable boundary phig-web needs (Elixir cannot consume C++), and requiring it
forces the interface to be a real contract rather than whatever headers happen to
be public. phig-gtk is the first consumer that proves the ABI is usable.

When phig-gtk needs a capability the interface does not expose, that capability
is added to *phig's* interface and tracked in phig's `plans/`.

## Architecture

Three processes:

```
┌──────────────────────────────────────┐
│  phig-gtk  (C++, GTK4/libadwaita)    │  GUI thread touches widgets only
│  ┌────────────────────────────────┐  │
│  │  libphig  (C ABI, in-process)  │  │  queries, metadata, thumb lookup,
│  └────────────────────────────────┘  │  small metadata writes
└──────┬──────────────────────┬────────┘
       │ decode requests      │ job control
       ▼                      ▼
┌──────────────────┐   ┌────────────────────┐
│  phig-decode     │   │  phig CLI (child)  │
│  worker pool     │   │  scan/mv/rm/faces  │
│  memfd transport │   │  --porcelain       │
└──────────────────┘   └────────────────────┘
```

**In-process (libphig).** The read-only and small-write surface: paginated
queries, metadata, tag/gallery/favorite reads and writes, thumbnail path
resolution, activity-log reads. These are SQLite operations that do not decode
pixels and so do not crash. The GUI's query context opens the database
**read-only**; phig runs SQLite in WAL mode, so reads never block against a
concurrently running job.

**phig-decode worker pool.** Every pixel decode: thumbnail generation on cache
miss and full-image decode for the viewer. A crashing worker loses one request,
not the app.

**phig CLI as job child.** Long-running mutating operations. The CLI already
implements the process-wide advisory writer lock, crash isolation, and the
activity log correctly; spawning it reuses all three rather than reimplementing
them.

### Why the job path is out-of-process

Long-running operations must survive a segfault *and* respect phig's advisory
writer lock. In-process threads satisfy neither. Designing the job interface
around "separate process, streamed progress, per-file failure" is the only shape
that does — and the CLI already provides that shape, which is why the first
implementation can be cheap.

## libphig C ABI

Design rules: opaque handles; no C++ types across the boundary; caller frees
phig-allocated memory only through `phig_*_free`; fallible calls return a status
code with a retrievable error string; no global state beyond explicit init.

### Lifecycle

```c
phig_status phig_init(const phig_config *cfg, phig_ctx **out);
void        phig_shutdown(phig_ctx *ctx);
const char *phig_last_error(phig_ctx *ctx);
uint32_t    phig_abi_version(void);
```

`phig_config` carries the database path, cache directories, and an access mode
(`PHIG_READ_ONLY` / `PHIG_READ_WRITE`).

`phig_abi_version()` returns a compile-time constant the client checks at init.
Additive changes bump minor; struct layout changes bump major.

### Query

A builder, so adding a filter later is an added function rather than an ABI
break.

```c
phig_status phig_query_new(phig_ctx*, phig_query **out);
void phig_query_set_text(phig_query*, const char *substr);
/* Dates are ISO-8601 strings ("YYYY-MM-DD"), matching phig's storage and CLI.
   NULL or "" means unbounded on that end. */
void phig_query_set_date_range(phig_query*, const char *after, const char *before);
void phig_query_set_camera(phig_query*, const char *model);
void phig_query_add_tag(phig_query*, const char *tag);
void phig_query_set_gallery(phig_query*, const char *gallery);
void phig_query_set_favorite(phig_query*, bool);
void phig_query_set_person(phig_query*, const char *name);
void phig_query_set_media(phig_query*, phig_media_type);
void phig_query_set_size_range(phig_query*, int64_t min_bytes, int64_t max_bytes);
void phig_query_set_sharpness(phig_query*, double min_score);
void phig_query_add_match(phig_query*, const char *glob);
void phig_query_add_exclude(phig_query*, const char *glob);
void phig_query_set_sort(phig_query*, phig_sort_key, bool descending);
void phig_query_free(phig_query*);

phig_status phig_query_count(phig_ctx*, const phig_query*, int64_t *out);
phig_status phig_query_page(phig_ctx*, const phig_query*,
                            int64_t offset, int32_t limit,
                            phig_image_list **out);
```

`phig_query_count` is separate because the grid must size its scrollbar before
any row has been fetched.

Sorting **must** include `id` as a final tiebreaker so that pages do not shuffle
when timestamps are equal. Pagination is pushed down into SQL (`LIMIT`/`OFFSET`);
it is not implemented by fetching everything and slicing.

### Records

```c
typedef struct {
    int64_t id;
    const char *path, *filename, *sha256;
    int64_t file_size;
    /* ISO-8601 strings, as phig stores them. `effective_date` is phig's
       "best available date": EXIF DateTimeOriginal, then DateTimeDigitized,
       then modified_at. It is what the gallery sorts and groups by. */
    const char *created_at, *modified_at, *effective_date;
    int32_t width, height;
    phig_media_type media_type;
    int64_t duration_ms;
    const char *gallery;
    double blur_score;
    bool favorite;
} phig_image;

typedef struct { const phig_image *items; int64_t count; } phig_image_list;
void phig_image_list_free(phig_image_list*);
```

Strings are owned by the list and valid until it is freed.

EXIF/metadata JSON is deliberately **not** in this struct — it is a large TEXT
blob, and fetching it for every grid cell would consume most of the query
budget. It is retrieved on demand:

```c
phig_status phig_image_metadata(phig_ctx*, int64_t id, char **json_out);
void        phig_string_free(char*);
```

### Thumbnails

```c
phig_status phig_thumb_lookup(phig_ctx*, const char *sha256,
                              char *out_path, size_t out_len, bool *exists);
```

Returns a **path**, not bytes: the cache is already files on disk, and
`GdkTexture` loads a file without a copy through the ABI. On `exists == false`
the client queues a decode request. This call is what lets a client avoid
knowing the cache layout.

### Mutations (small, in-process)

By image id, since the GUI always has a concrete selection:

```c
phig_status phig_tag_add(phig_ctx*, const int64_t *ids, size_t n, const char *tag);
phig_status phig_tag_remove(phig_ctx*, const int64_t *ids, size_t n, const char *tag);
phig_status phig_gallery_set(phig_ctx*, const int64_t *ids, size_t n, const char *gallery);
phig_status phig_gallery_clear(phig_ctx*, const int64_t *ids, size_t n);
phig_status phig_favorite_set(phig_ctx*, const int64_t *ids, size_t n, bool on);
```

These require a `PHIG_READ_WRITE` context and take phig's advisory writer lock
for the duration, which is sub-millisecond for a metadata write. This is the only
place the GUI holds a write lock itself; everything heavier goes through a job.

### Additional read surfaces

Same paginated shape: `phig_duplicates_page` (exact and near groups),
`phig_person_list`, `phig_tag_list`, `phig_gallery_list`, `phig_log_page`
(activity log).

### Required phig-side changes

- **Stored, indexed `effective_date` column on `images`.** phig's date semantics
  are not the `created_at` column: `Database::search()` filters on a COALESCE
  over `json_extract(metadata,'$.DateTimeOriginal')`, then `DateTimeDigitized`,
  then `modified_at`. No plain-column index can serve an `ORDER BY` on that
  expression, so a gallery sorted by date would full-scan, JSON-parse, and sort
  every row on every page fetch — the single largest threat to gallery load
  performance.

  Fix: add `effective_date TEXT` to `images`, populated at scan time and kept
  current on relink/metadata update, with `CREATE INDEX idx_effective_date ON
  images(effective_date DESC, id DESC)`. Existing databases get a schema
  migration that backfills it from the same COALESCE. The CLI's `--after` /
  `--before` filters should then use the column too, so CLI and GUI agree on
  what a photo's date is.

  Dates stay ISO-8601 TEXT throughout, which sorts lexicographically and needs
  no conversion layer in the ABI. Rows with no date sort last under `DESC`;
  `id DESC` is the tiebreaker so pages never shuffle.
- **Pagination pushed into the query layer.** `Database::search()` currently
  returns a full `std::vector<ImageInfo>` with no limit or offset.
- **Refactor criteria-to-SQL** so the CLI and the ABI share one implementation.
- **`libphig` shared-library target** in CMake, with an installed public header.
- **Machine-readable job progress** — see "Jobs".

## phig-decode worker pool

### Transport

One `socketpair(AF_UNIX, SOCK_SEQPACKET)` per worker: messages are framed, so
there is no partial-read parsing. Fixed-size binary headers — this is the hot
path for image viewing, so no JSON.

```c
struct decode_req  { uint64_t seq; uint8_t kind; uint32_t max_dim; char path[]; };
struct decode_resp { uint64_t seq; uint8_t status; int32_t w, h, stride;
                     uint8_t fmt; /* memfd passed via SCM_RIGHTS */ };
```

`kind` is `THUMB` (generate into the cache, reply with the path) or `FULL`
(decode at up to `max_dim`, reply with pixels).

For `FULL`, the worker allocates a region with `memfd_create`, writes decoded
RGB into it, seals it, and passes the fd over the socket with `SCM_RIGHTS`. The
GUI `mmap`s it, wraps it with `gdk_memory_texture_new`, and closes the fd; the
kernel frees the pages when the texture is dropped. No copy and no serialization
of pixel data.

`max_dim` is load-bearing for viewer performance: decoding a 24MP RAW at full
resolution to display in a 2560px window is wasted work. The GUI requests the
size it needs, letting the worker use LibRaw's half-size or embedded-preview
paths when the request is small enough.

### Crash handling

A supervisor on a dedicated GUI-process thread tracks the in-flight `seq` per
worker. On worker death (socket EOF / `SIGCHLD`), that `seq` completes with
`status = CRASHED`, the worker is reaped and respawned, and the queue continues.

- The affected cell shows a broken-image placeholder.
- Failures are **counted and surfaced** ("N files couldn't be decoded") rather
  than silently blank, so undecodable files are discoverable.
- A path that crashes a worker repeatedly is blacklisted for the session so it
  cannot spin.

### Scheduling

Two priority classes. `FULL` for the currently-viewed image preempts everything;
`THUMB` for off-screen cells is cancellable and dropped when the user scrolls
past. Cancellation is advisory — an already-dispatched request runs to completion
and its result is discarded, which is acceptable because thumbnail work is short.
The queue is bounded; fast scrolling discards stale requests rather than queuing
them.

### Why a separate binary rather than forking the GUI

Forking a GTK process copies the entire GTK/GDK/GL state into each worker —
wasteful, and hazardous around GL contexts and threads. A separate `phig-decode`
executable linking only OpenCV, FFmpeg, and LibRaw starts clean with a much
smaller RSS, and can be tested standalone.

Pool size defaults to core count, capped. Workers spawn **lazily** on first
request, not at startup (see "First-start latency").

## GUI structure

### Shell

`AdwApplicationWindow` → `AdwNavigationSplitView`.

- **Sidebar:** collection/filter list — All Photos, Favorites, Galleries,
  People, Tags, Duplicates, Activity.
- **Content:** `AdwToolbarView` with `AdwHeaderBar` over the gallery grid.
- **Adaptive:** `AdwBreakpoint` collapses the split view at narrow widths.
- **Viewer:** a separate `AdwNavigationPage` pushed onto the navigation stack —
  not a dialog — which provides back-gesture navigation.
- **Job status:** `AdwBanner` plus a progress popover in the header bar.
- **Transient errors:** `AdwToast`.

### Virtualized gallery grid

`GtkGridView` with a `GtkListItemFactory` over a **custom `GListModel`** that
does not hold the collection. It holds the total from `phig_query_count` plus an
LRU window of fetched pages. `get_item(n)` returns a lightweight row object; if
the containing page is not resident it returns a placeholder and schedules a
fetch. Page size ~200 rows, with a few pages resident either side of the
viewport.

`get_item` must never block.

Thumbnails follow the same discipline: on bind, a cell calls
`phig_thumb_lookup`; on a hit it loads the file into a `GdkTexture` off the main
thread; on a miss it enqueues a low-priority decode request. A bounded texture
cache (LRU, **sized in bytes**, not entries) keeps recently-seen thumbnails hot.
Cells cancel pending work on unbind.

### Threading rule

Stated once because every bug in this class comes from breaking it:

- GTK calls happen **only** on the main thread.
- libphig queries run on a worker thread; results post back via `g_idle_add`.
- The decode supervisor thread never touches a widget.
- Nothing else is concurrent.

### First-start latency

The window must appear before any query completes. Order:

1. Create and present the window with an empty grid (skeleton cells).
2. `phig_init` on a worker thread — a SQLite open, cheap.
3. `phig_query_count`.
4. First page.

Nothing eager runs before the window maps: no counting the whole collection, no
cache warming, no spawning decode workers.

### Viewer path

Opening an image displays the already-loaded thumbnail scaled up immediately,
then swaps in the full decode when it arrives — the user never sees an empty
frame. The next and previous images are prefetched at viewer size so arrow-key
navigation is instant.

## Jobs

One GUI-side interface covers every long-running operation:

```cpp
class Job {
  virtual void start(const Args&) = 0;
  virtual void cancel() = 0;
  sigc::signal<void(Progress)> on_progress;  // current, total, message
  sigc::signal<void(Result)>   on_finish;    // ok | cancelled | failed(err)
};
```

**v1 implementation — `CliJob`:** spawn `phig <cmd> --porcelain`, read stdout on
a worker thread, parse progress, `SIGTERM` on cancel, map exit status to
`Result`. The GUI knows only `Job`, so replacing the child with a purpose-built
worker later touches one class and no UI code.

Covered operations: scan, thumbs rebuild/clean, face detect/identify/rebuild,
organize (mv/cp), rm/trash, models download.

Because the CLI takes the advisory writer lock, jobs cannot interleave. The GUI
**enforces one active job at a time** and disables the corresponding actions
while one runs, rather than letting a second job block on the lock invisibly.

### Required phig-side change

The CLI's progress output is not machine-readable for all commands —
`--porcelain` covers `search` and the `list` subcommands, not `scan` progress.
Long-running commands need a stable progress line on stdout under `--porcelain`,
e.g. `PROGRESS <current> <total> <path>`.

### Destructive operations

`mv`, `cp`, and `rm` always run `--dry-run` first, present the planned changes as
a reviewable list, and execute only on confirmation. This mirrors safe CLI usage
and is what makes a GUI over destructive operations trustworthy.

The **Activity** view is the counterpart: it reads phig's existing operations log
so a completed job can be inspected and rolled back.

## Error handling

Three classes, deliberately separated because they warrant different UX:

| Class | Handling |
|---|---|
| Per-file decode failure | Placeholder in the grid; counted; surfaced in an "N files couldn't be decoded" detail view. Never a modal. |
| Query / ABI failure | `phig_last_error` string in an `AdwToast`; the grid keeps its last good state. |
| Job failure | Banner with exit status and stderr tail, kept until dismissed. |

## Testing

| Layer | Approach |
|---|---|
| libphig ABI | C-level unit tests against a fixture database: pagination boundaries, stable sort under equal timestamps, empty results, filter combinations. These catch page-shuffling. |
| phig-decode | Standalone binary, so tests feed it fixtures including deliberately corrupt CR2/JPEG files and assert the supervisor reports `CRASHED` and respawns. This is the "don't crash the app" guarantee and needs a real test. |
| Virtualized model | Unit-testable without realized GTK: fetch scheduling, LRU eviction, `get_item` never blocking. |
| GUI | Smoke level only — launch headless under `xvfb`, assert the window maps and the first page renders. No broad widget automation. |

libphig and phig-decode get genuine real-behavior coverage rather than mocks.

## Phasing

The design covers all of the above; implementation lands in this order.

1. **libphig core** — CMake target, ABI header, query builder with pagination,
   `created_at` index, thumbnail lookup, metadata call. Tests.
2. **phig-decode + supervisor** — standalone binary, memfd transport,
   crash/respawn. Tests.
3. **Read-only GUI** — shell, virtualized grid, sidebar filters, viewer, detail
   pane.
4. **Light mutation** — tags, galleries, favorites via the ABI.
5. **Jobs** — `CliJob`, progress plumbing, dry-run review; scan/thumbs/faces
   first.
6. **Destructive operations and activity log** — organize, rm/trash, rollback.
7. **Duplicates review** — side-by-side comparison workflow; effectively its own
   screen, so last.

**Phase 3 is the checkpoint.** It is the first point where the performance
requirement is verifiable end to end on a real collection. If the grid does not
scroll smoothly, the fix belongs in libphig or phig-decode — better learned
before jobs and destructive operations are built on top.

## Build

CMake with pkg-config, matching phig's existing build shape. Dependencies:
GTK4, libadwaita, and phig's `libphig`. `phig-decode` links OpenCV, FFmpeg, and
LibRaw but not GTK.

## Cross-references

- phig plan 006 (API Layer, `deferred`) — `libphig` supersedes its HTTP/JSON
  premise for local clients.
- phig plan 007 (TUI / GUI, `planned`) — this design is its GUI half.
- phig-web `docs/000-architecture.md` — the interface-discipline rule and the
  crash-isolation architecture originate there; `libphig` and `phig-decode` are
  the shared foundation.
