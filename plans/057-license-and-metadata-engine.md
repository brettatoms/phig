---
name: License & Metadata Engine
status: designing
created: 2026-08-08
description: >
  phig has no LICENSE file, and that omission is quietly constraining technical
  choices. Every library it links is LGPL, which is what keeps its own licence
  open; adopting exiv2 — the obvious upgrade over libexif, and the only
  realistic route to XMP write-back — would make phig GPL by consequence rather
  than by decision. Surfaced by plan 038, which needed one Apple MakerNote tag
  libexif cannot read. Two questions, in order: what licence phig is, and
  whether its metadata engine should change.
---

# 057 — License & Metadata Engine

## Why this exists

Plan 038 needed the Apple MakerNote Content Identifier from a HEIC. libexif
cannot read it — measured 2026-08-08, `exif_data_get_mnote_data()` returns NULL
and `libexif.dylib` 0.6.26 exports zero Apple symbols, shipping only Canon,
Olympus and Pentax MakerNote parsers. exiv2 reads it correctly.

exiv2 is GPL-2.0-or-later. Every library phig links today is LGPL — libexif
(LGPL-2.1), libheif (LGPL-3.0), LibRaw (LGPL-2.1 / CDDL). That is not a
coincidence: LGPL permits linking from software under any licence, which is
what keeps phig's own licence an open question.

**phig has no LICENSE file.** So adopting exiv2 would settle phig's licence as
a side effect of a dependency choice. 038 routed around it instead, and this
plan exists so the decision is made deliberately.

## Question 1: what licence is phig?

Unanswered, and it needs answering regardless of exiv2 — a public repository
with no LICENSE grants no rights to anyone who finds it. Default copyright
means nobody may legally use, modify or redistribute it.

The choice constrains dependencies, not the other way round:

| If phig is | exiv2 available | Dependency rule |
|---|---|---|
| GPL | yes | GPL and LGPL both fine |
| LGPL | no | LGPL and permissive only |
| MIT / Apache | no | LGPL (dynamically linked) and permissive |
| Proprietary / undecided | no | LGPL and permissive only |

Recorded 2026-08-08: **undecided, keep options open** — so LGPL-and-permissive
remains the working constraint, and exiv2 stays out until this is settled.

### Note on dynamic linking

A recurring misconception is that dynamically linking a GPL library avoids its
obligations. It does not. The FSF's position, and the conventional reading, is
that linking either way produces a derivative work; what triggers the
obligation is distributing the combined work. The distinction that actually
matters is GPL versus **LGPL**, which exists precisely to permit this.

Two genuine exceptions, neither useful here: a separate process (shelling out
to the `exiv2` CLI — correct, but a process spawn per file is hopeless at scan
scale), and a user-supplied library never distributed in combination (grey, and
void the moment a `.app` bundle or package ships).

## Question 2: should the metadata engine change?

Only reachable if question 1 permits it. exiv2 would bring:

- **Apple MakerNote support** — the tag 038 wanted, read properly rather than
  by containment.
- **Broad MakerNote coverage** — Nikon, Sony, Fujifilm, Panasonic and others
  that libexif does not parse, which matters for a general-purpose DAM.
- **XMP support, read and write.** `MARKET.md` records tag write-back to
  XMP/IPTC as repeatedly requested — Lap users want it, DAM buyers expect it —
  and phig currently stores tags only in `phig.db`. libexif cannot do this at
  all, so XMP write-back is blocked on this decision.
- Native HEIF handling, overlapping what plan 048 built.

### The cost is a migration, not a link line

`core/exif.cpp` keys the stored JSON with libexif's tag names via
`exif_tag_get_name_in_ifd`. exiv2 uses a different scheme (`Exif.Image.Make`).
Swapping rewrites **every key in the `metadata` column**, which:

- breaks every `json_extract()` predicate in `SearchEngine`,
- invalidates existing databases, requiring a schema migration and a full
  re-scan of every file to repopulate,
- changes what libphig returns to clients, so `gtk/` and the macOS app see it.

That is a substantial plan in its own right, which is the second reason 038 did
not absorb it.

## Open questions

1. **What licence?** Product decision. Note that the LGPL constraint is already
   load-bearing, so choosing permissive or LGPL mostly ratifies current
   practice, while GPL opens exiv2 and XMP write-back.
2. **If exiv2: swap or supplement?** Running both — libexif for the existing
   JSON keys, exiv2 only for what it cannot read — avoids the migration but
   means two metadata libraries and two tag vocabularies in one codebase.
3. **Does XMP write-back want exiv2 specifically?** It is the obvious route,
   but writing a narrow XMP sidecar is not obviously beyond a smaller
   dependency, and phig already writes no metadata to anything.
4. **Copyright holder and year** for the LICENSE file itself.

## What this defers

- **Any change to 038.** Its containment-based pairing needs no new dependency
  and works today; if exiv2 later lands, the still side can read tag 0x0011
  behind the same interface.
- **XMP write-back design** — a product feature, tracked separately once the
  engine question is answered.

## Cross-references

- `plans/038-live-photos.md` — surfaced this; documents the libexif measurement
  and the containment mechanism adopted instead.
- `plans/048-modern-image-codecs.md` — the precedent for rejecting hand-rolled
  parsing of untrusted binary input, and phig's other metadata dependency work.
- `MARKET.md` — the XMP/IPTC write-back demand.
