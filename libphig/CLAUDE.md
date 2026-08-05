# libphig/

The versioned C ABI (`libphig.so`) — the boundary for foreign callers
(`gtk/`, and later Elixir). ABI 1.2, soname 1, 42 exported symbols. Hidden
visibility by default; the exported surface is declared explicitly by a
linker version script (`libphig/phig.version` on Linux,
`libphig/phig.exported_symbols` on macOS), not inferred from headers.

Public header: `libphig/include/phig/phig.h`. This is the only header a
client may include — see `gtk/CLAUDE.md`.

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
- New symbols go in the version script alongside the header declaration —
  a symbol missing from `phig.version`/`phig.exported_symbols` is hidden
  and unlinkable even if it's declared in `phig.h`.

## Not covered by CI lint

`libphig/include/phig/phig.h` is nested below the `-maxdepth 1` clang-format
glob (see root `CLAUDE.md`). Format it manually before committing changes
to it.
