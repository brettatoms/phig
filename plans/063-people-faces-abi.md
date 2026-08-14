---
name: People & Faces ABI
status: planned
created: 2026-08-14
description: >
  Expose the people list, a person's images, one image's faces and the three
  labelling writes through libphig, returning bounding boxes so a client crops
  its own face thumbnails.
---

# 063 — People & Faces ABI

## Goal

047's Screen 4 has no data source. Reads (`list_people`,
`get_images_for_person`, `search_faces`, and the face rows for one image) and
writes (`set_face_person`, `identify_person`, `auto_label_face`) all exist in
`core/` and none is reachable.

## Shape

**Boxes, not crops.** Settled by 053's decision 5:
`Database::FaceSearchResult` already carries `face_x/y/w/h`, so the ABI returns
them and a client crops from the image it already has. Generating face
thumbnails would be defensible — `phig_thumb_generate` establishes that libphig
decodes — but it is more surface for less.

**Face ids are already safe to hold.** Plan 045 made `faces.id` AUTOINCREMENT
so a GUI can keep one across human-scale time, and R51 records it. 053's
decision 3 gives `images.id` the same property.

**Writes must report rows changed, not statements issued.** The face paths run
lock-free, so `set_face_person_if_unchanged` is a compare-and-set and a losing
write updates nothing. `libphig/CLAUDE.md` states the obligation; an ABI caller
inherits it.

## Scope

- Reads: people with counts, a person's images, one image's faces.
- Writes: name a face, identify a person, auto-label.
- Face-mode search is 053's, not this plan's — it is a query mode.

## Not in scope

- **What a faces rebuild may destroy.** R60 measured that a rebuild deletes
  labels a person typed, and R66's remaining gap is that `phig_op_faces_start`
  is destructive with no plan phase. Plan 060 owns both. This plan brings face
  labelling to the ABI; it does not decide what a rebuild is allowed to erase.
- **Merge, split and rename of clusters.** 047 describes them; `core/` has no
  primitive for any of the three, so each is engine work before it is ABI work.
- **The `vec_faces` delete failure** — 046.

## Depends on

053 — the selection surface. 060 — for the authorization question above, which
this plan must not answer by accident.
