# phig rules — design

**Status:** approved, ready to plan
**Date:** 2026-08-09
**Owns:** plan 054, widened from its original conflict/reconcile scope
**Splits out:** plan 059 — the `--on-conflict overwrite` decision
**Relates:** 020, 022, 023, 025, 026, 029, 031, 032, 041, 042, 045, 052, 053

## Why

phig's rules about conflicts, reconciliation and data integrity are real,
enforced, and mostly defensible. They are also written down nowhere. They live
as guards in `core/`, as doc comments above the functions that implement them,
and as prose inside the plan that settled each one.

The cost is not hypothetical. Plans 041 and 042 were each designed against what
the code looked like it did, and five times reading it more closely changed the
design — `--on-conflict overwrite` never overwrites, `move_file` is a second
independent guard, copy and move treat an occupied destination differently, the
restore snapshot omits tags and faces, and reconcile is three transactions
rather than one. None was reported as a bug. Each was found by someone reading
six call sites while designing something else, twice in two days.

Plan 054 was written to fix that for conflict and reconciliation. Its own
evidence argues for a wider frame: of those five findings, three are about an
occupied destination, one is about recoverability and one is about atomicity.
Nothing in the original scope covers concurrency, cancellation, attachment
carriage or authorization, each of which has produced the same kind of surprise.

The deeper problem is that a prose document decays. A rule stated in a spec and
enforced in code drift apart silently, and nothing fails when they do. So the
deliverable is not a document. It is a machine-checked record from which
documents are generated.

## What this produces

1. **A record set** — one structured record per rule, and one per operation.
   Hand-written, the only source of truth.
2. **Three generated views** of that record: by invariant, by operation, by
   asset. Build artifacts; nobody edits them.
3. **Ten checks** wired into the CI lint job, so drift between the record and
   the code fails a build rather than waiting to be noticed.
4. **A test per rule**, with the record stating what kind of evidence backs it.

It does not propose rewriting any guard. Layered protection on the path that
destroys bytes is worth having; the defect is that it was undocumented, not that
it was layered.

## 1. An operation is a `core/` primitive

The unit of the record is the primitive in `core/`, not the CLI subcommand and
not the ABI symbol. Surfaces hang off it:

```toml
[[operation]]
id = "organize.move"
core_entry = "core/organize_engine.h:OrganizeEngine::apply"
destructive = true
surfaces_abi = ["phig_op_move_start"]
surfaces_cli = ["mv"]
surfaces_http = []          # plan 052
```

`mv` and `phig_op_move_start` are one operation with two surfaces. A rule binds
the operation, so every surface inherits it — and a surface added later inherits
the whole rule set by declaring itself, not by anyone remembering.

Three consequences.

**Enforcement belongs in `core/` or `libphig/`, and that becomes checkable.**
Plan 054's first finding is the worked example: `is_protected` used to live in
`cmd_organize` inside `cli/main.cpp`, so the conflict guard was real, enforced,
and invisible to every non-CLI caller. Plan 042 moved it to
`core/organize_engine.cpp:55`. A client would have had different conflict
behavior from the CLI for as long as that lasted, with nothing to catch it. A
rule whose enforcement site resolves under `cli/` now fails the build — the root
`CLAUDE.md`'s interface-discipline rule made mechanical.

**`libphig/CLAUDE.md`'s "What is missing" table becomes generated.** It is 12
hand-maintained rows today and it is the inventory plan 053 works from. With
operations keyed on core entries and surfaces declared, `surfaces_abi = []` *is*
that list. Parity between libphig and the wire protocol becomes a diff of two
columns rather than an argument, once 052 populates `surfaces_http`.

**Coverage checking gets stronger.** "Every CLI subcommand maps to an
operation" catches CLI drift only. "Every mutating `core/` entry point is a
declared operation" also catches a primitive that no surface exposes, and a
primitive nobody wrote rules for.

## 2. The rule record

```toml
[[rule]]
id          = "R04"
tier        = "engine"
concern     = "recoverability"
statement   = "Deleting a row captures a restore snapshot sufficient to rebuild that row's user-authored metadata."
rationale   = "A rescan rebuilds hash, EXIF and thumbnails. It cannot rebuild a tag someone typed, a gallery assignment, or a face label."
enforced_at = ["core/database.cpp:delete_paths_logged"]
binds       = ["scan.reconcile", "remove.trash", "remove.hard", "remove.db-only", "remove.orphans"]
protects    = ["user-metadata"]
tests       = ["DatabaseTest.PurgeSnapshotMarksOmittedFaces"]
test_kind   = "conformance"
status      = "partial"
owner       = "041"
```

`binds` and `protects` are the pivot keys that generate the operation and asset
views. Everything else is carried through unchanged.

Much of this is consolidation rather than invention. `core/paths.h:95-99`
already states the conflict rule almost verbatim in the doc comment above
`resolve_conflict`, and `core/database.cpp` already states the snapshot
limitation in an inline comment citing the review finding that produced it. The
rules exist as scattered header prose; the work is giving them one home and a
check.

## 3. Two tiers

| Tier | Enforced in | Binds | Source material |
|---|---|---|---|
| **Engine rules** | `core/` | operations — every surface inherits | the plans and the header comments |
| **Boundary rules** | `libphig/` | surfaces, not operations | `libphig/CLAUDE.md`'s Rules section |

Five boundary rules are already written and already normative in
`libphig/CLAUDE.md`: no exception may cross the ABI, `phig_last_error` is
thread-local, adding a symbol bumps minor and never `SOVERSION`,
`phig_op_progress` cannot gain a field, and a destructive operation gets
plan/apply rather than a confirmation callback. They are liftable near-verbatim.
They are not checked today.

When `phigd` exists, its protocol rules (bearer auth, the `scan_roots`
allowlist, HMAC-signed media URLs) become a third tier against the same
machinery, bound to `surfaces_http`.

## 4. Taxonomy

**Nine concerns**, which become the columns of the operation view:

`conflict` (destination occupied) · `reconcile` (file gone, stale row, mount
guard) · `atomicity` (transaction boundaries, reachable interrupted states) ·
`recoverability` (activity log, restore snapshot, trash) · `metadata`
(preservation and transfer of user-authored data) · `attachments` (sidecars,
RAW+JPEG, Live Photo MOV) · `concurrency` (lock-taking versus lock-free) ·
`cancellation` (where a run may stop and what it leaves) · `authorization`
(dry-run, plan/apply, `--yes`, which decisions are overridable)

**Six assets**, which become the rows of the asset view, ordered by how
irreversible the loss is:

| Asset | Rebuildable? |
|---|---|
| `user-metadata` — tags, favorites, gallery, face labels | **No. The only asset nothing can reconstruct.** |
| `original-file` | Only from a backup |
| `attachment` — sidecar, paired JPEG, Live Photo MOV | Only from a backup, and easier to orphan |
| `db-row` | By rescan, except the user-metadata it carries |
| `activity-log` | No — it is the record of what happened |
| `thumb-cache` | Fully |

The last row exists so its rules are visibly cheap, not to guard it.

## 5. Evidence, and how debt is recorded

This section is the one the design hangs on. If a rule the code violates turns
CI red, the pressure is to delete the rule, and the record decays into a
description of the code instead of a statement of what should be true.

### `status` — does the code meet the rule?

- **`holds`** — a named test exists and passes. CI fails otherwise.
- **`partial` / `violated`** — the rule is normative and the code does not meet
  it. Must name an `owner` that is an **open** plan in `plans/000-INDEX.md`. CI
  fails on a missing, closed or completed owner. It does **not** fail on the
  absent or failing test.
- **`unenforced`** — stated, deliberately untested, with a rationale for why a
  test is not possible.

### `test_kind` — what kind of evidence backs it?

- **`witness`** — one hand-written test, direct assertion. What plan 042 already
  does with `OverwritePolicyStillRenamesAroundAnExistingFile`.
- **`conformance`** — one parameterized suite instantiated over every operation
  in `binds`.

  **Correction (2026-08-11): this definition did not survive.** It conflicted
  with `rules/README.md`, which defines `conformance` as evidence checked
  against a spec rather than one scenario, and the README's reading won — it is
  what the other three values describe, and three boundary-tier rules claim
  `conformance` with no `binds` at all, which is incoherent here. The check that
  compared an instantiation list against `binds` was dropped rather than built.
  `rules/README.md` is normative for the field.
- **`subprocess`** — the process is killed mid-run and the resulting state
  inspected.
- **`sanitizer`** — the evidence is a clean TSan or ASan run, not an assertion.

**Conformance is the form that makes this survive new operations.** A new
operation is declared in the operations record, lands in the instantiation list,
and is covered by every conformance rule without anyone writing a test. That is
the leverage: it is what stops the next plan from repeating this one.

Its cost is real. `INSTANTIATE_TEST_SUITE_P` appears nowhere in the suite today,
so this is new machinery rather than an extension of an existing pattern. A
conformance suite also needs a way to drive an arbitrary operation from a test —
an *adapter* per operation, hiding the difference between `ScanEngine` taking a
`ScanOptions` and `cmd_rm` taking a mode enum. The four engines already share
one shape per `core/CLAUDE.md`, and `tests/` is already organized one file per
core unit, so adapters slot into existing structure. How many are needed is a
phase 1 output, not a guess here.

### Three classes where the strong form is not testable

1. **Atomicity** needs the process to die mid-operation. The suite already has
   the machinery — one `EXPECT_EXIT` at `tests/test_libphig_abi.cpp:447` — so
   these get `subprocess`: kill a real run mid-reconcile and assert the database
   is in one of the states the rule declares reachable. Real behavior, no mocks,
   consistent with the repo's rule that data-safety changes ship with
   real-behavior tests. A test-only fault-injection seam in production code is
   the alternative and is deliberately rejected.
2. **Concurrency** — a passing run never proves the absence of a race. These get
   a regression witness plus `sanitizer`, and the record says so rather than
   claiming proof. 23 test declarations already name concurrency, so a
   meaningful fraction is pinned already.
3. **"Sufficient to rebuild"** — R04's strong form cannot be tested end to end
   because nothing can rebuild. Plan 020 deferred `phig undo`, and plan 053
   records that the activity log has no reader anywhere in `core/`. The testable
   form today is that the snapshot *contains* the fields, which is weaker than
   the statement.

### `test_gap` — when the test is weaker than the claim

Set when a test checks a necessary condition rather than the full statement.
Like `violated`, it must name an open owning plan.

The record set therefore tracks two kinds of debt the same way, and counts both:

| Debt | Recorded as |
|---|---|
| The code does not meet the rule | `status = "violated"` + `owner` |
| The test does not fully check the rule | `test_gap` + `owner` |

Neither can sit silently green. R04 lands on day one as `status = "partial"`,
`owner = "041"`; closing 041 flips it to `holds` and leaves a `test_gap` naming
whichever plan eventually builds a restorer.

## 6. The checks

1. Every `enforced_at` names a file that exists and a symbol found in it.
2. Every `holds` rule's tests appear in `--gtest_list_tests` and pass in the
   ctest XML.
3. Every `violated` rule names an open plan in `plans/000-INDEX.md`.
4. Every mutating `core/` entry point is a declared operation, and every
   `phig_*` symbol in `phig.h` and every CLI subcommand is either claimed by an
   operation's `surfaces` or on an explicit `read_only` allowlist.
5. Every operation × concern cell is a rule or an explicit `n/a`.
6. Every asset is protected by at least one rule.
7. Generated views match the record (`git diff --exit-code`).
8. Every `enforced_at` resolves under `core/` or `libphig/`.
9. For `test_kind = "conformance"`, the instantiation list equals `binds`.
10. Every `test_gap` names an open plan.

**Check 5 is the most valuable and the most expensive.** It is the matrix with
its empty cells made fatal — what turns "nobody considered cancellation for
`mv`" into a build failure. It is also nine concerns times the operation count,
each cell needing a rule or a written `n/a`. Requiring `n/a` to be *written* is
the whole point: an omission becomes visible only when silence is disallowed.
Whether it ships is a phase 1 decision, informed by how many cells are genuinely
`n/a`.

## 7. Tooling

One TOML record set parsed with `tomllib`; one Python 3 script under `scripts/`
that emits the views and runs the checks; stdlib only.

- **No new CI package.** The lint job already runs `pipx install`, so Python is
  resolved on that runner. `tomllib` is stdlib on 3.11+. YAML is deliberately
  not used — PyYAML would be a new pip dependency for readability alone.
- **Test pass/fail** is read from gtest's `--gtest_output=xml`.
- Python is new to a repo that is C++, CMake and bash today. That is the cost,
  and it is accepted because the alternatives are worse: the vendored `inih` is
  an INI parser with no list type, which forces `binds` and `protects` into
  comma-strings, and a hand-rolled awk parser is the most fragile component in
  any option considered.

Everything lives in one root-level `rules/` directory, a peer of `plans/`:
`rules/*.toml` as source, `rules/by-invariant.md`, `rules/by-operation.md` and
`rules/by-asset.md` generated. One row added to the root `CLAUDE.md` layout
table, with pointers from `core/`, `cli/` and `libphig/`'s `CLAUDE.md`.

## 8. Phasing

**Phase 1 — enumerate, no tooling.** Hand-write `rules/*.toml` and the
operations record from plans 020, 022, 023, 025, 026, 029, 031, 032, 041, 042,
045 and 054, from the seven source files that hold the guards, and from the
header comments that already state rules. Deliverable is the record set plus
reported counts:

- N rules, and how many already have a passing test
- M violated, and which plans own them
- K operations, and the `n/a` fraction of the nine-by-K grid
- for conformance rules, the union of their `binds` lists — which is the adapter
  count, and settles how many adapters phase 2 writes

**Decision point.** If N comes back near 20 rather than the estimated 40, phase 2
is rescoped or dropped with nothing wasted.

**Phase 2** — `scripts/rules.py`: emit the three views, implement the checks,
wire into the lint job. Build the adapters phase 1 identified.

**Phase 3** — write the missing tests for `holds` rules that lack one.

The ordering is deliberate: the rule count is currently an estimate from reading
plan prose, and this repo's convention is that a sizing claim gets measured
before it is built against.

## 9. What this does not do

- **It does not change any guard's behavior.** The one rule that looks wrong —
  `--on-conflict overwrite` has three values and two behaviors, and a user who
  types it silently gets `rename` — moves to its own plan. It is a user-visible
  change to a shipped flag and should not gate infrastructure.
- **It does not own the fixes for the debt it records.** Plan 041 owns the
  restore snapshot. This design records the rule and points at the owner.
- **It does not cover `cli/`.** The CLI appears only as a `surfaces_cli` string
  — never an enforcement site, never a test target. That follows the existing
  rule that `cli/` holds no business logic, and check 8 enforces it.
- **It does not generate the rules from the tests.** Tests cannot express a rule
  the code currently violates, and recording those honestly is the point.

## 10. Risks

**Phase 1 is large and judgment-heavy, and its quality determines everything
downstream.** No amount of tooling rescues a badly drawn rule set. The checks
verify that a rule points at real code and a real test; they cannot verify that
the rule says the right thing.

**The cultural failure mode has no mechanical defense.** Nothing stops someone
from editing a rule to match code they just changed rather than treating the
failure as a finding. The `status` and `test_gap` vocabulary makes the honest
path the easy one — recording debt costs one line and never breaks a build — but
it cannot force it.

**Check 5 may prove too expensive** once the real cell count is known. It is
sequenced behind the phase 1 decision point for that reason, and the design
stands without it.

## Appendix — worked example of a conformance rule

```
R03  No operation deletes a database row without capturing a restore
     snapshot of that row's user-authored metadata.

     binds: scan.reconcile, remove.trash, remove.hard,
            remove.db-only, remove.orphans
```

The test body is identical five times: seed a fixture image with a tag and a
gallery, run the operation in its destructive mode, assert an activity-log entry
exists carrying those fields. Only the driving differs, which is what the adapter
hides:

```cpp
struct Adapter {
    std::string id;                    // "remove.hard"
    void (*destroy_row)(Fixture&);     // drive that primitive
};
```

Five adapters, one assertion, five instantiations. A sixth operation that
deletes rows is covered by declaring itself.
