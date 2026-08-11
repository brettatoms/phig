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

## Rules of record

`cli/` is not an enforcement site for `rules/`. It appears there as an
operation's `surfaces_cli` entry in `rules/operations.toml` — the command a
`core/` primitive is reached through — and nothing more.

A rule whose `enforced_at` names `cli/main.cpp` is therefore a guard in the
wrong place, not a record of how `cli/` is meant to work: it is recorded with
`status = "violated"` and the plan that will move the guard into `core/` as its
`owner`. CI accepts a `cli/` anchor on that shape of rule alone. R68 is the
worked example — its `trash empty` guard has no `core/` primitive to live in
yet, and 053 owns creating one. For which rules are in that position now, grep
`rules/*.toml` for `cli/`; the answer changes as guards move, and nothing here
would notice.
