---
name: Reconciliation & Conflict Rules
status: designing
created: 2026-08-09
description: >
  Write down what phig actually does when a destination is occupied and when a
  row's file is gone, and pin each rule with a test that names it. Planning 041
  and 042 turned up five cases where the behavior differed from what the code
  appeared to say — `--on-conflict overwrite` never overwrites, `move_file` is a
  second independent guard, copy and move treat an occupied destination
  differently, the restore snapshot omits tags and faces, and reconcile is three
  transactions rather than one. None was a bug report; each was found by reading
  the code while designing something else. The rules are real and mostly
  defensible, but they live in six files and no document states them, so the next
  design will rediscover them the same way.
---

# 054 — Reconciliation & Conflict Rules

## Why this exists

Plans 041 and 042 were designed against what the code looked like it did. Five
times, reading it more closely changed the design:

| # | Believed | Actual |
|---|---|---|
| 1 | `--on-conflict overwrite` replaces an occupied destination | It never does. `cmd_organize`'s `is_protected` returned true for any existing path, and `resolve_conflict` renames around a protected destination (`paths.h:97-99`), so `overwrite` differed from `rename` only on an intra-run collision |
| 2 | Bypassing that guard is enough to honor an explicit override | `move_file` refuses to overwrite independently (plan 022's no-clobber rename), so an authorized replace still failed at the syscall |
| 3 | Copy and move treat an occupied destination alike | `execute_organize_action`'s copy branch backs the occupant up unconditionally as defense in depth; the move branch had no such path until 042 added one |
| 4 | A deleted row's metadata is recoverable from the activity log | The snapshot captures 16 `images` columns including gallery, but not tags or faces, both of which cascade-delete. Only a face *count* is recorded (`database.cpp:1017-1020`) |
| 5 | Reconcile is one atomic destructive phase | Three phases: relink and demote commit per row (`relink.cpp:35-41`, `68-74`), deletes commit as one transaction (`database.h:97`), and `log_begin_operation` autocommits ahead of all of them |

Every one of these is defensible in isolation. The problem is that the rules are
distributed across `cli/main.cpp`, `core/paths.cpp`, `core/organize_exec.cpp`,
`core/organize_engine.cpp`, `core/relink.cpp` and `core/database.cpp`, and no
file states them together. A design that needs to know "what happens when the
destination is taken" has to reconstruct the answer from six call sites, which
is what happened twice in two days.

## What this plan produces

1. **A normative rules document** — one section for conflict resolution, one for
   reconciliation, stating each rule, where it is enforced, and why. Location is
   an open question below.
2. **A test per rule, named after it**, so that changing a guard fails a test
   that says which rule broke rather than an incidental assertion somewhere
   else. 042 added several of these already
   (`OverwritePolicyStillRenamesAroundAnExistingFile` is the model).
3. **A decision on the one rule that looks wrong**, below.

It does not propose rewriting the guards. Layered protection on the path that
destroys bytes is worth having; the defect is that it is undocumented, not that
it is layered.

## The rule that looks wrong

**`--on-conflict overwrite` does not overwrite.** A user typing it reasonably
expects replacement and silently gets `rename`. The flag has three values and
two behaviors.

That is a real UX defect rather than a documentation gap, and it has three
candidate answers:

- **Make `overwrite` mean overwrite**, on the grounds that a user who typed it
  chose it, which is the same argument 042 accepted for per-item overrides. The
  cost is that a mistyped glob can then destroy untracked files with no undo,
  which is exactly what audit findings #1 and #4 added the guard to prevent.
- **Rename the value** to something honest (`rename-if-occupied`?) and keep the
  behavior. No data-safety change; breaks a shipped flag value.
- **Keep both**, with `overwrite` requiring a second opt-in (`--force-overwrite`,
  or a confirmation) to reach the destructive meaning.

Deciding this needs the answer to a question this plan cannot settle alone: is
`overwrite` reachable through the ABI's run-level policy meant to be as
destructive as a per-item override? 042 made per-item overrides destructive by
design. Leaving the run-level flag non-destructive is a coherent position — bulk
default safe, individual choice trusted — but it should be a stated position
rather than an accident.

## Reconciliation side

The five findings above are mostly organize-side. Reconcile needs the same
treatment, and 041 is where its rules were settled but not centralized:

- What `paths_safe_to_delete`'s three buckets mean, and that `check_failed` is
  never overridable under any flag (plan 032).
- That `--ignore-mount-warning` means the same thing on `scan` and `rm`.
- Relink's "exactly one on-disk match" rule, and that ambiguity means the
  metadata dies with the row.
- Demotion's authority: `has_sibling_raw` at `scan_engine.cpp:531` is a peek,
  `demote_jpeg_rows` is authoritative, and mistaking one for the other has bitten
  before.
- Which reconcile states are reachable after an interruption, given finding 5.

Plan 041's decision 6 (completing the restore snapshot) is the fix for finding 4
and stays there; this plan documents the rule, it does not re-own the work.

## Open questions

1. **Where the document lives.** `core/CLAUDE.md` is where engine rules go, but
   these span `core/` and `cli/` and inform `libphig/` and `phigd`. A
   `docs/specs/` document with pointers from each `CLAUDE.md` may fit better.
2. **Whether the rules become executable.** A table of rules and a test per rule
   drift apart unless something ties them. Naming each test after its rule is the
   cheap version; a generated table is the expensive one.
3. **Scope of the `overwrite` decision.** Whether it belongs here or in its own
   plan, given it is a user-visible behavior change to a shipped flag.

## Relationship to other plans

- **Plans 041 and 042** produced the findings; 041 owns the restore-snapshot fix.
- **Plan 022** added the no-clobber `move_file` and the audit findings the
  on-disk guard exists for.
- **Plan 032** owns the mount-guard semantics this documents.
- **Plan 053** exposes `rm` through the ABI and will need these rules stated.
- **Plan 052** faces the same questions over the wire, where a client cannot see
  a CLI flag's help text at all.
