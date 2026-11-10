---
name: People & Faces ABI
status: designing
created: 2026-08-14
closes_rules: [R60, R66]
description: >
  Expose the people list, a person's images, one image's faces and the three
  labelling writes through libphig, returning bounding boxes so a client crops
  its own face thumbnails. Also decides what authorizes a faces rebuild to drop
  a label it cannot carry (R60, and the faces half of R66), moved here from
  plan 060.
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

## Decision required: what authorizes a rebuild to drop a label

Moved here from plan 060 on 2026-11-10. This plan does not ship until it is
answered, because it puts face labelling in clients' hands and a rebuild is the
one operation that erases those labels.

**Where it stands.** Plan 060's R60 fix carries labels through a rebuild inside
each image: by embedding when the recognition model is unchanged, by box overlap
when the model changed and the recorded frame is identical. A label that matches
nothing is dropped, counted in `FacesResult::labels_dropped` and
`phig_faces_result.labels_dropped`, and warned about by `face rebuild`. Nobody
authorizes the drop beforehand.

**What the rules require.** R60: "An operation that destroys user-authored
metadata is authorized before it runs, by an explicit yes or by a plan the
caller approves." It is `partial` for exactly the unauthorized drop. R66
requires a destructive ABI operation to be authorized by a plan the caller
inspects and applies, and is `partial` for two causes: `phig_op_faces_start`
has no plan phase, and remove is not exposed through the ABI at all (064). This
plan claims both records; if it completes with only the faces half of R66
closed, the claim on R66 moves to 064.

**The shapes considered when 060 handed this over:**

1. **Plan/apply for faces.** A plan step detects and matches without writing and
   lists the labels that would not carry; an apply step writes, re-checking each
   file is unchanged. Closes R60 and the faces half of R66. Costs the most:
   detection results for every selected image are held in the plan or computed
   twice, and the ABI gains plan and apply symbols.
2. **Refuse the loss unless authorized.** A rebuild leaves untouched any image
   where a label would not carry and reports it; the caller re-runs with those
   images explicitly authorized, by a `--yes`-style option or an id list. Nothing
   is lost without a yes, which meets R60. R66 asks for a plan value, so its
   faces half would stay open.
3. **Keep the current behaviour** and amend nothing: report after the fact.
   Leaves R60 and R66 `partial`, which needs a plan to keep claiming them.

060's R60 decision rejected plan/apply "for now", on the ground that once
matching runs inside the rebuild little is left to authorize, and that a plan
could not list the lost labels without running the matching first. Shape 2
answers the second point without a plan object.

## Not in scope

- **Merge, split and rename of clusters.** 047 describes them; `core/` has no
  primitive for any of the three, so each is engine work before it is ABI work.
- **The `vec_faces` delete failure** — 046.

## Depends on

053 — the selection surface. 060 (completed) — built the label carry the
decision above starts from.
