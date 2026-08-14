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
  via `binds_surface` instead, naming a surface category (`abi`, `cli`, or
  `http`) — for a rule that lives at an interface rather than inside a
  `core/` primitive.
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
- `binds` (required for `tier = "engine"`; must not appear on a
  `tier = "boundary"` rule) — list of operation `id`s from `operations.toml`
  this rule applies to.
- `binds_surface` (required for `tier = "boundary"`; must not appear on a
  `tier = "engine"` rule) — list of surface categories this rule applies to.
  Exactly three legal values: `abi`, `cli`, `http`. Not a CLI command name or
  an ABI symbol — those live one level down, in `operations.toml`'s
  `surfaces_*` lists.
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
  `unenforced`. `holds` with a `test_gap` is the standard shape for a rule the
  code satisfies but no test fully pins — R26, R49, R53, R54 and R64 all take
  this shape, and it is preferred over `unenforced` whenever the code is right
  and only the test is weak. `unenforced` narrows to mean only "nothing can pin
  this today" — true of R34 and R65, where no test exists and none could be
  written short of new machinery.
- `test_gap` — free text describing what the listed tests do not cover. It
  says the evidence is thinner than the claim, so that a `holds` is never read
  as a pin it does not have.

## A rule names no plan

Which plan will close a gap is a scheduling question, and a plan is an
implementation document that gets completed and stops being touched. A rule is
a permanent statement about code safety, so the reference runs the other way: a
plan's frontmatter carries `closes_rules: [R04, R25]`, and a rule says nothing
about plans at all.

Two checks read it, both in `scripts/rules.py`'s `check_closures`:

- Every `partial` or `violated` rule is claimed by some open plan's
  `closes_rules`. Recorded debt with nobody working on it fails the build.
- No plan that is `completed`, `superseded` or `deferred` still claims a rule
  that is `partial` or `violated`. Closing a plan while the fault it promised
  to fix is still recorded fails the build.

A `test_gap` needs no claim. A weak test is not unsafe code, and the gap text
is the coverage information; the work of closing one finds it by reading the
records.

## Anchors outside `core/` and `libphig/`

`enforced_at` normally resolves under `core/` or `libphig/`, per the
enforcement-belongs-in-core rule in the root `CLAUDE.md`. An anchor outside
both is legal on exactly one shape of rule: `status = "violated"`, where the
anchor's location is itself the defect and an open plan's `closes_rules` claims
the rule. R68 is the example — its guard lives at `cli/main.cpp:cmd_trash_empty`
because no `core/` primitive for `trash.empty` exists yet, and plan 053 owns
creating one. When the code moves, the anchor updates to name the new location
and `status` becomes `holds`.

**A `cli/`-anchored rule must name a user asset that a command can actually
destroy.** A `cli/` anchor is a record that enforcement sits in the wrong
layer, so the rule's entire value is that some plan moves it into `core/` and
every surface inherits it. A finding with no asset behind it has nothing to
move and nothing to protect: it is an ordinary bug, and it belongs in a plan's
body rather than here. Without this bar the record drifts into a bug tracker
for `cli/`, which is the opposite of what it is for.

R82 is the one that failed it, written and then removed. Nine numeric flags let
`std::invalid_argument` escape `std::stoi` during argv parsing and abort the
process at exit 134. Measured: every one of them converts before any `Database`
is constructed, so `scan --parallel abc` creates no database file and corrupts
nothing — no asset, and no `core/` home for a missing `try`/`catch`, because
`core/` never sees argv. It lives in plan 043's body instead, with the measured
list of flags. The `cli/` anchors that pass each name a capability or a guard
that belongs in `core/` and is not there yet: R68 and R77 (no `core/` primitive
for emptying the trash or cleaning the thumbnail cache), R78 (the selection
guard sits beside one caller instead of beside the write), R69 and R81 (two
truncating writes in `cmd_search` with no `core/` renderer behind them), R73
(a parser that lets a destructive command run on a typo), R70 and R80.

## core_headers classification

`operations.toml`'s `core_headers` lists the `core/` headers whose declared
functions and methods must each be accounted for. Every name found there
goes into exactly one of three buckets:

- **An operation's `core_entry`** — the name is a thing phig can do,
  something a person invokes through a command, an ABI call, or (later) a
  URL.
- **`core_internal`** — the name mutates real data but is the *inside* of
  an operation already declared, with no caller outside `core/`. A rule
  that lives in such a function anchors its `enforced_at` there and binds
  the operation that covers it, rather than the internal function itself.
  `rules/conflict.toml`'s R01 is the worked example: it anchors at
  `core/paths.cpp:resolve_conflict` and binds `organize.copy` and
  `organize.move`, and `resolve_conflict` is not an operation. Do not
  promote an internal function to an operation to get it classified — an
  operation carries `surfaces_abi`/`surfaces_cli`, and the generated
  missing-coverage table treats an empty `surfaces_abi` as a capability
  libphig lacks, so an internal executor listed as an operation
  manufactures a false gap, and a rule bound to it drains the cell of the
  user-facing operation a reader would actually consult.
- **`core_read_only`** — the name changes nothing tracked, or is an
  artifact of the scan's regex (comment prose, a control keyword, a type
  name).
