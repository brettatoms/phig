---
name: Apple Live Photos
status: designing
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

**Status is `designing`; the file-availability gate has lifted.** This plan was
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

| File | Where | Notes |
|---|---|---|
| Still | Apple MakerNote tag **0x0011** | Reachable via libexif's `exif_data_get_mnote_data()`; libexif 0.6.26 ships an Apple MakerNote parser, so no new dependency. |
| MOV | QuickTime metadata key **`com.apple.quicktime.content.identifier`** | libavformat already exposes QuickTime metadata; `core/metadata.cpp` reads this container today. |

Same-basename matching is only a heuristic. It breaks on export and rename,
and it produces false pairs: this collection already contains
`/mnt/data/Pictures/Brett/2013/12/MVI_0010.MOV` and friends, Canon clips that
a basename rule could one day demote into attachments of unrelated stills.
**A wrong pairing is a data-safety bug** — it silently removes a video row and
makes `mv` drag the file to the wrong place — so the identifier is the rule,
not an optimization over the heuristic.

## Open decisions

**1. Fallback when the identifier is absent.** Exports and re-encodes strip
MakerNotes. Options: (a) identifier-only, refuse to guess — safest, misses
hand-edited exports; (b) identifier first, then same-basename *guarded* by
signals that exclude ordinary camera clips (still is HEIC, or MOV ≤5s with no
audio track). Recommendation: (b), because export-stripped pairs are likely to
be the common case in any ingested library, and the guards are what keep the
`MVI_*` clips out. Not settled.

**2. Migration of existing MOV rows.** Adopting the model demotes any already
indexed MOV that turns out to be paired, transferring user metadata to the
still row first — plan 023's relink primitive, exactly as plan 026 demotes a
JPEG. Needs confirming that the still-as-master direction is right, since it
inverts the current state where the MOV is a normal row. Recommendation: still
is master; it carries the EXIF, the faces, and the thumbnail.

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
