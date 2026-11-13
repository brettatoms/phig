---
name: Scan Keeps Face Labels on a Content Change
status: completed
created: 2026-11-13
completed: 2026-11-13
description: >
  Re-indexing a file whose contents changed deletes every face row for the
  image, person labels included, so an edited photo loses the names on it.
  Keep the labelled faces through the re-index, and let re-detection carry the
  labels onto the new faces the way face rebuild already does. Gates the scan
  in plan 071. Excludes what authorizes dropping a label that cannot be carried,
  which plan 063 decides.
---

# 072 — Scan Keeps Face Labels on a Content Change

## Problem

`Database::insert` runs for every file a scan indexes. When the row already
exists and the file's sha256 differs from the recorded one, it calls
`delete_faces_for_image` (`core/database.cpp`), which deletes every face row for
the image: boxes, embeddings and the `person_id` someone typed. R36 records this
as intended, on the ground that the boxes describe pixels that no longer exist.

That reasoning holds for boxes and embeddings, which a re-detection rebuilds. It
does not hold for labels, which nothing rebuilds. An edited, re-saved or
re-exported photo loses every name on it the next time any scan sees it: the
CLI's `scan`, libphig's index operation, and the GTK scan plan 071 adds.

Plan 060 built label carry for face rebuild and excluded this case. Rebuild can
already carry a label onto a changed file by embedding when the recognition
model is unchanged
(`FacesEngineRun.RebuildCarriesALabelWhenTheImageIsReplacedByARescaledCopy`),
but by then scan has deleted the labelled faces it would carry from.

No rule flags it. R36 states the deletion as the rule, and R60, which requires
authorization before user-authored metadata is destroyed, binds only
`faces.rebuild`.

## Design

1. **On a content change, scan keeps the labelled faces.** `Database::insert`
   deletes only the image's unlabelled faces, which carry nothing a person typed.
   A labelled face keeps its box, embedding and model, and is marked as measured
   on a replaced frame: `frame_source` becomes `replaced` and the frame's hash and
   size become unknown. Amended during implementation: comparing a face's recorded
   frame hash with the row's would mark every RAW face replaced, since a RAW's
   faces are measured on its sidecar JPEG or embedded preview and never on the
   file. With the frame unknown, `carry_labels` matches a kept face by embedding
   only, which is right: the frame it was measured in is gone.
2. **Re-detection carries the labels.** When a scan with faces enabled re-indexes
   a changed file, it detects, runs `carry_labels` from the kept labelled faces
   onto the new detections with the same rules and thresholds as face rebuild,
   writes the new faces, and deletes the kept ones, in one transaction per
   image. Today that path detects only when `has_faces` is false; it changes to
   detect whenever the image's faces were not measured in its current frame. A
   label that cannot be carried is dropped and counted in a new
   `ScanResult::labels_dropped`, and the CLI warns, as `face rebuild` does.
3. **Without face detection, the kept faces wait.** A scan without faces, which
   includes libphig's index and plan 071's GTK scan, leaves the kept labelled
   faces in place. The next face rebuild, or a scan with faces, carries them as in
   2.
4. **Authorization stays with 063.** Dropping a label in 2 is the same act as
   dropping one in face rebuild, unauthorized and reported afterwards. R60 gains
   `scan.walk` in its bindings, so 063's decision on what authorizes a drop
   covers both paths.

## What a kept face means meanwhile

Between the re-index and the next re-detection, a kept face describes the old
pixels:

- **Its box** may be in the wrong place on a cropped or rotated edit. Nothing in
  `core/` positions anything by it, and plan 071's client shows no faces.
- **Its embedding** is still that person's face, from the earlier version of the
  photo. As a reference for `auto_label_face` and `identify` it still matches
  the right person, and face search may return the image for a face that the
  edit cropped out.
- **The person's image list** still includes the image, which is right unless
  the edit removed them.

## Rules

- **R36** changes: a re-scan of a changed file clears its unlabelled face data
  and keeps labelled faces until re-detection carries or reports them; an
  unchanged file keeps everything, as now.
- **R60** gains `scan.walk` in `binds` and stays `partial`, claimed by 063.

## Testing

Real-behavior tests against a real database, without needing the face models
except where detection runs:

- A labelled face survives `insert` of a changed sha256 with its id, box and
  `person_id`; an unlabelled face on the same image does not.
- An unchanged sha256 keeps both, as now.
- A scan without faces of an edited file leaves the labelled face in place.
- With the models, a scan with faces of a rescaled copy carries the label onto
  the re-detected face and deletes the kept one; a label with no match is
  dropped and counted in `labels_dropped`.
- Face rebuild after a scan without faces carries the label, which is the case
  that fails today.

## Outcome

Built as designed, 2026-11-13, with the marker in point 1. `replace_faces`, moved
out of `FacesEngine::run`, is the one routine that replaces an image's faces
while carrying labels; face rebuild and both of scan's face paths call it. Five
new tests and one rewritten Database test, each failing before the change; the
rebuild-after-scan test is the case that lost the name. R36 holds with its new
statement, R76's statement is narrowed to unchanged files, and R60 binds
`scan.walk` and stays `partial` under 063.

## Relationship to other plans

- **060** (completed) built `carry_labels` and the frame provenance this uses,
  and excluded this case.
- **063** decides what authorizes dropping a label that cannot be carried, for
  rebuild and, after this plan, for scan.
- **071** gates its scan on this plan.
