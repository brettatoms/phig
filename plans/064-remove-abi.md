---
name: Remove ABI
status: planned
created: 2026-08-14
closes_rules: [R61, R68]
description: >
  Turn remove's preview into a value, move the trash-empty guard into `core/`,
  and expose rm's four modes and `trash empty` through libphig on the plan/apply
  shape organize established.
---

# 064 — Remove ABI

## Goal

047's Remove dialog has no action, and remove is the last destructive operation
a client cannot reach. `remove_exec.h`'s `cmd_rm` already takes a
`RemoveOptions`, so the engine work is largely done; what is missing is a
boundary shape.

## Two guards in the wrong place first

**R61 — the preview is prose.** `cmd_rm` computes exactly what organize's plan
carries and writes it to `std::cout`, returning an `int`. A caller that is not
a terminal gets no list, no counts, and no way to render "these 412 files will
be deleted". Remove cannot take the plan/apply shape R66 requires until its
preview is a value. This is the load-bearing half of a wider drift the record
describes: 23 lines across seven `core/` files write to stdout or stderr, ten
of them in `remove_exec.cpp`, against `core/CLAUDE.md`'s rule that an engine
carries no user-facing prose.

**R68 — `trash empty`'s guard lives in `cli/`.** `cmd_trash_empty`
(`cli/main.cpp`) is the one command with no restore snapshot and no
activity-log entry behind it, so what it deletes is gone. Its guard is real and
correctly shaped, and it is `cli/` code with no `core/` counterpart, so
libphig, `phigd` and every GUI would each have to reimplement the confirmation
rather than inherit it. `operations.toml` already names the missing primitive.
The record also notes no test calls `cmd_trash_empty` at all.

## Shape

**Plan/apply, never a callback** — R66, with organize as the worked example.
The plan is a caller-owned value that outlives the operation that computed it;
apply re-verifies every decision at execution time so a stale plan cannot cause
an action the re-check would not independently justify. `cmd_rm`'s `ConfirmFn`
is a `cli/` affordance and does not cross.

**Selection is ids**, per 053's decision 2 — which matters more here than
anywhere else, because the caller authorizes exactly the rows it inspected.
053's decision 3 is what makes that safe: without stable `images.id`, a
resolved id could name a different photo by the time apply runs, and the
consequence is deleting the wrong file.

## Scope

- R61: `cmd_rm`'s preview becomes a `RemovePlan`, and its prose moves to
  `cli/`.
- R68: a `core/` trash-empty primitive carrying the guard, with the subprocess
  test the record says does not exist.
- The trash-empty guard accounts for a catalog directory the user chose. A
  catalog can be any directory, so `<catalog>/trash` can hold entries phig did
  not put there; the primitive refuses those rather than deleting everything
  it finds.
- The four modes — trash, `--hard`, `--db-only`, `--orphans` — and
  `trash empty`, on plan/apply.
- Real-behavior tests, per root `CLAUDE.md`'s data-safety convention.

## Not in scope

- The other thirteen stdout lines R61 counts across `core/`. They are
  diagnostics and progress cosmetics, not an operation's result.

## Depends on

053 — the selection surface, and stable `images.id` in particular.
