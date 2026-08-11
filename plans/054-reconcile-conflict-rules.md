---
name: Rules of Record
status: in-progress
created: 2026-08-09
description: >
  Turn phig's unwritten conflict, reconciliation and data-integrity rules into a
  machine-checked record set. Rules live once as TOML under `rules/`; a stdlib
  Python script generates three views (by invariant, by operation, by asset) and
  runs five decay checks in CI, so drift between the record and the code fails a
  build rather than waiting to be noticed. An operation is a `core/` primitive
  with cli/abi/http surfaces hanging off it, so a rule binds the operation and
  every surface inherits it. Widened 2026-08-09 from its original
  conflict-and-reconcile scope, because its own five findings span four concern
  classes rather than two. The `--on-conflict overwrite` decision splits to 059.
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
3. **Five decay checks** in the CI build-and-test job, covering: enforcement
   sites exist and resolve; rules claiming to hold are pinned by passing tests;
   recorded debt names an open owning plan; every ABI symbol and CLI command is
   classified; generated views are current. Cut from ten by decision 2 — see the phasing
   below for what was dropped, relaxed and deferred, and why.
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
code. So neither `status = "violated"` nor a `test_gap` fails on a missing test,
and a `violated` or `partial` rule has to be claimed by an open plan's
`closes_rules`. Finding 4 lands day one as R04, `status = "partial"`,
claimed by 041. The side effect is the useful part: how much known data-safety
debt exists, and which plan owns each item, becomes a number.

## Phasing, and the decision point

- **Phase 1 — enumerate, no tooling. Done.** Hand-wrote the record set from
  plans 020, 022, 023, 025, 026, 029, 031, 032, 041, 042, 045 and this one, from
  the seven source files holding the guards, and from the header comments that
  already state rules — `core/paths.h:95-99` states the conflict rule almost
  verbatim. Shipped: 67 rules across the nine concerns in `rules/*.toml`, 17
  operations, the `rules/README.md` field reference, a Rules-of-record section in
  the root `CLAUDE.md`, and the report at
  `docs/plans/2026-08-09-054-enumeration-report.md`. Of the 67, 51 hold, 6 are
  `partial`, 8 `violated` and 2 `unenforced`; 40 carry a `test_gap`.
- **Decision point — passed at N = 67. Phase 2 reduced, not dropped.** The
  stated threshold was "if N comes back near 20, phase 2 is rescoped or dropped".
  N is 67 against a ~40 estimate, so the material is there. What cut phase 2's
  scope is a different measurement in the same report: the ten checks would have
  caught almost nothing during phase 1 itself — every serious record defect was
  caught by a human reading the SQL or the header — but none of the decay they
  exist to catch happens in five supervised days. Ten decisions were taken on the
  report; they are recorded verbatim in its section 10, and the phases below are
  what they leave.
- **Phase 2** — `scripts/rules.py`: view generation plus five decay detectors,
  wired into the build-and-test job's ubuntu leg, which is where the gtest XML
  check 2 reads already exists. TOML with `tomllib`, Python 3 stdlib only, no
  new CI package. The five: anchors resolve; `holds` names tests that exist and pass;
  recorded debt names an open owning plan; every ABI symbol and CLI command is
  classified; generated views are current. Three amendments came with them.
  Check 8 (every anchor under `core/` or `libphig/`) is relaxed rather than
  dropped — a non-`core/` anchor is legal only on a `violated` rule, which is
  what R68's `cli/main.cpp` guard forced; when 053 moves that code the check
  tightens on its own. Check 5 (the operation × concern matrix) is generated and
  read, not gated: 67 of 153 cells would need an explicit waiver and five
  operations account for 32 of them structurally, so the waivers would be ritual.
  Check 6 (conformance instantiations match `binds`) is deferred until
  `test_kind`'s two definitions are reconciled in `rules/README.md`, because
  gating on an ambiguity enforces the ambiguity. Phase 2 also writes **R64's
  replacement**: a check comparing `PHIG_ABI_VERSION_MAJOR` and
  `PHIG_ABI_VERSION_MINOR` in `libphig/include/phig/phig.h` against `VERSION` and
  `SOVERSION` in `libphig/CMakeLists.txt`, failing when the three disagree.
  That check landed and **R64 stayed**, against this plan's own expectation
  that it would leave the set. The expectation rested on the check being
  "strictly stronger than the record", which is true of R64's test and false of
  its statement: the check compares the three numbers to each other, and
  nothing anywhere counts exported symbols against the minor. Adding
  `phig_foo()` to the header and bumping nothing does trip check 4 — measured —
  but it stops the author once and sends them somewhere else, to classify the
  symbol in `rules/operations.toml`. Do that and every check passes with
  `MINOR` still 3 and `VERSION` still 1.3.0; no path through CI asks for the
  bump. Demoting on that basis would have taken the unenforced half from weakly
  recorded to unrecorded — the outcome the conditional demotion existed to
  avoid. R64's prose was corrected instead: `holds`, with the narrowed gap
  named.
- **Phase 2b — a rule must not name a plan. Done.** Raised after phase 2
  merged, and it corrects a mistake in this plan's own design rather than in
  its execution. The retired `owner` field answered "who will fix this", which
  is a work-tracking question, and putting it on the rule tied a permanent
  statement about code safety to a document that gets completed and stops being
  touched. The reference pointed the wrong way: the shorter-lived artifact
  should name the longer-lived one.

  Measured against the merged set before the change: **44 of 70 rules named a
  plan**, so closing any one of those plans reported every rule pointing at it
  — 26 for this plan alone. And **28 of the 44 were not about unsafe code at
  all**: the code satisfies the rule and only the test is weak.

  The link is now inverted. A plan declares the rules it closes in a
  `closes_rules` frontmatter list; a rule says nothing about plans and keeps
  only what bears on safety — its statement, its `status`, its `test_gap`. The
  old check is replaced by two that are stronger than the one they retire: **a
  plan cannot be closed while a rule it claimed to close is still `violated` or
  `partial`**, which gates real work and which nothing checked before; and
  **every `violated` or `partial` rule is claimed by some open plan**, which is
  the orphaned-debt guarantee the old check gave, stated from the side that can
  act on it. The first fires on any status outside `designing`, `planned` and
  `in-progress`, not on `completed` alone, because a stale claim beside a
  legitimate one is invisible to the second check whatever closed status the
  stale claimant carries.

  The question that decided 28 of the 44 — **does a `test_gap` need an owner at
  all?** — was answered no. A weak test is not unsafe code, so the gap text
  stays and the plan reference goes; the work of closing one finds its records
  by reading them. `rules/by-invariant.md` and `by-asset.md` badge such a rule
  `holds (test gap)`, so the disclosure survives in the views that have no room
  for the text.

  Sixteen rules across seven plans were code faults and got a `closes_rules`
  entry: 039, 041, 043, 053, 056, 059, 060.

- **Phase 3 — the missing tests, and flag coverage.** Depends on phase 2b; the
  `test_gap` records it works from are the ones phase 2b settled the shape of.
  Its test task list is written after phase 1, because which rules lack a test
  is what the enumeration discovers — the `test_gap` records are that list, and
  `rules/by-invariant.md` renders each one under its rule. The second
  workstream, added by decision 9, is flag coverage — write a rule for every
  mutation-capable CLI flag that has none, and record every read-only flag as an
  explicit `n/a` rather than omitting it. The mutation-capable split must be read
  off `cli/main.cpp`, which parses 52 flags; two mechanical attempts have already
  miscounted it, because `scan`, `cp`/`mv`, `rm` and `trash` parse inline in
  `main()` and a brace-matching classifier calls their flags read-only. No
  mutation-capable count is to be stated until it has been read from the file.

  The third workstream is **face models in CI**, which is a configuration gap
  rather than a missing test and closes R52 outright. R52 is `holds` and its
  single witness calls `models_or_skip()`, so it skips wherever the models are
  absent — including CI, which downloads none. A rule that guards a real branch
  currently has no evidence in the gate, which is a false green the `test_gap`
  discloses and nothing else would. Nine face tests skip there for the same
  reason.

  The download is two files from `github.com/opencv/opencv_zoo`, measured:
  `face_detection_yunet_2023mar.onnx` at 227 KB and
  `face_recognition_sface_2021dec.onnx` at 37 MB. Cache it with
  `actions/cache` on `~/.cache/phig/models` — `download_face_models()` already
  returns early for a file that exists, so a warm cache makes the step a no-op
  with no code change, and the filenames carry their model dates, so a key
  naming those dates stays valid until someone deliberately bumps a model.

  Three things to size before wiring it. The cache costs 37 MB against the
  repo's allowance and GitHub evicts entries unused for seven days, so a quiet
  week re-downloads and the step has to work cold as well as warm. Unskipping
  the nine tests means CI starts running face detection on every push, which is
  compute rather than just transfer, and no wall-clock for that has been
  measured on a runner. And the model files are pinned by name but not by hash,
  so `opencv_zoo` moving a file under the same name would change what CI tests
  with nothing noticing — worth a checksum if the cost is a line.

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
  its `closes_rules` claims finding 4.
- **022** added the no-clobber `move_file` and the audit findings the on-disk
  guard exists for.
- **032** owns the mount-guard semantics this records.
- **045** owns the `faces.id` AUTOINCREMENT rule the concurrency records cite.
- **046** owns the three `DatabaseTest` failures on Linux, which is what any
  rule pinned to them is waiting on.
- **053** exposes `rm` through the ABI and will add `surfaces_abi` entries to
  operations that have none. It also owns moving R68's guard out of `cli/`.
- **060** was opened out of phase 1, to own the three `violated` rules that
  phase 1 could find no plan able to close — R27, R48 and R60.
- **052** faces the same rules over the wire; `surfaces_http` exists for it.
- **059** owns the `--on-conflict overwrite` decision split out of here.
