# 054 Phase 1 — the enumeration report

Owning plan: `plans/054-reconcile-conflict-rules.md`. Execution script:
`docs/plans/2026-08-09-054-rules-record.md`, Task 7.

Phase 1 enumerated phig's data-integrity rules into `rules/*.toml` by hand, with
no tooling. This report is what Phase 1 exists to produce. It asks for three
decisions:

1. Build Phase 2 (the generator and the ten CI checks), rescope it, or drop it.
2. Enforce check 5 (every operation × concern cell is covered or explicitly
   waived), or generate the matrix without gating on it.
3. Which of the 66 rules stay in a maintained record, and which were worth
   writing once as an audit and should not be kept.

Every number below came from a script run against the tree at this commit. Where
a number in the execution ledger or in a task report disagrees with what the
tree says, the tree wins and the disagreement is named.

---

## 1. The counts

Step 1's script, run verbatim from the brief against `rules/`:

```
rules             66
operations        17
by status         {'holds': 50, 'unenforced': 3, 'violated': 7, 'partial': 6}
by test_kind      {'witness': 52, 'subprocess': 3, 'conformance': 10, 'sanitizer': 1}
with a test       60
with test_gap     39
owners cited      ['041', '053', '054', '056', '059']
conformance rules 10
adapters needed   13 ['faces.identify', 'faces.rebuild', 'organize.copy',
                      'organize.move', 'organize.plan', 'relink.demote',
                      'relink.demote_live_photo', 'relink.transfer',
                      'remove.db-only', 'remove.hard', 'remove.orphans',
                      'remove.trash', 'scan.reconcile']
matrix cells      153
cells covered     85
cells needing n/a 68
```

Supporting figures, from a second script over the same files:

| Figure | Value |
|---|---|
| rule files | **10**, not the 8 the execution ledger's closing line records — `conflict` 9, `atomicity` 8, `concurrency` 8, `attachments` 7, `recoverability` 7, `authorization` 6, `boundary` 6, `metadata` 6, `reconcile` 6, `cancellation` 3. Verified by `grep -c '\[\[rule\]\]' rules/*.toml`, which sums to 66 |
| rules by tier | 60 `engine`, 6 `boundary` |
| rules per concern | atomicity 11, concurrency 9, conflict 9, attachments 7, authorization 7, recoverability 7, metadata 6, reconcile 6, cancellation 4 |
| `holds` with no `test_gap` | 27 |
| `holds` with a `test_gap` | 23 |
| rules with `tests = []` | 6 (R26, R34, R48, R50, R60, R65) |
| distinct tests cited | 133 |
| distinct `enforced_at` anchors | 63, across 21 files |
| rules with exactly one anchor | 28 |
| `operations.toml` | 17 operations, 6 `core_internal`, 40 `core_read_only`, 57 `read_only_abi` |
| assets protected | db-row 37, user-metadata 30, original-file 22, activity-log 18, attachment 11, thumb-cache 1 |
| rules with `protects = []` | 3 (R63, R64, R65) |

All 133 cited tests were run as one filter against the worktree build:

```
$ ./build/phig-tests --gtest_filter="<133 names built from the TOML>"
[==========] 133 tests from 25 test suites ran. (2791 ms total)
[  PASSED  ] 132 tests.
[  SKIPPED ] 1 test, listed below:
[  SKIPPED ] MoveFileTest.CrossDeviceRefusesToOverwriteExistingDest
```

Three of those 133 are environment-dependent in ways the schema cannot express:

- `MoveFileTest.CrossDeviceRefusesToOverwriteExistingDest` (R02) skips unless
  the temp directory is on a second filesystem. R02 has three other witnesses.
- `FacesEngineRun.ImageDeletedMidRunIsSkippedRatherThanAbortingTheRun` is R52's
  **only** witness and needs face models, which CI does not download. R52 is
  `holds` on a test the gate never runs.
- `LivePhotoDemotion.MovesTheVideoRowsMetadataToItsStillAndDropsTheRow` (R17)
  needs `$HOME/phig-live-samples/IMG_1109.MOV`, which is not in the repo. R17
  has seven other witnesses.

Two more are on plan 046's list of three `DatabaseTest` cases that fail on
Linux: `DatabaseTest.UpdatePathOntoStaleDestClearsItsFaces` (R40) and
`DatabaseTest.PurgeSnapshotMarksOmittedFaces` (R04). Each of those rules has
exactly one other witness.

---

## 2. Flag coverage (Step 1b)

The plan's owner's stated goal was that "the permutation of each operation and
its flags" be well covered. The schema has no flags dimension, so this is not
answerable from the record set's structure and was measured directly.

**25 distinct flags** appear in root `CLAUDE.md`'s CLI command reference, parsed
from the fenced block by script. The 47-rule snapshot recorded 27; I could not
reproduce 27 from the same source and do not know what the extra two were. The
comparable claim from that snapshot — that `--db-only`, `--parallel` and
`--size` had no rule at all — is checked below and still holds.

Per flag: which commands accept it, and how many rules state something about it.
"States" was judged by reading every candidate match, because a literal `--flag`
search misses rules that write `dry run` or `on_conflict`, and a bare-word search
returns 21 hits for `--path` on the English word "path".

| Flag | Commands | Rules stating it |
|---|---|---|
| `--on-conflict` | cp, mv | 4 — R01, R02, R08, R11 |
| `--dry-run` | cp, mv, rm | 2 — R20, R21 (R44 and R61 mention it) |
| `--ignore-mount-warning` | scan | 2 — R13, R14 |
| `--hard` | rm | 2 — R22, and R23 for the default it displaces |
| `--yes` | rm, trash empty | 1 — R22 |
| `--db` | all eight commands | 1 — R50 |
| `--recursive` | scan | 0 |
| `--force` | scan, thumbs rebuild | 0 (R35 and R36 use it in a measurement) |
| `--thumbs` | scan | 0 |
| `--faces` | scan | 0 |
| `--media` | scan | 0 |
| `--hidden` | scan | 0 |
| `--on-error` | scan | 0 |
| `--match` | thumbs rebuild, duplicates, cp, mv, rm | 0 |
| `--exclude` | thumbs rebuild, duplicates, cp, mv, rm | 0 |
| `--size` | thumbs rebuild | 0 |
| `--parallel` | thumbs rebuild | 0 |
| `--type` | duplicates | 0 |
| `--threshold` | duplicates | 0 |
| `--format` | duplicates, cp, mv | 0 (R03's rationale names the format string) |
| `--output` | duplicates | 0 |
| `--path` | duplicates, cp, mv | 0 |
| `--count` | search | 0 |
| `--db-only` | rm | 0 |
| `--orphans` | rm | 0 |

**Six of 25 flags have a rule. Nineteen have none.** `--db-only`, `--parallel`
and `--size` are still at zero, unchanged from the 47-rule snapshot. So are the
whole of `duplicates`' flag set and the whole of the selection system
(`--match` / `--exclude` / `--path`), which every destructive command accepts.

Classifying all 66:

- **10 rules are flag/mode behaviour by their own statement text** — R01, R02,
  R08, R11 (quantified over or naming an `on_conflict` value), R13 ("under any
  flag"), R14 (names `--ignore-mount-warning`), R20 (a dry-run removal), R22
  (`--hard` and an explicit yes), R23 (the default removal mode), R25 (whether
  the activity log is enabled — a config key, not a CLI flag).
- **2 more are flag behaviour whose statement does not name the flag** — R21 is
  the engine half of `--dry-run`, R50 is "the lock must follow `--db`".
- **54 are internal invariants**: properties of the implementation, true
  whatever flags are passed.

At 47 rules the split was 8 flag/mode to 39 internal, judged by someone else.
Mine is 12 to 54. The ratio is materially unchanged: 0.21 then, 0.22 now. Task 6
added 19 rules and exactly one of them (R50) is flag behaviour.

The finished record set does not answer the question the owner asked. It is an
audit of phig's guards, not a coverage matrix over its command surface.

---

## 3. Findings

### 3.1 What the enumeration discovered

Ten behaviours that no plan and no design doc states. Each was measured with the
built binary unless the entry says otherwise; where something outside `plans/`
does state it, the entry says so.

1. **A `mv` or `cp` onto a path a stale row still names destroys that row, its
   tags and its gallery, with no restore snapshot and no log entry** (R40,
   `violated`). Reachable from the CLI with the default `--on-conflict skip` and
   no override. Printed `Done: 1  Skipped: 0  Errors: 0`, exit 0. Reproduced on
   both paths — `update_path_in_txn` for `mv`, `insert_copy` for `cp`. Plan 022
   finding 8 names both call sites, rules the deletion correct, and adds only the
   `vec_faces` cleanup; nothing says it goes unrecorded. Found while checking
   R35's claim about what a re-scan preserves.

2. **`--dry-run` creates and migrates the database.** `phig rm --dry-run --db
   <path that does not exist>` leaves a 122,880-byte v8 database. On a v7 file it
   performs the forward-only migration. `cmd_rm` and `cmd_organize` both call
   `init_schema` before they read `dry_run`. Filed as R20; the wider fact is that
   every CLI command that opens a collection opens it read-write, and
   `Database`'s `read_only` flag exists and `cli/` never uses it.

3. **`rules/operations.toml`'s own allowlist is wrong.** Line 31 asserts
   `read_only_cli = ["search", "duplicates", "help"]` under the comment "CLI
   top-level commands that mutate nothing". Re-measured for this report at a
   fresh path with XDG redirected:

   ```
   $ ./build/phig search --count --db $T/a.db     -> exit 1, a.db = 122,880 bytes
   $ ./build/phig duplicates --db $T/b.db         -> exit 1, b.db = 122,880 bytes
   $ ./build/phig help                            -> creates nothing
   $ (set a.db's schema_version to 7, re-run search) -> schema_version = 8
   ```

   `help` is the only member of that list that is read-only. This is a measured
   contradiction inside the record set's own data, not in the code it describes.

4. **`rm` and organize disagree about what a failed run is** (R27, `violated`).
   A run with per-file errors is recorded `interrupted` by `rm` and `completed`
   by `cp`/`mv`, because `result.completed` is false only for cancellation. The
   review round found it is worse than recorded: `cmd_organize` prints the error
   count and returns 0, so **a `cp` that failed on every file exits 0**.

5. **The CLI's advisory writer lock is taken on the default database's lock
   file whatever `--db` names** (R50, `violated`). `core/paths.cpp:lock_path()`
   is `default_db_path() + ".lock"` and takes no argument; `cli/main.cpp` calls
   it after `--db` has been parsed. Measured both directions: holding a flock on
   a scratch database's own `.lock` did not stop `phig --no-wait tag add --db
   <that database>`; holding the default database's lock did refuse it. So the
   CLI serializes commands that share no database and fails to exclude an ABI
   caller (which locks `ctx->db_path + ".lock"`) on the database it was given.
   Corroborated independently: after a scan at a scratch path, `ls` shows only
   `phig.db`, `-shm` and `-wal` — phig never creates the scratch `.lock` at all.
   Plan 029 contains both halves and contradicts itself (acceptance criteria say
   per-DB, the implementation step specifies the default path); 029 is completed.

6. **`phig face rebuild` destroys person labels** (R60, `violated`). Two images,
   one face named Alice: `face list` reported `Alice (1 faces)`; after
   `face rebuild`, `Alice (0 faces)`. No prompt, no warning, exit 0. Reachable
   from the ABI as `phig_op_faces_start`, which has no plan phase. Seven plans
   mention `face rebuild`; none says a rebuild deletes the labels.

7. **`mv --on-conflict rename` is not idempotent.** Each re-run onto the same
   destination renames every already-filed photo again (`p1.jpg` → `p1_1.jpg` →
   `p1_1_1.jpg`), and because it is a move the previous name is gone rather than
   duplicated. Three runs measured, each printing `Done: 3  Skipped: 0
   Errors: 0`. Neither shipped default walks into it — the CLI passes `Skip`
   explicitly and the ABI's zero value is `PHIG_CONFLICT_SKIP` — but
   `OrganizeOptions`' own default is `Rename`, so any future `core/` caller that
   omits `on_conflict` gets it. Not recorded as a rule: minting an "organize is
   idempotent" invariant is policy this plan has no authority to set. Probably
   059's.

8. **A failed attachment carry counts one item in both `applied` and `errors`**
   (R48, `violated`). Measured: `Done: 1`, `Errors: 1`, a `[2/1]` progress tick,
   exit 0, parent committed and logged. No plan states it — though commit
   `4435d0a`'s message does, in its closing paragraph.

9. **An already-cancelled run over an empty selection reports itself complete
   for scan and organize** (R58, `partial`). `ThumbsEngine` and `FacesEngine`
   guard the empty-selection path on purpose and carry comments saying so; scan
   returns before the line that assigns `result.completed`, and organize has no
   guard. Read from the code, corroborated by the test asymmetry.

10. **39 of libphig's 69 `PHIG_API` entry points have no exception handler**
    (R62, `partial`), against a rule `libphig/CLAUDE.md` states as though it
    holds. `phig_query_add_tag`, the ten `phig_query_set_*` and `phig_op_poll`
    can each throw `std::bad_alloc` into a C caller. The count was derived by
    script twice and got two answers (37, then 39); the difference is four entry
    points written `extern "C" <type> name(...)`, which a regex requiring the
    return type to start the line misses.

Three more that are documentation defects rather than code defects:

11. **Emptying the trash has no `core/` primitive at all.** `cmd_trash_empty` —
    the item count, the prompt, the `isatty` refusal, the delete loop — is
    entirely `cli/main.cpp:697`. The one step that makes a trashed file
    unrecoverable is unreachable by libphig, by `phigd`, and by every GUI.
    Recorded as R23's `test_gap` with `owner = "053"`, because no honest
    `enforced_at` exists (see section 7).

12. **Plan 027's justification for `rm` leaving RAW/JPEG siblings is stale.** It
    says "this matches how `mv` carries sidecars but not siblings"; the RAW/JPEG
    carry landed at `1c6c249`, six hours before `remove_exec` at `c138147` the
    same day, so the two behaviours have never matched. The unstated consequence,
    measured: after `rm` of a RAW, its JPEG is an orphan that the next scan
    indexes as a photo of its own with none of the removed row's metadata.

13. **Two `core/` comments contradict schema v8.** `core/faces_engine.h:70` and
    `core/database.h:227` both still say `faces.id` is "INTEGER PRIMARY KEY
    without AUTOINCREMENT" and explain the surrounding defences as compensating
    for reuse that plan 045 removed. This is exactly the failure mode the rules
    exist to prevent, in comment form.

Smaller, recorded here so they are not lost: `resolve_conflict` returns an empty
path both for "skip this file" and for "I tried 10,000 suffixes and gave up", and
callers cannot tell them apart, so a directory with 10,000 collisions silently
drops files into the skipped count (no test, no plan);
`libphig/CLAUDE.md:99-101` asserts the `records.cpp` free functions wrap in a
plain `try`/`catch (...)` and they do not; `plans/027-rm-command.md`'s
frontmatter says `status: planned` while `plans/000-INDEX.md` says `completed`,
which matters because Phase 2's owner check reads the index and any tool reading
frontmatter would wrongly accept 027 as an open owner.

### 3.2 A correction to how these findings have been framed

The execution ledger and Task 7's brief both list "the carried-attachment
clobber (`4435d0a`)" as one of four defects this enumeration found. It is not.
`git merge-base --is-ancestor 4435d0a c1c9b2a` succeeds: `4435d0a` was committed
on 2026-08-09 at 14:25 and is an ancestor of this branch's base. It is one of
the plan's founding motivations — the sixth instance of the pattern, named as
such in the execution script — and the defect was found, fixed and tested before
Phase 1 began. What Phase 1 did with it was write R41, R44, R45 and R47 into a
matrix cell that had been empty. Its commit message also already names the
double-count that became R48. Neither is a discovery of this enumeration.

The brief also states that every novel defect came from the internal-invariant
work and none from the flag work. That is not what the record shows. Of the ten
discoveries above, **two came out of flag work**: `--dry-run` creating and
migrating a database (finding 2) came from writing R20, a rule the brief asked
for by naming the flag, and `mv --on-conflict rename` not being idempotent
(finding 7) fell out of verifying R11, a rule about which `--on-conflict` value
produces a self-destination. A third, R50, was found while measuring R49 — an
internal invariant — but the rule it produced is flag-conditional.

The accurate version is narrower and still favours the internal-invariant work:
**seven of ten discoveries came from asking what a guard actually does, and
three came from asking what a flag actually does.** The framing that flag work
found nothing is not supported.

### 3.3 Mutating core primitives Task 2 found undeclared

The header scan over the seven `core_headers` turned up four declarations that
genuinely mutate and that Task 1's operation list did not cover:

- **`ScanEngine::run`** — a fully implemented primitive with 20+ call sites in
  `tests/test_scan_engine.cpp` and **zero production callers**. Both
  `cli/main.cpp` and `libphig/operations.cpp` call `run_workers` and
  `run_reconcile` separately, per the class's own SAFETY comment. Classified
  `core_internal` so it does not manufacture a false "libphig is missing
  `ScanEngine::run`" row; the finding survives in that list's comment.
- **`identify_person`** — reachable from `phig face identify <name>` and from
  nowhere else. No ABI equivalent exists and no comment marked it as
  known-missing. It also exposed a wrong record: `rules/concurrency.toml`'s R07
  anchored this exact function while binding `faces.rebuild`, a different
  function. Corrected to `faces.identify`.
- **`demote_live_photo_rows`** — the third of three siblings in `relink.h` called
  from the same place in the same pattern; Task 1 had covered two.
- **Five executors** — `execute_organize_action`, `carry_sidecars`,
  `execute_trash_action`, `execute_hard_delete_action`,
  `remove_media_with_sidecars`. Real mutators with no caller outside `core/`.
  These forced the schema change: the original two buckets had no home for
  "mutates real data but is the inside of a declared operation", so the plan
  gained a third, `core_internal`.

---

## 4. Debt inventory

**13 rules record known debt** — 7 `violated`, 6 `partial` — and **39 rules
carry a `test_gap`**.

| Rule | Status | Concern | Owner | What is owed |
|---|---|---|---|---|
| R08 | violated | conflict | 059 | `--on-conflict overwrite` never overwrites |
| R27 | violated | recoverability | 053 | a partly-failed run is recorded `completed`, and `cp` exits 0 |
| R40 | violated | metadata | 041 | a row dropped by mv/cp gets no snapshot and no entry |
| R48 | violated | atomicity | 053 | one item counted as both applied and failed |
| R50 | violated | concurrency | 053 | the lock is keyed to the default database, not `--db` |
| R60 | violated | authorization | 053 | `face rebuild` destroys person labels unauthorized |
| R61 | violated | authorization | 053 | `rm`'s preview is `std::cout` text, not a value |
| R04 | partial | recoverability | 041 | the restore snapshot carries no tags and no faces |
| R25 | partial | recoverability | 041 | with the log disabled there is no snapshot at all |
| R58 | partial | cancellation | 056 | scan and organize report an already-cancelled run complete |
| R59 | partial | authorization | 041 | reconcile has no plan whose decisions a caller could hold |
| R62 | partial | atomicity | 053 | 39 of 69 ABI entry points have no handler |
| R66 | partial | authorization | 053 | `phig_op_faces_start` is destructive with no plan phase |

By owner: **053 has seven**, **041 has four**, 056 and 059 have one each. Plan
054 owns none of the behavioural debt and 25 of the 39 test gaps — its own
Phase 3.

The three `unenforced` rules are R26, R34 and R65, and they mean three different
things (section 8).

`test_gap` by owner: 054 → 25, 053 → 8, 041 → 4, 056 → 1, 059 → 1. Fourteen of
the 39 are owned by a plan other than 054, meaning code has to change before the
test can be written. The remaining 25 are tests nobody has written yet, which is
the list Phase 3 was always going to be built from.

The design promised this number as its useful side effect. It delivered: phig
has **13 known data-safety defects**, seven of them behaving wrongly today, and
**more than half of them are owned by one plan (053) whose own text names none of
them** — which is section 8's problem.

---

## 5. What Phase 1 cost

The decision needs this as plainly as the benefits.

**Every one of the six implementation tasks required at least one fix round.**
Task 5 required two. Tallied from the ledger, review returned **3 Critical, 15
Important and 18 Minor findings** across the six tasks.

**Three tasks miscounted their own output**, in reports whose numbers feed this
one:

- Task 1 reported 14 operations and 56 `read_only_abi` entries; the shipped file
  had 15 and 57.
- Task 3 reported 26 prose-emitting lines across 10 `core/*.cpp` files; the
  correct figure is 22 across 6. The four extras were `snprintf`-to-local-buffer
  calls that write no console output.
- Task 4 hand-tallied its status counts from a draft rather than the committed
  file and got three of five wrong.

**Two rules shipped with statements that were plausible and false**, and both
were the same false belief:

- R37 said a metadata transfer "removes nothing from the source". The faces
  clause is `UPDATE faces SET image_id = ?` — a move. R37's own first cited test
  asserts the opposite.
- R30, written by a different implementer in a different file, said an
  interruption "leaves the transferred metadata on both rows" and its rationale
  repeated "removes nothing from the source". R30 is the only rule a reader
  would consult about that window.

**One fix introduced a new false claim while correcting an old one.** R37's
corrected rationale said the transfer and the delete happen "inside the same
transaction". They do not: each caller in `core/relink.cpp` commits its transfer
alone and the delete happens later in a separate transaction that
`run_reconcile` opens once. That contradicts R38 two rules away, and plan 054's
own founding finding 5. It took a second review round to catch.

**Eight known-imprecise fields were deliberately left in the shipped set**,
each recorded as a deferred minor: R05 and R09's diluted `enforced_at` (one
anchors a path helper that does not do the rename-aside, one anchors a
declaration); R08's `protects = ["original-file"]` reading backwards, since
honouring R08 is what would destroy original files; R01 not binding
`organize.plan` while R03 and R08 do, from the same anchor; R34's `tests = []`
missing a partial witness; R25 filed `partial` though on the unlogged branch its
invariant does not hold at all; R48's ownership commentary living in `rationale` where
R40 and R27 put theirs in `test_gap`; and R62's explanatory comment being
imprecise about `phig_op_copy_start`.

**Ongoing cost, measured.** Since the monorepo migration created `core/` on
2026-08-05 there have been **126 commits**, of which **29 touched a file the
record set anchors** and **24 touched a diff hunk naming an anchored symbol**.
The median such commit implicates **8.5 rules**. That is an upper bound on the
record-check burden, not the record-edit burden — a hunk header carries the
enclosing function name, so a commit changing an unrelated line inside
`Database::insert` counts. Read as a rate rather than a total, roughly one
commit in five would land on the "changing a guard means updating its record in
the same commit" rule and require a maintainer to at least open the record.

That rate is the cost of keeping all 66. It is the number section 6 is trying to
bring down.

---

## 6. Recommendation on Phase 2 and check 5

### Phase 2: build a reduced version

**N = 66 against an estimate of ~40, and a stated abandon threshold of "near
20".** The count clears the bar the plan set for itself, so the question is not
whether there is enough material but whether the checks catch anything.

Here the evidence cuts both ways and the honest answer is that it cuts against
the ambitious version.

**What the ten checks would have caught in this run: almost nothing.** Verified
for this report rather than taken from the task reports: all 63 anchors resolve
under `core/` or `libphig/`, all 133 cited tests exist and 132 pass, all five
owners (041, 053, 054, 056, 059) are `planned` or `designing` in
`plans/000-INDEX.md`, all 66 ids are unique and retired R24 is absent. The
record set passes its own
mechanical checks today and passed them at every task boundary — including at the
moments when R37 and R30 carried false statements, when R62 carried a count that
was wrong by two, and when R61 cited R65 where it meant R66. Task 6 added a
cross-reference existence check specifically for the last of these and recorded
that it would not have caught it, because R65 exists; the check catches dangling
references, not wrong ones. Every serious record defect this run produced was
caught by a human reading the SQL or the header.

**What the checks would catch is decay, which is the thing the plan was actually
built to stop.** A renamed symbol silently orphaning an anchor; a rule claiming
`holds` on a test that was deleted or started failing; a debt item whose owning
plan closed while the debt stayed open; a new ABI symbol nobody classified; a
guard added in `cli/` instead of `core/`. None of those happened in five days of
hand-maintenance with six reviewers watching. All of them will happen over a
year without one.

Recommendation: **build Phase 2, scoped to the mechanical checks and the
generator, and drop the ones that only produce work.**

Keep: anchors resolve and live under `core/` or `libphig/`; `holds` names tests
that exist and pass; recorded debt names an open owning plan; every ABI symbol
and CLI command is classified; generated views are current. These are cheap, they
are the decay detectors, and most of them already exist as ad-hoc validators in
the task transcripts — Task 6's covers required fields, all three enumerations,
`protects`, `binds`, `binds_surface`, owner presence and openness, anchor
resolution and test existence, and reports `problems: none` over all 66.

Drop or defer: check 6 (conformance instantiations match `binds`) as a gate. It
implies 13 parameterized adapters, one per operation named by the 10
`conformance` rules, and the record set does not use `test_kind` consistently
enough to build them from — R04 and R12 use `conformance` in the loose README
sense of "a test checks the shape, not one case", while the design defines it as
"one parameterized suite instantiated over every operation in `binds`". Three
`conformance` rules (R64, R65, R66) are `boundary`-tier and have no `binds` at
all, so the design's definition cannot even be applied to them. Reconcile the
definition first, in `rules/README.md`, or the check will be enforcing an
ambiguity.

Two decisions Phase 2 has to make that Phase 1 exposed and did not settle:
whether a test that **skips** satisfies "passes" (R52's only witness skips in
CI; R17 and R02 each carry one skipping test among several), and whether a
`violated` rule's tests are supposed to pass — R08's and R40's tests pin today's
contrary behaviour and are meant to fail once the rule is fixed.

### Check 5: generate the matrix, do not gate on it

**68 of 153 cells would need an explicit `n/a` waiver — 44% of the matrix.**

For: `organize.copy × attachments` held no rule until Task 5 wrote R41, R44, R45
and R47 into it, and `4435d0a`'s data-loss defect lived exactly there. Stated
precisely, since the matrix did not exist when that defect was found: had it
existed, the cell the defect lived in would have been empty and the check would
have pointed at it. That is one retrospective hit, and it is the strongest single
argument for the check.

Against: the empty cells are not distributed like defects. **Five operations
account for 33 of the 68** — `trash.empty` is empty in 8 of 9 concerns because
it has no `core/` primitive at all, and `metadata.write` (7), `faces.identify`
(7), `thumbs.rebuild` (6) and `remove.db-only` (5) are mostly empty because
those operations do not touch the assets those concerns are about. Writing 33
waivers to say "attachments do not apply to a tag write" is not evidence; it is
the ritual that turns a check green.

Recommendation: **generate `by-operation.md` — it is free once the generator
exists — and read its empty cells when designing, which is exactly how the
attachments cell would have been caught. Do not require a waiver per cell.**

If a gate is wanted, the affordable form is narrower: restrict it to the 11
operations marked `destructive = true`. That is 99 cells, 60 covered, **39
needing a waiver** rather than 68, and it puts the requirement where the
irreversibility is. Even then, `trash.empty` alone contributes 8 of the 39, for
the structural reason above.

---

## 7. Keep, prune, demote

This is the section the decision turns on, and it starts from the distinction the
brief names: **the enumeration and the maintained record are two different
artifacts.** A rule can pay for itself as an audit — it made someone read the SQL
once, and that reading found a defect — without paying for itself as a record,
because a record costs an edit every time its guard changes and a review every
time someone touches the file it anchors.

The measured rate is in section 5: roughly one commit in five over the last five
days landed on a file some rule anchors, implicating a median of 8.5 rules. That
is the tax on keeping all 66.

Against that stands the fact that **seven of the ten discoveries in section 3
came from the internal-invariant work**, and internal invariants are 54 of the
66. Pruning them is pruning the thing that worked. So the recommendation below is
mostly *demote*, not *delete*: a demoted rule stays in the file as a record of
what was found, and stops being something CI checks and someone updates.

### Per concern class

**conflict (9: R01-R03, R05, R06, R08-R11) — keep, all of it.** These are the
plan's founding findings and the area under active change: 059 owns R08's
resolution and will touch R01 and R11 with it; 042's per-item override is R09.
Cost is real — `core/organize_exec.cpp` is the most-changed anchored file in the
window (9 commits) — and this is precisely where a stale record would cause the
next mis-design. Highest value per rule in the set.

**reconcile (6: R12-R17) — keep.** Plan 041 is `planned` and rewrites this path
into plan/apply. These six are its acceptance criteria written in advance. R17
in particular (the peeks are peeks; the demote passes decide again) is the kind
of thing a reimplementation loses silently.

**metadata (6: R35-R40) — keep.** R40 came out of here, R37 and R39 state
transfer semantics that two separate review rounds proved are easy to get wrong
in prose, and R35/R36 are what a user would call "will a re-scan lose my tags".

**attachments (7: R41-R47) — keep.** The one cell where the empty-cell argument
was confirmed. Also the concern with the most recent code churn behind it (the
Live Photo and RAW/JPEG carries).

**recoverability (7: R04, R18, R19, R23, R25-R27) — keep five, demote two.**
Keep R04, R23, R25, R27 (all debt or trash semantics) and R18 (the log entry
commits with the change it describes — a genuine cross-cutting invariant with
five enforcement sites). **Demote R19**: "no entry is ever updated or deleted on
its own" was derived by grepping every SQL statement naming `operations` /
`operation_entries` and finding one INSERT, one counter bump, one status UPDATE
and one `DELETE ... WHERE id <= ?`. That grep is the check; the rule is a
transcript of it. **Demote R26**: `delete_paths_logged` skips a path with no row.
It is correct, it is one `if`, and it is one assertion away from being pinned by
a test — write the test in Phase 3 and the record adds nothing.

**atomicity (8 engine: R28-R34, R48) — keep five, demote three.** Keep R30, R31,
R32, R33: these describe what an interruption leaves behind, they span multiple
files, and R30 has already been wrong twice, which is evidence that nobody holds
it in their head. Keep R48 (debt). **Demote R28** (`BEGIN IMMEDIATE` everywhere)
— `grep -rn '"BEGIN' core/ libphig/` returns 12 hits, all in
`core/database.cpp`, all `IMMEDIATE`; that grep is a complete check and a
stricter one than a record. **Demote R29** (migrations re-read the version
inside the transaction) — one function, already pinned by
`DatabaseConcurrency.ConcurrentMigrationIsSafe`. **Demote R34** (WAL with
`synchronous=NORMAL` is not power-loss durable) — this is a true statement about
one PRAGMA with `tests = []`; it belongs in `core/CLAUDE.md`, not in a record
that binds 14 operations.

**concurrency (8: R07, R49-R55) — keep five, demote three.** Keep R07, R49, R50
(debt), R55 (the CLI/ABI lock asymmetry, which is what R50 is a symptom of) and
R52 — an image deleted while a rebuild worker waits for the write lock is a real
data-safety property, though its `holds` currently rests on the one witness CI
never runs, which Phase 3 should fix rather than the record absorb. **Demote
R51, R53, R54.** R51 is a DDL fact (`faces.id` is AUTOINCREMENT) a schema grep
confirms; R53 is the statement order inside one constructor, already pinned by
the `73d1e1a` flake fix; R54 is one open flag.

**cancellation (3: R56-R58) — keep.** Small, and R56 ("reconcile is not
cancellable") is a fact with real consequences that lives in a comment today.
R58 is debt.

**authorization (6: R20-R22, R59-R61) — keep five, demote one.** R20, R21, R22
are the only substantial flag/mode rules in the set and are exactly what the
owner asked the plan to produce; R59 and R60 are debt. **Demote R61** ("a
destructive operation's preview is a value, not text the engine prints"). It is
a layering opinion filed under `authorization` because the taxonomy has no
concern for presentation, and its author says as much. The finding underneath it
— `cmd_rm` renders that command's entire user-facing output from `core/`, which
is one concrete reason all four `remove.*` operations carry
`surfaces_abi = []` — belongs in `core/CLAUDE.md` and in plan 053, not in a
data-integrity record.

**boundary tier (6: R62-R67) — keep three, demote three.** Keep R62 (debt, and
the 39-of-69 hole is real) and R67 (`phig_op_free` joins rather than detaches —
a detached worker writing through a freed `Database` is a data-safety property,
not a lifetime nicety). **Demote R63, R64, R65.** R63
(`phig_last_error` is thread-local) is a `grep thread_local`. R64 (version bumps)
is a release-process rule whose anchor is `libphig/CMakeLists.txt:SOVERSION` —
the only non-source anchor in the set, and one that will break a generator that
assumes a translation unit. R65 (`phig_op_progress` gains no field) is an ABI
compatibility rule that a `sizeof` pin in `tests/test_libphig_c_compile.c` would
enforce better than any record. All three of these carry `protects = []`, which
is the set telling you itself that they guard none of the six assets. R66 stays
while it is genuine debt (`phig_op_faces_start` is destructive with no plan
phase), though as a record it largely duplicates R59.

### The specific rules a lint rule or a code reviewer would catch anyway

Named, not gestured at. Each of these is a single-site fact with a mechanical
check that is cheaper and stricter than a TOML record:

| Rule | The check that replaces it |
|---|---|
| R19 | grep every SQL statement naming `operations` / `operation_entries` |
| R26 | one gtest assertion (delete a path with no row) |
| R28 | `grep -rn '"BEGIN' core/ libphig/` — 12 hits today, any non-`IMMEDIATE` one is the defect |
| R29 | `DatabaseConcurrency.ConcurrentMigrationIsSafe`, which already exists |
| R34 | a sentence in `core/CLAUDE.md`; there is nothing to enforce |
| R51 | grep the `CREATE TABLE faces` DDL for `AUTOINCREMENT` |
| R53 | statement order in one constructor |
| R54 | grep `sqlite3_open_v2` for `SQLITE_OPEN_FULLMUTEX` |
| R61 | `grep 'std::cout\|std::cerr' core/` — the finding is the grep |
| R63 | `grep thread_local libphig/context.cpp` |
| R64 | compare `PHIG_ABI_VERSION_MINOR` against CMake `VERSION` |
| R65 | a `static_assert(sizeof(phig_op_progress) == …)` in the C compile test |

**That is 12 of 66 to demote.** Demoting them leaves **54 maintained rules**, and
removes from the maintained set every rule whose `protects` is empty and every
rule whose enforcement is a single line in a single file that no open plan
touches.

Worth stating plainly against my own recommendation: **five of those twelve
(R28, R51, R53, R54, R63) were written down precisely because somebody had to go
and check, and the checking is the value.** Demoting them does not undo that.
What it changes is whether a maintainer owes them an edit in 2027.

---

## 8. Unowned findings

**Five of 66 rules name an `owner` that cannot close them.** This is a pattern,
not five incidents.

| Rule | Owner named | Why it cannot close it |
|---|---|---|
| R27 | 053 | moved from 056 for this reason; 053's text is about ABI parity, not the `operations` table's status semantics |
| R40 | 041 | 041's decision 6 and acceptance criteria are written against `delete_paths_logged`, which **neither** of R40's two paths calls, so closing 041 as written leaves R40 open |
| R48 | 053 | provisional, matched to R27 because the schema demanded an owner; 056 is the other candidate |
| R50 | 053 | 029 wrote the defect and is completed; 053 is rhetorical here |
| R58 | 056 | 056 owns what an operation reports about itself but does not name this case |
| R60 | 053 | 053 owns bringing face labelling to the ABI; it does not own "a rebuild destroys the labels" |

(That is six rows: R48 was the third provisional owner and is listed for
completeness, though the ledger's headline count of five names R27, R40, R50,
R58 and R60.)

**053 is carrying seven of the thirteen debt items and its own index row
explicitly disclaims two of the areas involved** — "Organize and reconcile are
already owned by 042 and 041; this is the remainder". R27, R48 and R50 all sit in
organize or in the lock path, which 053 does not claim.

**R50's nearest real home is 039, not 053.** Plan 039 (Cross-Platform `core/`
and `libphig/`, `designing`) names both of R50's anchor files by name at
`plans/039-core-cross-platform.md:46-47` — `core/lockfile.cpp` "Rewrite (75
lines)" and `core/paths.cpp` "Platform split" — and already carries a "worth
checking while here" list of `paths.cpp` defects. It still cannot fully close
R50, because its index row says it excludes `cli/` and half the fix lives in
`cli/main.cpp`'s call to `lock_path()`. But 039's claim on the `core/` half is
real where 053's is rhetorical. **Recommend re-pointing R50 to 039.**

### The schema cannot express what these findings are

Four related gaps, all of which forced a guess:

1. **There is no `unowned` state.** `owner` is required whenever `status` is
   `partial` or `violated`. A finding nobody owns therefore cannot be filed
   honestly, which contradicts the design's own premise that debt is recorded
   rather than deleted — the pressure the design worried about (delete the
   inconvenient rule) has simply been replaced by a different one (name a plan
   that will not fix it). Three rules now carry ownership meta-commentary in
   free-text fields to compensate: R40 and R27 in `test_gap`, R48 in
   `rationale`.

2. **There is no status for "the code is right, nothing pins it".** `holds`
   demands a passing test; `partial` and `violated` assert the code is wrong;
   `unenforced` was defined as "deliberately untestable". Raised independently by
   Task 3, Task 4 and Task 6. The consequence is that `unenforced` now carries
   **three distinct meanings** across its three members — R26 is "correct,
   testable, nobody wrote the one assertion", R34 is "a true statement with
   nothing to test", R65 is "correct, enforceable by a `sizeof` pin, unenforced
   today". Any count of `unenforced` is a count of three different things. The
   workaround that emerged instead is `holds` + `test_gap` + `owner = "054"`,
   which is why 23 of the 50 `holds` rules carry a gap and why 054 owns 25 test
   gaps against its own Phase 3.

3. **The schema cannot express "enforced in the wrong layer".** Every
   `enforced_at` must resolve under `core/` or `libphig/`, and the guidance says
   a `cli/`-only guard is a finding to record as `violated`. For `trash empty`
   both cannot hold: there is no `core/` symbol to anchor at, because there is no
   `core/` primitive at all. Anchoring at `paths.cpp:trash_dir` would have been a
   lie (it builds a string). It was filed as R23's `test_gap` instead, which
   means the one operation whose guard is entirely outside the engine is
   invisible in the by-operation view. Fix: allow `enforced_at` to name a `cli/`
   path exactly when `status = "violated"`, or add an
   `enforced_at_wrong_layer` field.

4. **`protects = []` renders as an empty row.** R63, R64 and R65 endanger none
   of the six assets — the losses are a misattributed error message, an
   unloadable shared object, and a buffer overrun in the client's address space.
   They will render as an empty row in the generated `by-asset.md` and be
   invisible in the view that ranks by irreversibility. Silently vanishing is the
   worst of the available outcomes. Fix: a seventh asset (`client-process` or
   `abi-contract`), or an explicit unassigned bucket the view shows. My section 7
   recommendation demotes all three, which makes this moot if it is taken.

**Recommendation on ownership: open one plan that absorbs R27, R40, R48 and
R60**, all of which are "a destructive path does not record what it destroyed, or
does not ask before destroying it", and all of which are currently parked on
plans that do not claim them. Move R50 to 039. R58 is the one of the five that
needs no move — it is genuinely 056's subject (what an operation reports about
itself); 056 simply does not name the case, and adding a sentence to 056 closes
it. That leaves 053 with R61, R62 and R66, which are libphig-surface work and
genuinely its subject.

The alternative to a new plan is an explicit `unowned` state, with the honest
cost stated: it removes the pressure to name a plan that will not do the work,
and it removes the forcing function that has so far produced 041 and 053 taking
on work they had not scoped. On the evidence of this run — five guesses out of
thirteen debt items — the forcing function is not working well enough to be worth
the false attribution. Prefer the new plan, and add `unowned` only if a second
enumeration produces findings that no plan wants.

---

## 9. What could not be determined

- **The 47-rule snapshot's flag count of 27.** Parsing the same block from root
  `CLAUDE.md` yields 25 distinct flags. I do not know which two the earlier
  measurement counted, so the two flag figures are not exactly comparable. The
  named claim from that snapshot (`--db-only`, `--parallel`, `--size` have no
  rule) is comparable and still true.
- **The earlier 8-vs-39 flag/mode split.** Mine is 12-vs-54 under a stated rule
  (a statement quantified over or conditional on a user-selectable flag, mode or
  config key). The flag-to-internal ratio is 0.21 then and 0.22 now, so the
  conclusion is stable, but the two numbers were produced by different judgements
  and one is not a re-run of the other.
- **Whether the `read_only_cli` contradiction reaches the ABI.** The measurement
  is CLI-only. `Database`'s `read_only` constructor flag exists and libphig uses
  it; whether every libphig read path uses it was not checked.
- **The forward maintenance rate.** Section 5's 29-of-126 and 24-of-126 are
  measured over five days of one unusually active window, and the symbol-level
  figure is an upper bound because a diff hunk header carries its enclosing
  function's name. It is a rate, not a projection.
