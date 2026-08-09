---
name: Rules of Record
status: planned
created: 2026-08-09
description: >
  Turn phig's unwritten conflict, reconciliation and data-integrity rules into a
  machine-checked record set. Rules live once as TOML under `rules/`; a stdlib
  Python script generates three views (by invariant, by operation, by asset) and
  runs ten checks in CI, so drift between the record and the code fails a build
  rather than waiting to be noticed. An operation is a `core/` primitive with
  cli/abi/http surfaces hanging off it, so a rule binds the operation and every
  surface inherits it. Widened 2026-08-09 from its original conflict-and-reconcile
  scope, because its own five findings span four concern classes rather than two.
  The `--on-conflict overwrite` decision splits to 059.
---

# 054 — Rules of Record

**Design:** `docs/specs/2026-08-09-phig-rules-design.md`
**Execution script:** `docs/plans/2026-08-09-054-rules-record.md`

## Why this exists

Plans 041 and 042 were each designed against what the code looked like it did.
Five times, reading it more closely changed the design:

| # | Believed | Actual |
|---|---|---|
| 1 | `--on-conflict overwrite` replaces an occupied destination | It never does. `is_protected` returned true for any existing path, and `resolve_conflict` renames around a protected destination (`paths.h:95-99`), so `overwrite` differed from `rename` only on an intra-run collision |
| 2 | Bypassing that guard is enough to honor an explicit override | `move_file` refuses to overwrite independently (plan 022's no-clobber rename), so an authorized replace still failed at the syscall |
| 3 | Copy and move treat an occupied destination alike | `execute_organize_action`'s copy branch backs the occupant up unconditionally as defense in depth; the move branch had no such path until 042 added one |
| 4 | A deleted row's metadata is recoverable from the activity log | The snapshot captures 16 `images` columns including gallery, but not tags or faces, both of which cascade-delete. Only a face *count* is recorded |
| 5 | Reconcile is one atomic destructive phase | Three phases: relink and demote commit per row, deletes commit as one transaction, and `log_begin_operation` autocommits ahead of all of them |

None was a bug report. Each was found by someone reading six call sites while
designing something else, twice in two days.

## Why the scope widened

Of those five findings, three are about an occupied destination, one is about
recoverability and one is about atomicity. The original scope — conflict and
reconciliation — covers three of five. Nothing in it reaches concurrency,
cancellation, attachment carriage or authorization, each of which has produced
the same class of surprise.

The deeper problem is that a prose document decays. A rule stated in a spec and
enforced in code drift apart silently, and nothing fails when they do. So the
deliverable is not a document. It is a machine-checked record from which
documents are generated.

## What this produces

1. **A record set** — `rules/*.toml`, one record per rule and per operation.
   Hand-written, the only source of truth.
2. **Three generated views** — `rules/by-invariant.md`, `by-operation.md`,
   `by-asset.md`. Build artifacts nobody edits. `by-operation.md` is the matrix.
3. **Ten checks** in the CI lint job, covering: enforcement sites exist and live
   in `core/` or `libphig/`; rules claiming to hold are pinned by passing tests;
   recorded debt names an open owning plan; every ABI symbol and CLI command is
   classified; every asset is guarded; conformance instantiations match `binds`;
   generated views are current.
4. **A test per rule**, with the record stating what kind of evidence backs it —
   `witness`, `conformance`, `subprocess` or `sanitizer`.

It does not rewrite any guard. Layered protection on the path that destroys
bytes is worth having; the defect is that it was undocumented, not that it was
layered.

## Two ideas the design turns on

**An operation is a `core/` primitive, not a CLI command or an ABI symbol.**
`mv` and `phig_op_move_start` are one operation with two surfaces. A rule binds
the operation, so every surface inherits it, and 052's `POST /organize`
inherits the whole rule set by declaring a surface rather than by anyone
remembering. Two consequences: enforcement in `cli/` becomes a build failure —
finding 1's `is_protected` lived in `cmd_organize` in `cli/main.cpp` and was
therefore invisible to every non-CLI caller until 042 moved it to
`core/organize_engine.cpp:55` — and `libphig/CLAUDE.md`'s 12-row "What is
missing" table becomes generated output, since `surfaces_abi = []` *is* that
list.

**Debt is recorded, not deleted.** If a rule the code violates turns CI red, the
pressure is to delete the rule and the record decays into a description of the
code. So `status = "violated"` and a `test_gap` field each require an open
owning plan and neither fails on a missing test. Finding 4 lands day one as
`status = "partial"`, `owner = "041"`. The side effect is the useful part: how
much known data-safety debt exists, and who owns each item, becomes a number.

## Phasing, and the decision point

- **Phase 1 — enumerate, no tooling.** Hand-write the record set from plans 020,
  022, 023, 025, 026, 029, 031, 032, 041, 042, 045 and this one, from the seven
  source files holding the guards, and from the header comments that already
  state rules — `core/paths.h:95-99` states the conflict rule almost verbatim.
  Reports N rules, M violated, K operations, how many already have a passing
  test, and the conformance adapter count.
- **Decision point.** The ~40 rule estimate comes from reading plan prose,
  not from an enumeration. If N comes back near 20, phase 2 is rescoped or
  dropped with nothing wasted. Matrix completeness (check 5) is decided here
  too, against the measured `n/a` cell count.
- **Phase 2** — `scripts/rules.py`: three views, ten checks, wired into the lint
  job. TOML with `tomllib`, Python 3 stdlib only, no new CI package.
- **Phase 3** — the missing tests. Its task list is written after phase 1,
  because which rules lack a test is what the enumeration discovers.

## Open questions, all resolved

1. **Where the document lives** — `rules/` at the repo root, a peer of `plans/`.
   Source and generated views in one directory, with pointers from the root,
   `core/`, `cli/` and `libphig/` guidance files.
2. **Whether the rules become executable** — yes, via check 2: a rule claiming
   `status = "holds"` must name a test that exists and passes.
3. **Scope of the `overwrite` decision** — its own plan, 059. It is a
   user-visible change to a shipped flag and should not gate infrastructure.

## Known weakness

The design claims every mutating `core/` entry point is a declared operation.
That is implemented as a regex scan over seven named headers with an allowlist
for false positives. A regex over C++ over-reports, and it cannot see a mutating
function added to an eighth header. Widening the header list is a deliberate
act, which is the mitigation, and it is weaker than the claim implies.

## Relationship to other plans

- **041 and 042** produced the findings; 041 owns the restore-snapshot fix and
  is finding 4's `owner`.
- **022** added the no-clobber `move_file` and the audit findings the on-disk
  guard exists for.
- **032** owns the mount-guard semantics this records.
- **045** owns the `faces.id` AUTOINCREMENT rule the concurrency records cite.
- **046** owns the three `DatabaseTest` failures on Linux, which are the `owner`
  for any rule pinned to them.
- **053** exposes `rm` through the ABI and will add `surfaces_abi` entries to
  operations that have none.
- **052** faces the same rules over the wire; `surfaces_http` exists for it.
- **059** owns the `--on-conflict overwrite` decision split out of here.
