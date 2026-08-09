# rules/

Machine-checked records of phig's data-integrity rules. Each `[[rule]]` table
is one rule. `taxonomy.toml` defines the concern classes and the assets a
rule can protect; `operations.toml` defines the operations a rule can bind
to and reconciles them against the CLI and ABI surfaces. `rules/by-*.md`
files are generated from these records, not hand-maintained.

## Fields

- `id` (required) — short identifier, e.g. `R01`. Unique across all rule
  files.
- `tier` (required) — `engine` or `boundary`. `engine` binds operations via
  `binds`, naming entries from `operations.toml`. `boundary` binds surfaces
  via `binds_surface` instead, naming a CLI command or ABI symbol directly —
  for a rule that lives at an interface rather than inside a `core/`
  primitive.
- `concern` (required) — one of the nine values in `taxonomy.toml`'s
  `concerns` list.
- `statement` (required) — the rule itself, stated as an invariant that
  holds (or is claimed to hold) at all times, not as a description of what
  the code currently does.
- `rationale` (required) — why the rule exists: what it protects against
  and why that outranks the alternative.
- `enforced_at` (required) — list of `path:symbol` anchors naming the code
  that enforces the rule. Verified to exist; a rule whose anchor cannot be
  grepped is a rule that has drifted from the code.
- `binds` (required for `tier = "engine"`) — list of operation `id`s from
  `operations.toml` this rule applies to.
- `binds_surface` (required for `tier = "boundary"`) — list of CLI commands
  or ABI symbols this rule applies to.
- `protects` (required) — list of asset `id`s from `taxonomy.toml`'s
  `[[asset]]` entries.
- `tests` (required) — list of `Suite.TestName` entries from `tests/`.
  Verified to exist.
- `test_kind` (required) — one of `witness` | `conformance` | `subprocess`
  | `sanitizer`, naming what kind of evidence the tests in `tests` give for
  the statement:
  - `witness` — a test demonstrates the rule holds in one concrete case.
  - `conformance` — a test checks the rule's effect against a spec (e.g. a
    snapshot's shape), not just one scenario.
  - `subprocess` — the rule is exercised by running the built `phig`
    binary rather than calling `core/` directly.
  - `sanitizer` — the rule's evidence depends on a sanitizer run (ASan,
    UBSan, TSan) catching a violation that a normal run would not surface.
- `status` (required) — one of `holds` | `partial` | `violated` |
  `unenforced`.
- `owner` — a plan number (from `plans/`) responsible for closing the gap.
  Required whenever `status` is `partial` or `violated`, and required
  whenever `test_gap` is present, even if `status` is `holds`.
- `test_gap` — free text describing what the listed tests do not cover.
  Its presence requires `owner`, per the rule above.

## Required-owner rules

- `status` of `partial` or `violated` requires `owner`.
- Any `test_gap` requires `owner`, regardless of `status`.
