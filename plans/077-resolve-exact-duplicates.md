---
name: Resolve Exact Duplicates
status: completed
created: 2026-11-14
completed: 2026-11-14
description: >
  A `core/` operation that keeps one file from each group of byte-identical
  files and trashes the rest, after carrying their user metadata to the file
  kept, plus a `phig duplicates resolve` command. Shaped as plan, override and
  apply, so a GTK review screen can drive it later. Depends on 076's mtime rule
  and verify-then-trash sequence. Excludes near-duplicates and the GTK screen.
---

# 077 — Resolve Exact Duplicates

## Problem

phig reports duplicate groups and does nothing about them. Removing the extra
copies today means running `phig rm --match <path>` once per file, and `rm`
does not carry tags, galleries, favorites or faces to the copy that stays.

That metadata matters even in a collection with few tags. The folder a copy
sits in is information: a photo in a `trip` folder and its copy in a dated folder
should end up as one file with the `trip` gallery, which
requires setting the gallery from the folder first and then carrying it to the
copy kept.

Measured on the real collection, 2026-11-14: 5,036 exact groups and 5,400
redundant files (29 GB). Plan 076 removes the groups whose copies would be
organized onto the same name, about 3,868 files. This plan handles the
remaining ~1,500 locally — a file with a `_1` copy beside it (1,070) and
copies under different names (462) — plus whatever a second copy of the collection adds.

## Decisions

### 1. Exact only

A group is a set of rows with the same SHA-256. Near-duplicates differ in
bytes and often in metadata, so choosing between them needs a person; that
belongs to the review screen, not to rules.

### 2. Plan, override, apply

The same shape as organize (R66), with an engine that follows `core/CLAUDE.md`:

- **Options** by value: the ids selecting which groups to act on, the ordered
  keep rules, and the log settings. As in plan 002, the ids select which groups
  are *shown*. Group membership always comes from the whole collection, so a
  copy outside the selection is still a member.
- **Plan** writes nothing. It returns a caller-owned `DuplicatesPlan`. Each
  group in it lists its members, the proposed keeper, the rule that chose it,
  and whether the group can be resolved, with a reason when it cannot.
- **Overrides.** `set_keeper(group, member)` and `set_skip(group)`. The CLI
  uses neither; a review screen uses both.
- **Apply** acts group by group, re-verifies everything it depends on, and
  polls the cancel token between groups, never inside one.

### 3. Keep rules

An ordered list. The first rule that picks one member decides; the rules after
it break ties.

| Rule | Picks |
|---|---|
| `under:<dir>` | members whose path is under `<dir>` (repeatable; earlier wins) |
| `unsuffixed` | a member whose name lacks a `_N` copy suffix, when the unsuffixed name is also in the group |
| `oldest` | the member with the oldest plausible mtime (076's limits) |
| `shortest` | the shortest path |

The default order is `unsuffixed, oldest, shortest`. The lowest row id is the
final tie-breaker, so a plan is deterministic. The CLI takes the list as
`--keep <rule>`, repeated.

### 4. A group is resolved only when nothing would be lost

The plan marks a group unresolvable, with a reason, and apply leaves it alone,
when:

- two members have different non-empty galleries — an image has only one, and
  R37 would silently drop one;
- a member to be removed has named faces and the keeper already has faces;
- a member to be removed has a sidecar, RAW JPEG or Live Photo video whose
  counterpart beside the keeper has different bytes;
- fewer than two members exist on disk. Missing files are orphans, which
  `rm --orphans` handles.

A removed member's attachment with no counterpart at the keeper is carried to
the keeper's stem, as organize carries attachments (R45).

### 5. Apply, per group

1. Hash every member from disk. A member whose bytes no longer match the
   group's SHA-256 leaves the group; if fewer than two remain, the group is
   skipped.
2. Apply 076's mtime rule to the keeper, taking the oldest plausible mtime in
   the group.
3. Trash each removed member and its sidecars through the existing trash
   action (R23, R46), carrying any attachment that has no counterpart.
4. In one transaction: transfer each removed member's metadata to the keeper
   (R37), delete the removed rows with restore snapshots (R04), and write the
   log entries (R18).
5. If the transaction fails, bring the trashed files back and restore the
   keeper's mtime.

One activity-log operation per run, `duplicates-resolve`, with `trash`,
`relink` and `set_mtime` entries.

### 6. The CLI

```
phig duplicates resolve [--keep <rule>]... [--match <glob>] [--exclude <glob>]
                        [--dry-run] [--format text|json] [--yes]
```

`--dry-run` prints the plan: each group's keeper and its rule, the files to be
trashed, and the unresolvable groups with their reasons. `--format json` gives
the same plan to a script, using 078's escaping. Without `--dry-run`, the
command prints the summary and asks for confirmation unless `--yes` is given,
as `rm` does. It takes the catalog's writer lock.

`--keep` and `--yes` are classified as mutating flags in
`rules/operations.toml`.

### 7. Records

- A new operation, `duplicates.resolve`, in `rules/operations.toml`, with
  `surfaces_cli = ["duplicates"]` and an empty `surfaces_abi`. The generated
  coverage table will show the ABI gap, which 062 and 064 close.
- New rules:
  - Resolving duplicates trashes a file only after its bytes are compared on
    disk at apply time.
  - A group is resolved only when every removed member's user metadata can be
    carried to the keeper; otherwise it is left as it is.
  - The removed files go to the trash, never to a hard delete.

## Testing

Real-behavior tests against the engine and the built binary:

- Each keep rule picks the expected member, and the default order and the id
  tie-break are deterministic.
- A resolved group leaves one file on disk. The rest are in the trash, and the
  keeper's row has the union of tags, the favorite, and the gallery.
- Each condition in decision 4 leaves its group untouched and reports it.
- A member edited between plan and apply is not trashed.
- `set_keeper` and `set_skip` change what apply does.
- A failed transaction brings the trashed files back.
- Cancelling between groups leaves every finished group complete and every
  other group untouched, and the operation is recorded as interrupted (R27).
- `--dry-run` writes nothing; without `--yes`, declining the prompt writes
  nothing.

## Risks

- **The trash lives in the catalog.** With the catalog on the NVMe and the
  photos on the HDD, every trashed file is copied across devices. On the
  current collection that is up to 29 GB into `~/.local/share/phig/trash`,
  which has room, but it is slow and stays on disk until `trash empty`.
  Changing where the trash lives is out of scope; the runbook accounts for it.
- **Hashing cost.** Apply reads every member of every group it resolves.
  Measure it on the real collection before calling it acceptable.

## Not in scope

- **The GTK review screen.** It comes later, as its own plan, on 047's Screen 3
  design. It needs the duplicate groups (062) and this operation through
  libphig.
- **Near-duplicates**, for the reason in decision 1.
- **Remembering "these are not duplicates"**, which 062 already excludes.

## Relationship to other plans

- **076** provides the mtime rule and the verify-then-trash sequence.
- **062** exposes duplicate groups through libphig. It says acting on a group
  belongs to 064; this plan provides the `core/` operation that the ABI will
  expose.
- **064** gives remove a plan/apply ABI; exposing this operation follows the
  same shape.
- **078** provides the JSON escaping for `--format json`.

## Outcome

Built 2026-11-14 on branch `076-organize-mtimes-identical`, as decided above,
with these details settled in the code:

- `core/duplicates_resolve.{h,cpp}`: `DuplicatesResolver::plan` and `apply`, a
  caller-owned `DuplicatesPlan` with `set_keeper` and `set_skip`, and keep rules
  parsed by `parse_keep_rule`. Reasons and blockers are enums; the CLI words
  them, per `core/CLAUDE.md`.
- A keep rule that matches no candidate is passed over rather than emptying the
  set. `under:` compares path components, so `under:/a/lib` does not match
  `/a/library`. `oldest` reads mtimes from disk and ignores implausible ones.
- `set_keeper` re-checks the group against the new keeper, which is why the plan
  keeps a pointer to the database.
- Named faces block a group when a removed member has named faces and any other
  member, the keeper included, has faces: R37 moves faces only onto a target
  with none, so only the first face-bearing member's faces would land.
- `organize_exec`'s `settle_attachments` and `plausible_mtime` are shared with
  076's identical-move path; `settle_attachments` now checks the bytes of an
  attachment already present before trashing the one being removed.
- `--format json` needs `--dry-run`: it prints the plan. The first `--keep`
  replaces the default order.
- `duplicates` left `read_only_cli`; `duplicates.resolve` is a new operation with
  no ABI surface yet.

Rules R95–R99 record it, and R93's older-mtime rule now binds
`duplicates.resolve` too. 24 tests, each confirmed to fail with its piece of the
resolver disabled.

### Dry run on the real catalog

2026-11-14, read-only, 53 s: 5,036 groups, all resolvable, 5,400 files to
trash. With the default rules, `oldest` chose 4,157 keepers and `unsuffixed`
879. In 3,081 groups the two copies' mtimes were both copy times, five days
apart, so `oldest` kept whichever copy was written first rather than the one
in the tree meant to be kept. A collection like this one should name that tree
with `--keep under:<dir>` rather than rely on `oldest`.

Apply reads every member of every group it resolves, the same cost 076
measured: about 58 GB here, on the order of ten minutes on this HDD. Not
measured; closed on 076's measurement of the same reads, 2026-11-14.
