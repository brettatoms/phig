---
name: libphig Feature Parity
status: designing
created: 2026-08-08
closes_rules: [R61, R62, R66, R68, R77, R78]
description: >
  The gap between what phig can do and what a client can reach. `libphig`
  covers paged queries, per-image metadata, thumbnails, settings,
  tag/gallery/favorite writes and the three long-running operations; phig also
  has duplicates, people and faces, remove, four search modes, glob selection,
  tag/gallery listings, model download and an activity log, none of which a
  client can touch. Organize and reconcile are already owned by 042 and 041;
  this plan is the remainder, ordered by what plan 047's nine screens need.
  Three of those screens have no data source today and two have no action.
  Held as one plan because the inventory and its cross-cutting decisions —
  how a caller names its subject, whether a read that scans the collection is
  a request or an operation, how confirmation crosses a boundary that forbids
  callbacks — are the design work; individual items get their own number when
  one turns out to be large.
---

# 053 — libphig Feature Parity

## Goal

Make `libphig` reach all of phig's features. Root `CLAUDE.md`'s Interface
discipline states the rule: a capability that exists only in `core/` and `cli/`
is a gap with a schedule, not a boundary on what a client may do. This plan is
the inventory behind that sentence and the order the gaps get closed in.

It is not a demand that every `core/` function become an entry point. The test
is whether a client can accomplish what phig can accomplish, through a surface
shaped for a client rather than for argv.

## Why one plan

The individual items are mostly small — a people list is a query and a struct.
What is not small is the set of decisions they share, and answering those once
is the point of holding them together:

- **How a caller names its subject.** The metadata writes take image ids
  because a GUI holds a selection; the operations take `match`/`exclude` globs
  because a bulk job describes a set. `phig_query` takes neither, which is why
  there is no filename search. Every item below has to pick one, and picking
  per-item is how an ABI ends up with three conventions.
- **Request or operation.** `phig duplicates` compares the whole collection.
  Anything that scans rather than pages has to be either a call that blocks for
  seconds or a `phig_op*` with progress. Plan 052 has the identical question
  for `GET /duplicates` and both interfaces must answer it the same way, or S2
  learns two models.
- **Confirmation across a boundary that forbids callbacks.** `cmd_rm` takes a
  `ConfirmFn`. Plan 031 ruled out running client code on an engine thread, so
  remove needs either a decide/apply split like 041's or a pre-authorized
  request. This is the same shape as 042's per-file conflict resolution.
- **Not repeating 044's mistake.** 044 measured `get_all_images` loading
  111.6 MB of EXIF that no caller reads. A new read surface that returns rows
  should carry what its screen draws and nothing else.

## Inventory

Ordered by what plan 047 needs. "Own plan" marks items likely to outgrow this
one.

### 1. Duplicates — Screen 3 has no data source

`core/duplicates.h` (`find_near_duplicates`, `hamming_distance`) and
`Database::get_exact_duplicates` exist and the CLI renders them. A client gets
nothing. Needs a group-shaped result rather than a flat page, exact and near
in one surface, and the request-or-operation decision above. Measure with
`perf_duplicates` against the real collection before choosing. **Own plan.**

### 2. People and faces — Screen 4 has no data source

Read: `list_people`, `get_images_for_person`, `search_faces`, and the face rows
for one image. Write: `set_face_person`, `identify_person`, `auto_label_face`.
Plan 045 made `faces.id` stable specifically so a GUI could hold face ids
across human-scale time, so the prerequisite is done. Open: whether the ABI
returns bounding boxes and lets the client crop, or generates face thumbnails
itself — the latter reopens the pixel-decode question this directory
deliberately leaves unresolved. **Own plan.**

### 3. An image's tags, and the tag and gallery lists

`get_tags_for_image`, `get_tag_counts`, `get_gallery_counts`. The sharpest of
the small gaps: a client can add and remove tags through
`phig_tag_add`/`phig_tag_remove` but cannot read back what an image has, and
`phig_image` carries `favorite` and `gallery` and no tag list. 047's sidebar
sections and the viewer's tag chips both need this. Small, and everything above
it in 047 depends on it.

### 4. Query selection and the other three search modes

`phig_query` covers `Database::SearchCriteria` completely and stops there.
Missing: `SearchQuery::matches`/`excludes` and the path prefix — so no filename
search — plus `SearchMode::Similar` (phash), `::Face` and `::Person`.
`SearchEngine` holds no OpenCV dependency and takes embeddings and phashes the
caller computed, which was a deliberate choice for exactly this boundary: the
engine cannot be crashed by a malformed query file. It also means the ABI has
to decide whether a client computes a phash itself or phig grows an entry point
that does.

### 5. Remove — the Remove dialog has no action

`remove_exec.h`'s `cmd_rm` already takes a `RemoveOptions` and a `ConfirmFn`,
so the engine work is done; what is missing is a boundary shape for
confirmation and for the four modes (trash, `--hard`, `--db-only`,
`--orphans`), plus `trash empty`. Destructive, so it ships with real-behavior
tests per the root `CLAUDE.md` convention. Pairs naturally with 042 — a GUI
that can organize but not remove is half a library manager. **Own plan.**

### 6. Face model download

`models.h`'s `download_face_models`. Today the ABI will happily start a faces
operation that fails because ~150 MB of models are absent, and offers no way to
fetch them; 047's Settings→Faces section is the screen that needs it. Small,
but it is what makes the faces operation usable on a fresh machine rather than
merely present.

### 7. `thumbs clean`

Orphaned thumbnail cleanup. `is_thumbnail_filename` is in `core/`; the loop is
in `cli/main.cpp` and moves to `core/` first. Smallest item here, and nothing
depends on it.

R77 records what has to survive that move, and is claimed by this plan's
`closes_rules`: only files matching the thumbnail naming contract are removed,
only when no row claims their hash, and `--dry-run` removes nothing including
the empty-directory sweep. All three live in the `cli/` loop today, so a client
that grew its own cache cleaning would inherit none of them. Plan 054's flag
pass also added a `thumbs.clean` operation to `rules/operations.toml` — `thumbs`
was claimed by `thumbs.rebuild` alone, so the clean subcommand's deletions had
been accounted for by nothing.

### 7a. Glob selection on a metadata write — R78

`tag add|remove`, `gallery set|clear` and `fav add|remove` each refuse to run
without a `--match` or a `--path`, because an empty match list selects every row
(R74) and the write would then strip a tag, or empty a gallery, collection-wide
with no prompt and no log entry. The check is three copies in `cli/main.cpp`;
`Database::set_tag`, `set_gallery` and `clear_gallery` act on whatever list they
are handed.

Nothing is exposed today — `phig_tag_add` and its five neighbours take an
explicit array of image ids, so a client cannot ask for everything by omission.
It becomes reachable exactly when item 4 brings glob selection to the ABI, which
is why R78 is claimed here rather than left to whoever writes that. The guard
belongs beside the write.

### 8. Activity log — Screen 7 has no data source, in `core/` first

`Database` has `log_begin_operation`, `log_add_entry`, `log_finish_operation`
and `log_prune` — and no reader at all. Plan 020 deferred both the `phig log`
viewer and undo, so this is core work before it is ABI work, and the CLI has
the same gap. Sequencing it last is deliberate: a reader designed alongside
undo is better than one designed twice. **Own plan, and it is 020's successor
rather than only an ABI item.**

## Not in scope

- **Organize (cp/mv)** — plan 042, including its conflict-resolution question.
- **Scan's reconcile phase** — plan 041.
- **`ScanOptions::thumbs`/`faces`** — the standing exclusion. The ABI models
  index, thumbs and faces as independent operations and those flags refuse
  that.
- **Pixel decoding.** Unresolved for its own reasons (see `libphig/CLAUDE.md`
  and `gtk/CLAUDE.md`); item 2 must not be the thing that decides it by
  accident.
- **Features phig does not have yet.** 009 (watch), 013 (geo search), 021
  (import) and 028 (`.phigignore`) each need ABI exposure when built, and each
  owns that as part of its own plan rather than accruing here.

## Open questions

1. **Request or operation, for duplicates.** Same question as 052's. Decide
   from measurement, and decide once for both interfaces.
2. **One selection model or two.** Ids, globs, or both on every surface. The
   present split is defensible per surface and indefensible as a whole.
3. **Confirmation shape for remove.** Decide/apply like 041, or a request that
   carries its own authorization. A callback is excluded by 031.
4. **Face representation.** Bounding boxes to the client, or face thumbnails
   from phig. Coupled to the pixel-decode question.
5. **Where the phash for a similarity search comes from.** The client, or a new
   entry point that hashes a file.

## Relationship to other plans

- **047** — the demand side. Three of its nine screens (Duplicates, People,
  Activity Log) have no data source through the ABI and two (Organize, Remove)
  have no action. 052 counts two, reading only the screens whose data does not
  exist at the ABI; Activity Log is blocked in `core/` first, which is the
  difference.
- **052** — states the same rule from the other side ("`libphig` should
  eventually expose every phig feature, so today's gaps are a schedule fact")
  and needs the same capabilities over the wire. Every item here is also a
  `phigd` endpoint; deciding them once is what keeps the two interfaces one
  vocabulary.
- **041, 042** — the two ABI-surface gaps already owned.
- **044** — what a new read surface must not do, and it owns pagination at
  scale.
- **045** — stable `faces.id`, the prerequisite item 2 was waiting on.
- **046** — the `vec_faces` delete failure sits under the faces operation on
  the affected machine; item 2's write paths touch the same code.
- **031, 034** — the operation lifecycle, progress convention, and the
  ctx/error/threading rules everything here has to follow.
- **020** — deferred the activity log's reader and undo; item 8 picks that up.
