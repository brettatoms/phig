---
name: phig Server — Protocol & Daemon
status: designing
created: 2026-08-08
description: >
  `phigd`, a headless daemon that serves a phig collection over HTTP so
  clients can reach it remotely: paged queries, per-image metadata, duplicates,
  people and faces, tag/gallery/favorite writes, the three long-running
  operations with SSE progress, and media — thumbnail bytes, originals, and
  byte-range video. A peer of `cli/` over `core/`, not a client of `libphig`,
  which is what puts duplicates and faces on the protocol at all. Clients gain
  a second interface: `libphig` when the collection is local, this protocol
  when it is remote. Single identity, bearer token, behind a reverse proxy.
  First of six sub-projects; the remaining five are listed below and get their
  own numbers when designed.
---

# 052 — phig Server: Protocol & Daemon

## Goal

Run phig on the machine that holds the collection, and reach it from
everywhere else. Two outcomes drive this:

- **Compute offload.** Indexing, thumbnailing and face detection run on the
  box with the storage, continuously and headlessly, instead of over a network
  mount from a laptop.
- **Remote access.** A browser, and eventually the native clients, work
  against a collection they do not have mounted.

## Decomposition

This plan is **S1** of six. It was split this way because the pieces have
different dependencies and different risk, and a single spec covering all of
them would not be executable.

| | Sub-project | Depends on |
|---|---|---|
| **S1** | **Protocol + `phigd`** — this plan | — |
| S1b | Import: upload from a client, organize server-side | S1, plan 042, plan 021 |
| S2 | Native clients speak the protocol | S1 |
| S3 | Web client (server-rendered UI) | S1 |
| S4 | Accounts and multi-user | S1 |
| S5 | Share a single gallery by link | S4 |

S2 and S3 are parallel after S1. S4 is the only piece that touches phig's
schema: tags, favorites and galleries are global today, so per-user metadata
is a migration the CLI and the local clients also have to live with. The
cheaper alternative — one shared set of metadata, per-user *access* only —
should be evaluated before that migration is written.

Compute offload is not on the list because it is not a separate project: it
is what S1 delivers the moment the daemon can run index, thumbs and faces on
the collection's host.

## Design decisions

### `phigd` is a peer of `cli/` over `core/`, not a client of `libphig`

`phigd` is phig running headless, not a foreign caller. It links `phig-core`
and includes `core/` headers directly, exactly as `cli/` does.

The alternative — building the daemon on the C ABI — was rejected on
coverage. `libphig` has no duplicates, no people or faces, no organize, no
remove and no reconcile, so a server built on it could not serve two of
047's nine screens, and every gap would become an ABI plan before the server
could move.

**Consequence: clients now have two interfaces, not one.**

| Collection is | Client uses |
|---|---|
| Local | `libphig` C ABI |
| Remote | phig wire protocol (this plan) |

Both cover the same capabilities. That is deliberate — S2's job is choosing a
transport at connect time, not learning a second vocabulary. Root `CLAUDE.md`'s
Interface discipline section currently reads as though `libphig` is the only
way in and needs amending, not footnoting.

### HTTP, chosen for reach rather than speed

The media path is effectively forced: `<img>` and `<video>` are HTTP clients
that cannot be replaced, a photo grid renders hundreds of thumbnails per
screen, and range requests, conditional requests and `Cache-Control:
immutable` on content-addressed thumbnails are semantics already implemented
correctly in every proxy, browser and player. Bulk byte transfer is
throughput-bound, so a custom protocol would reimplement seeking and caching
for no gain.

The control path could have been binary, and the arithmetic says it should
not bother: HTTP/1.1 headers run a few hundred bytes against a 30 KB
thumbnail, and against a query page the cost is SQLite and serialization, not
framing. Plan 044 measured where phig's time actually goes — 111.6 MB of EXIF
loaded and discarded by `get_all_images`, `has_sibling_raw` making scan
quadratic per directory — and those are three orders of magnitude from
protocol overhead.

gRPC and Cap'n Proto were both considered. gRPC's generated clients and
bidirectional streaming are real advantages, paid for with one of the heaviest
C++ dependency trees available and a grpc-web translation layer for browsers.
Cap'n Proto's capability model is a better fit for S5's share links than any
bearer token, but its JVM and Swift bindings are community-maintained and
there is no browser story. Both cost `curl`, which for a solo maintainer
debugging a daemon is worth more than it sounds. Either could be added later
as a second control channel if measurement ever justifies it.

**HTTP/1.1 only in `phigd`.** Its one structural weakness here is request
count — a grid scroll firing hundreds of thumbnail requests hits the
six-connections-per-origin limit and head-of-line blocks. The reverse proxy
terminates HTTP/2 and solves that without `phigd` implementing it.

### Behind a reverse proxy; no TLS in the daemon

`phigd` binds loopback (or a Unix socket for same-host callers) and speaks
plain HTTP. Caddy or nginx terminates TLS and owns certificate lifecycle.
This keeps OpenSSL out of the part of the stack least suited to it, and the
bearer token still applies, so a proxy misconfiguration does not expose an
open collection.

### Operations are server-scoped resources

An operation outlives the client that started it. Starting a three-hour index
from a laptop and closing the lid must not stop it — that is the whole point
of compute offload. Any authenticated client can list, watch or cancel any
run.

Operations do **not** survive a daemon restart. The work is idempotent — a
rescan skips unchanged files — so resume is complexity bought for no safety.

Progress is delivered by SSE. `GET /operations/{id}` is not a rival polling
path but the resource itself: listing, result-fetching and SSE
reconnect-resync all need it.

### Creation splits per type; lifecycle does not

Plan 031's thesis was exposing index, thumbs and faces as *three separate
operations rather than one fused scan*, and `libphig` has three
`phig_op_*_start` functions rather than one with a type argument. A generic
`POST /operations {type}` would re-fuse them one level up, and would make the
request body a discriminated union — the shape that validation, documentation
and code generation all handle worst. Three endpoints have three flat bodies
validated on arrival, and give S4 route-level policy instead of a rule that
must read the body.

Lifecycle stays shared, because a running operation genuinely is one thing and
"what is this server doing" has to be a single endpoint. Response
polymorphism remains and is fine: the ABI already has three typed result
getters, so the envelope is uniform (state, 031's uniform progress record,
bounded error list) with a `result` member whose shape follows a `type` field
the client has already read.

### Lock-free, matching `libphig`

`phigd`'s operations take no writer lock, exactly as the ABI's do: index's
write set is unknowable before the walk, thumbs writes only the filesystem
cache, and faces commits per image. Each operation opens its own `Database`
so client queries keep working during a run.

`libphig/CLAUDE.md` already records the consequence — a lock-free operation is
not excluded by the CLI's lock. A daemon does not create that problem, it
widens the window from minutes to always. Small metadata writes fail fast with
409 and `Retry-After` when a local CLI command holds the lock, which is the
documented ABI behaviour surfaced rather than hidden.

**Index is the additive half of scan only**, matching `libphig`: it runs
`ScanEngine::run_workers`, never `run_reconcile`, so rows for deleted or moved
files survive a run and the daemon must not present it as "sync with disk".
The destructive reconcile waits on plan 041, whose decide-then-authorize split
is the shape a remote client needs anyway.

### Signed URLs for media

`<img src>` and `<video src>` cannot set an `Authorization` header, so a
bearer token covers every JSON endpoint and zero media endpoints in a browser.
Cookies would tie `phigd` to browser session semantics; proxying every byte
through the web app defeats fetching media directly.

`phigd` mints short-lived HMAC-signed media URLs for an authenticated caller.
This is worth more than the immediate fix: a signed, scoped, expiring URL is
what S5's shared gallery link needs, which makes sharing a narrowing of an
existing mechanism rather than a new one.

### Offset pagination, not cursors

The gallery grid is virtualized and jumps to arbitrary scroll positions —
built that way in the GTK client (plans 035, 037), specified for all three
clients in 047 — and cursors cannot serve a jump to an arbitrary position.
`limit` is capped by config. Plan 044 owns making offset scale.

## Architecture

```
browser ──────┐                          ┌─ phig-core ─┬─ SQLite (WAL)
              │   ┌──────────────┐       │             ├─ media files
web app (S3) ─┼──►│ reverse proxy├──────►│  phigd      └─ thumb cache
              │   │  TLS, certs  │       │  loopback
native (S2) ──┘   └──────────────┘       └─────────────
```

New top-level `server/`, producing `phigd`. Four units:

- **transport** — routing, byte ranges, SSE framing, JSON encoding. Knows
  nothing about phig; testable against a stub.
- **resources** — one file per resource (images, media, operations,
  duplicates, people, tags). The only layer that knows both HTTP and phig.
- **operations registry** — id allocation, lifecycle, cancel tokens, progress
  fan-out to SSE subscribers. Owns the fact that operations outlive their
  originating client.
- **auth** — constant-time bearer check and signed-URL minting, nothing else.

Configuration extends `core/config.cpp`'s existing INI with a `[server]`
section rather than introducing a second config format.

**Handlers never call phig directly.** `core/` is synchronous and blocking
throughout — SQLite, OpenCV decode, LibRaw. On an event loop a blocking
handler stalls every connection multiplexed onto its thread, so an unrelated
client's SSE stream stutters because someone missed the thumbnail cache. The
design is an event loop for I/O plus an explicit worker pool for phig calls.
The loop makes connections cheap; it does not make phig non-blocking.

**Portability.** `core/` plus an HTTP library is portable, so `phigd` belongs
in the Linux/macOS/Windows row alongside `core/` and `libphig/`, building in
CI on both current runners. Windows is gated by plan 039 regardless.

## Protocol surface

Base `/api/v1/`. Additive changes do not bump the version; `GET /server`
reports it.

### Read

```
GET /api/v1/images?<filters>&sort=&order=&offset=&limit=
    → { total, offset, limit, items: [...] }
GET /api/v1/images/{id}
GET /api/v1/images/{id}/metadata      raw EXIF/container JSON
```

Filters mirror `phig_query_*`: extension, date range, camera, make, size
range, media type, duration, tags (AND), gallery, sharpness, sort key and
direction. `total` exists to size a scrollbar before any page is fetched.
Metadata is a separate call because fetching a large blob per grid cell would
waste the whole query budget — the same reasoning as `phig_image_metadata`.

### Media

```
GET /api/v1/images/{id}/original      Accept-Ranges, 206, ETag = sha256
GET /api/v1/thumbs/{sha256}?size=N    JPEG bytes
```

Thumbnails are keyed on content rather than id, so two identical photos share
one cache entry and `Cache-Control: immutable` is honest. **`size` comes from
a configured allowlist**; free-form sizes let a client fill the disk. Misses
generate synchronously under a concurrency cap and return 503 with
`Retry-After` at the cap rather than queueing without bound.

Video is range-served as it sits on disk. Formats a client cannot play get a
clear "not playable here, download it" rather than a silent failure.
Transcoding is deliberately excluded — see below.

**The same problem exists for still images, and plan 048 is the harder half
of it.** A browser renders HEIC only in Safari and JXL nowhere, so serving
originals is not a display path for iPhone photos on most clients — the
thumbnail endpoint is. But 048 measured that phig decodes none of heic, heif,
avif or jxl today: `known_extensions()` advertises them, the walk finds them,
`cv::imread` rejects them and they are counted as scan errors. So there is no
thumbnail to serve either, and a web gallery of an iPhone library would render
empty cells. **048 is therefore a hard prerequisite for S3 being useful**, and
worth having before S1's media path is judged. It does not block S1 itself:
the endpoints are format-agnostic and a collection without HEIC is unaffected.

### Write

```
POST   /api/v1/tags/{tag}/images      { ids: [...] }
DELETE /api/v1/tags/{tag}/images      { ids: [...] }
PUT    /api/v1/images/{id}/gallery    { gallery: "..." }
DELETE /api/v1/images/{id}/gallery
PUT    /api/v1/images/{id}/favorite   { favorite: true }
GET    /api/v1/tags                   list with counts
GET    /api/v1/galleries              list with counts
```

Bulk-by-ids matches the ABI's all-or-none-per-call shape. The tag and gallery
lists are exposed by neither the ABI nor the CLI today and every UI needs
them.

### Duplicates, people, faces

```
GET  /api/v1/duplicates?type=&threshold=&path=&match=&exclude=
GET  /api/v1/people
GET  /api/v1/people/{id}/images
GET  /api/v1/images/{id}/faces        boxes + person, for overlays
PUT  /api/v1/faces/{id}/person        { person: "..." }
```

Face→person assignment is only safe because plan 045 shipped: `faces.id` is
AUTOINCREMENT, so an id never names a different face. 045 did that for 047's
People screen, which holds face ids across human-scale time; a remote client
holds them longer still.

### Operations

```
POST   /api/v1/index    → 202 + Location: /api/v1/operations/{id}
                          body names a directory under `[server] scan_roots`
POST   /api/v1/thumbs   → 202 + Location: /api/v1/operations/{id}
POST   /api/v1/faces    → 202 + Location: /api/v1/operations/{id}

GET    /api/v1/operations
GET    /api/v1/operations/{id}          state, progress, errors, result
GET    /api/v1/operations/{id}/events   SSE
DELETE /api/v1/operations/{id}          → 202; cancel is prompt, not instant
```

`DELETE` returns 202 because plan 031 established that cancellation stops an
operation promptly rather than immediately.

### Server info

```
GET /api/v1/server
```

phig version, protocol version, schema version, thumbnail sizes on offer, caps
in force — so a client negotiates instead of guessing.

## Auth and security

- Bearer token on every JSON endpoint, constant-time compare, tokens in
  `[server]` in phig's existing INI. A 32-byte random token makes brute force
  irrelevant without lockout logic.
- Signed, expiring URLs on media endpoints. **No endpoint is
  unauthenticated** — a thumbnail URL must not be a way around the token.
- **No client-supplied path is ever resolved against the filesystem, with one
  allowlisted exception.** Media and metadata endpoints take only ids and
  sha256 digests, and `phigd` resolves paths from the database. phig indexes
  files in place across arbitrary locations, so an endpoint that opens a path
  a client chose is an arbitrary-file-read primitive wearing a bearer token.
  Asserted in tests, not merely documented.

  Two things look like exceptions and are not. `GET /duplicates` takes
  `path`, `match` and `exclude`; those are **matched against indexed rows and
  never opened**, so they filter what the caller can already retrieve rather
  than reaching anywhere new.

  `POST /index` is the genuine exception, because scanning a directory means
  naming one. Left unconstrained it is a privilege escalation with a delay:
  index `/home/someone-else/photos`, then read every original back out through
  a legitimate endpoint. So **scan targets must canonicalize to a location
  under a configured `[server] scan_roots` allowlist**, checked after symlink
  resolution, and a request outside it is 400 rather than a silently narrowed
  scan. In S1 the token holder owns the machine, so this is defence in depth;
  under S4 it becomes load-bearing.
- **A valid token reads the entire collection.** There is no partial access in
  S1. Stated as a non-goal so S5's sharing is built as a scoped capability
  rather than retrofitted as a filter someone forgets to apply.
- Destructive operations are absent by design; see exclusions.
- Every cap returns an error rather than queueing.

## Error handling

`{ "error": { "code", "message" } }` where `code` is a stable string mirroring
`phig_status`, so a client branches on a field rather than parsing prose.

| Condition | Status |
|---|---|
| Invalid argument | 400 |
| Missing or bad token, bad signature | 401 |
| Unknown id, source file gone | 404 |
| Writer lock held, cap conflict | 409 + `Retry-After` |
| Unsatisfiable range | 416 |
| At capacity | 503 + `Retry-After` |

No exception escapes a handler — the rule `PHIG_TRY` enforces at the ABI. A
source file that is gone is an ordinary state, not corruption, matching what
`phig_thumb_generate` already documents.

## Caps

All configurable, all with defined behaviour at the limit: page size, allowed
thumbnail sizes, scan roots, concurrent operations, concurrent thumbnail
generations, SSE subscribers, HTTP connection pool and worker pool sizes.

The pool sizes are not incidental. SSE holds a connection open and a browser
opens several per origin, so a pool sized for concurrent *requests* rather
than concurrent *connections* exhausts and the daemon stops answering — a
hang, not an error.

## Testing

Real-behavior, no mocks: the daemon runs in-process on an ephemeral port and
tests drive it with a real HTTP client. Every fixture takes a temp directory
from `tests/temp_dir.h`; a hardcoded temp path passes alone and fails under
`ctest --parallel`.

- Range requests: normal, suffix, unsatisfiable → 416, multi-range.
- Auth: absent token, wrong token, expired signature, tampered signature.
- Path safety: assert no media or metadata endpoint accepts a client-supplied
  path; assert a scan target outside `scan_roots` is rejected, including via a
  symlink that resolves outside it and a `..` traversal that normalizes
  outside it.
- Operations: start, poll, cancel mid-run, completion, and survival of client
  disconnect.
- SSE: delivery from a worker thread, reconnect resync against
  `GET /operations/{id}`.
- Concurrency: a metadata write during a running index; a CLI writer lock
  producing 409 rather than a block.
- Caps: each one returning its error rather than queueing.

## Open questions

1. **HTTP library — Drogon or libmicrohttpd.** To be settled by spiking the
   same three endpoints on each: a JSON query, a range-served original, an SSE
   progress stream.

   Drogon is an event loop with routing, JSON, file responses with ranges,
   streaming responses and a thread pool; verified in Ubuntu universe
   (`libdrogon-dev`, noble onward) and Homebrew 1.9.13; 14,172 GitHub stars,
   pushed 2026-08-04, MIT. It brings trantor — its own event library *and*
   logger — plus jsoncpp and its own config format, overlapping three things
   phig already owns, and `CLAUDE.md` requires all logging to go through
   `core/log.h`.

   libmicrohttpd brings nothing and imposes nothing: no logger, no config, no
   JSON, and `MHD_create_response_from_fd_at_offset64` covers most of the
   media half. In Ubuntu since 22.04 and Homebrew 1.0.9; LGPL 2.1+, so
   dynamic linking. Debian popcon puts it at 5,102 installs against Drogon's
   13 — though that overstates the gap, since Drogon's users skew to
   containers and vcpkg, which popcon does not see. Its answer to "progress
   arrives on a worker thread and must reach an open SSE connection" is
   `MHD_suspend_connection`/`MHD_resume_connection` plus a content-reader
   callback, which is the most awkward corner of the whole design.

   The spike must also show, for whichever wins: logging routed into
   `core/log.h`, SSE fed from a worker thread, a multi-gigabyte file
   range-served from a database-resolved path, and a clean build from packages
   on both CI runners.

2. **Is `GET /duplicates` a request or an operation?** Near-duplicate
   detection compares against the full database by design (plan 002), so it
   may be a sub-second read at 29,925 images and a multi-minute job at a
   million. `tests/perf_duplicates.cpp` exists to answer this against the real
   collection. Measure before choosing.

3. **Does the image record expose the server's filesystem path?** It is
   informational and phig's whole philosophy is that files stay where they are
   with names unchanged, so a user wants to see it — but it leaks server
   layout to every authenticated client, and S4 may have clients that should
   not see it.

## Excluded from S1

| Excluded | Why / where it goes |
|---|---|
| Organize (cp/mv), remove (rm) | A bearer token that can permanently delete a photo library is a different risk class from one that can add a tag. Depends on plans 042 and 041. |
| Scan's destructive reconcile | Plan 041 splits it into a decision and an authorized application — the shape a remote client needs. |
| Upload / import | S1b. Needs plan 042's organize engine; cannot be a per-file POST (see below). |
| Video transcoding | Its own plan, once the collection has been probed for which files actually fail to play. |
| Accounts, per-user metadata | S4. |
| Gallery sharing | S5, built on the signed-URL mechanism above. |
| Web UI | S3. |
| Native clients speaking this | S2. |

**One finding worth recording now, because it constrains S1b's protocol
shape:** import cannot be a per-file POST. RAW+JPEG pairing is decided by
`has_sibling_raw` listing the file's parent directory on disk, and sidecar
discovery is likewise same-basename within a directory. Uploading
`IMG_1234.CR2` and `IMG_1234.JPG` as independent requests landing in separate
staging areas indexes two photos instead of one, permanently — the demotion
only happens if the sibling is visibly there when the scan runs. An import has
to be a session: open, upload a set, commit, then scan and organize the staged
set as a unit. S1 should reserve that shape so S1b needs no protocol revision.

## Documentation this obsoletes

- Root `CLAUDE.md`: Layout, Interface discipline (currently reads as though
  `libphig` is the only way in), Platform boundaries, Project structure, Tech
  stack.
- `libphig/CLAUDE.md`: the ABI is no longer the only interface for clients.
- `MARKET.md`: needs a dated amendment. Its r/unRAID analysis recommends
  against targeting the self-hosted segment because Immich owns it, and notes
  commenters objecting to web UIs on principle. That recommendation was about
  market positioning, not about personal utility, but the document currently
  argues against building this and should say why it is being built anyway.

## Cross-references

- Plan 006 (API Layer, `deferred`) — the HTTP/JSON API this replaces. 006 was
  deferred with its open questions unresolved and was later treated as
  superseded by `libphig`; this plan supersedes it properly.
- Plan 007 (TUI/GUI, `superseded`) — assumed a GUI on 006's HTTP API.
- Plan 031 — the three-separate-operations decision, the uniform progress
  record, the bounded retained error list, and prompt-not-instant cancel.
- Plan 041 — reconcile as decision plus authorized application.
- Plan 042 — organize engine; prerequisite for S1b.
- Plan 044 — where phig's time actually goes; owns making offset pagination
  scale.
- Plan 045 — stable `faces.id`; prerequisite for exporting face ids to a
  remote client.
- Plan 047 — the nine screens this protocol has to be able to serve; Screen 3
  (Duplicates) and Screen 4 (People / Faces) are the two `libphig` cannot
  currently feed.
- Plan 048 — modern image codecs. Hard prerequisite for S3; without it a web
  gallery of an iPhone library renders empty cells.
- Plan 050 — macOS app M0. A future S2 consumer: it page-fetches through
  `phig_query_page` today, and remote support means choosing this protocol
  instead at connect time.
