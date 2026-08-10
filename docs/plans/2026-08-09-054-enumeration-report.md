# 054 Phase 1 — the enumeration report

Owning plan: `plans/054-reconcile-conflict-rules.md`. Execution script:
`docs/plans/2026-08-09-054-rules-record.md`, Task 7.

Phase 1 enumerated phig's data-integrity rules into `rules/*.toml` by hand, with
no tooling. This report is what Phase 1 exists to produce. It was read on
2026-08-10 and ten decisions were taken against it; **section 10 states each
decision and its reasoning**, and sections 1-9 are the evidence, re-measured
against the record set as it now stands.

Two of those decisions changed the report itself. The record set is now 67
rules, not 66 — R68 was added, a seventh asset `client-process` exists,
`protects = []` is down to zero rules, `unenforced` is down to two, R26 moved to
`holds`, R50 moved to plan 039 and R27/R48/R60 to plan 060. Section 2 was
rebuilt from the CLI parser after the original was found to have derived its
flag list from a stale secondary source, and section 7 was rebuilt after the
blanket prune it recommended was declined.

Every number below came from a command run against the tree at this commit.
Where a number in the execution ledger or in a task report disagrees with what
the tree says, the tree wins and the disagreement is named.

---

## 1. The counts

Step 1's script, run verbatim from the brief against `rules/`:

```
rules             67
operations        17
by status         {'holds': 51, 'unenforced': 2, 'violated': 8, 'partial': 6}
by test_kind      {'witness': 52, 'subprocess': 4, 'conformance': 10, 'sanitizer': 1}
with a test       60
with test_gap     40
owners cited      ['039', '041', '053', '054', '056', '059', '060']
conformance rules 10
adapters needed   13 ['faces.identify', 'faces.rebuild', 'organize.copy',
                      'organize.move', 'organize.plan', 'relink.demote',
                      'relink.demote_live_photo', 'relink.transfer',
                      'remove.db-only', 'remove.hard', 'remove.orphans',
                      'remove.trash', 'scan.reconcile']
matrix cells      153
cells covered     86
cells needing n/a 67
```

Supporting figures, from a second script over the same files:

| Figure | Value |
|---|---|
| rule files | **10** — `conflict` 9, `atomicity` 8, `concurrency` 8, `attachments` 7, `authorization` 7, `recoverability` 7, `boundary` 6, `metadata` 6, `reconcile` 6, `cancellation` 3. Verified by `grep -c '^\[\[rule\]\]' rules/*.toml`, which sums to 67 |
| rules by tier | 61 `engine`, 6 `boundary` |
| rules per concern | atomicity 11, concurrency 9, conflict 9, authorization 8, attachments 7, recoverability 7, metadata 6, reconcile 6, cancellation 4 |
| `holds` with no `test_gap` | 27 |
| `holds` with a `test_gap` | 24 |
| rules with `tests = []` | 7 (R26, R34, R48, R50, R60, R65, R68) |
| distinct tests cited | 133 |
| distinct `enforced_at` anchors | 64, across 22 files |
| rules with exactly one anchor | 29 |
| `operations.toml` | 17 operations, 6 `core_internal`, 40 `core_read_only`, 57 `read_only_abi` |
| assets protected | db-row 37, user-metadata 30, original-file 23, activity-log 18, attachment 12, client-process 4, thumb-cache 1 |
| rules with `protects = []` | 0 |

`protects = []` is empty because of decision 6: `client-process` was added as a
seventh asset and R63, R64 and R65 moved onto it. R62 joined them in the final
review pass — its enumerated hole is an exception leaving a C entry point, which
is a client-process harm, and it was the only rule naming that harm without
claiming the asset.

All 133 cited tests were run as one filter against the worktree build:

```
$ ./build/phig-tests --gtest_filter="<133 names built from the TOML>"
[==========] 133 tests from 25 test suites ran. (2913 ms total)
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

**This section was rebuilt.** The original derived its flag list from root
`CLAUDE.md`'s CLI command reference, which was a stale secondary source and
wrong in both directions — it named a flag the parser rejects and omitted 26 it
accepts. The list below comes from the parser:

```
$ grep -oE '"--[a-z0-9-]+"' cli/main.cpp | tr -d '"' | sort -u | wc -l
52
```

### 2.1 Which flags can cause a mutation

Measuring rule coverage across all 52 would be measuring the wrong thing: a
search filter cannot lose data, so a rule about `--min-sharpness` would be
noise. The 52 are split below into flags that shape what phig writes to disk or
to the database, and flags that only select or format what is displayed. The
second group is listed for completeness and marked `n/a`, not counted as a gap.

**The split was made by reading `cli/main.cpp`, not by script.** Two mechanical
attempts failed for the same structural reason: `scan`, `cp`/`mv`, `rm` and
`trash` are parsed inline in `main()` rather than in `cmd_*` functions, so 74 of
the file's 107 `"--flag"` literals sit inside `main`'s body and brace-matching
attributes them all to it. A classifier that then treats `main` as read-only puts
`--on-conflict`, `--hard`, `--orphans` and `--yes` in the read-only bucket,
which is precisely backwards. The parse sites are: `cmd_thumbs` (1127-1143,
1235-1245), `cmd_face` (1420-1431), `cmd_tag` (1571-1585), `cmd_gallery`
(1637-1653), `cmd_fav` (1691-1705, 1724-1748), and inside `main` the global
prologue (1795-1868), `scan` (1893-1941), `duplicates` (1957-1995), `cp`/`mv`
(2024-2053), `rm` (2070-2098), `trash` (2133-2141) and `search` (2218-2306).

**24 flags are mutation-capable. 28 are read-only.**

Two entries need their placement justified. `--output` is in the first group
despite being parsed only by `duplicates`, `search` and `fav list`: it opens the
named path with a default `std::ofstream`, which truncates — see finding 14.
`--config` is in it because the file it names sets `db`, `thumbs_dir`,
`models_dir` and `organize_format`, so it decides which database a destructive
run writes to and what destination template `mv` files under. `--no-wait` is in
it because it decides whether a mutating command waits for the advisory writer
lock or refuses.

### 2.2 Mutation-capable flags, and what the record set says about them

"States it" means at least one rule's `statement` or `rationale` says what
happens when the flag is passed. Judged by reading every candidate match,
because a literal `--flag` search misses rules that write `dry run` or
`on_conflict`, and a bare-word search returns 21 hits for `--path` on the
English word "path". Where the only coverage is inside a rationale, the entry
says so.

| Flag | Commands that parse it | Rules stating it |
|---|---|---|
| `--on-conflict` | cp, mv | 5 — R01, R02, R08, R11, R40 |
| `--dry-run` | cp, mv, rm, thumbs clean | 2 — R20 (rm), R21 (cp/mv); nothing covers `thumbs clean` (R44 and R61 mention the flag) |
| `--ignore-mount-warning` | scan, rm | 2 — R13, R14 |
| `--hard` | rm | 2 — R22, and R23 for the default it displaces |
| `--yes` | rm, trash empty | 2 — R22, R68 |
| `--db` | scan, duplicates, cp, mv, rm, thumbs, tag, gallery, fav, face, search | 1 — R50 |
| `--force` | scan, thumbs rebuild | 1 — R36's rationale ("`--force hash` alone must not either"); R35 measures with it |
| `--no-wait` | global | 1 — R49's rationale, which names which commands were refused under it; R50 measures with it |
| `--config` | global | 0 |
| `--db-only` | rm | 0 |
| `--exclude` | thumbs rebuild, face rebuild, tag, gallery, fav, duplicates, cp, mv, rm, search | 0 |
| `--faces` | scan | 0 |
| `--format` | duplicates, cp, mv, fav list, search | 0 (R03's rationale names the format string) |
| `--hidden` | scan | 0 |
| `--match` | thumbs rebuild, face rebuild, tag, gallery, fav, duplicates, cp, mv, rm, search | 0 |
| `--media` | scan, duplicates, search | 0 |
| `--no-overwrite` | gallery set | 0 |
| `--on-error` | scan | 0 |
| `--orphans` | rm | 0 (R12, R13 and R14 state the deletable set the mode selects, without naming the flag) |
| `--output` | duplicates, fav list, search | 0 |
| `--parallel` | scan, thumbs rebuild, face rebuild | 0 |
| `--path` | tag, gallery, fav add/remove | 0 |
| `--recursive` | scan | 0 |
| `--thumbs` | scan | 0 |

**Eight of 24 mutation-capable flags have a rule. Sixteen have none.**

The sixteen are not evenly interesting. Three groups account for most of them:

- **The whole selection system** — `--match`, `--exclude`, `--path`. Every
  destructive command takes at least two of these, and they decide which files
  a `rm --hard` deletes. Nothing states what an empty selection means, what
  happens when exclude and match overlap on the same file, or that `--path` is
  a prefix rather than a glob.
- **The two `rm` modes that are not `--hard`** — `--db-only` and `--orphans`.
  Sixteen rules bind `remove.db-only` and nineteen bind `remove.orphans`, so
  the operations are covered; what no rule says is what the flag itself selects.
  `--db-only` leaving the file on disk is the whole point of the mode and
  appears in no statement.
- **Everything scan takes** — `--recursive`, `--hidden`, `--media`,
  `--on-error`, `--faces`, `--thumbs`. `--media` is the one with a data-safety
  edge: it restricts what the walk indexes while reconcile still runs over the
  directory.

`--no-overwrite` on `gallery set` is the single clearest miss. It is a
mutation-shaping flag on user-authored metadata — it decides whether an existing
gallery assignment is replaced — it was in no documentation until 2026-08-10,
and no rule mentions it.

### 2.3 Read-only flags — n/a

Twenty-eight flags select or format output and cannot cause a write. No rule
coverage is expected or counted:

`--after`, `--before`, `--blurry`, `--camera`, `--count`, `--ext`, `--face`,
`--face-threshold`, `--fav`, `--gallery`, `--help`, `--limit`, `--make`,
`--max-duration`, `--max-sharpness`, `--max-size`, `--min-duration`,
`--min-sharpness`, `--min-size`, `--person`, `--porcelain`, `--print0`,
`--similar`, `--similar-hash`, `--tag`, `--threshold`, `--type`, `--version`.

One caveat on "cannot cause a write": every command that opens a collection
opens it read-write and migrates it, including `search` and `duplicates` — see
finding 3. That is a property of the command, not of these flags.

### 2.4 The rules, classified

Classifying all 67 by the same rule the original used — a statement quantified
over or conditional on a user-selectable flag, mode or config key:

- **11 rules are flag/mode behaviour by their own statement text** — R01, R02,
  R08, R11 (quantified over or naming an `on_conflict` value), R13 ("under any
  flag"), R14 (names `--ignore-mount-warning`), R20 (a dry-run removal), R22
  (`--hard` and an explicit yes), R23 (the default removal mode), R25 (whether
  the activity log is enabled — a config key, not a CLI flag), R68 (`--yes`
  skips the prompt).
- **2 more are flag behaviour whose statement does not name the flag** — R21 is
  the engine half of `--dry-run`, R50 is "the lock must follow `--db`".
- **54 are internal invariants**: properties of the implementation, true
  whatever flags are passed.

At 47 rules the split was 8 flag/mode to 39 internal, judged by someone else.
At 66 it was 12 to 54. It is now 13 to 54 — a flag-to-internal ratio of 0.21,
0.22 and 0.24 across the three points. The one rule added since the 66-rule
count, R68, is flag/mode; R26's move to `holds` changed no classification.

The finished record set does not answer the question the owner asked. It is an
audit of phig's guards, not a coverage matrix over its command surface.
Decision 9 folds the remainder into 054's Phase 3.

### 2.5 Three defects the flag re-measurement found

All three were re-verified for this revision.

1. **`--size` does not exist.** It was documented in root `CLAUDE.md`, it is
   advertised by phig's own help text — `cli/main.cpp:129` prints
   `(--size N for a size other than 512)` — and the engine supports it
   (`ThumbsOptions::size`, `core/thumbs_engine.h:18`). The parser has no branch
   for it. Measured: `phig thumbs rebuild --size 768` prints
   `Unknown option: --size` and exits 1. Plan 040 shipped the size-partitioned
   cache and the engine parameter and never wired the CLI. The help text is the
   part that matters: a user is told the flag exists by the binary itself.
   Removed from `CLAUDE.md`'s CLI reference block on this branch and scheduled
   to plan 043 (decision 10). It still appears in the Thumbnails design bullet,
   which now records the defect rather than advertising the flag. The help text
   is `cli/` behaviour and was deliberately left alone here.

2. **`--path` is rejected by `duplicates`, `cp` and `mv`**, all three of which
   documented it. It parses only in `cmd_tag`, `cmd_gallery` and `cmd_fav`.
   Measured on all three: `Unknown option: --path`, exit 1. Not scheduled — it
   reads as a natural fit for all three, but adding it is a CLI change nobody
   has asked for.

3. **26 parsed flags appeared nowhere in root `CLAUDE.md`.** The old CLI
   reference's fenced block named 25 distinct flags; the parser accepts 52. 28
   parsed flags were absent from that block, of which `--config` and
   `--version` appeared elsewhere in the file, leaving 26 undocumented
   anywhere — including `--no-overwrite` on `gallery set` and the whole of
   `search`'s filter set. Corrected on this branch; the block and the parser now
   agree in both directions, 52 to 52, checked by script.

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
   R35's claim about what a re-scan preserves. Plan 041 was widened on
   2026-08-10 to cover it (decision 5).

2. **`--dry-run` creates and migrates the database.** `phig rm --dry-run --db
   <path that does not exist>` leaves a 122,880-byte v8 database. On a v7 file it
   performs the forward-only migration. `cmd_rm` and `cmd_organize` both call
   `init_schema` before they read `dry_run`. Filed as R20; the wider fact is that
   every CLI command that opens a collection opens it read-write, and
   `Database`'s `read_only` flag exists and `cli/` never uses it.

3. **`rules/operations.toml`'s own allowlist is wrong.** Line 31 asserts
   `read_only_cli = ["search", "duplicates", "help"]` under the comment "CLI
   top-level commands that mutate nothing". Re-measured for this revision at a
   fresh path with XDG redirected:

   ```
   $ ./build/phig search --count --db $T/a.db     -> exit 1, a.db = 122,880 bytes
   $ ./build/phig duplicates --db $T/b.db         -> exit 1, b.db = 122,880 bytes
   $ ./build/phig help                            -> creates nothing
   ```

   `help` is the only member of that list that is read-only. This is a measured
   contradiction inside the record set's own data, not in the code it describes.
   Finding 14 makes it worse: `search` can also truncate an arbitrary file.

4. **`rm` and organize disagree about what a failed run is** (R27, `violated`).
   A run with per-file errors is recorded `interrupted` by `rm` and `completed`
   by `cp`/`mv`, because `result.completed` is false only for cancellation. The
   review round found it is worse than recorded: `cmd_organize` prints the error
   count and returns 0, so **a `cp` that failed on every file exits 0**. Now
   owned by plan 060.

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
   Now owned by plan 039.

6. **`phig face rebuild` destroys person labels** (R60, `violated`). Two images,
   one face named Alice: `face list` reported `Alice (1 faces)`; after
   `face rebuild`, `Alice (0 faces)`. No prompt, no warning, exit 0. Reachable
   from the ABI as `phig_op_faces_start`, which has no plan phase. Seven plans
   mention `face rebuild`; none says a rebuild deletes the labels. Now owned by
   plan 060, and it is that plan's gating item.

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
   `4435d0a`'s message does, in its closing paragraph. Now owned by plan 060.

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
    unrecoverable is unreachable by libphig, by `phigd`, and by every GUI. Now
    recorded as R68, `violated`, anchored at `cli/main.cpp:cmd_trash_empty`
    with owner 053 — see decision 7 for why the anchor points where the defect
    is rather than at a `core/` symbol that does not exist.

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

Two more found during the decision work, both in the `--output` path:

14. **`phig search --output <path>` truncates whatever is at that path**, with
    no prompt and exit 0. `cmd_search` opens the file with a default
    `std::ofstream` (`cli/main.cpp:1038-1040`), as do `cmd_duplicates` (582-584)
    and the two `fav list` branches (960-962, 989-991). Measured: a 23-byte file
    named as `--output` came back 122 bytes of search results, original contents
    gone. Nothing checks whether the destination exists, and nothing checks
    whether it is a file phig itself tracks — so `phig search --output` onto an
    indexed photo destroys it and the row still names it. This is a write to
    disk from a command `operations.toml` classifies as read-only, reached by a
    flag no rule mentions.

15. **`--output` is silently ignored with `--count`.** `cmd_search` returns at
    `cli/main.cpp:1032-1035` when `count_only` is set, before the `--output`
    branch, so the count goes to stdout and the named file is not touched.
    Measured: the file was unchanged at 23 bytes and the count printed to the
    terminal. Harmless on its own; recorded because it is the same flag behaving
    two ways in one command.

Smaller, recorded here so they are not lost: `resolve_conflict` returns an empty
path both for "skip this file" and for "I tried 10,000 suffixes and gave up", and
callers cannot tell them apart, so a directory with 10,000 collisions silently
drops files into the skipped count (no test, no plan). Two documentation defects
listed here in the first draft have since been fixed under decision 10 —
`libphig/CLAUDE.md:99-101` now names the three free functions that do not wrap
in `try`/`catch`, and `plans/027-rm-command.md`'s frontmatter now says
`completed`, matching the index.

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
found nothing is not supported, and the flag work done for this revision added
findings 14 and 15 plus the three defects in section 2.5.

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

**14 rules record known debt** — 8 `violated`, 6 `partial` — and **40 rules
carry a `test_gap`**.

| Rule | Status | Concern | Owner | What is owed |
|---|---|---|---|---|
| R08 | violated | conflict | 059 | `--on-conflict overwrite` never overwrites |
| R27 | violated | recoverability | 060 | a partly-failed run is recorded `completed`, and `cp` exits 0 |
| R40 | violated | metadata | 041 | a row dropped by mv/cp gets no snapshot and no entry |
| R48 | violated | atomicity | 060 | one item counted as both applied and failed |
| R50 | violated | concurrency | 039 | the lock is keyed to the default database, not `--db` |
| R60 | violated | authorization | 060 | `face rebuild` destroys person labels unauthorized |
| R61 | violated | authorization | 053 | `rm`'s preview is `std::cout` text, not a value |
| R68 | violated | authorization | 053 | `trash empty`'s only guard lives in `cli/` |
| R04 | partial | recoverability | 041 | the restore snapshot carries no tags and no faces |
| R25 | partial | recoverability | 041 | with the log disabled there is no snapshot at all |
| R58 | partial | cancellation | 056 | scan and organize report an already-cancelled run complete |
| R59 | partial | authorization | 041 | reconcile has no plan whose decisions a caller could hold |
| R62 | partial | atomicity | 053 | 39 of 69 ABI entry points have no handler |
| R66 | partial | authorization | 053 | `phig_op_faces_start` is destructive with no plan phase |

By owner: **041 and 053 have four each**, 060 has three, and 039, 056 and 059
have one each. Plan 054 owns none of the behavioural debt and 25 of the 40 test
gaps — its own Phase 3.

This is the redistribution decision 5 produced. Before it, 053 carried seven of
thirteen; it now carries four, all of which are genuinely libphig-surface work.

The two `unenforced` rules are R34 and R65. Under decision 4 that status now
means one thing — "nothing can pin this today" — where it previously meant three
(section 8).

`test_gap` by owner: 054 → 25, 053 → 5, 041 → 4, 060 → 3, 056 → 1, 039 → 1,
059 → 1. Fifteen of the 40 are owned by a plan other than 054, meaning code has
to change before the test can be written. The remaining 25 are tests nobody has
written yet, which is the list Phase 3 was always going to be built from.

The design promised this number as its useful side effect. It delivered: phig
has **14 known data-safety defects**, eight of them behaving wrongly today.

---

## 5. What Phase 1 cost

The decision needed this as plainly as the benefits.

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

Task 7 itself is a fourth: its section 2 was built on a documentation file
rather than on the parser, and got both the flag count (25 against 52) and the
direction of the error wrong.

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

**Seven known-imprecise fields were deliberately left in the shipped set**,
each recorded as a deferred minor: R05 and R09's diluted `enforced_at` (one
anchors a path helper that does not do the rename-aside, one anchors a
declaration); R08's `protects = ["original-file"]` reading backwards, since
honouring R08 is what would destroy original files; R34's `tests = []`
missing a partial witness; R25 filed `partial` though on the unlogged branch its
invariant does not hold at all; R48's and R27's ownership commentary living in
`rationale` where R40 puts its in `test_gap`; and R62's explanatory comment
being imprecise about `phig_op_copy_start`. An eighth — R01 not binding
`organize.plan` while R03 and R08 do, from the same anchor — was fixed in the
final review pass rather than deferred: `OrganizeEngine::plan` calls
`resolve_conflict`, so the binding was simply missing.

**Ongoing cost, measured.** Re-measured at this commit against the current
64-anchor set. Since the monorepo migration created `core/` on 2026-08-05 there
have been **130 commits**, of which **30 touched a file the record set anchors**
and **25 contained a diff naming an anchored symbol**. The median such commit
implicates **8 rules**. That is an upper bound on the record-check burden, not
the record-edit burden — the symbol match is over the whole diff, so a commit
changing an unrelated line inside `Database::insert` counts. Read as a rate
rather than a total, **roughly one commit in five** would land on the "changing a
guard means updating its record in the same commit" rule and require a
maintainer to at least open the record. The earlier measurement over a
four-commit-shorter window and the 63-anchor set gave the same rate.

Per-file, over that window: `core/organize_exec.cpp` 9 commits,
`cli/main.cpp` 7, `core/database.cpp` 5, `libphig/CMakeLists.txt` 5.
`cli/main.cpp` and `libphig/CMakeLists.txt` entered the anchored set with R68
and R64, and `cli/main.cpp`'s 7 put it second among the anchored files behind
`core/organize_exec.cpp` — a cost that ends when 053 moves `cmd_trash_empty`
into `core/`, which is what R68 exists to force.

---

## 6. Phase 2 and check 5

Decisions 2 and 3 settled these. The evidence they rest on:

### Phase 2: build the reduced version

**N = 67 against an estimate of ~40, and a stated abandon threshold of "near
20".** The count clears the bar the plan set for itself, so the question was not
whether there is enough material but whether the checks catch anything.

Here the evidence cuts both ways and the honest answer is that it cuts against
the ambitious version.

**What the ten checks would have caught in this run: almost nothing.** Verified
for this report rather than taken from the task reports: all 64 anchors resolve,
63 of them under `core/` or `libphig/` and the 64th (R68's
`cli/main.cpp:cmd_trash_empty`) legal because R68 is `violated`; all 133 cited
tests exist and 132 pass; all seven owners (039, 041, 053, 054, 056, 059, 060)
are `planned` or `designing` in `plans/000-INDEX.md`; all 67 ids are unique and
retired R24 is absent. The record set passes its own mechanical checks today and
passed them at every task boundary — including at the moments when R37 and R30
carried false statements, when R62 carried a count that was wrong by two, and
when R61 cited R65 where it meant R66. Task 6 added a cross-reference existence
check specifically for the last of these and recorded that it would not have
caught it, because R65 exists; the check catches dangling references, not wrong
ones. Every serious record defect this run produced was caught by a human
reading the SQL or the header.

**What the checks would catch is decay, which is the thing the plan was actually
built to stop.** A renamed symbol silently orphaning an anchor; a rule claiming
`holds` on a test that was deleted or started failing; a debt item whose owning
plan closed while the debt stayed open; a new ABI symbol nobody classified. None
of those happened in five days of hand-maintenance with six reviewers watching.
All of them will happen over a year without one.

Decision 2 takes the five decay detectors plus view generation, wired into the
lint job: anchors resolve; `holds` names tests that exist and pass; recorded
debt names an open owning plan; every ABI symbol and CLI command is classified;
generated views are current. These are cheap, and most already exist as ad-hoc
validators in the task transcripts — Task 6's covers required fields, all three
enumerations, `protects`, `binds`, `binds_surface`, owner presence and openness,
anchor resolution and test existence.

Check 8 — "every anchor lives under `core/` or `libphig/`" — is relaxed rather
than dropped: a non-core anchor is legal only on a `violated` rule, where the
location is itself the defect and the owner tracks moving it. R68 is the case
that forced it, and when 053 moves the code the anchor updates to `core/`, the
status becomes `holds`, and the check tightens on its own.

Deferred: check 6 (conformance instantiations match `binds`) as a gate. It
implies 13 parameterized adapters, one per operation named by the 10
`conformance` rules, and the record set does not use `test_kind` consistently
enough to build them from — R04 and R12 use `conformance` in the loose README
sense of "a test checks the shape, not one case", while the design defines it as
"one parameterized suite instantiated over every operation in `binds`". Three
`conformance` rules (R64, R65, R66) are `boundary`-tier and have no `binds` at
all, so the design's definition cannot even be applied to them. Reconcile the
definition first, in `rules/README.md`, or the check will be enforcing an
ambiguity.

Two decisions Phase 2 still has to make that Phase 1 exposed and did not settle:
whether a test that **skips** satisfies "passes" (R52's only witness skips in
CI; R17 and R02 each carry one skipping test among several), and whether a
`violated` rule's tests are supposed to pass — R08's and R40's tests pin today's
contrary behaviour and are meant to fail once the rule is fixed.

### Check 5: generate the matrix, do not gate on it

**67 of 153 cells would need an explicit `n/a` waiver — 44% of the matrix.**

For: `organize.copy × attachments` held no rule until Task 5 wrote R41, R44, R45
and R47 into it, and `4435d0a`'s data-loss defect lived exactly there. Stated
precisely, since the matrix did not exist when that defect was found: had it
existed, the cell the defect lived in would have been empty and the check would
have pointed at it. That is one retrospective hit, and it is the strongest single
argument for the check.

Against: the empty cells are not distributed like defects. **Five operations
account for 32 of the 67** — `trash.empty` is empty in 7 of 9 concerns because
its only guard is in `cli/`, and `metadata.write` (7), `faces.identify` (7),
`thumbs.rebuild` (6) and `remove.db-only` (5) are mostly empty because those
operations do not touch the assets those concerns are about. Writing 32 waivers
to say "attachments do not apply to a tag write" is not evidence; it is the
ritual that turns a check green.

Decision 3: generate `by-operation.md` — free once the generator exists — and
read its empty cells when designing, which is exactly how the attachments cell
would have been caught. No waiver per cell.

If a gate is ever wanted, the affordable form is narrower: restrict it to the 11
operations marked `destructive = true`. That is 99 cells, 61 covered, **38
needing a waiver** rather than 67, and it puts the requirement where the
irreversibility is.

---

## 7. Keep, prune, demote

The first draft recommended demoting 12 of 66 rules on a mostly flags-based
argument. That was declined (decision 1). Each of the twelve is re-tested below
against one question, individually, on its merits:

> **Does keeping this rule help catch a future data-integrity problem?**

Not "is it a flag rule", not "could a grep confirm it today". A grep confirms
the present; the question is about the future. And a check that nobody runs is
not a check — where a demotion is recommended below, the replacement is a test
or a lint step that actually executes, and it is named.

Three of the twelve have lost one argument that was made against them: R63, R64
and R65 now protect `client-process` (decision 6), so "the set is telling you
itself that they guard none of the six assets" is no longer available. R26 has
separately moved from `unenforced` to `holds` (decision 4).

The first draft's own caveat is a live consideration here, and it argues both
ways. **Five of the twelve (R28, R51, R53, R54, R63) were written down because
somebody had to go and check, and the checking was the value.** That is an
argument for having enumerated them. It is not an argument for maintaining them
in 2027 — the audit is already banked either way. So it is weighed below per
rule, not applied as a blanket.

### R19 — the activity log is append-only. **Keep.**

The first draft called it a transcript of the grep that produced it. The grep
describes the four SQL statements that exist today; the rule constrains the ones
that will be written. Root `CLAUDE.md` says undo is a future logged operation,
and R18's rationale reasons about "a future undo". The obvious way to implement
undo is `UPDATE operation_entries SET undone = 1`, and R19 is the only thing in
the repo that says: no, append a new operation. That is a future data-integrity
decision the record steers, on the one table whose whole value is that it cannot
be edited. Four passing tests already, protects `activity-log`, binds ten
operations.

### R26 — `delete_paths_logged` skips a path with no row. **Keep.**

The first draft said it is one `if`, one assertion away from a test. Both true,
and neither is the point. Plan 041's guard inventory names this as the third of
three guards on the delete path, and 041's plan/apply split is what makes the
branch reachable in ordinary use — today it fires only when another writer
removes a row mid-operation. The record is 041's acceptance criterion written in
advance, which is the same argument that keeps the rest of `reconcile`.

It also carries a finding the assertion would not: both callers discard the
returned count, so a skipped path is still reported as removed
(`remove_exec.cpp:198`, `scan_engine.cpp:588`). That is a live reporting defect
sitting in a rule about deletion.

### R28 — every write transaction opens `BEGIN IMMEDIATE`. **Keep.**

The proposed replacement, `grep -rn '"BEGIN' core/ libphig/`, was re-run: 12
hits, all in `core/database.cpp`, all `IMMEDIATE`, no lowercase variants. So the
grep is exact today, and it is also not run by anything — it is not among the
five detectors decision 2 authorizes, and adding it would mean a lint step that
greps for a string literal.

What keeps the rule is the rationale rather than the statement: it names the
read-then-write shape that makes a deferred transaction unsafe in WAL, and names
the three paths that have that shape (`delete_paths_logged` snapshots before
deleting, `log_prune` scans before deleting, `init_schema` reads the version
before writing it). A thirteenth write path is likely — there have been twelve —
and its author needs that reasoning, not a count of string literals. Two tests
pin the mechanism; nothing pins the "every".

### R29 — migrations re-read the version inside the transaction. **Keep.**

One function and one test, as the first draft said. But it is a function that
gets edited every time the schema moves, and the schema is at v8 with more
coming. The failure mode is specific and repeats: a migration step added outside
the IMMEDIATE transaction re-runs `ALTER TABLE ADD COLUMN` against a database
another process already migrated, and dies. The single pinning test,
`DatabaseConcurrency.ConcurrentMigrationIsSafe`, is a race that catches
regressions statistically — it is the test that flaked at roughly 10% when a
neighbouring ordering was wrong (see R53). A statistical test is not a
substitute for the record.

### R34 — WAL with `synchronous=NORMAL` is not power-loss durable. **Keep.**

This is the weakest keep in the twelve, and it is a keep for two reasons.

It is the only place in the repo that states phig's durability model, and that
model is an input to work now in flight: 041's snapshot completion is the
foundation of a future undo, and R18 asserts "the log never records a change
that did not commit". Both reason about what survives a stop, and neither says
which kinds of stop. Its rationale also carries the bound, which is not written
anywhere else: `move_file` and `atomic_copy_file`
fsync the file and its parent before the rename, so the state a power loss can
leave — file at the destination, row still naming the source — is the one
`execute_organize_action`'s compensation already handles.

Second, it is one of the two rules that legitimately hold the narrowed
`unenforced` status decision 4 just defined. Demoting it leaves that status with
one member.

The honest cost: R34 cannot fail a check. It is documentation with a binding and
a `protects` list, and it should be read that way.

### R51 — `faces.id` is AUTOINCREMENT, so an id names one face. **Keep.**

The first draft's replacement was a grep of the `CREATE TABLE faces` DDL. The
DDL is exactly what will change: v8 migrated the table by copying rows into
`faces_new`, and the rationale records that it deliberately left `vec_faces`
untouched because it is a vec0 virtual table keyed on `faces.id`. The next faces
migration walks into the same shape, and three lock-free code paths depend on
the property — `face name`, `face identify` and `face rebuild` — the last of
which plan 060 is about to change. Three passing tests, and the rule
additionally records finding 13 (two `core/` comments that still describe the
pre-v8 world).

### R53 — the busy timeout is set before the first statement. **Keep.**

This is the strongest keep in the twelve, for a reason the first draft had in
front of it: **the property has already been forgotten once.** Setting
`sqlite3_busy_timeout` after `PRAGMA journal_mode=WAL` made
`DatabaseConcurrency.ConcurrentMigrationIsSafe` flake at about one run in ten
until `73d1e1a`. A property that regressed once, in two adjacent lines, whose
failure is a 10% flake in an unrelated-looking test, is the definition of one
worth writing down.

`core/database.cpp:240-242` does carry a comment saying why the order matters,
which weakens the case slightly. What the comment does not carry is the rule's
`test_gap`: the listed test would catch a regression only statistically, and
nothing asserts the ordering itself. That gap is owned by 054's Phase 3 and is
the useful part of the record.

### R54 — the connection is opened `SQLITE_OPEN_FULLMUTEX`. **Keep.**

The first draft called it "one open flag" with the check
`grep sqlite3_open_v2 for SQLITE_OPEN_FULLMUTEX`. That grep is the reason to
keep it. The rule's own `test_gap` says both cited tests would also pass with
the flag removed on a `THREADSAFE=1` build, because such a build mutexes the
connection anyway — and no CI job provides a `THREADSAFE=2` build. So this is a
property that **no test in this project can catch**, whose removal is invisible
everywhere phig currently builds, and whose failure on a `THREADSAFE=2` distro
is undefined behaviour on a shared `phig_ctx` rather than an error. There is a
comment at `core/database.cpp:221` explaining it; the record adds the fact that
the tests do not distinguish it from the default.

### R61 — a destructive operation's preview is a value, not printed text. **Keep.**

The first draft demoted this as "a layering opinion filed under `authorization`
because the taxonomy has no concern for presentation". Decision 7 removes that
argument: the question was reframed from "how do we model wrong-layer code" to
"the real fix is to move the code", and plan 053 already owns doing so.

With the layering objection gone, what is left is an authorization fact:
`cmd_rm` computes which files will be deleted and writes it to `std::cout`, so a
caller that is not a terminal gets no list, no counts, and no way to show a user
"these 412 files will be deleted" before they are. Remove cannot take the
plan/apply shape R66 requires until its preview becomes data. That is the same
property R66 states for faces and 042 built for organize, on the one operation
family whose default mode is destructive.

### R63 — `phig_last_error` is thread-local. **Demote.**

This is one of the two that fail the test.

The property is fully pinned by a test that already runs in CI on every push.
`AbiMutate.EachThreadSeesOnlyItsOwnError` (`tests/test_libphig_mutate.cpp:285`)
drives one `phig_ctx` from two threads for 2,000 iterations under real lock
contention, and asserts both clauses: that the failing thread's message never
appears on the succeeding thread, and that a successful call leaves
`phig_last_error` empty — which is the "only until that thread's next ABI call"
half. `Abi.LastErrorOnNullCtxReturnsEmptyString` covers the null case. If the
storage stopped being `thread_local`, that test fails.

So the record is a second copy of a fact CI already enforces, on a boundary rule
whose harm model is indirect: a client mis-attributing an error might retry a
destructive call, but no phig-owned asset is touched. Keeping it costs an edit
obligation and buys nothing the existing test does not already buy.

**The check that replaces it:** `AbiMutate.EachThreadSeesOnlyItsOwnError` and
`Abi.LastErrorOnNullCtxReturnsEmptyString`, both already written, both already
in the fast suite, both run by CI on every push.

### R64 — a symbol bump moves MINOR and CMake VERSION, never SOVERSION. **Demote.**

This is the other one that fails, and it fails on the question itself: it is a
release-process rule, not a data-integrity invariant. Its harm model is a client
that cannot load the library (loud) or cannot feature-detect a symbol it can
still call (harmless). No asset in `taxonomy.toml` is at risk, which is why it
had `protects = []` until decision 6 gave it `client-process` — and
`client-process` is where R65's genuine memory corruption lives, which is a
different order of harm.

Its own `test_gap` concedes the position: the cited test checks that
`phig_abi_version()`'s high half equals `PHIG_ABI_VERSION_MAJOR`, "a property of
one expression that would still pass with the CMake VERSION and SOVERSION set to
anything at all", and "both halves of this rule rest on whoever remembers". A
record that admits nothing enforces it is not enforcement.

It also carries a structural cost: `libphig/CMakeLists.txt:SOVERSION` is the
set's only non-source anchor, and Task 6 flagged that it will break a generator
that assumes a translation unit.

**The check that replaces it:** a lint step comparing
`PHIG_ABI_VERSION_MAJOR`/`PHIG_ABI_VERSION_MINOR` in
`libphig/include/phig/phig.h` (today 1 and 3) against `VERSION` and `SOVERSION`
in `libphig/CMakeLists.txt` (today `1.3.0` and `1`), failing when the three
disagree. That is a two-file text comparison, it is strictly stronger than the
record, and it belongs in the lint job decision 2 authorizes. **The demotion
should land with that check, not before it** — otherwise the property goes from
weakly recorded to unrecorded. Plan 054's Phase 2 owns writing it, which costs
nothing structurally: 054 is already R64's `owner` and is already building that
lint job.

### R65 — `phig_op_progress` gains no field. **Keep.**

The first draft demoted this on the grounds that a `sizeof` pin in
`tests/test_libphig_c_compile.c` would enforce it better than any record. The
pin would, and it should be written — that is R65's `test_gap`, owned by 054's
Phase 3. But the pin catches the edit; the record is what stops the edit being
proposed, and it is doing that work right now.

Plan 056 is `designing` and adds `phig_op_snapshot`, `phig_op_get_type`,
`phig_op_list` and `phig_op_info`. Its decision 6 states this rule's substance —
caller-allocated, no size member, a client built against an older header gets a
newer `libphig.so` writing past the end of its buffer — citing
`libphig/CLAUDE.md`, which is where the same statement lives in prose. It
concludes "the embedded `phig_op_progress` stays frozen; growth happens after
it", and gives `phig_op_info` an explicit size parameter instead. That is a live
plan obeying this rule while designing the exact struct that would have violated
it, and the harm it avoids is memory corruption in a foreign caller's address
space that the caller cannot detect. R65 is the machine-readable copy of the
constraint 056 is already following; the prose copy is what it happened to cite.

This is the clearest "yes" of the twelve.

### Result

**2 of 12 fail the test: R63 and R64.** Both are boundary-tier, both are ABI
housekeeping rather than data integrity, and both have a cheaper replacement
that actually runs — an existing CI test for R63, and a two-file version
comparison for R64 that has to be written before the demotion lands.

That leaves **66 maintained rules today and 65 once R64's replacement lands**.
R63's demotion is unconditional — its replacement tests already run in CI — so
it drops now. R64 stays in the set until the header-versus-CMake comparison
exists in the lint job; section 9 left which plan writes it undetermined, and it
is now assigned to 054's Phase 2, where the lint job is already being built.
The ten that survive did so on their own arguments, and three distinct ones
recur: a property that has already regressed once (R53), a property no test in
this project can catch (R54, R34), and a property a plan in flight is currently
obeying (R65, R26, R51, R19).

---

## 8. Unowned findings

**Five of 67 rules named an `owner` that could not close them** — R27, R40, R50,
R58 and R60 — **with R48 a sixth, provisional from the start.** This was a
pattern, not five incidents. Decision 5 resolved all six: four moved to a plan
that claims them, and two kept their owner because that plan was widened or
annotated to reach them.

| Rule | Owner then | Owner now | Why it moved |
|---|---|---|---|
| R27 | 053 | **060** | 053's text is about ABI parity, not the `operations` table's status semantics |
| R40 | 041 | **041** | 041's decision 6 was widened from `delete_paths_logged` to "every path that drops a row writes a snapshot", which reaches R40's two paths |
| R48 | 053 | **060** | provisional, matched to R27 because the schema demanded an owner |
| R50 | 053 | **039** | 029 wrote the defect and is completed; 053 was rhetorical here |
| R58 | 056 | **056** | genuinely 056's subject; a scope note was added because 056 does not name the case |
| R60 | 053 | **060** | 053 owns bringing face labelling to the ABI; it does not own "a rebuild destroys the labels" |

R50's move to 039 rests on 039 naming both of its anchor files at
`plans/039-core-cross-platform.md:46-47` — `core/lockfile.cpp` "Rewrite (75
lines)" and `core/paths.cpp` "Platform split" — and already carrying a "worth
checking while here" list of `paths.cpp` defects. 039 still cannot fully close
R50, because its index row excludes `cli/` and half the fix lives in
`cli/main.cpp`'s call to `lock_path()`. But 039's claim on the `core/` half is
real where 053's was rhetorical.

Plan 060 was opened for the three that had no home: R60, R27 and R48, ordered so
the two mechanical items (a status assignment and a counter) cannot gate the one
that destroys data.

**R58's scope note, checked.** Master widened plan 056 while this branch ran
(`c5e8029`, `07a80d0`), which raised the question of whether the note is still
needed. Checked against master at `07a80d0`: the widening is about
`phig_op_snapshot`, `phig_op_list` and `phig_op_info`, and the only empty
selection it discusses is a thumbs run publishing `Selected` with `total = 0`.
The word `completed` appears once, in the enum listing at line 38. Nothing in
master's 056 covers what `ScanEngine::run_workers` and `OrganizeEngine::apply`
report for an already-cancelled run over an empty selection, which is R58. **The
scope note stands.** Re-check if 056 moves again before the merge.

053 now carries R61, R62, R66 and R68 — libphig-surface work and the `cli/`-only
trash guard, all genuinely its subject.

### What the schema could not express, and what was decided

Four related gaps forced a guess during enumeration. Decisions 4, 6 and 7
resolved three of them.

1. **There is no `unowned` state.** `owner` is required whenever `status` is
   `partial` or `violated`, so a finding nobody owns cannot be filed honestly —
   the pressure the design worried about (delete the inconvenient rule) was
   replaced by a different one (name a plan that will not fix it). Decision 5
   took the alternative: open the plan rather than add the state. `unowned` is
   deliberately not added, and should be reconsidered only if a second
   enumeration produces findings no plan wants. Three rules still carry
   ownership meta-commentary in free-text fields from before the move — R40 in
   `test_gap`, R27 and R48 in `rationale`.

2. **There was no status for "the code is right, nothing pins it".** `holds`
   demands a passing test; `partial` and `violated` assert the code is wrong;
   `unenforced` was defined as "deliberately untestable". Raised independently
   by Tasks 3, 4 and 6. The consequence was that `unenforced` carried three
   distinct meanings across its three members. **Decision 4** standardised on
   `holds` + `test_gap` + open `owner` for that case and narrowed `unenforced`
   to "nothing can pin this today". R26 moved to `holds`; R34 and R65 stayed.
   `unenforced` now means one thing and has two members. The cost is visible in
   the counts: 24 of the 51 `holds` rules carry a gap, and 054 owns 25 test gaps
   against its own Phase 3.

3. **The schema could not express "enforced in the wrong layer".** For `trash
   empty` the rule that every anchor lives under `core/` or `libphig/` and the
   rule that a `cli/`-only guard is a finding could not both hold: there is no
   `core/` symbol to anchor at, because there is no `core/` primitive. It was
   originally filed as R23's `test_gap`, which made the one operation whose
   guard is entirely outside the engine invisible in the by-operation view.
   **Decision 7** rejected modelling "wrong layer" as a category and took the
   narrower route: R68 records it as `violated`, anchored where the code
   actually is (`cli/main.cpp:cmd_trash_empty`), owned by 053, which already
   lists "Remove — `rm`'s modes, trash empty" as missing. Check 8 relaxes to
   "a non-core anchor is legal only on a `violated` rule". When 053 moves the
   code, the anchor updates and the check tightens on its own. No new concern,
   no permanent category.

4. **`protects = []` rendered as an empty row.** R63, R64 and R65 endangered
   none of the six assets — the losses are a misattributed error message, an
   unloadable shared object, and a buffer overrun in the client's address space
   — so they would have rendered as an empty row in `by-asset.md` and vanished
   from the view that ranks by irreversibility. **Decision 6** added a seventh
   asset, `client-process`: a foreign caller's address space and its ability to
   keep running against this library. `protects = []` is now zero rules and
   `by-asset.md` gains a seventh section. This decision and section 7's are
   independent: R63 and R64 are recommended for demotion on their own merits,
   and having an asset neither caused nor blocks that.

---

## 9. What could not be determined

- **The 47-rule snapshot's flag count of 27.** That snapshot and this report's
  first draft both counted flags from root `CLAUDE.md` rather than from the
  parser, which is now known to be the wrong source in both directions. The
  question is moot: the parser accepts 52 and that is the number section 2 uses.
  What the earlier 27 counted is still unknown and no longer matters.
- **Whether `--media` on `scan` can cause a deletion.** It restricts what the
  walk indexes while reconcile still runs over the directory. Whether
  `scan --media image` over a directory holding videos can drop their rows was
  not measured, and is the one read-only-looking scan flag with a plausible
  destructive path.
- **Whether the `read_only_cli` contradiction reaches the ABI.** The measurement
  is CLI-only. `Database`'s `read_only` constructor flag exists and libphig uses
  it; whether every libphig read path uses it was not checked.
- **The forward maintenance rate.** Section 5's 30-of-130 and 25-of-130 are
  measured over one unusually active window, and the symbol-level figure is an
  upper bound because the match is over the whole diff rather than the changed
  lines. It is a rate, not a projection.
- **Whether the R64 replacement check is worth its own plan.** Section 7
  recommends demoting R64 only once a header-versus-CMake version comparison
  exists in the lint job. Resolved after this report was first written: it goes
  to 054's Phase 2, which builds that lint job anyway, and 054 is already R64's
  `owner`. Until it lands R64 stays in the set.

---

## 10. The decisions taken

Read on 2026-08-10 against the first draft of this report. Each is stated with
the reasoning that produced it.

**1. Keep the expanded set; re-test the twelve individually.** The first draft
recommended demoting 12 of 66 rules on a largely flags-based argument. Declined:
the flags framing is not the test, and a blanket prune decides twelve questions
at once. Each of the twelve was to be re-tested against "does keeping this rule
help catch a future data-integrity problem?", with an argument rather than a
verdict, and only genuine failures brought back. Section 7 does that and returns
two: R63 and R64.

**2. Build Phase 2 reduced.** The five decay detectors — anchors resolve,
`holds` tests exist and pass, owners open, ABI and CLI symbols classified,
generated views current — plus view generation, wired into the lint job. The
conformance gate is deferred until `test_kind`'s two definitions are reconciled
in `rules/README.md`; building an adapter suite against an ambiguity would
enforce the ambiguity. The evidence is in section 6: the checks would have
caught almost nothing this run, and every serious defect was caught by a human
reading code, but decay is what the plan exists to stop and none of it happens
in five supervised days.

**3. Generate the matrix; do not gate on it.** 67 of 153 cells would need an
explicit waiver, and five operations account for 32 of them for structural
reasons — those operations do not touch those assets. The one retrospective hit
(the attachments cell where `4435d0a`'s defect lived) comes from *reading* the
empty cells while designing, which generation delivers. The waiver ritual adds
nothing to it.

**4. Standardise on `holds` + `test_gap`; narrow `unenforced`.** `unenforced`
was carrying three different meanings across three rules, so any count of it
counted three things. It now means "nothing can pin this today" and nothing
else; "the code is right and no test fully pins it" is `holds` + `test_gap` +
open owner, which the record set was already drifting toward. R26 moved to
`holds`; R34 and R65 stayed. One paragraph in `rules/README.md`, and no fifth
status — a fifth would re-open the same ambiguity one level down.

**5. Widen 041 for R40; open plan 060; move R50 to 039.** Plan 041's decision 6
was written against `delete_paths_logged` alone, which neither of R40's two
paths calls, so closing 041 as written would have shipped complete and left R40
open. Widened to "every path that drops a row writes a restore snapshot", so R04
and R40 close under one plan and one mechanism. R60, R27 and R48 had no plan
that claimed them and got a new one, 060, ordered so the two mechanical items
cannot gate the one that destroys data. R50 went to 039, which names both of its
anchor files. R58 stayed on 056 with a scope note.

**6. Add a seventh asset, `client-process`.** Three boundary rules protected
none of the six assets, so they would have rendered as an empty row in
`by-asset.md` and disappeared from the view that ranks by irreversibility.
Silently vanishing was the worst of the available outcomes. The asset is a
foreign caller's address space and its ability to keep running against this
library — which is exactly what R65's missing size member endangers.
`protects = []` is now zero rules.

**7. Record a wrong-layer guard as `violated` with its real `cli/` anchor.**
The first draft proposed modelling "enforced in the wrong layer" as a schema
category. Rejected, and the question reframed: the real fix is to move the code,
and plan 053 already owns doing so — `libphig/CLAUDE.md:56` lists "Remove —
`rm`'s modes, trash empty" as missing with owner 053, and libphig links
`phig-core` rather than `cli/`, so 053 cannot expose `trash empty` without
moving it. `cmd_trash_empty` is 33 lines (`cli/main.cpp:697-729`) with an
obvious split: `trash_dir()` is already `core/`, the counting loop and
`fs::remove_all` are the destructive work, and the `isatty` prompt and its
prose are genuinely CLI — the shape `cmd_rm` already has
with `ConfirmFn`. So R68 anchors where the defect is, check 8 relaxes to allow a
non-core anchor only on a `violated` rule, and the anchor and the status both
correct themselves when the code moves. No new concern, no permanent category.

**8. Apply everything on this branch, then merge once.** Master must not see the
report carrying a flag measurement built on a stale source and a prune
recommendation that has been superseded.

**9. Fold the flag work into 054's Phase 3.** Phase 3 becomes: write the missing
tests for `holds` rules, write rules for the mutation-capable flags that have
none, and record the read-only flags as `n/a`. The mutation-capable split must
be read off `cli/main.cpp` rather than scripted — a mechanical split failed
twice, because `scan`, `cp`/`mv`, `rm` and `trash` parse inline in `main()`, so
brace-matching attributes their flags to `main` and a classifier treating `main`
as read-only puts `--on-conflict`, `--hard`, `--orphans` and `--yes` in the
wrong bucket.

**10. Fix the three documentation defects here; schedule `--size`.** Root
`CLAUDE.md`'s CLI reference now matches the parser in both directions, 52 to 52,
with `--size` removed until it is wired. `plans/027-rm-command.md`'s frontmatter
now says `completed`, matching the index — it mattered because Phase 2's owner
check reads the index and any tool reading frontmatter would have accepted 027
as an open owner. `libphig/CLAUDE.md:99-101` now names the three free functions
that do not wrap their body in `try`/`catch`. `--size` itself is scheduled to
plan 043: it is in the help text at `cli/main.cpp:129` and backed by
`ThumbsOptions::size`, and wiring it is a `cli/` behaviour change that does not
belong on this branch.
