# libphig/

The versioned C ABI (`libphig.so`) — the boundary for foreign callers
(`gtk/`, and later Elixir). ABI 1.2, soname 1, 56 exported symbols. Hidden
visibility by default; the exported surface is declared explicitly by a
linker version script (`libphig/phig.version` on Linux,
`libphig/phig.exported_symbols` on macOS), not inferred from headers.

Public header: `libphig/include/phig/phig.h`. This is the only header a
client may include — see `gtk/CLAUDE.md`.

## Coverage — and what it lags

**The target is every phig feature, not the subset below.** The ABI is what a
client is allowed to use, so a capability reachable only from `core/` and
`cli/` is a gap with a schedule, not a statement about what clients may do.
Root `CLAUDE.md`'s Interface discipline carries the rule; plan 053 is the
inventory and the order of work.

The ABI covers paginated queries, per-image metadata, thumbnail lookup and
generation, settings, tag/gallery/favorite writes, and three long-running
operations — index, thumbs, faces (plan 031).

**Operations are handle-plus-poll.** `phig_op_*_start` spawns a worker and
returns a `phig_op*`; the caller polls progress and status from whatever thread
suits it, and must call `phig_op_free`, which joins. No callback ever runs on
an engine's thread. Each operation opens its own `Database` rather than sharing
`phig_ctx::db`, so the client's queries keep working while one runs.

**Operations take no writer lock.** Index's write-set is unknowable before the
walk, thumbs writes only the filesystem cache, and faces commits per image.
SQLite serializes writers itself, so metadata writes stay available for an
operation's whole duration. `PHIG_ERR_BUSY` comes from those small mutations
contending with a CLI command that does hold the lock.

**A lock-free operation is not excluded by the CLI's lock**, which is the
consequence that bites: `phig face rebuild` and the faces operation do the same
delete-and-reinsert of face rows, and `faces.id` is reused (plan 045), so a
read-then-write elsewhere can land on a face it never read. `identify_person`
handles this itself — its write matches on the embedding it compared, not the
row id — but `phig face name` still relies on the lock and is therefore only
safe against other lock-takers. Any new read-then-write on `faces.id` must
assume an ABI operation is running beside it.

**Index is the additive half of scan only.** It runs
`ScanEngine::run_workers`, never `run_reconcile`, so rows for deleted or moved
files survive a run. A client must not present it as "sync with disk". Plan 041
splits reconcile into a decision and an application the caller authorizes.

### What is missing

| Missing | Exists in `core/` as | Tracked by |
|---|---|---|
| Organize — cp/mv | `organize_plan.h`, `organize_exec.h` | 042 |
| Scan's reconcile phase | `ScanEngine::run_reconcile` | 041 |
| Remove — `rm`'s modes, `trash empty` | `remove_exec.h`'s `cmd_rm` | 053 |
| Duplicates — exact and near groups | `duplicates.h`, `Database::get_exact_duplicates` | 053 |
| People and faces, read | `list_people`, `get_images_for_person`, `search_faces` | 053 |
| Face labelling | `set_face_person`, `identify_person`, `auto_label_face` | 053 |
| Person / face / similar search | `SearchEngine`'s other three `SearchMode`s | 053 |
| Glob and path selection on queries | `SearchQuery::matches`/`excludes` | 053 |
| One image's tags, read | `get_tags_for_image` | 053 |
| Tag and gallery lists with counts | `get_tag_counts`, `get_gallery_counts` | 053 |
| Face model download | `models.h`'s `download_face_models` | 053 |
| `thumbs clean` | `is_thumbnail_filename`; the loop itself is in `cli/` | 053 |
| Activity log, read | nothing — `Database` writes the log and never reads it | 053 |

Two of those are more than a missing entry point. The activity log has no
reader anywhere in `core/`, so it is core work before it is ABI work. And
query selection is an inconsistency inside the ABI rather than only a
shortfall: `phig_thumbs_options` and `phig_faces_options` take
`match`/`exclude` while `phig_query` does not, so a client can restrict a
thumbnail run by glob but cannot search for a filename.

Writes and operations also disagree on how a caller names its subject — the
metadata writes take image ids because a GUI holds a selection, the operations
take globs. Anything added above has to pick one deliberately.

`ScanOptions::thumbs`/`faces` is the deliberate exclusion and must never be
exposed: the ABI models the three as independent operations, and those flags
refuse that.

`cli/` links `phig-core` and includes `core/` headers directly rather than
going through libphig. **The CLI is a peer of libphig over `core/`, not a
wrapper around it** — it is phig running in a terminal, entitled to whatever
`core/` has when core has it, which is the same standing plan 052 claims for
`phigd`. Anything written as though the CLI sits on libphig is wrong.

Pixel decoding is also absent. `gtk/CLAUDE.md` anticipates decode moving
out-of-process; measurement (0 crashes in 12,684 probes) argues in-process is
defensible. Unresolved, and out of scope for plan 031.

## Rules

- **No exception may cross the ABI boundary.** Where a `phig_ctx*` is in
  scope, wrap the body in `PHIG_TRY`/`PHIG_CATCH` — it converts a C++
  exception into an error code plus a message. `PHIG_TRY` expands to a null
  check on `ctx`, so it needs a context it can check; the ctx-less entry
  points (`phig_init`, the `records.cpp` free functions, and all of
  `settings.cpp`) can't use it and instead wrap their body in a plain
  `try`/`catch (...)`.
- **`phig_last_error` is thread-local.** Read it on the thread whose call
  just failed — reading it from another thread, or after another call on
  the same thread has run, gets you the wrong (or no) message.
- **Adding a symbol bumps `PHIG_ABI_VERSION_MINOR`** (in `phig.h`) **and
  the CMake `VERSION`** (in `libphig/CMakeLists.txt`) — **never
  `SOVERSION`.** `SOVERSION` is the major alone; an additive, backward
  -compatible change keeps the soname so existing callers don't need a
  rebuild. Only an incompatible change bumps `SOVERSION`/`MAJOR`.
- **The export lists match on the `phig_` prefix, not on a list of names.**
  `phig.version` is `global: phig_*` and `phig.exported_symbols` is `_phig_*`,
  so a new entry point is exported by being named `phig_something` and needs no
  edit to either file. A public symbol that does not start with `phig_` is
  hidden and unlinkable however it is declared in `phig.h`.
- **A destructive operation gets plan/apply, never a confirmation callback.**
  Anything that deletes, moves or overwrites publishes a plan the caller
  inspects, then authorizes with a second call. It does not ask mid-run: no
  callback may run on an engine thread (see handle-plus-poll above), and a
  callback has no meaning over the wire protocol, where nothing can call back
  into a browser mid-request. `cmd_rm`'s `ConfirmFn` is a `cli/` affordance for
  a command with no plan phase, not a model to copy here. Three properties come
  with the shape: the plan is a caller-owned value the caller frees, so it
  outlives the operation that computed it and can be edited without touching
  engine-owned memory; apply re-verifies every decision at execution time, so a
  stale plan can never cause an action the re-check would not independently
  justify; and whether an item's decision is overridable is a property of the
  decision type, not of the mechanism — organize's conflict resolutions are,
  reconcile's deletions are not. Plans 041 and 042.

## Not covered by CI lint

`libphig/include/phig/phig.h` is nested below the `-maxdepth 1` clang-format
glob (see root `CLAUDE.md`). Format it manually before committing changes
to it.
