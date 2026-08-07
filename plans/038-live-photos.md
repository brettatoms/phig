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
  mis-pair the Canon `MVI_*.MOV` clips already in the collection. Not
  scheduled: the collection contains zero Live Photos today, so there is
  nothing to validate against. Three open decisions plus a real-file source
  come before implementation.
---

# 038 — Apple Live Photos

**Status is `designing`, and the build is gated on files existing.** As of
2026-08-06 the collection has zero HEIC rows and nine MOV rows, all of them
Canon camera clips (`MVI_*.MOV`, 2008–2014) — no Live Photos. Building against
synthetic fixtures alone would break the project's rule that data-safety
changes ship with real-behavior tests. This plan exists to record the design
and, more importantly, the Content Identifier finding, so neither has to be
rediscovered.

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
except that HEIC must be decodable at all (see open decision 3).

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

**3. HEIC decode.** Unverified. `heic`/`heif` are in the extension list, but
whether the OpenCV build actually decodes them was not established — the
`opencv4.pc` files found on this machine are stale Nix paths left over from
before plan 036 swapped devenv for mise, so the build's real capabilities were
not measured. libheif 1.23.1 *is* installed. If OpenCV cannot decode HEIC,
this plan has a prerequisite: a libheif decode tier structurally like plan
026's LibRaw tiers, which is its own piece of work and arguably its own plan.
**Measure before designing around it.**

## Before this becomes implementable

- **A real source of Live Photos.** Whether an iPhone feeding this collection,
  or an ingested library (iCloud/Takeout export — note Takeout mangles naming,
  which strengthens decision 1). Without files, goal 4 in particular cannot be
  verified: round-trip fidelity means an actual Apple client accepting the
  result.
- **Resolve decision 3 by measurement**, since it may insert a prerequisite
  plan ahead of this one.
- **Plan 031 (libphig operations surface)** for goal 3 — the GTK client cannot
  surface a Live badge or play motion until libphig exposes the pairing.
  031 is itself still `designing`.

## Dependencies

Depends on 025 (sidecar carry machinery), 026 (the master/attachment model and
its scan transitions), 023 (relink primitive for demotion). Goal 3 additionally
depends on 031. Possibly gated behind a HEIC decode plan, pending decision 3.

## Scope notes

Round-trip fidelity (goal 4) raises a question phig has not faced: whether it
ever *writes* Apple metadata. Today it writes none, for anything. If preserving
the pair requires rewriting a Content Identifier — e.g. after a rename — that
is a new capability and should be split out rather than smuggled in here.
