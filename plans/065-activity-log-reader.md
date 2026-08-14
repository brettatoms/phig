---
name: Activity Log Reader & Undo
status: designing
created: 2026-08-14
description: >
  The activity log has no reader anywhere in phig. Design one alongside undo,
  then surface it as `phig log` and through libphig. Picks up what plan 020
  deferred.
---

# 065 — Activity Log Reader & Undo

## Goal

047's Screen 7 has no data source, and neither does the CLI. `Database` has
`log_begin_operation`, `log_add_entry`, `log_finish_operation` and `log_prune`,
and no reader at all — so this is `core/` work before it is ABI work. Plan 020
built the log and deferred both the `phig log` viewer and undo; this is that
plan's successor rather than only an ABI item.

## Why the two are designed together

020 deferred them together and that was right. A reader shaped only for
display, then reshaped later for undo, is a reader designed twice: undo needs
to know which entries in an operation are reversible, in what order, and
against what current state, and none of that is visible from a list built to
render rows on a screen.

The log is append-only and undo is a future logged operation, not an erasure —
so undoing an undo is a case the design has to answer, not a corner.

## Open questions

1. **What makes an entry reversible.** A `move_file` has a source and a
   destination. A `delete_file` has neither the bytes nor a snapshot. Trash
   sits between the two. The reader's record has to distinguish these, and the
   answer determines whether undo is per-entry, per-operation, or both.
2. **What a stale entry means.** Undoing a move whose destination was
   subsequently edited, moved again, or deleted is the ordinary case rather
   than the exception. Organize's apply-time re-verification (R59, R66) is the
   nearest precedent.
3. **Whether the reader pages.** Consistent with 044 it should, and
   `log_prune`'s default cap of 100,000 entries sets the scale.
4. **Whether undo is an operation.** It moves files, so by 053's decision 1 and
   R66 it is a `phig_op*` on plan/apply, not a call.

## Scope

- A reader in `core/`, designed against undo's requirements.
- `phig log`.
- The ABI surface for 047's Screen 7.
- Undo, if the design above holds together; otherwise undo splits out again
  with the reader shaped to accept it.

## Depends on

053 — the selection surface. 020 — the log this reads.
