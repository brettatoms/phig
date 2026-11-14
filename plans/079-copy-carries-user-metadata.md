---
name: Copy Carries User Metadata
status: planned
created: 2026-11-14
description: >
  `phig cp` gives the copy a catalog row filled from the original's, but leaves
  out the original's tags, favorite, gallery and faces, so a tagged photo
  arrives at its destination bare. Make every copy carry them, including a copy
  that adopts an identical file already at the destination (plan 076).
  Excludes writing metadata into the files or sidecars, which phig does not do.
---

# 079 — Copy Carries User Metadata

## Problem

Tags, favorites, galleries and faces exist only in the catalog. `phig cp`
copies a photo's file and then adds a row for the copy with `insert_copy`
(`core/database.cpp`), which fills it from the original's row: path, size,
dates, hashes, EXIF and dimensions. It does not copy the original's
`image_tags` rows, its `gallery` column or its `faces` rows. The copy is
indexed but carries none of what the user added.

The same happens on plan 076's identical-destination path:

- **The destination is untracked.** The copy adopts it through `insert_copy`,
  so it arrives bare in the same way.
- **The destination is tracked.** Nothing changes, so the original's tags and
  gallery never reach the destination's row.

A move does not have the problem: the row itself moves, or, on 076's path, R37
merges it into the destination's row.

This came up while planning a consolidation that sets galleries from folder
names and then copies everything into a new tree. Copying is the safe way to do
that, since the originals stay untouched until the result is checked, but the
galleries would not survive the copy.

## Decisions

1. **A copy carries the original's tags, favorite, gallery and faces.** The
   favorite is the built-in `favorite` tag, so it travels with the tags. A
   face is copied with its box, embedding, provenance and person label:
   a new `faces` row and its `vec_faces` entry, written through `insert_face`
   so the two stay in step. The copy's faces are its own: renaming or removing
   one never affects the original's.
2. **It happens in the transaction that inserts the copy's row**, beside the
   log entry (R18), in a new `Database::copy_user_metadata(from_id, to_id)`.
   A copy whose metadata cannot be written is rolled back like any other failed
   copy.
3. **Adopting an untracked identical file carries the same set.** 076's `adopt`
   path runs `insert_copy` and then `copy_user_metadata`.
4. **Copying onto an identical tracked file merges into its row**, as R37 does
   for a move but without removing anything from the original: tags are
   unioned, the gallery fills only an empty one, and faces are copied only
   when the destination has none. 076's blockers already route a gallery
   conflict, or named faces against existing faces, to the conflict policy, so
   this merge never drops anything. It is logged as `copy_metadata`.
5. **No ABI change.** libphig's copy runs the same executor.

## Rules

- New rule, concern `metadata`, binding `organize.copy`: a copy's row carries
  the original's tags, favorite, gallery and faces, and a copy onto an identical
  tracked file merges them into its row.
- R40 is unaffected: it covers the snapshot written when a copy's row replaces
  a row at the destination path.

## Testing

Real-behavior tests against the engine and the built binary, each failing first:

- `cp` of a photo with tags, the favorite, a gallery and a named face: the
  copy's row has all of them, and the original's are unchanged.
- Renaming the person on the copy's face leaves the original's face alone.
- `cp` onto an identical untracked file: the adopted row carries the set.
- `cp` onto an identical tracked file: tags unioned, an empty gallery filled,
  faces copied only when the destination has none.
- A failure writing the metadata rolls the copy back: no file, no row.
- `phig cp` through the binary, then `phig search --tag` and `--gallery` on the
  destination, find the copy.

## Relationship to other plans

- **076** added the identical-destination path whose copy branch this extends.
- **020** (activity log) gains the `copy_metadata` entry kind; **065**'s undo
  reader needs to know it.
