---
name: Test Parallel Isolation
status: completed
created: 2026-08-05
description: Give every test fixture a unique temp directory via one shared helper (`tests/temp_dir.h`), replacing the ~50 hardcoded `temp_directory_path() / "fixed-name"` paths that make `ctest -j` fail. Fixes the collisions rather than lowering the job count, then makes `-j` the default in CMake and CI so the 50s serial suite runs in ~7s. In-memory SQLite rejected as the mechanism: most affected fixtures test on-disk behavior.
---

# 030 — Test Parallel Isolation

`ctest --test-dir build` takes 50s to run the same 640 tests the two binaries run in 6s.
`-j12` cuts it to 7s but fails 2-8 tests per run, with the failing names changing every
run. Diagnosis in `docs/plans/2026-08-05-ctest-parallel-safety.md`: ~50 fixtures across
~30 files derive their temp path from `fs::temp_directory_path()` with a **fixed name**, so
under `-j` one case's `TearDown` deletes a directory another case is using.

This plan makes the paths unique. It does not change what any test asserts.

## Cause, in one example

`tests/test_hasher.cpp:22`:

```cpp
test_dir = fs::temp_directory_path() / "image-cleanup-test-hasher";
fs::remove_all(test_dir);
fs::create_directories(test_dir);
```

Every `HasherTest` case shares that directory and wipes it twice — once in `SetUp`, once in
`TearDown`. The `remove_all` in `SetUp` is as much of the problem as the one in `TearDown`:
it exists to clear residue from a previous run, which per-run-unique paths make impossible.

Observed failures: `SQL error: disk I/O error` (10 across 3 runs), `database is locked` (2),
`cannot remove: Directory not empty` (3), `Cannot open file for hashing` (1), `Cannot decode
image` (1).

## Approach

One helper, used by every fixture. Uniqueness comes from PID plus a per-process counter:
ctest gives each test case its own process, so PID separates concurrent cases, and the
counter separates multiple directories within one case.

```cpp
// tests/temp_dir.h
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

// A unique temp directory per caller. ctest runs each test case as its own
// process, so two cases sharing a fixed path race: one's teardown deletes the
// directory the other is reading. PID separates concurrent cases; the counter
// separates multiple directories inside one case.
//
// Callers own the returned directory and should remove_all() it in TearDown.
// Never remove a parent shared with another test.
inline std::filesystem::path make_temp_dir(const std::string& label) {
    static std::atomic<unsigned> counter{0};
    auto unique = std::to_string(::getpid()) + "-" + std::to_string(counter++);
    auto dir = std::filesystem::temp_directory_path() / ("phig-" + label + "-" + unique);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
```

Each fixture becomes:

```cpp
void SetUp() override {
    test_dir = make_temp_dir("hasher");        // creates it
}
void TearDown() override { fs::remove_all(test_dir); }
```

Three call-site shapes need converting:

1. **Fixture directory** (`TEST_F` with SetUp/TearDown) — the common case.
2. **Bare `.db` file path** — e.g. `temp_directory_path() / "image-cleanup-test.db"`.
   Becomes `make_temp_dir("db") / "test.db"`, so the database sits inside an owned
   directory and SQLite's `-wal`/`-shm` siblings are contained with it.
3. **Bare `TEST` bodies** that build their own dir inline (several in
   `test_database.cpp`'s `DatabaseConcurrency`, `test_paths.cpp`, `test_video.cpp`).

`DatabaseConcurrency` tests deliberately open one database from several threads. That is
intra-test concurrency and stays as-is; only the enclosing directory becomes unique.

## Tasks

**1. Add `tests/temp_dir.h`** → verify: a scratch test calling `make_temp_dir("x")` twice
gets two different existing paths.

**2. Convert fixtures, file by file.** Start with the three files with confirmed failures
and the most cases — `test_hasher.cpp`, `test_database.cpp`, `test_config.cpp` — then the
rest. Delete the now-pointless `remove_all` in each `SetUp`. → verify after each file:
`./build/phig-tests --gtest_filter='<Suite>.*'` passes, and `ctest -R '<Suite>\.' -j12`
passes.

**3. Sweep for stragglers** → verify: `grep -rn 'temp_directory_path()' tests/` shows only
`temp_dir.h` and deliberate exceptions. Two known exceptions to leave alone:
`test_settings.cpp:129` passes the temp root itself to assert a load failure, and
`test_paths.cpp` cases that assert on path *construction* may not touch disk. Confirm each
before converting; note any left and why.

**4. Make `-j` the default** → verify: a bare `ctest --test-dir build` runs parallel and
takes ~7s, not 50s.
   - CMake: `set(CTEST_BUILD_FLAGS -j)` does not affect a direct `ctest` invocation.
     Prefer a `ProcessorCount`-derived default committed in `CTestConfig.cmake` or a
     documented `CTEST_PARALLEL_LEVEL`. Confirm which actually takes effect before
     claiming it works — this is the step most likely to silently do nothing.
   - `.github/workflows/ci.yml`: add `-j` to the ctest step.
   - `CLAUDE.md`: update the build block's `ctest --test-dir build` line and its
     "both — 640, what CI runs" comment.

**5. Verify the gate** → see below.

## Gate

`ctest -j12` passing **640/640 across at least 20 consecutive runs**. One green run proves
nothing — the current failure rate is 2-8 per run with a shifting set, and the most frequent
single test only failed 4/10.

Report the pass count out of runs attempted. If any run fails, the isolation is incomplete;
fix the named test rather than re-running for a green.

Also confirm serial still passes (`ctest --test-dir build -j1`) and the real-run checks from
`CLAUDE.md` are unaffected:

```bash
./build/phig search --count --db ~/.local/share/phig/phig.db   # 29925
./build/phig --version                                          # phig 0.3.0
```

One pre-existing skip is expected: `ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing`.

## Out of scope

- **Lowering the job count.** `-j$(nproc)/2` reduces collision probability without
  removing a collision, trading a reproducible failure for a rare flake paid on every CI
  run. Rejected in the diagnosis doc.
- **Disabling, skipping, or loosening any test.** They pass serially; they are correct
  tests finding a real isolation gap.
- **Changing assertions.** If a test only passes because of state another test left behind,
  that is a finding to report, not to paper over.
- **In-memory SQLite.** Rejected as the mechanism: most affected fixtures need real files
  (`HasherTest`, `SidecarTest`, `OrganizeExecTest` have no database to relocate), and
  `CreatesDatabaseDirectory`, `ReadOnlyOpenFailsOnMissingFileAndDoesNotCreateIt`, and the
  `<db>.lock` flock tests assert on-disk semantics.
- **`gtk/`'s separate suite** (13 tests, own build). Same audit may apply; not this plan.

## Outcome

Gate met: **25/25 consecutive `ctest --parallel` runs at 640/640**, down from 2-8 failures
per run. Serial (`-j1`) also 640/640. 48s → 7s. Real-run checks unchanged (29925,
phig 0.3.0). No new clang-tidy findings (44 pre-existing `bugprone-unused-local-*` in
`core/`, identical before and after).

Two things the plan did not predict:

- **A shared path outside `temp_directory_path()`.** Three `MoveFileTest` EXDEV tests
  shared a hardcoded `/dev/shm/phig-move-test`, invisible to a `temp_directory_path()`
  sweep. This was the last residual failure and only surfaced once in ~20 runs.
  `make_temp_dir` took an optional `root` parameter so second-device tests get the same
  per-process uniqueness.
- **`-j` cannot be defaulted from CMake.** Measured: `CTEST_PARALLEL_LEVEL` in
  `CTestConfig.cmake` is ignored by a direct `ctest` invocation (still 48s). Only the env
  var or the `--parallel` flag works. So this is a docs + CI change, not a build-system
  one — `--parallel` in `.github/workflows/ci.yml` and in `CLAUDE.md`'s build block, with
  a note on why it is explicit.

The 13 files already using ad-hoc `getpid()` uniqueness were left alone rather than
normalized onto the helper: they are collision-safe as written, and rewriting working
fixtures is churn the isolation fix does not need.

## Risk

Mechanical but wide — ~50 fixtures across ~30 files. The failure mode of a bad conversion
is a test that silently stops testing what it did (e.g. pointing at a directory it never
creates, or a `remove_all` on a parent that deletes a sibling's data). Per-file verification
in task 2 is there to catch that, and no assertion should change in the diff.
