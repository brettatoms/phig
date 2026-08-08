---
name: phig Server — Protocol & Daemon
status: designing
created: 2026-08-08
description: >
  `phigd`, a headless daemon that serves a phig collection over HTTP so
  clients can reach it remotely: paged queries, per-image metadata, duplicates,
  people and faces, tag/gallery/favorite writes, the three long-running
  operations with polled progress, and media — thumbnail bytes, originals, and
  byte-range video. A peer of `cli/` over `core/` rather than a client of
  `libphig`: it is phig running headless, entitled to whatever `core/` has the
  moment core has it. Clients gain a second interface: `libphig` when the
  collection is local, this protocol when it is remote. Single identity,
  bearer token, behind a reverse proxy, on libmicrohttpd. Holds no long-lived
  connections: push-to-browser is the web tier's concern, not phig's.
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
| S3 | Web client (server-rendered UI); also owns push-to-browser | S1 |
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
and includes `core/` headers directly, exactly as `cli/` does, and is entitled
to whatever `core/` has the moment core has it rather than waiting on an ABI
plan.

**That is the durable argument; the ABI's present coverage is not.** `libphig`
should eventually expose every phig feature, so today's gaps are a schedule
fact. What they do establish is the cost of choosing wrong: with no
duplicates, people, faces, organize, remove or reconcile in the ABI, a daemon
built on it could not serve two of 047's nine screens today, and each gap
would become an ABI plan blocking the server.

**Consequence: clients now have two interfaces, not one.**

| Collection is | Client uses |
|---|---|
| Local | `libphig` C ABI |
| Remote | phig wire protocol (this plan) |

Both cover the same capabilities. That is deliberate — S2's job is choosing a
transport at connect time, not learning a second vocabulary. Root `CLAUDE.md`'s
Interface discipline section currently reads as though `libphig` is the only
way in and needs amending, not footnoting.

### The web tier is an HTTP client too, not a `libphig` caller

S3 could call `libphig` in-process through Java's FFM instead of speaking this
protocol. It should not, for three reasons that hold no matter how complete the
ABI becomes:

- **Parity by construction.** A web app with a private in-process path can do
  things native clients cannot, and over time it will. Making it an ordinary
  HTTP client is what keeps the two surfaces equal — the same reasoning behind
  Interface discipline generally.
- **Colocation, permanently.** `libphig` needs the database, the media and the
  thumbnail cache on its own filesystem, and SQLite in WAL mode over a network
  mount is unsafe. Calling it directly welds the web tier to the collection's
  host. As an HTTP client it can live in a container, on a VPS in front of a
  home NAS, anywhere with a route.
- **`phig_last_error` is thread-local** and documented as readable only on the
  thread whose call failed. FFM gives no guarantee a call stays on one carrier
  thread and virtual threads migrate, so error text would come back wrong or
  empty — intermittently, on failure paths only.

The rule this leaves: **`libphig` is the local, in-process interface; the wire
protocol is the one that works from anywhere.** The web app is not a special
case, it is a remote client that renders HTML.

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

**Progress is polled, not pushed.** `GET /operations/{id}` is the resource,
and clients read it on whatever interval suits them; at 1 Hz a progress bar is
indistinguishable from a pushed one. This reverses an earlier choice of SSE,
and the reason is that push-to-browser is a web-app problem rather than a phig
one: S3 polls this endpoint once and fans out to however many browser
subscribers it has, over a runtime built for that. Native clients poll
directly, which is also what `libphig`'s operations already do.

The consequence is architectural, not cosmetic: **`phigd` holds no long-lived
connections.** That removes the hardest constraint from the daemon — see the
HTTP library decision below, which it largely settles.

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

### libmicrohttpd, and why not the alternatives

With progress polled, `phigd`'s HTTP needs are routing, JSON bodies,
byte-range file responses, and handlers that block on phig. Nothing holds a
connection open.

**libmicrohttpd, chosen.** Its epoll modes
(`MHD_USE_EPOLL_INTERNAL_THREAD` with a thread pool) multiplex connections, so
idle keep-alives and slow transfers cost file descriptors rather than threads.
`MHD_create_response_from_fd_at_offset64` makes range-serving a media file the
library's job — phig resolves the path and hands over an fd, MHD runs the
write loop from its event loop with no thread parked per transfer, and range
responses fall out of the offset and length arguments. Connection limits,
per-connection memory pool and timeouts are configured rather than discovered.
It brings no logger, no config format and no JSON to overlap what phig already
owns. In Ubuntu since 22.04, Homebrew 1.0.9, LGPL 2.1+ so dynamic linking.
Debian popcon puts it at 5,102 installs against Drogon's 13 — which overstates
the gap, since Drogon's users skew to containers and vcpkg that popcon cannot
see, but this is an HTTP parser facing a network and adversarial exposure
counts for something.

Accepted costs: routing and JSON are hand-written, and it is a C API inside a
C++20 codebase.

**One thing to confirm before writing much against it**, because the whole
worker-dispatch design rests on it: that `MHD_suspend_connection` /
`MHD_resume_connection` behave as needed under the epoll-plus-thread-pool mode
specifically, including that a client disconnecting while a connection is
suspended is detected and cleaned up rather than leaked. Suspend/resume did
**not** leave with SSE — it is how a handler hands blocking phig work to a
worker without occupying an event loop. The usage is far simpler than the SSE
one would have been (suspend, work, resume, one complete response, once per
request, rather than a content-reader callback driving a long-lived stream),
but it is the same primitive and it is load-bearing. An afternoon against a
stub, not a comparative spike.

**Thread-per-connection was rejected**, which rules out `cpp-httplib` despite
it being header-only, vendorable, immune to distro availability and the least
work to adopt. Two things hold connections open here with no SSE involved: a
client pulling a multi-gigabyte video over a slow link occupies a thread for
the duration, and browser keep-alive holds up to six idle connections per
origin for a minute or more, so a few tabs sit on a dozen-plus threads doing
nothing. `cpp-httplib` sizes its pool from `hardware_concurrency`, so on an
eight-core NAS exhaustion is reachable — and the failure mode is the daemon
silently not answering rather than an error anyone can see. It is tunable, but
the tuning is a number you discover was wrong in production.

**Drogon was rejected** once SSE moved out. Its advantages here were the event
loop and streaming responses; its costs did not move — trantor's own event
library *and* logger against `CLAUDE.md`'s rule that all logging goes through
`core/log.h`, plus jsoncpp and its own config format, three subsystems
overlapping things phig already owns. Verified available (`libdrogon-dev` in
Ubuntu universe from noble, Homebrew 1.9.13, MIT, 14,172 stars, pushed
2026-08-04) and it remains the fallback if hand-written routing proves worse
than the dependency overlap.

## Architecture

```
browser ──────┐                          ┌─ phig-core ─┬─ SQLite (WAL)
              │   ┌──────────────┐       │             ├─ media files
web app (S3) ─┼──►│ reverse proxy├──────►│  phigd      └─ thumb cache
              │   │  TLS, certs  │       │  loopback
native (S2) ──┘   └──────────────┘       └─────────────
```

New top-level `server/`, producing `phigd`. Four units:

- **transport** — routing, byte ranges, JSON encoding, and the MHD callback
  plumbing. Knows nothing about phig; testable against a stub.
- **resources** — one file per resource (images, media, operations,
  duplicates, people, tags). The only layer that knows both HTTP and phig.
- **operations registry** — id allocation, lifecycle, cancel tokens, and a
  progress snapshot any number of pollers can read without disturbing the
  running engine. Owns the fact that operations outlive their originating
  client.
- **auth** — constant-time bearer check and signed-URL minting, nothing else.

Configuration extends `core/config.cpp`'s existing INI with a `[server]`
section rather than introducing a second config format.

**Handlers never call phig directly.** `core/` is synchronous and blocking
throughout — SQLite, OpenCV decode, LibRaw. MHD multiplexes connections onto a
small set of threads, so a handler that blocks for 300 ms on a thumbnail cache
miss stalls every other connection sharing that thread. Phig calls go to an
explicit worker pool and the response completes from there, using MHD's
suspend and resume. Multiplexing makes connections cheap; it does not make
phig non-blocking, and forgetting that converts a slow request into a stalled
server.

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
DELETE /api/v1/operations/{id}          → 202; cancel is prompt, not instant
```

No streaming endpoint. Clients poll `GET /operations/{id}`; S3 polls it once
and pushes to browsers itself.

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
generations, concurrent connections, MHD's per-connection memory pool, MHD's
thread pool size, worker pool size, and connection timeouts.

These are not incidental. Because handlers suspend rather than block, the MHD
thread pool sizes the *event loops* and the worker pool sizes *phig
concurrency* — conflating them is how you end up with either idle threads or a
stalled server. The connection limit and timeout are what stop a client
holding transfers open indefinitely from consuming the descriptor budget.

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
- Progress snapshots: many concurrent pollers on one running operation, with
  no effect on its throughput and no torn reads of the progress record.
- Suspend/resume: a handler whose phig work runs on a worker completes
  correctly; a client that disconnects mid-work does not leak a suspended
  connection; and a slow handler does not stall unrelated connections sharing
  its event loop — the specific failure the design exists to prevent.
- Concurrency: a metadata write during a running index; a CLI writer lock
  producing 409 rather than a block.
- Caps: each one returning its error rather than queueing.

## Open questions

1. **Is `GET /duplicates` a request or an operation?** Near-duplicate
   detection compares against the full database by design (plan 002), so it
   may be a sub-second read at 29,925 images and a multi-minute job at a
   million. `tests/perf_duplicates.cpp` exists to answer this against the real
   collection. Measure before choosing.

2. **Does the image record expose the server's filesystem path?** It is
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
