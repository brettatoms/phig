---
name: Silent Loss on a Successful Run
status: designing
created: 2026-08-10
description: >
  Three measured defects that share one shape: the operation finishes, reports
  success, exits 0, and something that mattered is gone or wrongly counted.
  `phig face rebuild` destroys person labels with no prompt and no warning, and
  is reachable through the ABI as `phig_op_faces_start` with no plan phase to
  authorize it. `cp` and `mv` write `completed` to the activity log for a run in
  which every file failed, where `rm` writes `interrupted` for the same fact. A
  failed attachment carry counts one item in both `applied` and `errors`,
  producing `Done: 1 / Errors: 1` against a `[2/1]` progress tick. Filed out of
  plan 054's enumeration, where all three are `violated` rules whose recorded
  owner cannot close them. R60 is the one that destroys data a rescan cannot
  rebuild; the other two are near-mechanical, and the plan is ordered so they
  cannot gate it.
---

# 060 — Silent Loss on a Successful Run

Plan 054's enumeration produced three findings that nobody owns. Each was
measured against the built binary, each is recorded as `status = "violated"` in
`rules/`, and each names 053 as its owner — provisionally in R60 and R48, which
say so, and by transfer in R27, which the enumeration moved 056 → 053. None of
those can close it: 053 is about what a client can reach through the ABI, not
about what an operation destroys or how it counts. This plan is the home.

Adopting it means re-pointing `owner` on R60 (`rules/authorization.toml`), R27
(`rules/recoverability.toml`) and R48 (`rules/atomicity.toml`) at 060.

## What unifies them

An operation runs to completion, reports success and exits 0, and the user is
given no signal at the time and no record afterwards that something was lost or
that the numbers do not describe what happened.

That is a narrower claim than "these are bugs". Each has a working guard
somewhere else in phig that it does not have: `rm` asks before it destroys and
writes `interrupted` when it fails partway (R22, R27); organize publishes a plan
the caller inspects and applies (R61, R66); reconcile transfers a stale row's
metadata onto a surviving row before deleting it (R38). The three findings below
are where that discipline is absent, and in each case its absence is invisible
from the outside — the exit code, the summary line and the activity log all say
the run went fine.

## R60 — `face rebuild` destroys person labels

`FacesEngine::run` deletes every selected image's face rows inside a per-image
transaction and re-inserts what the detector found
(`core/faces_engine.cpp:188-194`). The deleted rows carry the `person_id` that
is a label someone typed. Labels are re-derived only by `auto_label_face`
(`core/database.cpp:1924-1969`), which compares each newly inserted face against
faces that still carry a `person_id` at that moment — so a label survives only
when a reference within threshold has not yet been deleted by the same run. The
`people` rows themselves survive, which is why the person is still listed
afterwards with nothing attached to them.

R60's measurement, with the built binary: a two-image collection with one face
named Alice reported `Alice (1 faces)`; `phig face rebuild` then reported
`Done. 3 faces detected.` and `phig face list` reported `Alice (0 faces)`.

Nothing asks. `cmd_face`'s `rebuild` branch (`cli/main.cpp:1416-1431`) parses
`--match`, `--exclude`, `--parallel` and `--db` and starts the engine — no
confirmation, no `--yes`, no warning, exit 0. And no plan documents the loss:
R60 records that `grep -rln "face rebuild" plans/` returns seven plans plus the
index and none of them says a rebuild destroys labels.

The ABI is the sharper half. `phig_op_faces_start`
(`libphig/include/phig/phig.h:543`) is the whole surface; there is no
`phig_op_faces_plan` symbol anywhere in the header. So the one ABI operation
that deletes user-authored metadata is the only destructive one with no
authorization step, which is exactly what R66 requires and what 042 already
built for organize. A GUI triggers it with one call.

Face labels are the asset here. Boxes and embeddings are re-derivable from the
file; the name is not, and there is no other copy of it.

## R27 — a run that lost files is logged `completed`

`OrganizeEngine::apply` writes its terminal status from `result.completed`
(`core/organize_engine.cpp:199`), and `completed` is set false in exactly one
place: the cancellation check at the top of the loop
(`core/organize_engine.cpp:144-146`). A file whose move or copy threw increments
`result.errors` and leaves `completed` true. `cmd_rm` writes
`failed ? "interrupted" : "completed"` (`core/remove_exec.cpp:194`) for the same
fact.

So "some files did not make it" produces two different terminal statuses
depending on which command wrote the log, and a reader — plan 053's, when it
exists — cannot tell a clean copy run from one that lost every file.

The CLI compounds it. `cmd_organize` prints the error count and returns 0
(`cli/main.cpp:690-692`), where `cmd_rm` returns `failed ? 1 : 0`
(`core/remove_exec.cpp:201`). R27's measurement: a `cp` into a `chmod 500`
destination where every file errored still exited 0.

## R48 — a half-done item is counted twice

`OrganizeEngine::apply` increments `applied` and `files_written` the moment
`execute_organize_action` returns, then calls `carry_sidecars` inside the same
`try` (`core/organize_engine.cpp:179-185`). A throw from the carry lands in the
catch that increments `errors`. So an item whose photo moved, committed and
logged is counted in both totals, and the error text names the parent as the
thing that failed with the attachment's destination inside the message.

The denominator makes it visible: `actionable` counts plan entries with a
destination (`core/organize_engine.cpp:133-136`) and progress reports
`p.done = result.applied + result.errors`
(`core/organize_engine.cpp:168, :190`). R48's measurement: a `cp` of one photo
whose sidecar destination was occupied printed `Done: 1, Skipped: 0, Errors: 1`
with a `[2/1]` progress tick, exited 0, and left the destination row and the
`copy` entry for the parent in place.

The item is genuinely half done — R41 is what makes the carry throw rather than
destroy the occupant, and R33 records that a carried file is logged outside the
parent's transaction — so the caller needs to be told which half. Instead both
totals go up and their sum exceeds the count the ticks are denominated against.

## Ordering: R60 does not wait for the other two

R60 destroys metadata no rescan can rebuild and needs a design decision. R27 and
R48 are counter and status arithmetic in one file each. Held as one plan because
they share a shape, but the shape is the reason to write them down together, not
a reason to ship them together.

So: R60 first and on its own. R27 and R48 second, and if R60's design is still
open they go ahead of it rather than behind it. Nothing in R27 or R48 depends on
how R60 is answered, and nothing in R60 depends on either of them.

## Open

1. **What authorizes a face rebuild.** Three shapes, and they are not
   alternatives at the same layer. A CLI confirmation plus `--yes` matches R22's
   rule for `rm` and fixes nothing for the ABI. A plan/apply pair
   (`phig_op_faces_plan_start` / `phig_op_faces_apply_start`) matches R66 and
   042's worked example, and is the only one that reaches every surface — but a
   faces plan has to say something useful, and "these 1,240 images will have
   their faces re-detected" is not what the caller needs to authorize; "these 38
   labels will not survive" is. Whether that second list is computable before
   the run is the question the shape turns on, and it is not obvious: the
   re-derivation depends on which references are still labelled when each face
   is inserted, so it is order-dependent within the run.
2. **Whether labels can survive a rebuild at all.** If they can, the
   authorization question mostly goes away. A re-detected face has no stable
   identity — the box moves, the embedding changes, and plan 045 made `faces.id`
   AUTOINCREMENT precisely because a face id must never name a different face.
   Re-attaching a label would mean matching old labelled embeddings against new
   faces, which is what `auto_label_face` already does; the defect is that the
   references are being deleted by the same run. Snapshotting the labelled
   embeddings before the run and matching against the snapshot is one candidate
   and is not costed here.
3. **Whether `cp`/`mv` change their exit code.** Making `cmd_organize` return
   non-zero on errors is the obvious half of R27, and it is a user-visible
   change to a shipped command that scripts depend on. It also lands in `cli/`,
   which plan 043 owns. Whether it belongs here or there is a scoping question,
   not a design one.
4. **What a half-done item is counted as.** `OrganizeResult` has `applied`,
   `skipped`, `errors`, `files_written` and `completed`
   (`core/organize_engine.h:94-98`), with no bucket for "the photo landed, the
   attachment did not". Counting it only in `errors` loses the fact that the
   photo moved; counting it only in `applied` loses the failure. A new counter
   is the honest answer and it is constrained at the boundary:
   `phig_organize_result` (`libphig/include/phig/phig.h:638-644`) is
   caller-allocated with no size or version member, the same shape
   `libphig/CLAUDE.md` says makes `phig_op_progress` unable to gain a field, so
   the same hazard applies and a new field there is not free.
5. **Whether R27's terminal status is binary.** `interrupted` for a cancelled
   run and `interrupted` for a run where 3 of 900 files failed are different
   facts sharing one word. Widening the status vocabulary touches every writer
   and the reader that does not exist yet, so it may be right to keep it binary
   and let the entry count carry the detail.

## Relationship to other plans

- **Plan 054** (rules of record) surfaced all three and holds the evidence:
  R60 in `rules/authorization.toml`, R27 in `rules/recoverability.toml`, R48 in
  `rules/atomicity.toml`. Each is `violated` with a `test_gap`; none of the
  three is pinned by a test that asserts the statement.
- **Plan 053** (libphig feature parity) is the owner all three records name
  today, and is the plan that picks up 020's deferred activity-log reader. It
  stays the owner of *reaching* faces and the log through the ABI; it is not the
  owner of what a rebuild destroys.
- **Plan 042** (organize engine, completed) is where R27 and R48 both live, and
  its plan/apply shape is R60's model.
- **Plan 043** (CLI primitive split) owns `cli/`, including `face rebuild`'s
  flag surface and `cmd_organize`'s exit code — open question 3.
- **Plan 045** (faces.id rowid reuse, completed) is why a face id cannot carry a
  label across a rebuild, which constrains open question 2.
- **Plan 056** (uniform operation status) is adjacent and distinct: it is about
  what a client can read from an operation in flight, not about the terminal
  status written to the activity log.
- Related: 010 (face recognition), 020 (activity log), 031 (the operations
  surface and its error reporting), 041 (restore snapshots on every deleting
  path).
