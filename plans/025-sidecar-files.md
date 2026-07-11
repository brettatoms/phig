---
name: Sidecar Files (carry on move/copy + date fallback)
status: planned
created: 2026-07-11
description: >
  Make phig sidecar-aware. A new pure `sidecar` module pairs a media file with
  its sidecars (both `IMG_1234.xmp` stem and `IMG_1234.CR2.xmp` full-name styles,
  known-extension allowlist) and reads an organizing date from them. On mv/cp,
  each parent's sidecars travel with it, following the parent's fate (skip /
  overwrite / rename / dry-run) and recorded as `move_sidecar` / `copy_sidecar`
  log entries linked to the parent's image id for safe undo. On organize, a
  sidecar date fills the date fallback chain after EXIF and before mtime, fixing
  EXIF-stripped photos that misfile under the copy date.
---

# Sidecar Files

## Goal

phig currently ignores sidecar metadata files. Two concrete failures followed
from that:

1. **Orphaned sidecars.** `phig mv` moved a tree of photos and left every f-spot
   `.xmp` sidecar behind, because `.xmp` is not a known media extension. All the
   ratings/captions/edits were stranded from their images.
2. **Misfiled by date.** EXIF-stripped exports (`image54.jpg`, `metadata = {}`)
   have no embedded date, so organize fell through to file mtime — the *copy*
   time — and filed 2010 photos under `2026/07`. The real date sat in the `.xmp`
   sidecar, which phig never read.

This plan makes phig understand sidecars as first-class companions of a media
file, without indexing them as media. One new module provides the shared
primitive; the mv/cp path and the organize date logic both build on it.

## Scope

In scope:
- New `sidecar.{h,cpp}` module with two pure functions: `find_sidecars` and
  `sidecar_date`.
- mv/cp carry each parent's sidecars, following the parent's fate.
- Sidecar moves/copies logged as `move_sidecar` / `copy_sidecar` entries linked
  to the parent's image id, undoable.
- Organize date fallback consults sidecar dates after EXIF, before mtime.

Out of scope:
- Indexing sidecars into the `images` table (no row, hash, thumbnail).
- A sidecar-specific conflict prompt — sidecars inherit the parent's
  `--on-conflict` mode.
- Any change to the mtime fallback itself (mtime stays trusted as-is).
- Reading sidecar fields beyond the organizing date (rating / tags / GPS).
- The source-folder-name date heuristic.
- Correcting the already-misfiled `2026/07` files (a separate, manual cleanup).
- **The RAW+JPEG relationship.** How a same-basename JPEG relates to a RAW of
  the same shot is defined by plan 026 (RAW support), not here. This module only
  concerns *metadata* sidecars (`.xmp`, `.json`, etc.). Under plan 026 the RAW is
  the indexed shot and a same-basename JPEG is one of its carried attachments;
  see 026. This plan does **not** treat `.jpg`/`.cr2` as sidecars of each other.

## The `sidecar` module (shared primitive)

`src/sidecar.{h,cpp}`, two pure path-in/data-out functions — no DB, no move
executor — so they test with tmpdir fixtures alone.

```cpp
// Existing sidecar files that belong to `media_file`. Matches both naming
// conventions against the known-sidecar allowlist:
//   stem:        IMG_1234.xmp        (basename replaced with sidecar ext)
//   full-append: IMG_1234.CR2.xmp    (full filename + sidecar ext)
// A media file's own extension (.cr2/.jpg/...) is a *parent* extension, never a
// metadata sidecar. This function pairs a media file with its *metadata*
// sidecars only (allowlist below); the RAW↔JPEG same-shot relationship is plan
// 026's concern, not this module's.
std::vector<fs::path> find_sidecars(const fs::path& media_file);

// Organizing date from a sidecar, or "" if none. Parses:
//   XMP   — xmp:CreateDate / exif:DateTimeOriginal
//   JSON  — Google Takeout photoTakenTime (formatted timestamp)
// Returns a string parseable by the existing parse_date (EXIF or ISO shape).
std::string sidecar_date(const fs::path& sidecar_file);
```

**Known sidecar extensions (allowlist):** `xmp, pp3, dop, on1, aae, json, thm,
lrv`. Matching is case-insensitive (mirrors `is_known_*_extension`).

**Matching detail.** For a media file `DIR/STEM.EXT` (e.g.
`.../IMG_1234.CR2`), a directory entry `DIR/NAME` is a sidecar iff its extension
is on the allowlist AND either:
- **stem style:** `NAME` without its sidecar extension equals `STEM`
  (`IMG_1234.xmp` → `IMG_1234`), or
- **append style:** `NAME` without its sidecar extension equals `STEM.EXT`
  (`IMG_1234.CR2.xmp` → `IMG_1234.CR2`).

Both styles are checked, so a RAW and a JPEG that each carry their own metadata
sidecar (`IMG_1234.CR2.xmp` vs `IMG_1234.xmp`) resolve each `.xmp` to the right
file. This is about pairing *metadata sidecars* to a media file — it does not
make the JPEG and RAW sidecars of each other. Their same-shot relationship is
plan 026's.

**Date parsing.** `sidecar_date` reuses the shape `parse_date` already accepts
(`YYYY:MM:DD HH:MM:SS` or ISO). XMP dates are read from the `xmp:CreateDate` /
`exif:DateTimeOriginal` attributes; Google JSON from `photoTakenTime.formatted`
or the epoch `timestamp`. Malformed / dateless sidecars return `""`.

## Move/copy path

The organize executor already, per matched media file: resolves the
destination, applies conflict mode, does the disk op, updates the DB row, and
writes a log entry. Sidecars hook in **per parent**, right after the parent's
own op resolves:

1. Resolve the parent's destination and **final** name first, including any
   rename-to-avoid-conflict (`IMG_1234.JPG` → `IMG_1234-1.JPG`).
2. `find_sidecars(parent_src)` lists the sidecars beside the source.
3. For each sidecar, derive its destination name from the parent's **final**
   name:
   - stem style: `IMG_1234.xmp` → `IMG_1234-1.xmp`
   - append style: `IMG_1234.CR2.xmp` → `IMG_1234-1.CR2.xmp`
     (rewrite the leading stem, keep the `.CR2.xmp` tail)
4. The sidecar follows the parent's fate:
   - parent **moved/copied** → sidecar moved/copied the same way;
   - parent **skipped** → sidecar untouched;
   - parent **overwritten** → sidecar overwrites too;
   - **dry-run** → sidecar shown in the plan, nothing moved.
5. Each real sidecar op writes its own log entry:
   - `kind = 'move_sidecar'` (mv) or `'copy_sidecar'` (cp),
   - `src` / `dst` = the sidecar paths,
   - `restore_json = {"parent_image_id": <id>}` via the existing
     `get_image_id(parent_dst)` (the parent row is already at its new path when
     the sidecar entry is written).

The sidecar is never inserted into `images`. It exists as files on disk plus the
log entry.

**Conflict edge case.** A sidecar destination derived from a *renamed* parent is
collision-free by construction (the parent's fresh name was chosen to be free).
Under `overwrite`, a pre-existing sidecar at the target is overwritten, matching
the parent. There is no independent sidecar conflict resolution.

## Undo path

`undo` replays an operation's entries in reverse; today a `move`/`copy` entry
moves the file back and restores the images row from `restore_json`. Sidecar
entries add one branch:

- **`move_sidecar`:** move the file back `dst → src`. **No DB-row restore** —
  there is no images row. `parent_image_id` in `restore_json` is informational
  (grouping/display), not replayed.
- **`copy_sidecar`:** delete the copied-in `dst`, as `copy` undo does today,
  minus DB work.

Entries replay in reverse insertion order and sidecars are inserted right after
their parent, so no special ordering is needed. Undo's existing
verify-and-refuse-on-conflict guard applies: if a sidecar's `dst` changed or its
`src` is now occupied, that entry refuses rather than clobbering.

## Organize date fallback

`get_best_date` (`src/main.cpp`) gains one step, inserted after the EXIF checks
and before mtime:

```
1. EXIF DateTimeOriginal          (unchanged)
2. EXIF DateTimeDigitized         (unchanged)
3. Sidecar date  ← NEW           find_sidecars(img.path) → first valid
                                  sidecar_date(...)
4. File mtime                     (unchanged — still trusted)
5. otherwise → unsorted/          (unchanged)
```

EXIF wins over the sidecar: the sidecar only speaks when the embedded EXIF has
no date. Files with neither EXIF nor a dated sidecar (e.g. `image54.jpg`) still
fall to mtime exactly as today — an accepted limitation, not solved here.

## Testing

Real-behavior tests (this repo's standing rule for data-safety changes):

`find_sidecars` (tmpdir fixtures):
- stem style `IMG_1234.xmp` attaches to `IMG_1234.JPG`;
- append style `IMG_1234.CR2.xmp` attaches to `IMG_1234.CR2`;
- RAW+JPEG both present with distinct sidecars — each attaches to the right
  parent, not the other;
- same-stem non-allowlist file (`IMG_1234.txt`) is **not** returned;
- no sidecar → empty; multiple sidecars for one file → all returned;
- case-insensitive extension (`.XMP`).

`sidecar_date`:
- real f-spot XMP fixture (`xmp:CreateDate="5/17/2010 ..."` shape we observed) →
  correct date;
- Google Takeout JSON `photoTakenTime` → correct date;
- dateless / malformed sidecar → `""`.

Move/copy integration (porcelain-style):
- parent moved → sidecar moved beside it; log has `move_sidecar` with
  `parent_image_id`;
- parent renamed (conflict) → sidecar renamed to match (`-1`);
- append-style sidecar renamed correctly on parent rename;
- parent skipped → sidecar untouched at source;
- dry-run → nothing moved, sidecar appears in preview;
- overwrite mode → sidecar overwrites.

Undo integration:
- `move_sidecar` reversed — file back at source, no DB row touched, parent row
  restored by its own entry;
- `copy_sidecar` reversed — copied sidecar deleted;
- conflict (sidecar dst changed) → that entry refuses.

Date fix:
- file with dated sidecar + no EXIF → filed by sidecar date, not mtime;
- file with EXIF date + a differently-dated sidecar → filed by EXIF (sidecar
  ignored);
- file with neither → mtime (unchanged behavior).

## Checklist

- [ ] `src/sidecar.{h,cpp}`: `find_sidecars` (both naming styles, allowlist,
      case-insensitive) and `sidecar_date` (XMP + Google JSON).
- [ ] Wire sidecar carry into the organize executor, per parent, after the
      parent's op resolves; derive dest name from the parent's final name for
      both naming styles.
- [ ] Sidecar follows parent fate (move/copy/skip/overwrite/dry-run).
- [ ] Log `move_sidecar` / `copy_sidecar` entries with
      `restore_json = {"parent_image_id": <id>}`.
- [ ] Undo branch for `move_sidecar` (file-only) and `copy_sidecar`
      (delete copy); verify-and-refuse guard applies.
- [ ] Insert sidecar-date step into `get_best_date` after EXIF, before mtime.
- [ ] Tests above pass.
- [ ] CMakeLists: add `sidecar.cpp` to the library and its test file.
- [ ] AGENTS.md "Key Design Decisions" + README note that mv/cp carry sidecars
      and organize reads sidecar dates.

## File structure

- `src/sidecar.h` / `src/sidecar.cpp` — the module (new).
- `src/main.cpp` — sidecar carry in the organize executor; `get_best_date`
  fallback step; undo branch.
- `src/database.*` — no schema change (`kind` is free text; `restore_json` is
  free-form JSON). Undo replay gains the sidecar branch wherever it lives.
- `tests/test_sidecar.cpp` — `find_sidecars` / `sidecar_date` unit tests (new).
- `tests/test_organize.cpp` (or `test_porcelain.cpp`) — carry/undo/date
  integration.
- `CMakeLists.txt` — new source + test.
- `AGENTS.md`, `README.md` — document sidecar handling.

## Open questions to resolve during implementation

1. Where the undo replay currently lives (plan 020 deferred `phig undo`; confirm
   whether undo exists yet or this plan lands the sidecar branch alongside the
   first undo implementation). If `undo` is not yet implemented, this plan
   only needs to *write* correct `*_sidecar` entries; the reverse-replay branch
   lands with whatever plan implements `undo`.
2. XMP date attribute precedence when a sidecar carries several
   (`xmp:CreateDate` vs `exif:DateTimeOriginal` vs `photoshop:DateCreated`) —
   pick the most specific capture date; settle exact order against real
   fixtures.
3. Whether `find_sidecars` should be called once per parent and threaded through
   both the carry step and (if the same file is organized) the date step, to
   avoid a second directory stat.
