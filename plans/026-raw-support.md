---
name: RAW Support (RAW as master, JPEG as attachment)
status: completed
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

**Scan rule (stateless, disk-based — order-independent).** When scanning a file:
a JPEG is indexed **unless a same-basename RAW sits beside it ON DISK** in the
same directory. A RAW is always indexed, with its pixel source resolved per the
model.

The sibling check queries the **filesystem**, not the DB and not processing
order. This matters because scan enumerates files in unsorted filesystem order
and processes them across a thread pool — the JPEG and its RAW may be handled in
either order or concurrently. Since both files are on disk for the entire scan,
"does a same-basename RAW exist on disk?" returns the same answer no matter which
thread runs first or whether the RAW has been indexed yet. Seeing the JPEG
before the RAW is therefore not a problem: the JPEG is skipped because the RAW is
*present on disk*, never because the RAW was *already indexed*.

**`--media image` interaction.** The RAW extensions are image extensions, so a
default scan sees both. If a `--media`/match filter excludes the RAW from the
scanned set while including the JPEG, the disk check still sees the RAW on disk
and demotes the JPEG — the JPEG's row is not created even though the RAW won't be
indexed this run. Settle in planning whether the demotion should require the RAW
to actually be in the scanned set (default: demote on disk presence, matching the
"one shot" intent; the RAW gets indexed on a later unfiltered scan).

**Re-scan transitions (lossless, via metadata transfer).** These are the hard,
concurrency-sensitive cases and the exact hook point is a planning decision (see
open question 6):
- **RAW arrives beside an already-indexed JPEG.** The JPEG previously scanned
  alone has a row (possibly with tags / faces / favorite / gallery). On re-scan
  with the RAW now present: index the RAW, **transfer the JPEG row's
  user-authored metadata to the RAW row**, log a `relink` entry, and **delete the
  JPEG row** (it becomes an attachment). Key subtlety: because processing is
  parallel, the thread that decides to *skip* the JPEG and the thread that
  *indexes* the RAW are different and may run concurrently — so the transfer
  cannot be driven inline from the JPEG's skip decision. Two candidate
  mechanisms, decided in planning:
  - **(a) Reconcile at scan cleanup (recommended).** The RAW indexes as master;
    the now-unindexed JPEG becomes a stale row at end-of-scan cleanup (the same
    path that removes rows for vanished files), and its metadata transfers to the
    RAW **by basename** before its row is deleted. Order-independent, reuses the
    cleanup path. NOTE: plan 023's `relink_moved_rows` triggers on identical
    **sha256** — a RAW and its JPEG differ, so 023's existing trigger will NOT
    fire. This needs a **basename-triggered** transfer that reuses 023's
    `transfer_user_metadata(from_id, to_id)` primitive and `relink` log entry,
    but with a same-basename-RAW-on-disk trigger instead of the sha256 one.
  - **(b) Transfer inline during RAW processing.** The RAW's processing looks up
    a same-basename JPEG row and absorbs it then. More immediate but races the
    JPEG's own skip thread under `db_mutex`.
- **RAW deleted, lone JPEG remains.** The RAW row goes through normal stale-row
  removal; the now-lone JPEG is indexed as a normal photo (**promoted**) — the
  disk check no longer finds a RAW, so the JPEG is indexed on the next scan.

## Move/organize, purge, duplicates

**Organize (as a unit).** Moving a RAW carries its same-basename JPEG and any
`.xmp`/other metadata sidecars to the RAW's destination. Only the RAW has a row,
so there is no second row to repoint; each carried file is logged as a
`move_sidecar`/`copy_sidecar` entry (025) linked to the RAW's image id. The pair
is derived from the source directory **at move time**, so it cannot desync.

**Mechanism gap to close (planning).** Plan 025's `carry_sidecars` finds what to
carry via `find_sidecars`, which matches only the **metadata-sidecar allowlist**
(xmp/pp3/dop/on1/aae/json/thm/lrv). `.jpg` is deliberately NOT on that list (a
lone JPEG is a real photo, not a sidecar), so `carry_sidecars` as it exists will
**not** pick up the attached JPEG. The design intent — "carry the JPEG via 025's
machinery" — therefore needs an explicit **attachments pass**: when moving a RAW,
also carry the same-basename JPEG (found by basename, not the sidecar allowlist),
logged the same way. This is a small generalization of the carry step (extend
`carry_sidecars`, or a sibling `carry_attachments`, that adds the same-basename
JPEG to the set). Do NOT add `jpg` to the sidecar allowlist — that would make
`find_sidecars` treat any same-stem JPEG as a metadata sidecar of any media file
and misclassify lone JPEGs. Settle the exact shape in planning.

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
6. **Transition transfer hook (RAW arrives beside an already-indexed JPEG).**
   Mechanism (a) reconcile-at-cleanup vs (b) inline-during-RAW. Recommended (a):
   a **basename-triggered** metadata transfer at end-of-scan cleanup reusing
   023's `transfer_user_metadata` + `relink` entry (023's own trigger is sha256,
   which won't fire for RAW≠JPEG). Settle where it hooks into the cleanup path.
7. **Carry the attached JPEG on mv/cp.** `carry_sidecars` only matches the
   sidecar allowlist; add an attachments pass so the same-basename JPEG travels
   with the RAW (basename-matched, logged as `move_sidecar`/`copy_sidecar`). Do
   not pollute the sidecar allowlist with `jpg`.
8. **`--media`/filter + demotion.** Whether demoting a JPEG requires the RAW to
   be in the scanned set or just present on disk (default: on-disk presence).

---

## Outcome (2026-07-12)

Implemented via subagent-driven development (8 tasks + 2 review fixes), each with
real-behavior tests + spec/quality review; final whole-branch review returned
READY TO MERGE (thread-safety, demotion/transfer composition, and carry
round-trip all verified sound). Full suite: 348 tests passing. Depends on plan
023 (`transfer_user_metadata`) and reuses plan 025 (`carry_sidecars`).

Commits (on `master`):

| Task | Commit(s) |
|------|-----------|
| 1 — LibRaw dependency (devenv/CMake `libraw_r` var-style/CI/README) | `546f593` |
| 2 — `src/raw.{h,cpp}` (`is_raw_extension` + `raw_decode` 3-tier) | `f07218b` |
| 3 — route RAW through `decode_image_with_warning`; faces.cpp → `decode_image` | `a0667ec` |
| 4 — scan JPEG-demotion (`has_sibling_raw`, disk-based, order-independent) | `822d0cb` |
| 5 — `demote_jpeg_rows` transfer at cleanup + restructure | `9ee08eb`; `889cd44` (only demote a JPEG whose file still exists — fixes a double-count found in review) |
| 6 — carry a RAW's attached JPEG on mv/cp (attachments pass) | `85f9a28` |
| 7 — comment/duplicates verification (NO-OP: no stale comments; duplicates needs no change) | — |
| 8 — binary e2e (proved demotion + carry) + docs | `c5155fa` |

Model as built:
- **RAW is the only indexed row.** A same-basename JPEG in the same directory is
  NOT indexed (scan skips it via `has_sibling_raw` — a disk check, so it's
  parallel- and order-independent). A lone JPEG stays a normal photo.
- **Pixel source** for the RAW row's phash/thumbnail/faces/blur: sidecar JPEG →
  LibRaw embedded preview → LibRaw full decode, resolved in `raw_decode` and
  routed at the single decode chokepoint `decode_image_with_warning`. This
  replaces OpenCV's flaky `cv::imread` for RAW. The RAW's `sha256` is still the
  RAW file's bytes (dedup identity); its phash/thumbnail come from the JPEG
  pixels (visual similarity) — intended.
- **Transition** (JPEG indexed alone, then a RAW appears): `demote_jpeg_rows` at
  scan cleanup transfers the JPEG row's tags/faces/gallery to the RAW row (023's
  `transfer_user_metadata`, basename-triggered — 023's sha256 trigger doesn't
  fire for RAW≠JPEG), logs a `relink` entry, deletes the JPEG row. Runs after
  workers join, so the RAW row is committed before the lookup.
- **Organize** carries the JPEG (Task 6 attachments pass) and any `.xmp` (025) to
  the RAW's destination, logged as `move_sidecar`/`copy_sidecar`. mv-then-rescan
  is stable (the JPEG is re-demoted at the destination).

Notes / accepted:
- **LibRaw success paths** (valid CR2 → embedded preview / full decode) are NOT
  unit-tested — the only committable CR2 fixture (`tests/data/truncated_ojpeg.cr2`)
  is truncated and undecodable even by LibRaw, so it only exercises graceful
  failure. Success is covered by real-world use and the Task 8 binary e2e. A
  small valid CR2 fixture would close this gap (deliberately not committed to
  avoid multi-MB repo bloat).
- **Non-blocking follow-ups** from the whole-branch review: per-file
  `directory_iterator` for the sibling check (vs. a per-directory index —
  design open question 3); a demoted JPEG's faces are dropped when the CR2 was
  scanned `--faces` (equivalent pixel source, acceptable); raw.cpp's tier
  imread/imdecode lack `StderrSuppressor` (cosmetic stderr leak on a broken
  sidecar).
