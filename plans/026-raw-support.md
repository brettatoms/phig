---
name: RAW Support (RAW as master, JPEG as attachment)
status: planned
created: 2026-07-11
description: >
  Treat a RAW + same-basename JPEG of one shot as a single logical photo: the
  RAW is the only indexed row; the JPEG is a carried attachment (no row), like a
  metadata sidecar. The RAW row's phash/thumbnail/faces come from the best
  pixel source — sidecar JPEG, else the RAW's embedded JPEG preview, else a
  LibRaw full decode (which also replaces OpenCV's unreliable RAW path). Scan
  demotes a JPEG to a sidecar when a same-basename RAW is present (transferring
  any user metadata to the RAW row via plan 023's primitive) and promotes a lone
  JPEG. Organize carries the JPEG with the RAW (plan 025's carry machinery).
  Adds LibRaw (thread-safe `libraw_r`) as a dependency.
---

# RAW Support

## Goal

Make phig treat a RAW file and its same-basename JPEG as **one photo**, with the
RAW as master. Today phig indexes every known image extension independently, so a
`IMG_1234.CR2` + `IMG_1234.JPG` pair would become two rows and cluster as a
near-duplicate. Worse, phig hashes/thumbnails via OpenCV `cv::imread`, which
fails on many RAW files ("Internal imread issue" on CR2), so RAW-only shots get
no reliable phash, thumbnail, or faces.

This plan establishes: the RAW is the shot's identity and only DB row; the JPEG
is a carried attachment used as the RAW row's pixel source; and RAW pixels come
from a JPEG-quality source (sidecar JPEG, embedded preview, or LibRaw decode)
rather than OpenCV's flaky RAW path.

## Model

**One shot, one row.**
- A RAW (`.cr2`, `.nef`, `.dng`, …) gets an `images` row — the shot's identity,
  metadata, and searchable record.
- A **same-basename JPEG in the same directory** is **not indexed**. It is a
  carried attachment of the RAW (like an `.xmp` sidecar under plan 025) and
  serves as the RAW row's pixel source.
- A **lone JPEG** (no RAW sibling) is a normal photo → its own row, as today.

**Pixel source** for the RAW row's phash / thumbnail / face detection, resolved
at scan time in priority order:
1. same-basename **sidecar JPEG** on disk;
2. the RAW's **embedded JPEG preview** (extracted via LibRaw);
3. **LibRaw full decode** of the RAW.

Tier 3 also **replaces OpenCV's current RAW `cv::imread` path**, fixing the
existing CR2 decode failures.

**Accepted give-up:** the JPEG is not independently searchable or taggable —
`search`/`tag` act on the RAW row. The RAW *is* the shot; this is intended.

## Scan & re-scan transitions

**Scan rule (stateless).** When scanning a file: a JPEG is indexed **unless a
same-basename RAW sits beside it** in the same directory. A RAW is always
indexed, with its pixel source resolved per the model. (Scan today processes
each file independently; this adds a per-file sibling check.)

**Re-scan transitions (lossless, via metadata transfer):**
- **RAW arrives beside an already-indexed JPEG.** The JPEG previously scanned
  alone has a row (possibly with tags / faces / favorite / gallery). On re-scan
  with the RAW now present: index the RAW, **transfer the JPEG row's
  user-authored metadata to the RAW row** (plan 023's `transfer_user_metadata`,
  triggered by same-basename pairing, not sha256), log a `relink` entry, and
  **delete the JPEG row** (it becomes a sidecar).
- **RAW deleted, lone JPEG remains.** The RAW row goes through normal stale-row
  removal; the now-lone JPEG is indexed as a normal photo (**promoted**).

## Move/organize, purge, duplicates

**Organize (as a unit).** Moving a RAW carries its same-basename JPEG and any
`.xmp`/other metadata sidecars to the RAW's destination, reusing plan 025's
`carry_sidecars` — the JPEG is one more carried attachment. Only the RAW has a
row, so there is no second row to repoint; the JPEG move is a `move_sidecar`
entry (025) linked to the RAW's image id. The pair is derived from the source
directory **at move time**, so it cannot desync.

**Purge.** `phig purge` removes DB rows only (never disk files). Only the RAW has
a row, so purging the shot purges the RAW row; the JPEG on disk is unaffected,
like any sidecar. No unit logic needed.

**Duplicates.** No RAW-specific handling. Only RAWs and lone JPEGs have rows, so
a RAW+JPEG pair never self-clusters as a spurious duplicate. The RAW's phash
comes from its sidecar JPEG (or preview), so genuine near-dups still cluster.

## LibRaw integration & build

**Dependency: LibRaw**, thread-safe variant **`libraw_r`** (scan runs
`--parallel`). Two roles:
1. extract the embedded JPEG preview (pixel-source tier 2);
2. full RAW decode (tier 3), replacing OpenCV's `cv::imread` for RAW.

**New pixel-source module** `src/raw.{h,cpp}`: given a RAW path, return a decoded
`cv::Mat` for hashing/thumbnail/faces by resolving the 3-tier priority (sidecar
JPEG → LibRaw preview → LibRaw decode). The hasher/thumbnail/face paths call this
instead of `cv::imread` when the file is a RAW.

**Build wiring (verified gotchas):**
- Link `libraw_r` via `pkg-config`. The stock `.pc` files ship correct
  Cflags/Libs (Arch confirmed: `libraw_r.pc`; CI ubuntu/macos per plan 018
  resolve normally).
- The **Nix devenv** must add `libraw` so `pkg-config` finds it in the dev shell
  (the system `.pc` lives in `/usr/lib/pkgconfig`, outside Nix's search path).
  A `devenv.nix` / `PKG_CONFIG_PATH` change — build config, not a design point.

RAW extensions are already in `scanner.cpp`'s known-image list, so RAWs are
already scanned. The change is *how their pixels are obtained* plus the
JPEG-demotion rule.

## Testing

Real-behavior tests (repo standing rule), fixtures in `tests/data/`:
- **Pixel-source resolution** — RAW + sidecar JPEG → uses the JPEG; RAW with only
  an embedded preview → uses the preview; RAW with neither → LibRaw decode.
  Needs a small real CR2 fixture (with and without a sidecar JPEG).
- **Scan demotion** — dir with `IMG_1.CR2` + `IMG_1.JPG` → one row (the RAW), no
  JPEG row; lone `IMG_2.JPG` → a row.
- **Re-scan transitions** — JPEG indexed, then RAW appears → JPEG's
  tags/faces/favorite/gallery transferred to the RAW row, JPEG row gone,
  `relink` logged; RAW deleted → lone JPEG promoted to a row.
- **Organize as a unit** — moving the RAW carries the JPEG (and `.xmp`) to the
  destination; `move_sidecar` entry linked to the RAW's image id.
- **LibRaw replaces OpenCV** — a CR2 that failed `cv::imread` now
  hashes/thumbnails via the new pixel-source path.

## Scope

Out of scope:
- JPEG as an independently searchable/taggable entity (intended — RAW is the
  shot).
- Storing the RAW↔JPEG pairing as a schema column (derived at scan/move time).
- Any change to lone-JPEG or non-RAW behavior beyond the demotion rule.
- Editing/developing RAW files; only reading pixels for hash/thumbnail/faces.

## Plan relationships

- **Depends on plan 023 (Scan Re-link).** Reuses its `transfer_user_metadata(
  from_id, to_id)` primitive and `relink` log entry for the demote case
  (triggered by same-basename pairing, not sha256). 023 lands first.
- **Reuses plan 025 (Sidecar Files).** The JPEG is carried on organize via 025's
  `carry_sidecars` / `move_sidecar` machinery, exactly like a metadata sidecar.
- **Amends plan 025.** 025's original "RAW+JPEG are two separate parents" stance
  is removed; a JPEG beside a RAW is the RAW's attachment, defined here. 025's
  metadata-sidecar machinery (`.xmp`/`.json` following parents, date fallback) is
  unchanged.

## Open questions to resolve during implementation

1. **RAW extension set for demotion.** Which extensions count as "a RAW" for the
   demotion rule — the full RAW list already in `scanner.cpp`, or a subset?
   Default: the existing RAW list.
2. **Embedded-preview quality.** LibRaw may expose multiple previews (thumb vs.
   half-size). Pick the largest JPEG preview available; settle the exact LibRaw
   call (`unpack_thumb` / `libraw_get_thumbnail`) against real files.
3. **Sibling lookup cost at scan.** Determining "is there a same-basename RAW/
   JPEG beside this file?" per file — do it via a per-directory index built once
   during the scan walk rather than a stat per candidate.
4. **Basename matching for demotion** — case sensitivity of the stem match
   (`IMG_1.CR2` vs `img_1.jpg`) and multiple RAWs sharing a stem; settle against
   real libraries. Default: case-insensitive stem, first RAW wins.
5. **Faces from the pixel source** — confirm face detection runs on the resolved
   pixel `cv::Mat` (JPEG/preview) and stores against the RAW row's image id.
