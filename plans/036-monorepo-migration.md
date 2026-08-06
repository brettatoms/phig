---
name: Monorepo Migration
status: completed
created: 2026-08-05
description: >
  Combine `phig` and `phig-gtk` into one repository with per-product
  directories, preserving both histories: `phig-gtk` merged at `gtk/` via
  `git subtree` (all 14 commits), and the flat `src/` — which held the engine,
  the C ABI, and the CLI together — split into `core/`, `libphig/`, and `cli/`.
  Also swapped devenv/Nix for mise and repointed the lint gates. Executed as
  `docs/plans/2026-08-05-monorepo-migration.md`. Retroactively indexed
  2026-08-06.
---

# 036 — Monorepo Migration

Retroactive index entry for shipped work. The execution plan is the detailed
record; this file exists so the index shows the change.

This is the change that produced the layout the root `CLAUDE.md` documents.
Before it, `src/` held the engine, the C ABI, and the CLI in one directory —
so the boundary `libphig` was meant to establish was invisible in the tree —
and `phig-gtk` was a separate repo consuming installed `libphig` through
pkg-config.

## What shipped

`phig` became the monorepo root. `phig-gtk` merged in at `gtk/` with
`git subtree`, keeping all 14 of its commits. `src/` split into `core/`,
`libphig/`, and `cli/`, siblings of the product directories, each with its own
`CMakeLists.txt` and `CLAUDE.md`.

The environment moved from devenv/Nix to mise, which pins
`cmake`/`clang-format`/`clang-tidy` to CI's versions but is optional — native
libraries come from the system package manager and a plain `cmake` works.

Lint gates were repointed at the new directories (commit `6a13144` for
clang-format, `9c2751f` for clang-tidy, which exposed violations that were
fixed).

Constraint held throughout: every task ended with a working build and a green
suite — 640 phig tests, 13 gtk tests.

## Execution artifacts

- `docs/plans/2026-08-05-monorepo-migration.md` — the plan

## Consequences worth knowing

- `gtk/` keeps its **own** build (`cmake -B build && cmake --build build -j`
  from inside `gtk/`) and its own test binaries. It is not part of the root
  build.
- CI's clang-format glob is `core libphig cli tests -maxdepth 1`, so nested
  files are not linted — `libphig/include/phig/phig.h` in particular.
- Docs written before this migration cite `src/` paths that no longer exist.
  `docs/` files dated on or before 2026-08-05 need their paths translated when
  read: `src/paths.cpp` → `core/paths.cpp`, `src/main.cpp` → `cli/main.cpp`,
  `src/libphig/` → `libphig/`.
