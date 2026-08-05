# cli/

The `phig` command-line tool. `main.cpp` is ~2,363 lines and holds only:
argv parsing, usage text, output rendering (text/csv/json and the other
formats each command supports), the SIGINT handler, and the progress
printer. Everything else — scanning, hashing, duplicate detection, search —
is `core/` engines called from here; this directory has no business logic
of its own.

## Rules

- Flag order convention: `phig <subcommand> <args> --db <path>`.
- New commands call into a `core/` engine rather than reimplementing logic
  here. If a command needs new engine behavior, that goes in `core/` (and,
  if a non-CLI client will eventually need it too, in `libphig/`) — see the
  root `CLAUDE.md`'s interface-discipline rule.
- Output formatting (text/csv/json rendering) belongs here, not in `core/`
  — engines return structured results with no user-facing prose, and this
  is where prose gets attached.
- The progress printer renders `ProgressQueue<T>` updates from a `core/`
  engine; it does not decide wording for engine-side stages, only how to
  print what the engine already labeled.
