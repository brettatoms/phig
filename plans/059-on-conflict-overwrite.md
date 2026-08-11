---
name: --on-conflict overwrite
status: designing
created: 2026-08-09
closes_rules: [R08]
description: >
  `--on-conflict` has three values and two behaviors: a user who types
  `overwrite` silently gets `rename`. `is_protected` returns true for any
  existing path and `resolve_conflict` renames around a protected destination,
  so the run-level policy has never replaced anything on disk, and `move_file`'s
  no-clobber rename would refuse at the syscall even if it did. Split out of 054,
  which records the rule but must not gate infrastructure on a user-visible
  change to a shipped flag. The decision needs an answer 054 cannot settle
  alone: whether the run-level policy is meant to be as destructive as 042's
  per-item override, which was made destructive by design. Three candidate
  answers, and a stated position is the deliverable either way.
---

# 059 — `--on-conflict overwrite`

Split out of plan 054, which found this while enumerating conflict rules and
records it as the one finding that is a real defect rather than a documentation
gap.

## The defect

`--on-conflict` accepts `skip`, `overwrite` and `rename`. It has two behaviors.

`cmd_organize` supplies an `is_protected` predicate that returns true for any
path that exists on disk, and `resolve_conflict` renames around a protected
destination rather than replacing it (`core/paths.h:95-99` documents exactly
this). So `overwrite` and `rename` differ only on an intra-run collision —
two source files resolving to the same destination within one batch. Against
anything already on disk, they are the same behavior.

Two independent guards sit behind that. `move_file` refuses to clobber at the
syscall (plan 022's no-clobber rename), so bypassing `is_protected` alone would
not produce a replace. Plan 042 added `allow_overwrite` to
`execute_organize_action` for per-item overrides, which renames the occupant
aside and restores it if the operation fails.

A user who types `overwrite` reasonably expects replacement and gets neither
replacement nor a warning.

## The question this turns on

Is `overwrite`, reachable as a run-level policy and through the ABI, meant to be
as destructive as a per-item override?

Plan 042 made per-item overrides destructive by design, on the grounds that a
user choosing a resolution for one specific file has made a genuine judgement.
Leaving the run-level flag non-destructive is a coherent position — bulk default
safe, individual choice trusted — but right now it is an accident rather than a
position, and no document states it.

## Three candidate answers

- **Make `overwrite` mean overwrite.** A user who typed it chose it, which is
  the argument 042 accepted for per-item overrides. The cost is that a mistyped
  glob can then destroy untracked files with no undo, which is what the guard
  was added to prevent. `allow_overwrite` already exists, so the mechanism is
  built; this is a policy change, not new machinery.
- **Rename the value** to something honest — `rename-if-occupied` — and keep the
  behavior. No data-safety change. Breaks a shipped flag value, and the config
  file accepts `on_conflict` as a key.
- **Keep both.** `overwrite` requires a second opt-in (`--force-overwrite`, or a
  confirmation) to reach the destructive meaning. Preserves the safe default and
  makes the destructive path reachable, at the cost of a fourth thing to explain.

## Open questions

1. Which of the three. The answer needs the position on run-level versus
   per-item destructiveness stated first.
2. Whether the ABI's run-level policy and the CLI flag must agree. They are the
   same field today; 052 would make it three surfaces.
3. Migration for the config key `on_conflict` if the value is renamed.

## Scope

The deliverable is a stated position plus whichever code change follows, and the
corresponding update to plan 054's conflict records — `rules/conflict.toml`
carries this as `status = "violated"`, claimed by this plan's `closes_rules`,
until it resolves. It does not touch the reconcile side, the layered guards
themselves, or the per-item override semantics 042 settled.

## Relationship to other plans

- **054** records the rule and owns the record set; this plan owns the fix.
- **042** built `allow_overwrite` and settled per-item override semantics.
- **022** added the no-clobber `move_file` that is the second guard.
- **052** would add a third surface for whatever this decides.
