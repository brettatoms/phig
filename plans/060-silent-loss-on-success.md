---
name: Silent Loss on a Successful Run
status: designing
created: 2026-08-10
closes_rules: [R27, R48, R60, R69, R73, R81]
description: >
  Four measured defects that share one shape: the operation finishes, reports
  success, exits 0, and something that mattered is gone or wrongly counted.
  `phig face rebuild` destroys person labels with no prompt and no warning, and
  is reachable through the ABI as `phig_op_faces_start` with no plan phase to
  authorize it. `cp` and `mv` write `completed` to the activity log for a run in
  which every file failed, where `rm` writes `interrupted` for the same fact. A
  failed attachment carry counts one item in both `applied` and `errors`,
  producing `Done: 1 / Errors: 1` against a `[2/1]` progress tick. And
  `--output` truncates whatever file it names, on `search` and on `duplicates`,
  with no prompt and no log entry. Filed out of plan 054's enumeration, where
  all four are `violated` rules no other open plan could close. R60 is
  the one that destroys data a rescan cannot rebuild; the other three are
  near-mechanical, and the plan is ordered so they cannot gate it.
---

# 060 — Silent Loss on a Successful Run

Plan 054's enumeration produced three findings that nobody owned. Each was
measured against the built binary, each is recorded as `status = "violated"` in
`rules/`, and each named 053 as its owner — provisionally in R60 and R48, which
said so, and by transfer in R27, which the enumeration moved 056 → 053. None of
those could close it: 053 is about what a client can reach through the ABI, not
about what an operation destroys or how it counts. This plan is the home.

All three records were re-pointed at 060 when it opened: R60
(`rules/authorization.toml`), R27 (`rules/recoverability.toml`) and R48
(`rules/atomicity.toml`) are all claimed by this plan's `closes_rules`.

A fourth arrived on 2026-08-10 from the same enumeration's unowned residue, and
was written against this plan rather than transferred to it: R69
(`rules/boundary.toml`) records that `--output` truncates whatever file it
names.

## What unifies them

An operation runs to completion, reports success and exits 0, and the user is
given no signal at the time and no record afterwards that something was lost or
that the numbers do not describe what happened.

That is a narrower claim than "these are bugs". Each has a working guard
somewhere else in phig that it does not have: `rm` asks before it destroys and
writes `interrupted` when it fails partway (R22, R27); organize publishes a plan
the caller inspects and applies (R61, R66); reconcile transfers a stale row's
metadata onto a surviving row before deleting it (R38); and an occupied
destination is refused at three separate points on the organize path (R01, R02,
R41). The four findings below are where that discipline is absent, and in each
case its absence is invisible from the outside — the exit code, the summary line
and the activity log all say the run went fine.

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

## R69 — `--output` destroys the file it is pointed at

`cmd_search` and `cmd_duplicates` hand `--output`'s path to a default
`std::ofstream::open`, whose mode truncates. There is no `--on-conflict` for
`--output`, no prompt, no backup and no activity-log entry, so the bytes that
were there leave no record anywhere.

There are four such open sites, which is what open question 6 has to be
written against: `cli/main.cpp:584` in `cmd_duplicates`, and `:962`, `:991` and
`:1040` in `cmd_search`'s person, face and general branches. A fix applied to
one of them leaves the other three truncating.

R69's measurement, with the built binary against a scratch two-image
collection: a 14-byte file reading `IRREPLACEABLE` came back as search results
from `phig search --output`, exit 0 with nothing on stdout, as duplicate groups
from `phig duplicates --output`, exit 0, as a path and a `Found 1 images` line
from `phig search --person Alice --output`, and as `Found 0 matching faces`
from `phig search --face <image> --output`. Sizes are not quoted because they
are not properties of the rule — every one of those outputs prints collection
paths, so its length follows where the scratch collection lives. A search
matching nothing truncates the file just the same — `--ext nosuchext` left 15
bytes reading `Found 0 images`. `fav list --output` is the same code, since
`cmd_fav`'s list branch fills a `SearchOptions` and calls `cmd_search`
(`cli/main.cpp:1749`); it reaches the `:1040` site only, and measured, it left
the same 15 bytes.

Two cases leave the file alone on a run that exits 0. `--count` prints the
count and returns before any stream is opened, on all three search branches
(`cli/main.cpp:950-952`, `:983-985` and `:1032-1035`), so `phig search --count
--output` left the 14-byte file at 14 bytes and did nothing else with the flag,
and `fav list --count --output` did the same. And a person search that matches
nobody returns at `cli/main.cpp:954-957`, before the `:962` open — measured, 14
bytes unchanged, exit 0. `cmd_duplicates` has no equivalent: with nothing to
report it truncates all the same, `--type exact` against a collection with no
exact duplicates leaving 85 bytes of empty summary. The remaining survivals are
error paths that exit 1 — either command's empty-database guard, and a `--face`
query image phig cannot decode or finds no face in. Whether the command
destroys the file therefore turns on flags that have nothing to do with output.

This is the finding whose asset is not phig's. The destroyed file is an
arbitrary path outside the collection — no row points at it, no rescan rebuilds
it, and phig was given the path only as somewhere to write. R69 records it under
`original-file` and says so, because what is lost is the same kind of thing;
that is the one place this plan's shape strains the taxonomy.

The fix lands in `cli/` and is the same scoping question as open question 3.

## R81 — staging a stdin query image destroys a file nobody named

R69's sibling, found in 054's flag fix round and claimed here for the same
reasons: same file, same function, same asset, and open question 6 has to be
answered for it at the same time as for the four `ofstream` sites. It is worse
than R69 on every axis. `--output` at least names the path it destroys; here
the operator names nothing, and the destroyed path is not in the command line.

`search --similar -` and `search --face -` both call `read_stdin_to_temp`
(`cli/main.cpp:821`). Three properties, independent of each other:

1. **The name is fixed.** `fs::temp_directory_path() / "phig-stdin-image"` —
   no PID, no randomization, no `mkstemp`. A regular file there is truncated
   and then removed by `TempCleanup` (`:879-884`), so it is gone rather than
   overwritten. Two concurrent stdin searches share the path, and nothing
   serializes them: `search` takes no writer lock (R49).
2. **The open follows a symlink.** `std::ofstream` writes through it, so the
   bytes land outside the temp directory entirely, and the `fs::remove`
   afterwards unlinks only the link — the victim keeps phig's bytes and the
   evidence is deleted.
3. **It destroys on a run that reports failure.** The stream truncates before
   `file_size` is consulted, so empty stdin leaves the target at 0 bytes, then
   prints `No data received on stdin` and exits 1.

Measured with the built binary against a two-image collection, the temp path
symlinked to a 24-byte file reading `IRREPLACEABLE USER DATA`: `cat
faces_small.jpg | phig search --similar -` printed one result, exited 0, and
left a 27070-byte 320x320 JPEG in its place with the symlink gone. `--face -`
did the same to another file. Empty stdin left a 26-byte file empty on an
exit-1 run. `cmd_search`'s empty-database guard (`:858`) returns above the
stdin read, so a collection with no rows exits first and the file survives —
which is why a first attempt at reproducing this can show nothing.

This one is not "silent loss on a *successful* run" in every branch — case 3
exits 1 — so it stretches the plan's title. It is kept here anyway because the
plan's subject is the shape, and cases 1 and 2 are the shape exactly.

## R73 — `face rebuild` ignores a flag it does not recognize

Added by plan 054's flag pass and claimed here because it is R60's loss with a
cheaper trigger. Twelve of `cli/main.cpp`'s thirteen flag loops end in an else
that prints `Unknown option:` and returns 1. `face rebuild`'s (`:1421-1432`)
has none: it steps over an argument it does not recognize, steps over that
argument's value too, and runs. `opts.matches` therefore stays empty, which
selects every image (R74), and the rebuild deletes every selected image's face
rows including the labels somebody typed.

Measured with the built binary against a two-image scratch collection: `phig
face rebuild --match '*a.jpg'` printed `Rebuilding face data for 1 images...`;
the same command with the flag misspelled `--mtach` printed `Rebuilding face
data for 2 images...` and exited 0. A typo widens a one-image rebuild to the
whole collection and reports success.

The fix is one else branch, so this does not wait on R60's design decision —
but the two are worth deciding together, because an authorization step in front
of a rebuild also blunts this, and a parser that refuses unknown flags does not
make the unprompted rebuild safe. R73 is recorded in `rules/boundary.toml`.

## Ordering: R60 does not wait for the others

R60 destroys metadata no rescan can rebuild and needs a design decision. R27 and
R48 are counter and status arithmetic in one file each. R69 is one decision
applied at four sites. Held as one plan because they share a shape, but the
reason to write them down together, not a reason to ship them together.

So: R60 first and on its own. R27, R48 and R69 second, and if R60's design is
still open they go ahead of it rather than behind it. None of the three depends
on how R60 is answered, and R60 depends on none of them.

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
6. **What `--output` does when the path is occupied.** Refusing unless a flag
   says otherwise matches what organize does three times over (R01, R02, R41),
   and it breaks `phig search --output results.txt` run twice, which is the
   ordinary use. A flag (`--force`, or `--on-conflict` reused) keeps that
   working and costs a shipped command a new one. Appending is not a candidate:
   the formats are whole documents, and a JSON array concatenated onto another
   is not JSON. Whichever way it goes, it has to reach all four open sites, and
   `--count` should stop accepting a flag it ignores.

## Relationship to other plans

- **Plan 054** (rules of record) surfaced all four and holds the evidence:
  R60 in `rules/authorization.toml`, R27 in `rules/recoverability.toml`, R48 in
  `rules/atomicity.toml`, R69 in `rules/boundary.toml`. Each is `violated` with
  a `test_gap`; none of the four is pinned by a test that asserts the statement.
- **Plan 053** (libphig feature parity) was the plan all three records named
  before this one opened, and is the plan that picks up 020's deferred
  activity-log reader. It stays the owner of *reaching* faces and the log
  through the ABI; it is not the owner of what a rebuild destroys. R66 stays on
  053 for the same reason: it is `partial` for two causes — faces having no plan
  phase, which this plan closes, and remove not being exposed through the ABI at
  all, which it does not.
- **Plan 042** (organize engine, completed) is where R27 and R48 both live, and
  its plan/apply shape is R60's model.
- **Plan 043** (CLI primitive split) owns `cli/`, including `face rebuild`'s
  flag surface, `cmd_organize`'s exit code (open question 3) and `--output`'s
  parser branches (open question 6). It also owns R70, the neighbouring fact
  that every CLI command opens the collection read-write and migrates it — same
  file, different shape: nothing is lost there, so it is not this plan's.
- **Plan 045** (faces.id rowid reuse, completed) is why a face id cannot carry a
  label across a rebuild, which constrains open question 2.
- **Plan 056** (uniform operation status) is adjacent and distinct: it is about
  what a client can read from an operation in flight, not about the terminal
  status written to the activity log.
- Related: 010 (face recognition), 020 (activity log), 031 (the operations
  surface and its error reporting), 041 (restore snapshots on every deleting
  path).
