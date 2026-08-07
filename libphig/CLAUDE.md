# libphig/

The versioned C ABI (`libphig.so`) — the boundary for foreign callers
(`gtk/`, and later Elixir). ABI 1.2, soname 1, 56 exported symbols. Hidden
visibility by default; the exported surface is declared explicitly by a
linker version script (`libphig/phig.version` on Linux,
`libphig/phig.exported_symbols` on macOS), not inferred from headers.

Public header: `libphig/include/phig/phig.h`. This is the only header a
client may include — see `gtk/CLAUDE.md`.

## Coverage — and what it lags

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

**Still absent:** organize (cp/mv) and remove, plus that reconcile phase —
plans 041, 042. `ScanOptions::thumbs`/`faces` must never be exposed: the ABI
models the three as independent operations, and those flags refuse that.

Consequently `cli/` links `phig-core` and includes `core/` headers directly
rather than going through libphig. **The CLI is a peer of libphig over
`core/`, not a wrapper around it.** Anything written as though the CLI sits on
libphig is wrong.

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

## Not covered by CI lint

`libphig/include/phig/phig.h` is nested below the `-maxdepth 1` clang-format
glob (see root `CLAUDE.md`). Format it manually before committing changes
to it.
