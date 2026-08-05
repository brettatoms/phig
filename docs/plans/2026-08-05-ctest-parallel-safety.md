# `ctest` is 7x slower than it needs to be because tests share state

**Status:** fixed in `plans/030-test-parallel-isolation.md`
**Date:** 2026-08-05
**Found:** while measuring the feedback loop during the monorepo migration
**Updated:** 2026-08-05, after running `-j12` 13 times to get the real failure set

## The problem

`ctest --test-dir build` takes **50 seconds** to run the same 640 tests the two test
binaries run in **6 seconds** between them (`phig-tests` 3s for 636, `phig-perf-tests` 3s
for 4). ctest registers each test as its own process and runs them serially, so the wall
clock is dominated by process startup and per-test fixture setup rather than by the tests.

`ctest -j12` cuts it to **7 seconds**. But 8 tests fail under `-j12` that pass serially,
so parallel is not currently a safe default.

Measured on 12 cores:

| Step | Time |
|---|---|
| incremental build | 0s |
| clang-format (100 files) | 0s |
| clang-tidy (34 files) | 5s |
| `phig-tests` (636 tests) | 3s |
| `phig-perf-tests` (4 tests) | 3s |
| **`ctest` (serial)** | **50s** |
| `ctest -j12` | 7s, 8 failures |
| full clean rebuild `-j12` | 34s |

This matters because `ctest` is the gate every task and every CI run uses. 45 of those 50
seconds are waste, paid several times per change.

## Failures under `-j12`

**There is no fixed set of 8.** The count is reproducible at roughly 2-8 per run, but the
names change every time. Across 10 runs, 29 distinct tests failed and none failed in all
10; the worst was 4/10. An earlier draft of this doc named two `DatabaseTest` cases and
inferred shared SQLite state from them — that was one run's sample, and the inference was
wrong.

Most frequent across 10 runs:

| Test | Failed in |
|---|---|
| `HasherTest.Sha256AllLengthsUpTo200MatchReference` | 4/10 |
| `HasherTest.PhashDifferentSizes` | 3/10 |
| `DatabaseTest.LogOperationRoundTrip` | 3/10 |
| `HasherTest.PhashPNG`, `PhashConsistent`, `PhashDifferentImages` | 2/10 each |
| `DatabaseTest.ReinsertPreservesGallery`, `InsertBatchCannotNestInsideATransaction` | 2/10 each |
| 21 others across Database, Sidecar, Organize, Hasher | 1/10 each |

Failures spread across `HasherTest`, `DatabaseTest`, `ScannerTest`, `ConfigTest`,
`SidecarTest`, `OrganizeExecTest`, `RemoveExecTest`, `RelinkTest`, `FaceDatabaseTest`,
`PorcelainTest`, and `MoveFileTest` — not one suite.

## Cause: fixtures share hardcoded temp paths

Every failure message is a path collision. Nothing timed out and no test computed a wrong
value. Exception text over 3 runs:

| Message | Count |
|---|---|
| `SQL error: disk I/O error` | 10 |
| `SQL error: database is locked` | 2 |
| `cannot remove: Directory not empty [/tmp/image-cleanup-test-hasher]` | 2 |
| `cannot remove: Directory not empty [/tmp/phig-test-config]` | 1 |
| `Cannot open file for hashing: /tmp/image-cleanup-test-hasher/sweep.bin` | 1 |
| `Cannot decode image: /tmp/image-cleanup-test-hasher/a.jpg` | 1 |

The mechanism, from `tests/test_hasher.cpp:22`:

```cpp
test_dir = fs::temp_directory_path() / "image-cleanup-test-hasher";
fs::remove_all(test_dir);
fs::create_directories(test_dir);
```

A fixed directory name shared by every case in the suite, wiped in `SetUp` and again in
`TearDown`. ctest runs each case as its own process, so under `-j` one case's teardown
deletes the directory another case is reading. That accounts for all four symptoms,
including the two that look unrelated to concurrency: `Cannot open file for hashing` is a
fixture file removed from under a reader, and `Directory not empty` is a teardown racing
another process's setup.

`HasherTest` opens no database, which is why a database-only explanation could not have
covered these — the shared-directory explanation covers both the DB and non-DB failures.

**Scope: ~50 fixtures across ~30 files** build a path from `temp_directory_path()` with a
fixed name. This is the default pattern in the suite, not a few stragglers.

## Rejected: lowering the job count

`-j$(nproc)/2` would cut the collision probability without removing a single collision.
That converts a reproducible failure into a rare flake and pays the cost on every CI run
forever. Keep it in reserve only for a test that proves genuinely un-isolatable.

`RESOURCE_LOCK` remains the escape hatch for that case: it serializes named tests while
the rest run parallel. Prefer real isolation; if used, say which tests and why.

## Not the cause

Recorded so it is not re-investigated:

- **Resource exhaustion / OpenMP oversubscription.** `-j12` runs 12 OpenCV processes each
  with its own thread pool, which fits the symptom shape (random tests, low individual
  rates, unrelated suites). Ruled out by the failure text: every message names a path, and
  the fastest failure was 18 ms. No timeouts.
- **Ordering assumptions in the migration tests.** The failing set is not stable, so it is
  not a fixed order dependency.

## What to build

Give every fixture a unique temp directory via one shared helper in a test support header,
then make `-j` the default so the speedup is collected rather than requiring a flag people
forget.

In-memory SQLite (`:memory:`) was considered and rejected as the primary mechanism: most of
these fixtures need real files to hash, decode, or move (`HasherTest`, `SidecarTest`,
`OrganizeExecTest` have no database to relocate), and specific tests assert on-disk
semantics — `CreatesDatabaseDirectory`,
`ReadOnlyOpenFailsOnMissingFileAndDoesNotCreateIt`, and the `<db>.lock` flock tests. One
mechanism that fixes DB and non-DB cases alike beats two that each cover part.

Detail:

- Set `PROCESSORS`/parallel defaults in CMake, or document `ctest -j` in `CLAUDE.md`'s
  build block and use it in `.github/workflows/ci.yml`.
- **The gate is that `ctest -j12` passes 640/640 repeatedly**, not once. Run it at least 20
  times — an intermittent shared-state failure will not show up in a single green run. Report
  the pass count out of the runs attempted.

## Scope notes

- **Do not disable, skip, or loosen the failing tests to make parallel green.** They pass
  serially, so they are correct tests finding a real isolation gap. Weakening them trades a
  measurable problem for an invisible one.
- Fixing isolation should not change what any test asserts. If a test only passes because
  of state another test left behind, that is a finding worth reporting on its own.
- The 50s serial number is not a regression from the monorepo migration — it predates it.
  The migration is just where it got measured.
