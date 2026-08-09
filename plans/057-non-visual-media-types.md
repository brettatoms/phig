---
name: Non-Visual Media Types (MIME)
status: designing
created: 2026-08-09
description: >
  Whether phig should index files that are not images or videos — audio, PDFs,
  documents — and whether MIME types are the mechanism. `media_type` is already
  free-text with two values, so a third costs no migration by itself; the real
  cost is `phash INTEGER NOT NULL`, which is the column behind phig's most
  measured asset and has no meaning for an MP3. Six of phig's primitives
  (sha256 dedup, activity log, tags, organize, ignore, trash) are file
  primitives that work on anything; four (phash, the date cascade, thumbnails,
  faces) are visual and are three of the four items MARKET.md files under
  "Strong". MIME is worth adopting on its own merits regardless of this
  question — plan 052 serves originals over HTTP and never says where
  Content-Type comes from — but it cannot replace the extension, because CR2,
  NEF, ARW and DNG all begin with a TIFF header. Recorded to think about, not
  to build.
---

# 057 — Non-Visual Media Types (MIME)

## The idea

Index more than images and video: audio (possibly per format), PDFs,
documents. Use MIME types as the classifier rather than the current
two-valued `media_type`, so a new format is a new MIME string instead of a new
C++ extension set.

Raised 2026-08-09 in the context of Daminion and iMatch, which catalogue
documents alongside photos.

## What is already general, and what is not

Verified against the tree, not assumed.

| Primitive | Visual-specific? | Where |
|---|---|---|
| sha256, exact dedup, relink-by-content | No | `core/hasher.cpp`, plan 023 |
| Activity log + restore snapshots | No | plan 020 |
| Tags, galleries, favorites | No | plan 011 |
| `mv`/`cp` `%Y/%m/%d` format strings | No, **if** the file has a real content date | plan 003 |
| `.phigignore`, `rm`, trash | No | plans 028, 027 |
| phash / near-duplicate index | **Yes** | `database.cpp:396`, plan 002 |
| Date cascade (EXIF → sidecar → mtime) | **Yes** | plan 025 |
| Thumbnails, faces, blur, RAW pairing | **Yes** | plans 014, 010, 012, 026 |

So the narrowing is real but partial. Six primitives are file management and
are reachable only by visual files today, which is the arbitrary part. The four
that are not happen to be three of the four entries under MARKET.md's "Strong —
rare in the field and already built" (line 1180): duplicate detection, correct
dates, RAW+JPEG pairing.

`media_type` is already `TEXT NOT NULL DEFAULT 'image'` (`database.cpp:400`)
with two values by convention, not by constraint. A third value costs no
migration. **`phash INTEGER NOT NULL` is the actual cost**, and it is the
column behind the one asset with published numbers (160k in 1.7s).

## Why MIME is worth adopting independently of this question

The strongest argument for MIME does not depend on ever indexing a PDF.

Plan 052 serves media over HTTP — thumbnail bytes, originals, byte-range video
— and a browser decides between `<img>`, `<video>` and a download on
Content-Type. Searched 2026-08-09: neither 052, nor 053, nor
`libphig/include/phig/phig.h` mentions MIME or Content-Type anywhere. Today
that header would have to be derived from the file extension at serve time, in
the daemon, from a table that already exists in `core/scanner.cpp` and is not
exposed. Clients have the same problem — 047's nine screens and 052's web tier
both need "can I render this?" without shipping their own extension list.

MIME's `type/subtype` split also happens to be the coarse/fine distinction phig
already makes by hand: engines branch on the top level (`image/`, `video/`),
decoders branch on the subtype. `--media image|video|all` is a prefix match on
the top-level type.

That is a real near-term consumer for a MIME column with or without documents,
and it may be the part of this plan worth doing first.

## What MIME cannot do

**It cannot be sniffed from content and still describe a RAW file.** CR2, NEF,
ARW and DNG all begin with a TIFF header (`II*\0` or `MM\0*`); so does a plain
TIFF. Content sniffing collapses most of the 36 RAW extensions and TIFF into
`image/tiff`, and plan 026's three-tier pixel source branches on
`is_raw_extension` to decide whether to open a file with LibRaw at all. RAF and
X3F have their own magic; CR3 is ISOBMFF, not TIFF. There is no consistent
answer from bytes alone.

So the extension cannot be abandoned. The open question is whether sniffing has
a job beside it (question 2).

One structural fact in sniffing's favour: `compute_sha256` already streams
every file in 1 MiB blocks (`core/hasher.cpp:216`), so the first block is
already in memory at the point every scanned file is hashed. Inspecting its
first bytes costs no additional I/O and no additional open. That is an argument
about cost, not about correctness.

## Market position, recorded honestly

The competitive framing that prompted this does not survive examination, and
the reason it does not is worth writing down.

- **The demand evidence is weak in both directions.** MARKET.md mentions
  PDF/Office docs exactly once, in the iMatch row (line 119); Daminion's row
  claims no document handling. None of the six primary sources asked for it.
  But that count is weak evidence for the same reason MARKET.md's own P2P
  sampling-bias correction gives (line 1082): every source is a forum where
  individual photo hobbyists talk to each other, and Daminion's actual buyer —
  a small studio or marketing team cataloguing photos, PSDs, InDesign files and
  PDFs with approval workflow — is not there.
- **Following that audience does not lead to a wider phig.** It leads to
  multi-user access, permissions, approval states and InDesign previews,
  against Bynder, Canto and Daminion Server. A different product. MARKET.md's
  2026-08-05 decision not to pursue phig commercially (line 1454) applies with
  more force there, not less.
- **Breadth is the mechanism behind the complaint against the one product that
  does this.** iMatch handles Office docs and its standing criticism is the
  learning curve — "a grow-up application with a very large feature set,"
  its author agreeing. The most-cited buying criterion across all six sources
  is "easy to learn."

**Conclusion for the plan: do not justify this competitively.** If it is worth
doing it is worth doing because phig's own primitives are more general than its
schema admits, or because MIME serves 052. Those are the two live arguments.

## Open questions

None are answerable from the repository as it stands. Each names what would
answer it.

### 1. Does MIME replace `media_type` or sit beside it?

Replacing gives one source of truth, but `media_type` is exposed in
`libphig/include/phig/phig.h`, in `--media image|video|all`, and at 22 sites in
`core/database.cpp`. Plans 042 and 056 both note that the `SOVERSION` budget is
effectively one-shot, because `phig_op_progress` has no size member and the
next breaking change pays for all of them at once; spending it on a field
rename is a poor use of it.

Sitting beside gives two columns that can disagree, and nothing to say which
wins.

**What would answer it:** read the 22 `media_type` sites and check whether a
top-level MIME prefix serves every query they perform. If it does, this is a
representation change with no semantic loss and the question becomes purely one
of ABI timing — which argues for landing it with whatever else breaks the ABI,
not alone.

### 2. Where does the MIME type come from?

Three sources, and the answer is probably not one of them exclusively:

- **An extension map** — today's behaviour relabelled. No new dependency,
  no new failure mode, and no new information. A `.jpg` that is really a PNG
  stays wrong.
- **Content sniffing** — free at hash time (above), but cannot distinguish the
  RAW formats (above), and libmagic is a dependency `core/` would have to carry
  on three platforms where macOS would rather use `UTType`. That is a platform
  split in a portable directory, which root `CLAUDE.md` allows only for a
  narrow concern in one file.
- **The decoder that succeeded** — authoritative, but only for what phig can
  already decode, which is circular for anything new.

**Recommended shape to test, not to assume:** extension is authoritative,
sniffing is a disagreement detector that logs rather than overrides.

**What would answer it:** run `file --mime-type` across the 29,925-image
reference collection, compare against the extension map, and count
disagreements by class. If the count is near zero, sniffing earns nothing and
the extension map is the whole answer. This is cheap and needs no code.

### 3. What does a row with no phash mean?

`phash INTEGER NOT NULL` becomes nullable in a v9 migration, and then every
duplicates query, `libphig`'s records surface, and 052's protocol acquire a
class of rows they cannot compare. The question is not the migration; it is
whether `phig duplicates` silently excludes such rows or reports them as a
separate exact-only class, and what a client shows for a row that has no
visual identity at all.

**What would answer it:** decide what `phig duplicates` prints for a directory
of PDFs. If the honest answer is "the same thing `fdupes` prints," that is
worth knowing before building it.

### 4. What is indexed by default, and what does that do to `mv`?

This is the data-safety question and it gates the others.

If `scan` starts indexing documents by default, an existing user's next scan
adds rows for every PDF in their photo tree, and `phig mv` then moves those
files on the next organize — into a `%Y/%m/%d` layout derived from mtime,
because a document has no content date to cascade to. That is the failure plan
025 exists to prevent, applied to files the user never asked phig to touch.

Any answer here has to be opt-in, and the opt-in has to be visible in `mv`'s
selection, not only in `scan`'s.

**What would answer it:** plan 054 is already writing down what phig does when
a destination is occupied and when a row's file is gone. This belongs in the
same document, as a rule about what is eligible to be organized at all.

### 5. Does the sidecar model survive?

Plan 025 defines `xmp, pp3, dop, on1, aae, json, thm, lrv` as attachments that
are never indexed as rows. A documents mode makes a standalone `.json`
ambiguous, and MIME does not disambiguate it — a Google Takeout sidecar and a
config file are both `application/json`. `.thm` is a JPEG; `.lrv` is a video.

The existing disambiguator is sibling presence, which is already how plan 026
demotes a JPEG that has a same-basename RAW.

**What would answer it:** state whether "attachment" is a property of a file or
of a pair. Plan 026 implies the latter and this plan should not contradict it
by accident.

### 6. What does the user-facing surface become?

`--media image|video|all` does not extend gracefully to a MIME vocabulary, and
`--mime application/pdf` is not a flag a casual user types. Audio subtypes
compound it: `audio/mpeg` versus `audio/flac` is a distinction a search surface
probably should not expose, which is an argument against the "specific media
types per audio format" half of the original idea and for grouping at the
top-level type.

**What would answer it:** plan 047's screens. If no screen wants to filter
below the top-level type, the subtype is storage and serving detail, not a
query axis.

## Alternatives considered

Recorded so they are not rediscovered.

- **Audio as a third media type, without documents.** Technically the cheapest
  and most coherent: `duration_ms` already exists, embedded cover art gives a
  real thumbnail, ID3 gives a real content date, and Chromaprint is the direct
  structural analogue of phash. Argued against on merit rather than cost:
  **songs have canonical identity and photos do not.** MusicBrainz gives every
  release a global ID, so the metadata mess phig exists to untangle was solved
  for music by the industry maintaining a database. Picard plus Navidrome or
  Jellyfin occupies this, and does it better than a photo tool would.
- **Companion files only** — extend plan 025's attachment model so a PDF beside
  a shoot travels with it, without ever becoming a row. Nearly free, but it
  answers a mixed-directory hazard rather than the generality question.
- **State the boundary and do not widen** — write down that phig's unit of
  management is a visual asset and that the six generic primitives are scoped
  to it deliberately. Costs nothing and closes the question. Still the right
  outcome if question 4 has no satisfying answer.

## Explicitly not in scope

- **Full-text search, OCR, and version history.** These are what a document DAM
  is actually bought for, and phig has none of the three. Indexing PDFs without
  them yields filename-and-date search over PDFs, which Spotlight and `find`
  already do better. If documents are ever in scope, this is the work — not the
  extension list.
- **Office document rendering.** No thumbnail without a LibreOffice-class
  dependency.
- **Team/multi-user DAM.** Permissions, approval workflow, shared catalogues —
  the market Daminion actually sells to. Out, per the market section above.
- **Deciding 052's Content-Type on its behalf.** This plan notes the gap; 052
  owns the endpoint.

## Relationship to other plans

- **Plan 052** (phig server) — serves originals over HTTP and has no stated
  source for Content-Type. The strongest independent consumer of a MIME column.
- **Plan 053** (libphig parity) — would carry the MIME field to clients, and
  already owns the question of how a client names its subject.
- **Plan 054** (reconcile & conflict rules) — the natural home for question 4's
  eligibility rule.
- **Plan 056** (uniform operation status) — the other plan tracking the
  one-shot `SOVERSION` budget question 1 would spend.
- **Plan 026** (RAW support) — `is_raw_extension` is why content sniffing
  cannot be authoritative.
- **Plan 025** (sidecar files) — the attachment allowlist question 5 threatens.
- **Plan 002** (duplicate detection) — owns the phash the nullable migration
  affects.
- **Plan 051** (broader image formats) — deferred for the same reason this one
  might be: no evidence of a real collection containing files phig skips.
