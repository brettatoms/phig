---
name: CI & Linting
status: planned
created: 2026-07-05
description: >
  Add GitHub Actions CI that builds and runs the test suite on ubuntu-latest and
  macos-latest against system packages, plus a lint gate: clang-format
  (formatting/whitespace), compiler warnings (-Wall -Wextra), and clang-tidy
  (curated static analysis). Adopt a standard clang-format style with a one-time
  repo-wide reformat. Depends on plan 018 (stock-OpenCV build).
---

# CI & Linting

## Goal

phig has no CI, no lint/format configuration, and no compiler warning flags. Add a
GitHub Actions workflow that, on every push and pull request, builds phig and runs
its test suite on both primary user platforms using **system packages**, and gates
on formatting and static analysis. This locks in the portability work from plan 018
and enforces the project's whitespace-hygiene and quality standards automatically.

## Dependency on plan 018

CI builds against **stock system OpenCV** (apt/Homebrew), which does not include the
`opencv_contrib` `img_hash` module. Plan 018's reimplementation of pHash against core
OpenCV must land first (or in the same change) so the CI build resolves without a
contrib package. This plan assumes `img_hash` is gone.

## Design Decisions

### Runners: ubuntu-latest + macos-latest

The repo is public, so GitHub-hosted runners (including macOS) have no minute limits
or cost. A two-entry matrix builds and tests on:

- `ubuntu-latest` — deps via `apt`
- `macos-latest` — deps via Homebrew (Apple Silicon, matching the dev machine)

The multi-distro container matrix (debian/fedora/arch) remains a **manual**
verification step in plan 018, not part of per-push CI, to keep CI fast.

### Lint gate: clang-format + warnings + clang-tidy

Three layers, all blocking:

1. **clang-format** — `clang-format --dry-run --Werror` over `src/**` and `tests/**`.
   Enforces formatting and the whitespace-hygiene rule (no trailing whitespace, no
   whitespace on blank lines).
2. **Compiler warnings** — build with `-Wall -Wextra`; CI treats them as errors.
   Introduce a CMake option `PHIG_WERROR` (default `OFF`) that adds `-Werror`; CI
   configures with `-DPHIG_WERROR=ON` so local builds aren't broken by newer-compiler
   warnings.
3. **clang-tidy** — curated checks over `src/**`, using the existing
   `compile_commands.json` (`CMAKE_EXPORT_COMPILE_COMMANDS` is already `ON`). Run on
   ubuntu only (one platform is sufficient for static analysis).

clang-tidy will likely flag existing code on first run; an **initial cleanup pass** is
part of this plan so the gate starts green. The vendored `vendor/sqlite-vec/` and
`vendor/inih/` (plan 017) are excluded from both clang-format and clang-tidy.

### clang-format style: adopt LLVM, reformat once

Use a standard base style (`BasedOnStyle: LLVM`, `ColumnLimit: 100`) rather than
reverse-engineering the current ad-hoc style. This means a **one-time repo-wide
reformat** committed separately from logic changes so the diff is reviewable. LLVM is
the clang-format default and a common choice for modern C++ CLI tools; the base is a
one-line change if a different standard is preferred later.

### clang-tidy check set

A pragmatic starting set in `.clang-tidy`, tuned to avoid high-noise checks:

```yaml
Checks: >
  -*,
  bugprone-*,
  performance-*,
  modernize-*,
  -modernize-use-trailing-return-type,
  readability-*,
  -readability-magic-numbers,
  -readability-identifier-length,
  clang-analyzer-*,
  cppcoreguidelines-*,
  -cppcoreguidelines-avoid-magic-numbers,
  -cppcoreguidelines-pro-bounds-pointer-arithmetic,
  -cppcoreguidelines-pro-bounds-array-to-pointer-decay,
  -cppcoreguidelines-owning-memory
WarningsAsErrors: '*'
HeaderFilterRegex: '^src/'
```

The exact enabled/disabled list is refined during the cleanup pass; the intent is
"real bugs and clear modernizations, not stylistic noise."

## Workflow Shape

`.github/workflows/ci.yml`, triggered on `push` (master) and `pull_request`:

- **`test`** job (matrix: ubuntu-latest, macos-latest):
  1. Checkout
  2. Install deps (apt / brew — the plan 018 dependency lists)
  3. `cmake -B build -DPHIG_WERROR=ON`
  4. `cmake --build build`
  5. `ctest --test-dir build --output-on-failure` (or run `./build/phig-tests`)
- **`lint`** job (ubuntu-latest):
  1. Checkout
  2. Install deps + `clang-format`, `clang-tidy`
  3. `clang-format --dry-run --Werror` over `src/**` `tests/**`
  4. `cmake -B build` (generates `compile_commands.json`)
  5. `run-clang-tidy -p build src/` (or `clang-tidy` per file)

## Implementation

### New Files

- `.github/workflows/ci.yml` — the workflow above
- `.clang-format` — `BasedOnStyle: LLVM`, `ColumnLimit: 100`
- `.clang-tidy` — curated check set above

### Modified Files

- `CMakeLists.txt`:
  - Add `option(PHIG_WERROR "Treat warnings as errors" OFF)`
  - Add `-Wall -Wextra` to `phig-lib`/targets; add `-Werror` when `PHIG_WERROR`
- All `src/**` and `tests/**` — one-time clang-format reformat (separate commit)
- `src/**` — clang-tidy cleanup fixes to reach a green gate
- `AGENTS.md` — note CI + `clang-format`/`clang-tidy` under a "Development" section
- `README.md` — optional CI status badge

## Verification

- Workflow runs green on a PR: both `test` matrix legs pass, `lint` passes.
- `clang-format --dry-run --Werror` produces no diff after the reformat commit.
- Local `cmake -B build && cmake --build build` still succeeds **without**
  `PHIG_WERROR` (warnings don't break local dev).
- clang-tidy reports zero findings under the curated set after cleanup.

## Deviations from the original draft

- **Style:** chose **Google + 4-space + 100-col** (not LLVM 2-space) — 4-space
  matches the existing code and is the smaller reformat.
- **clang-tidy set narrowed to high-signal:** the drafted broad set (`cppcoreguidelines-*`,
  full `readability-*`/`modernize-*`) produced ~340 mostly-stylistic findings. Kept
  `bugprone-*` + `clang-analyzer-*` + `performance-*`, and disabled noisy/idiomatic
  checks: `easily-swappable-parameters`, `branch-clone`, `narrowing-conversions`,
  `empty-catch`, `exception-escape`, `clang-analyzer-deadcode.DeadStores`.
- **Vendored code split** into a separate `phig-vendor` target so `-Werror` and
  clang-tidy apply only to first-party code.
- **`scripts/clang-tidy.sh`** added: the Nix `clang-tidy` can't find libstdc++
  headers, so the wrapper injects g++'s C++ header dirs at runtime. CI runs
  `run-clang-tidy` directly (system toolchain).
- **`gtest_discover_tests` WORKING_DIRECTORY** set to the source root so `ctest`
  resolves the video tests' `tests/data/` relative paths.
- **Actions pinned to full commit SHAs** (`actions/checkout` v4.3.1), least-privilege
  `permissions: contents: read`, and `concurrency` cancellation.

## Checklist

- [x] Confirm plan 018 `img_hash` removal has landed (CI build prerequisite)
- [x] Add `.clang-format` (Google, 4-space, 100-col)
- [x] One-time repo-wide reformat commit (src + tests; exclude vendor/)
- [x] Add `.clang-tidy` high-signal check set
- [x] clang-tidy cleanup pass to reach green (0 findings)
- [x] CMake `PHIG_WERROR` option + `-Wall -Wextra` (vendored code isolated)
- [x] `.github/workflows/ci.yml` — test matrix (ubuntu + macOS)
- [x] Workflow lint job (clang-format + clang-tidy)
- [ ] Verify green on a PR (pending push)
- [x] AGENTS.md "Linting & CI" note (format/lint/CI)
- [ ] Optional README CI badge (skipped for now)
