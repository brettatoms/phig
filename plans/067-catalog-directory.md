---
name: Catalog Directory and Test Isolation
status: planned
created: 2026-11-08
closes_rules: [R50, R70]
description: >
  Make a catalog one directory holding a collection's database, lock, trash,
  thumbnails and log, selected by a global `--catalog <dir>` that replaces
  `--db`. The default catalog becomes `~/.local/share/phig/` with the same
  layout. The writer lock follows its database (R50), a read never creates or
  migrates a catalog (R70), and the test suite stops writing to the developer's
  real files. `PHIG_CATALOG` selects a catalog for local development. Config
  and face models stay user-level.
---

# 067 — Catalog Directory and Test Isolation

## Problem

phig resolves each of a collection's files separately, and `--db` moves only
one of them. Everything else follows the XDG defaults or the config file,
whatever `--db` says. Measured 2026-11-08:

- **The lock goes to the default collection.** `lock_path()`
  (`core/paths.cpp:55`) takes no argument and returns
  `default_db_path() + ".lock"`. `--db` never reaches `default_db_path()`; only
  the config file's `db` key does, through `set_path_overrides`. So
  `phig tag add --db /tmp/x.db` locks `~/.local/share/phig/phig.db.lock`.
  - This is R50 (`rules/concurrency.toml`, `violated`).
  - The CLI therefore serializes commands that share no database.
  - It also fails to exclude a libphig caller from the database it is actually
    writing, because libphig locks `<db>.lock`.
- **The log goes to the default collection.** `~/.local/state/phig/phig.log`
  holds 7,241 lines naming `/tmp` test databases.
- **Trash goes to the default collection.** `rm` against `--db /tmp/x.db`
  moves files into `~/.local/share/phig/trash`. `trash empty`, which takes no
  `--db`, deletes everything there. Trash holds user files, and nothing restores
  what `trash empty` deletes (R68).
- **Thumbnails are shared, and `thumbs clean` assumes they are not.**
  `thumbs clean --db /tmp/x.db` deletes every file in `~/.cache/phig/thumbs`
  whose hash `/tmp/x.db` does not reference (`cli/main.cpp`, around line 1275).
  Run against a test database, that is the whole real cache: 25,212 files,
  986 MB here. Thumbnails can be regenerated, but only by decoding every image
  again.
- **A read creates and migrates.** This is R70 (`rules/boundary.toml`,
  `violated`).
  - All twelve `Database` constructions in `cli/main.cpp` take the default
    `read_only = false`.
  - `cmd_search` and `cmd_duplicates` call `init_schema`.
  - So `phig search --count` against a path with no database creates an empty
    one and reports it empty.
  - A read against an older catalog migrates it forward, and an older build
    can then no longer open it.

### How the tests reach real files

`tests/phig_binary.h`'s `run_phig` points `HOME` and all four XDG variables
into the test's scratch directory, and its comment explains why. Four call
sites run the binary with a bare `std::system` instead:

- `tests/test_database.cpp:2307`
  (`DatabaseTest.CliMetadataWritesRefuseAnUnnamedSubject`) rewrites
  `~/.local/share/phig/phig.db.lock` and the real log on every run.
- `tests/test_faces_engine.cpp:592` and `:640` write the real log.
- `tests/test_remove_exec.cpp:649` redirects only `XDG_DATA_HOME`, so its log
  lines land in the real log.

In-process tests that reach a default path rely on each fixture redirecting
the right variable. Nothing makes that the default.

## Decisions

### 1. A catalog is a directory

A catalog is phig's record of a collection. The images themselves stay
wherever they were scanned; the catalog holds what phig keeps about them.

```
<catalog>/
  phig.db
  phig.db.lock
  trash/
  thumbs/<size>/ab/<sha>.jpg   (thumbs/ carries a CACHEDIR.TAG)
  phig.log                     (rotates to phig.1.log)
```

- **Flag.** A global `--catalog <dir>` selects the catalog. Like `--config`
  and `--no-wait`, it is recognized anywhere in argv.
- **`--db` is removed** from every command. Backward compatibility is not a
  constraint.
- **The name.** "Catalog" is the word Lightroom uses for an index of photos
  stored elsewhere, with previews and a lock beside it. "Library" was
  rejected because it suggests the place the images themselves live.
- **Thumbnails are marked as a cache.** `thumbs/` carries a `CACHEDIR.TAG`, the
  standard cache marker, written when phig creates the directory. borg, restic
  and GNU tar can skip a directory that carries one, so a catalog backup need
  not include 986 MB of regenerable thumbnails. Not every backup tool honours
  it.

### 2. The default catalog is `~/.local/share/phig/`

Without `--catalog`, phig uses `$XDG_DATA_HOME/phig/` (`~/.local/share/phig/`).
It has the same layout as any other catalog, so there is one code path and
nothing special about the default.

### 2a. Which catalog a command uses

Highest precedence first:

1. `--catalog <dir>`
2. `PHIG_CATALOG`, so a project's `.envrc` can point every command in that
   directory at a development catalog
3. `catalog = <dir>` in the config file
4. `~/.local/share/phig/`

`core/` resolves this once, in
`std::string resolve_catalog_dir(const std::optional<std::string>& flag, const Config&)`,
and the CLI and gtk both call it. libphig does not: `phig_config.catalog_dir` is
required, because a library that reads its host's environment would pick a
catalog the host never named.

This departs from XDG convention in two places:
- **Thumbnails** normally belong in `~/.cache`. `CACHEDIR.TAG` recovers part of
  what that location buys.
- **The log** normally belongs in `~/.local/state`. It is under 1 MB, and
  keeping it beside the catalog it describes is more useful than following the
  convention.

### 3. Existing files move once

On first use of the default catalog, phig moves `~/.cache/phig/thumbs/` to
`<catalog>/thumbs/`, and `~/.local/state/phig/phig.log` and `phig.1.log` to
`<catalog>/`.

- **How it moves.** A rename when source and destination share a filesystem,
  as they do here (`/home`). Otherwise phig copies to a temporary name, renames
  into place, then deletes the source, so a concurrent run sees the old
  location or the new one but never a partial copy.
- **When it is skipped.** If the destination already exists, phig leaves both
  in place and logs a warning rather than merging.
- **Where it runs.** It lives in `core/` beside `migrate_flat_thumb_cache`
  (`core/thumbs.h:35`), which is the precedent. The CLI, `phig_init` and gtk's
  `phig_collection_open` all call it.
- **It is not a schema migration**, so R70 does not forbid it on a read.
  Thumbnails and the log are not user assets.

`~/.local/share/phig/models/` (157 MB, dated 2026-03-29) is a leftover from an
earlier models location, including the dlib files phig no longer uses. It ends
up inside the default catalog, and phig ignores it. Removing it is the user's
call, outside this plan.

### 4. A read never creates or migrates a catalog (R70)

- **Read commands open read-only.** That covers `search`, `duplicates`, and
  the `list` forms of `tag`, `gallery`, `fav` and `face`. They pass
  `read_only = true` to `Database` and do not call `init_schema`, as libphig's
  read-only context already does (`libphig/context.cpp:31-45`).
- **A missing catalog refuses.** A read against a catalog directory or database
  that does not exist exits 1 and names the path.
- **An outdated catalog refuses.** A read against a catalog at an older schema
  exits 1 and says that a write command will upgrade it.
- **Only `scan` creates a catalog.** Every other write has nothing to act on in
  an empty catalog, so `tag add`, `rm`, `cp`, `mv` and the rest refuse a
  missing one, the same way a read does.

This takes R70 over from 043, whose subject is the scan/thumbs/faces split and
which held R70 only because it was the open plan that owned `cli/`. 067 already
changes every one of those twelve open sites.

### 5. core/ resolves the paths once

```cpp
// core/paths.h
struct CatalogPaths {
    std::string dir;
    std::string db;      // dir + "/phig.db"
    std::string lock;    // lock_path(db)
    std::string trash;   // dir + "/trash"
    std::string thumbs;  // dir + "/thumbs"
    std::string log;     // dir + "/phig.log"
};

std::string lock_path(const std::string& db_path);  // db_path + ".lock"
CatalogPaths catalog_paths(const std::string& dir);
std::string default_catalog_dir();                   // data_dir()
void set_catalog(const CatalogPaths& paths);         // process-wide
```

- **Engines don't change.** `default_db_path()`, `thumbs_dir()`, `trash_dir()`
  and `default_log_path()` read the installed `CatalogPaths`, so their call
  sites stay as they are.
- **`set_path_overrides` goes,** and `models_dir` gets its own setter.
- **The catalog is process-wide,** as the thumbnail directory already is for
  libphig (`phig.h`'s `thumbs_dir` comment). One process works on one catalog.
  A `phigd` serving several catalogs would have to pass `CatalogPaths` through
  the engines instead. That is out of scope here, recorded rather than hidden.

### 6. The lock belongs to the database (R50)

`acquire_writer_lock` takes the installed catalog's `lock`. libphig's
`phig_acquire_write_lock` calls `lock_path(db)` instead of building the string
itself, so there is one definition. 039 keeps `lock_path`'s portability work,
and stops claiming R50 once R50 holds.

### 7. Config and models stay user-level

- **Config.** `~/.config/phig/config.ini` (or `--config`) holds preferences
  about the machine, such as `parallel` and thresholds. It is also where the
  default catalog is set, so it cannot live inside one. Its `db` and
  `thumbs_dir` keys are removed, `catalog` is added, and `models_dir` stays.
- **Models.** `~/.cache/phig/models` is a 37 MB download that every catalog uses
  unchanged.

### 8. The clients name a catalog the same way

- **gtk:** `phig-gtk [catalog-dir]` replaces the database-path argument.
  `phig_collection_open` takes the directory and installs it, so thumbnail
  lookups find `<catalog>/thumbs`. With no argument, gtk uses
  `resolve_catalog_dir`, so `PHIG_CATALOG` and the config file apply to it as
  they do to the CLI.
- **libphig:** `phig_config.db_path` and `thumbs_dir` become `catalog_dir`, and
  `models_dir` stays. This changes 53 `.db_path =` assignments across six
  `tests/test_libphig*.cpp` files. `PHIG_READ_ONLY` already refuses a missing
  database and an outdated schema.

### 9. Tests never see the developer's files

- **Scratch home.** A gtest global `Environment`, in a new file linked into both
  test binaries, runs before any test. It points `HOME` and
  `XDG_{CONFIG,DATA,CACHE,STATE}_HOME` into a per-process scratch directory
  from `tests/temp_dir.h`. ctest runs each test in its own process, so tests
  stay parallel-safe, and a direct `./build/phig-tests` run is covered too.
- **Models.** Before redirecting, it resolves the real `models_dir()` and
  symlinks `<scratch>/cache/phig/models` to it. The face tests keep finding the
  models, and a test that wants them missing still redirects `XDG_CACHE_HOME`
  itself.
- **The four bare `std::system` call sites go through `run_phig`,** which
  passes `--catalog` instead of `--db`.
- **`PHIG_CATALOG` is unset** in the test process and in `run_phig`'s child.
  A developer whose `.envrc` sets it runs the suite inside that environment,
  and a test must never resolve to that catalog.
- **Existing fixtures keep working.** Those that save and restore
  `XDG_CACHE_HOME` or `XDG_DATA_HOME` restore to the scratch values.

## Steps

Each step lands green on its own.

1. **Test isolation (decision 9).** Independent of the flag, so it goes first
   and stops the leak now. Verify by recording the mtimes of
   `~/.local/state/phig/phig.log` and `~/.local/share/phig/phig.db.lock`,
   running `ctest --parallel` including the perf suite, and comparing.
2. **`CatalogPaths` in core/ (decisions 5 and 6).** Add `catalog_paths`,
   `default_catalog_dir`, `set_catalog` and `lock_path(db)`, and reroute the
   existing getters. Write `CACHEDIR.TAG` where `thumbs/` is created. Point
   libphig's lock at `lock_path`. Unit tests cover the layout, `lock_path`, and
   the tag file.
3. **The one-time move (decision 3).** Test against a scratch home:
   - thumbnails and log move into an empty catalog;
   - nothing moves when the destination exists;
   - a second run is a no-op.

   The cross-filesystem path is tested where `MoveFileTest.CrossDevice*` can
   run, and skips with them elsewhere.
4. **`--catalog` in the CLI.** Parse it globally, remove `--db` from all twelve
   parsers and from the usage text, and replace the config keys.
   `acquire_writer_lock` takes the catalog's lock. Add `resolve_catalog_dir`,
   with unit tests for each precedence level. In `rules/operations.toml`,
   remove `--db` from `mutating_cli_flags` and add `--catalog`.
5. **Reads open read-only (decision 4).** Subprocess tests:
   - `search --count` and `duplicates` against a missing catalog exit 1 and
     create nothing;
   - against a copy set back to schema 7, they exit 1 and leave it at 7;
   - `tag add` against a missing catalog exits 1 and creates nothing;
   - `scan` against a missing catalog creates it.

   Set R70 to `holds`, with these as its tests.
6. **R50 test (decision 6).** Extend the subprocess test in
   `tests/test_lockfile.cpp`:
   - hold the flock on catalog A's lock and run
     `phig --no-wait --catalog A tag add`; it is refused;
   - run the same with `--catalog B`; it succeeds;
   - hold the default catalog's lock and run `--catalog A`; it succeeds.

   Set R50 to `holds`, and remove R50 from 039's `closes_rules` and paragraph.
7. **Trash and thumbs follow the catalog.** Tests:
   - `rm` under `--catalog A` puts files in `A/trash`;
   - `trash empty --catalog B` leaves `A/trash` alone;
   - `thumbs clean --catalog A` leaves `B/thumbs` alone.

   Update the trash rationales in `rules/authorization.toml` and
   `rules/recoverability.toml`, which name `<data_dir>/trash`. Run
   `rules.py --check --emit` and commit the regenerated views.
8. **Clients (decision 8).** Change gtk's argument and `phig_collection_open`,
   libphig's `phig_config`, and the libphig tests.
9. **Docs.** Update root `CLAUDE.md` (command reference, the paths paragraph,
   Concurrency, and the Real-run checks note, since a read against a missing
   catalog can now fail), `README.md`, `libphig/CLAUDE.md` and
   `gtk/CLAUDE.md`.

## Relationship to other plans

- **039:** owns `lock_path`'s portability. This plan changes its signature and
  takes over R50.
- **043:** gave R70 to this plan.
- **064:** moves `trash empty`'s guard into `core/` (R68). Its tests should be
  written against `--catalog`.
- **068:** rewrites `gtk/src/collection.h`. Whichever lands second carries
  decision 8's gtk change.
