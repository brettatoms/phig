# `ctest` is 7x slower than it needs to be because tests share state

**Status:** ready to plan
**Date:** 2026-08-05
**Found:** while measuring the feedback loop during the monorepo migration

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

## Confirmed failures under `-j12`

Two named in the run output; the summary reported 8 total:

- `DatabaseTest.UpdatePathOntoStaleDestClearsItsFaces`
- `DatabaseTest.MigratesV4ToV5`

Both pass serially and both are `DatabaseTest`, which points at contention over shared
SQLite state — most likely a fixed database path or a shared temp file reused across
cases, rather than a per-test temporary.

**Get the full list first.** `ctest -j12` and capture all 8 names before changing anything;
do not assume the other 6 are also `DatabaseTest`.

## What to investigate

1. **Where does each failing test put its database?** If cases share one path, two
   processes racing on the same file explains it directly. A per-test unique temp
   directory is the obvious fix.
2. **Is anything else shared** — the thumbnail cache (`~/.cache/phig/thumbs`), the models
   directory, the advisory `flock` on `<db>.lock`, the diagnostic log? A test that writes
   to a real XDG path rather than a temp one will collide with any other test doing the same.
3. **Does the migration test have an ordering assumption?** `MigratesV4ToV5` implies a
   specific starting schema version. If it mutates a database another test also opens, the
   failure is ordering, not concurrency.
4. **`RESOURCE_LOCK`** is the escape hatch if a test genuinely cannot be isolated: it lets
   ctest serialize just those cases while everything else runs parallel. Prefer real
   isolation; use this only where isolation is impractical, and say which tests and why.

## What to build

Make the failing tests independent, then make `-j` the default so the speedup is actually
collected rather than requiring a flag people forget:

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
