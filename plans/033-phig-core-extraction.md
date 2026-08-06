---
name: phig-core Extraction
status: completed
created: 2026-08-01
description: >
  Move the five commands' orchestration out of a monolithic `main.cpp` into
  engines in `core/`, on shared concurrency primitives — `CancelToken`,
  `parallel_for_each`, `ProgressQueue<T>` — so each is unit-testable and
  callable from a library instead of only from argv. Executed as five
  superpowers plans in `docs/plans/` (part1, part2a, part2b-i, part2b-ii,
  search-engine). Retroactively indexed 2026-08-06.
---

# 033 — phig-core Extraction

Retroactive index entry for shipped work. The execution plans and their SDD
ledgers are the detailed record; this file exists so the index shows the
change.

Before this, `main.cpp` held scan, thumbs, face, and search orchestration
inline, each hand-rolling its own thread pool around a file-scope `SIGINT`
flag (`g_interrupted`). Nothing was testable without running the binary, and
nothing was callable from a library — which blocked both `libphig` (plan 034)
and any GUI.

## What shipped

Five engines in `core/`, all following one shape documented in
`core/CLAUDE.md`: options struct in (stored by value), result struct out, no
user-facing prose inside the engine.

- `ScanEngine`, `ThumbsEngine`, `FacesEngine` — take a `CancelToken`, publish
  to an optional `ProgressQueue<T>*`
- `SearchEngine` — neither, and the only engine with no OpenCV dependency: the
  caller decodes the query image and passes embeddings or a phash in
- `organize_exec`, `remove_exec` — the cp/mv and rm executors

Shared primitives: `CancelToken`, `parallel_for_each` (whose exception
handling replaced a terminate-on-error abort), and a bounded
`ProgressQueue<T>` that lets an engine publish progress without invoking
caller code — which is what keeps client callbacks out of the database lock.

`main.cpp` kept argv parsing, output rendering, and signal handling.
`g_interrupted` was deleted.

Defects fixed along the way: stale test forks, a whole-file SHA256 read, an
O(n²) duplicate scan, a face-match dedup bug, plus two found during part2b
research.

## Execution artifacts

- `docs/plans/2026-08-01-phig-core-part1.md` — primitives + three defect fixes
- `docs/plans/2026-08-03-phig-core-part2a.md` — commands onto the primitives,
  `g_interrupted` deleted
- `docs/plans/2026-08-03-phig-core-part2b-i.md` — `ProgressQueue<T>`,
  `ScanEngine`
- `docs/plans/2026-08-03-phig-core-part2b-ii.md` (+ ledger) — `ThumbsEngine`,
  `FacesEngine`
- `docs/plans/2026-08-04-search-engine.md` (+ ledger) — `SearchEngine`
- `docs/specs/2026-08-01-phig-core-extraction-design.md` — the design

## Notes for later work

`ScanEngine`'s `run_workers`/`run_reconcile` split is not cosmetic: reconcile
mutates user-authored metadata inside one logged operation and may not poll the
cancel token. No other engine needed the split. See `core/CLAUDE.md`
"Cancellation boundaries" before assuming cancellation covers a whole `run()`.
