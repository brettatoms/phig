---
name: Duplicates ABI
status: planned
created: 2026-08-14
description: >
  Expose exact and near duplicate groups through libphig as an operation with
  progress and cancellation, in the group shape 047's Screen 3 reviews rather
  than as a flat page.
---

# 062 — Duplicates ABI

## Goal

047's Screen 3 has no data source. `core/duplicates.h`
(`find_near_duplicates`, `hamming_distance`) and `Database::get_exact_duplicates`
exist and `cli/main.cpp`'s `cmd_duplicates` renders them; a client gets nothing.

## Shape

**An operation, not a request.** Settled by 053's decision 1: measured at
threshold 5, `find_near_duplicates` runs 72 ms at 30,000 images and 69,406 ms
at 1,000,000, growing about 4x per doubling above 100k. A `phig_op*` with
progress and cancellation is the only shape that survives a large collection,
and 052 must expose `GET /duplicates` the same way.

**A group result, not a page.** Exact and near arrive from one surface, each
group carrying its type, its distance and its members. `phig_op_duplicates_result`
follows the organize plan's ownership convention — a caller-owned value the
caller frees, so it outlives the operation that produced it.

**Selection is ids**, per 053's decision 2. Note the asymmetry plan 002 fixed
and this must preserve: `--match` selects which groups are *shown*, not which
images are *compared*. Comparison is always against the full collection.

## Scope

- The operation: start, poll, result, free.
- Group-shaped result records and their accessors.
- Exact and near in one surface, with the type selector and threshold 047's
  toolbar exposes.
- Real-behavior tests, since the screen above this one deletes files.

## Not in scope

- **Acting on a group.** Trashing the non-keep members is remove — 064.
- **The algorithm above 100k images.** 044 owns it. This plan makes the cost
  visible and cancellable; it does not reduce it.
- **The "keep all / not duplicates after all" state** 047 describes. That is
  persistent per-group metadata phig does not have, and it needs its own
  design.

## Depends on

053 — the selection surface and the request-or-operation decision.
