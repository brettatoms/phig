---
name: Apple Live Photos
status: completed
created: 2026-08-06
description: >
  Treat an Apple Live Photo — a still (HEIC/JPEG) plus its ~3s MOV — as one
  photo: the still is the indexed row, the MOV a carried attachment, following
  plan 026's RAW+JPEG model. Pairing must key on Apple's Content Identifier
  UUID (still: Apple MakerNote 0x0011; MOV:
  `com.apple.quicktime.content.identifier`), not same-basename, which would
  mis-pair the Canon `MVI_*.MOV` clips already in the collection. Unblocked
  2026-08-08: 048 made the HEIC still decodable, and 049's Photos probe found
  390 Live Photos to validate against. Now on 049's critical path, since 21% of
  a real library is Live Photos and they must import as master+attachment. Two
  open decisions remain — identifier fallback and MOV row migration.
---

# 038 — Apple Live Photos

**Shipped 2026-08-09.** Goals 1 and 2 are done: one row per shot, and organize
keeps the pair together. Execution script:
`docs/plans/2026-08-08-038-live-photo-pairing.md`.

## What shipped

`core/live_photo` reads the Content Identifier from the MOV and tests the still
for containment of it, because libexif cannot parse Apple's MakerNote at all.
Six commits: identifier read, containment, pairing over a set, scan skipping the
attachment, organize carrying it, and demotion of pre-existing video rows.

Measured end to end: four exported Live Photos plus two ordinary stills index as
6 rows from 10 files; `mv` moves both halves into the still's dated directory;
and a collection indexed before this plan re-scans down to one row per shot with
both files intact on disk.

Deferred, unchanged from the original scope:

- **Goal 3, libphig visibility.** A Live badge and playable motion is ABI
  surface, not pairing; it belongs with the macOS client in plan 050.
- **Goal 4, round-trip fidelity.** Needs a device, and writing Apple metadata
  would be a new capability — see Scope notes.
- **Identifier-stripped pairs.** No fallback, by decision; revisit only with real
  stripped files to measure a guard against.

**Coverage limit worth knowing:** no committable fixture can carry an Apple
MakerNote, so every still-side assertion skips in CI and is proven only on a
machine with exported samples at `~/phig-live-samples`. The MOV side has a
synthetic fixture and is covered everywhere.

---

**Original status note, kept for the record: the file-availability gate lifted.** This plan was
written on 2026-08-06 when the collection had zero HEIC rows and nine MOV rows,
all Canon camera clips (`MVI_*.MOV`, 2008–2014) and no Live Photos — building
against synthetic fixtures alone would have broken the project's rule that
data-safety changes ship with real-behavior tests.

That gate is gone. On 2026-08-08 plan 049's Photos probe measured 390 Live
Photos in a 1,894-asset library, and plan 048 made the HEIC still decodable.
Real files and a working decoder both exist, so what is left is this plan's own
two open decisions.

## Goal

A Live Photo is two files written together by an iPhone: a still image
(`IMG_1234.HEIC`, or `.JPG` on older devices / after export) and a short MOV
holding the motion. phig currently indexes both — `heic`/`heif` are in the
image extension list and `mov` in the video list (`core/scanner.cpp`) — so one
shot becomes two rows, the MOV inflates video counts and search results, and
`mv`/`cp` can file the two halves under different dates and separate them.

Four things this should deliver:

1. **One row per shot.** The MOV stops appearing as an independent video.
2. **Organize keeps the pair together**, filed under the still's date.
3. **The pairing is visible through libphig**, so the GTK client can show a
   Live badge and play the motion.
4. **Round-trip fidelity** — the pair survives phig intact enough to work as a
   Live Photo again in an Apple ecosystem.

Goals 2 and 4 fall out of the model; 1 is the model; 3 is surface on top.

## Model

Follows plan 026 (RAW as master, JPEG as attachment) almost exactly:

- The **still** gets the `images` row — the shot's identity, metadata, phash,
  thumbnail, faces, and searchable record.
- The **MOV** is **not indexed**. It is a carried attachment of the still,
  moved and copied alongside it like a metadata sidecar (plan 025's carry
  machinery).
- A MOV with no paired still is a normal video → its own row, as today.

The pixel source is the still; unlike RAW there is no decode-tier question,
except that HEIC must be decodable at all — which it is not today, and which
plan 048 fixes (see decision 3).

## The pairing rule — why basename is wrong

Apple's authoritative link is a **Content Identifier UUID** stored in both
files:

| File | Where | How it is actually reached |
|---|---|---|
| MOV | QuickTime metadata key **`com.apple.quicktime.content.identifier`** | One `av_dict_get` on the container metadata `core/metadata.cpp` already opens. Verified on four exported pairs; the value differs per shot. |
| Still | Apple MakerNote tag **0x0011** | **Not reachable through libexif.** See below. |

### Correction: libexif cannot read the still's identifier

This plan originally stated that the still's tag was "reachable via libexif's
`exif_data_get_mnote_data()`; libexif 0.6.26 ships an Apple MakerNote parser,
so no new dependency". **That is false, and was measured 2026-08-08:**

- `exif_data_get_mnote_data()` returns NULL on real Live Photo HEICs.
- `libexif.dylib` 0.6.26 exports **zero** Apple symbols; its MakerNote modules
  are Canon, Olympus and Pentax only.

The data is present — the EXIF block contains the UUID and carries a MakerNote
tag of ~1700 bytes — libexif simply has no parser for it.

Nor is a pattern scan of the MakerNote a safe substitute. Measured: a Live
Photo still carries **three** UUID-shaped strings in its MakerNote and an
ordinary still carries **two**, so "take the first UUID" works only by
positional accident, and Apple owes no such contract.

### The mechanism: identify from the MOV, test containment on the still

The MOV's identifier is authoritative and cleanly readable. The still is then
tested for **containment** of that exact UUID within its MakerNote bytes:

- No parsing of untrusted binary structure — the same objection that ruled out
  a hand-rolled ISOBMFF parser in plan 048.
- No positional assumption about Apple's MakerNote layout.
- No new dependency, and therefore no licensing decision (see plan 058: exiv2
  would read tag 0x0011 properly but is GPL, while every dependency phig links
  today is LGPL).
- A false pair would require a 36-character UUID collision.

Ambiguity is refused rather than guessed: if two stills contain the same MOV's
identifier, neither is paired and the MOV stays an independent row.

If phig later adopts a metadata library with Apple MakerNote support, the still
side can be swapped to read tag 0x0011 directly behind the same interface.

Same-basename matching is only a heuristic. It breaks on export and rename,
and it produces false pairs: this collection already contains
`/mnt/data/Pictures/Brett/2013/12/MVI_0010.MOV` and friends, Canon clips that
a basename rule could one day demote into attachments of unrelated stills.
**A wrong pairing is a data-safety bug** — it silently removes a video row and
makes `mv` drag the file to the wrong place — so the identifier is the rule,
not an optimization over the heuristic.

## Open decisions

**1. ~~Fallback when the identifier is absent.~~ Settled 2026-08-08:
identifier only, refuse to guess.** A wrong pairing is a data-safety bug — it
silently removes a video row and makes `mv` drag a file to the wrong place — so
an unpaired still and MOV simply stay independent rows, which is today's
behaviour. The failure mode is "no improvement", never "corrupted". A guarded
basename fallback was considered and deferred: there are no real
identifier-stripped files to measure a guard against, and inventing heuristics
without them is how the `MVI_*` mis-pairing happens.

**2. ~~Migration of existing MOV rows.~~ Direction settled 2026-08-08: the
still is master.** Live Photos are to be treated the way a RAW is treated with
its sidecar, which fixes the direction. Adopting the model demotes any already
indexed MOV that turns out to be paired, transferring user metadata to the
still row first — plan 023's relink primitive, exactly as plan 026 demotes a
JPEG. What remains is implementation, not decision.

**3. HEIC decode. Answered 2026-08-08: OpenCV cannot decode HEIC**, and the
prerequisite this decision anticipated is now plan 048.

Measured by scanning a HEIC alongside a JPEG control: the HEIC was found by the
walk, rejected by `cv::imread` with `Cannot decode image`, counted as an error
and never indexed, while the control indexed normally. Homebrew's OpenCV
`imgcodecs` links jpeg-turbo, webp, png, tiff and openjpeg — no HEIF codec —
though libheif 1.23.1 is installed as another package's dependency.

This decision's own guess at the shape of the fix was right: a decode tier
structurally like plan 026's LibRaw tiers, as its own plan. Plan 048 is that
work, with one refinement measurement forced — on macOS the tier is ImageIO
rather than libheif, which decodes 4.5x faster in wall time and at 1/14th the
CPU. 048 also covers HEIC's EXIF, which libexif's file loader cannot read
either, so the still's date and camera survive.

**This plan now depends on 048** for the still to be decodable at all. Nothing
else here changes: 048 makes the still a normal indexed row and does no
still/MOV pairing, which remains this plan's subject.

## Before this becomes implementable

**All three of these cleared on 2026-08-08.** This plan was blocked on evidence
it now has.

- ~~**A real source of Live Photos.**~~ Cleared. Plan 049's read-only Photos
  probe found **390 Live Photos** in a 1,894-asset library — 21% of it. That is
  a development corpus and a validation corpus in one. The probe can export
  samples with their identifier metadata intact, since it writes original
  resource bytes.
- ~~**Resolve decision 3 by measurement.**~~ Cleared, and it did insert a
  prerequisite plan: OpenCV cannot decode HEIC, and plan 048 is the decode tier
  this decision predicted would be needed. 048 shipped 2026-08-08, so the still
  is now decodable.
- ~~**Plan 031.**~~ Stale as written: 031 completed 2026-08-07.

What remains is this plan's own two open decisions — identifier fallback and
MOV row migration — neither of which is blocked by anything external.

### Why this is now on 049's critical path

Plan 049 imports a Photos library, and Live Photos are not a rare corner of
one: 21% in the library measured. They are to be treated the way a RAW is
treated with its sidecar — always paired, the still indexed, the MOV a carried
attachment — which is exactly this plan's model. So 049 cannot export them
correctly until this lands, and 049 depends on it.

## Dependencies

Depends on 025 (sidecar carry machinery), 026 (the master/attachment model and
its scan transitions), 023 (relink primitive for demotion). Goal 3 additionally
depends on 031 (completed). Also depends on 048, which makes the HEIC still
decodable — decision 3, previously an open question, resolved as a hard
prerequisite and shipped. Plan 049 depends on this plan in turn: it cannot
export Live Photos as master+attachment until the pairing exists.

## Scope notes

Round-trip fidelity (goal 4) raises a question phig has not faced: whether it
ever *writes* Apple metadata. Today it writes none, for anything. If preserving
the pair requires rewriting a Content Identifier — e.g. after a rename — that
is a new capability and should be split out rather than smuggled in here.
