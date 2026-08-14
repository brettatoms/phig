# phig

phig is an image-management engine: it scans image and video directories,
builds a metadata/hash database, detects duplicates, and organizes files into
a structured folder layout. Three consumers sit on top of the engine: the
`cli/` command-line tool, the `gtk/` desktop client (M0 — a working thumbnail
grid, not a finished app), and `libphig/`, the versioned C ABI for foreign
callers such as a future Elixir web client.

This file is the single root guidance file for the monorepo. Each directory
below has its own `CLAUDE.md` with directory-specific rules; this file covers
what's shared. `AGENTS.md` is a pointer to this file, kept only so agent
tools that look for that filename resolve to something.

## Layout

| Path | What it is | Stability |
|---|---|---|
| `core/` | C++ engine: scanning, hashing, EXIF, database, the five engines. | Internal, no promise |
| `libphig/` | Versioned C ABI (`libphig.so`) for foreign callers. | Versioned boundary |
| `cli/` | `phig` command-line tool: argv parsing, output rendering. | — |
| `gtk/` | GTK4/libadwaita desktop client. | M0, in progress |
| `tests/` | GoogleTest suite (fast + perf) covering `core/`, `libphig/`. | — |
| `vendor/` | Vendored third-party code, built without `-Wall -Wextra`. | — |
| `scripts/` | Dev tooling (e.g. `clang-tidy.sh`). | — |
| `docs/` | Cross-project specs/ledgers from when phig, phig-gtk, and phig-web were separate repos, plus execution artifacts owned by a numbered plan. Date-stamped files. | Historical record |
| `plans/` | Numbered implementation plans (`NNN-name.md`) for the whole monorepo — the single index of what's outstanding. | Working index |
| `rules/` | phig's data-integrity rules as TOML records: what must be true when a destination is occupied, when a row's file is gone, and what may destroy user-authored metadata. Plus three views generated from them by `scripts/rules.py`, which CI also runs as a decay check. Plan 054. | Normative |
| `MARKET.md` | Competitive analysis: the field, its recurring weaknesses, phig's differentiators. Dated, additive. | Working document |

See each directory's `CLAUDE.md` for what it is, its stability contract, and
directory-specific rules. Command reference, tech stack, and phig's key
design decisions (RAW+JPEG pairing, activity log, concurrency, etc.) are
below in this file.

## Building and testing

Native libraries (OpenCV, LibRaw, libheif, libjxl, FFmpeg, libexif, SQLite3,
spdlog, gtest, CURL) come from the system package manager — see `README.md` for per-distro
package names. No prefix is needed; a plain `cmake` works:

```bash
cmake -B build && cmake --build build
./build/phig-tests        # fast suite, 851 tests
./build/phig-perf-tests   # performance suite (~2s), 4 tests
ctest --test-dir build --parallel   # both — 855 (~15s; 48s without --parallel)
clang-format -i <files>
```

Optionally install [mise](https://mise.jdx.dev/) and run `mise install`
(`mise.toml`) to get the exact `cmake`/`clang-format`/`clang-tidy` versions
CI pins — not required for a build, only for the lint gate to agree with CI.

Performance tests live in a separate target so the everyday suite stays
fast. Which tests skip depends on the machine, so a skip count only matches
an environment, not the tree. A CI leg reports fewer than a local run — 846 on
both ubuntu legs against 855 here — because fixtures those tests need are
absent there, not because tests are missing.

Ten tests need the face path — `ScanEngine.DetectsFacesWithMultipleWorkers`
plus nine of the ten `FacesEngineRun` cases — and two more exist only to cover
a way it can be unavailable. Two conditions gate them, checked in this order by
`face_path_blocker()` in `tests/face_path.h`:

- **OpenCV ≥ 4.8.0.** Below it the ten skip naming the version, and
  `ScanEngine.FacesModeFailsClearlyBelowTheOpenCVFloor` runs instead of
  skipping. `ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing` also skips,
  because the version guard fires ahead of the models check.
- **The models in `~/.cache/phig/models`.** Absent, the ten skip naming the
  missing file and `FacesModeFailsClearlyWhenModelsAreMissing` runs.

So on an above-floor machine with the models present — the everyday case, and
the ubuntu-26.04 CI leg — the two clear-failure tests are the only face-path
skips. `MoveFileTest.CrossDevice*` (three) skips unless the temp directory is
on a different filesystem from the test's working directory. If a single
unattributed failure shows up, check
`DatabaseConcurrency.ConcurrentMigrationIsSafe` first — see Known issues
below before assuming your change caused it.

`--parallel` has to be passed explicitly: CMake cannot make a bare `ctest`
parallel (`CTestConfig.cmake`'s `CTEST_PARALLEL_LEVEL` is ignored by a direct
`ctest` invocation — measured, still 48s). Tests get a unique temp directory
from `tests/temp_dir.h`, which is what makes parallel safe; a new fixture that
hardcodes a temp path instead will pass alone and fail under `--parallel`.

`gtk/` has its own build (`cmake -B build && cmake --build build -j` from
inside `gtk/`) and 13 tests across two binaries — see `gtk/CLAUDE.md`.

### Sanitizers

CI runs ASan+UBSan and TSan as two ubuntu jobs, configured through
`CMAKE_CXX_FLAGS` (there is no `PHIG_SANITIZE` option). They run on the weekly
schedule and on `workflow_dispatch`, not on every push — see "CI" below. To
reproduce one locally:

```bash
cmake -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
OMP_NUM_THREADS=1 GTEST_DEATH_TEST_STYLE=threadsafe \
  ctest --test-dir build-san --parallel -E DuplicatesPerf
```

Three things about that command are load-bearing:

- **`-E DuplicatesPerf`.** Those tests assert wall-clock budgets that
  instrumentation blows through — measured, TSan takes 1957ms and 2526ms
  against a 1500ms budget. Excluding them is not hiding a regression.
- **`GTEST_DEATH_TEST_STYLE=threadsafe`.** The suite has two `EXPECT_EXIT`
  tests, and gtest's default `fast` style forks without re-exec, leaving the
  child holding a sanitizer runtime that is not fork-safe. Measured on the
  concurrent-thumb `EXPECT_EXIT` under TSan: 3 of 40 runs died in the child
  with the default, 0 of 40 with `threadsafe`.
- **`OMP_NUM_THREADS=1`** — see below.

Leak detection is off (`ASAN_OPTIONS=detect_leaks=0`) because it reports
through OpenCV and LibRaw, which this gate cannot fix. Measured cost on an
M-series Mac, perf excluded: ASan+UBSan 25s, TSan 82s, against ~12s
uninstrumented. CI runners are slower.

### ThreadSanitizer: always set `OMP_NUM_THREADS=1`

```bash
OMP_NUM_THREADS=1 ./build/phig-tests   # TSan build
```

OpenCV's parallel framework here is OpenMP, and `libgomp` is not
TSan-instrumented, so TSan cannot see its barriers — every OpenCV parallel
region inside a `detect()` or decode call then looks like unsynchronized
access. Measured: `*Face*` 77-86 reports and `*Scan*` 42 at the default
thread count, 0 for both with `OMP_NUM_THREADS=1` and no code change. Run
TSan without this and roughly 120 false positives will bury any genuine
finding.

### Real-run checks

```bash
./build/phig --version                                          # phig 0.3.0
```

**The `search --count` check that used to sit here was unsound and has been
removed.** It read `./build/phig search --count --db
~/.local/share/phig/phig.db   # 29925`. Two problems, both measured 2026-08-13:

- That database holds zero images. It contains schema v8, no operations, and
  one tag — `favorite`, the one `init_schema` creates — so it is a database
  that was never populated rather than one that was emptied. No `scan`, `rm`,
  `mv` or `cp` in `~/.local/state/phig/phig.log` has ever named that path.
- **Running the check creates the thing it verifies.** phig opens the database
  read-write and initializes the schema even for a read (R70), so
  `search --count` against a missing path makes an empty database and then
  reports it empty. The check could never fail with "no such database", which
  is the failure it existed to catch.

A real-run check against a live collection is still worth having. It needs a
path that a read cannot bring into existence, so write it against a collection
you have actually scanned and state where.

### Linting & CI

- **Formatting:** clang-format, Google style / 4-space / 100-col
  (`.clang-format`). CI's lint step globs `core libphig cli tests` for
  `*.cpp`, `*.h` and `*.c` at any depth (commit `6a13144` — `src/` no longer
  exists, this replaced a glob on it). `vendor/` is excluded. Check with
  `clang-format --dry-run --Werror <files>`.
- **Warnings:** first-party code builds with `-Wall -Wextra`; configure with
  `-DPHIG_WERROR=ON` to make them fatal (CI does). Vendored code (`vendor/`)
  is a separate `phig-vendor` target without these flags.
- **clang-tidy:** high-signal set in `.clang-tidy` (bugprone/performance/
  analyzer, noisy checks disabled). Both CI and local runs go through
  `./scripts/clang-tidy.sh`, which supplies the standard-library headers
  clang-tidy's frontend cannot find on its own — g++'s search dirs on Linux
  (discovered via `g++ -E -x c++ - -v`, never hardcoded), the SDK sysroot
  from `xcrun` on macOS.
  - **Scope is sources under `core/`, `libphig/`, `cli/`.** `tests/` is
    deliberately outside it. Headers are covered as those sources include
    them, through `HeaderFilterRegex` — they cannot be listed directly,
    because the file list comes from `compile_commands.json`, which holds
    translation units only.
  - **`HeaderFilterRegex` must stay unanchored.** clang-tidy matches it
    against the absolute path, so a leading `^` matches nothing and silently
    suppresses every finding in phig's own headers — which is what it did,
    hiding 16 of them, until commit `b89d6e8`'s follow-up.
- **Rules record:** the build-and-test job runs it across its two ubuntu legs.
  On **ubuntu-latest**: `scripts/rules.py`'s own unit tests, then `python3
  scripts/rules.py --check --emit`, then `git diff --exit-code rules/`. That is
  four decay checks over `rules/*.toml` — schema, anchors, surface
  classification, plan closures — plus an ABI-version comparison, and a
  requirement that the three generated `rules/by-*.md` views match what was
  committed — regenerate them, never edit them. On **ubuntu-26.04**: `python3
  scripts/rules.py --check --test-results <gtest xml>`, which adds check 2, that
  a `holds` rule's tests exist and pass. Check 2 is the only check that reads a
  test run, so it goes on the leg with the face models: a skip is neither a pass
  nor a failure to `rules.py`, so on a models-less leg it would report OK for
  R52 with its only witness never having run. The rest goes on the stable label
  rather than on `ubuntu-26.04`, which is Preview. Deliberately not in the lint
  job: check 2 reads a gtest XML, which needs the compiled test binary that
  `lint` has no reason to build. See "Rules of record" below.
- **CI:** `.github/workflows/ci.yml` runs four jobs — build + test on
  ubuntu-latest, ubuntu-26.04 and macos-latest (with `-DPHIG_WERROR=ON`, and
  the rules gate above split across the two ubuntu legs), ASan+UBSan and TSan
  (see Sanitizers above), a gtk job that installs libphig and then builds and
  tests `gtk/` against it through pkg-config, and the lint gate. GitHub Actions
  are pinned to full commit SHAs.

  **Two ubuntu legs, because OpenCV's version decides whether phig can detect a
  face.** `ubuntu-latest` is 24.04 and ships libopencv-dev 4.6.0, below the
  4.8.0 the YuNet model needs, so the face tests skip there and the version
  guard's failure path is what runs. `ubuntu-26.04` ships 4.10.0, is where the
  face models install, and is the only leg that detects a face. `ubuntu-26.04`
  is a GitHub **Preview** label: it can change or be withdrawn without a
  deprecation window, and when 26.04 becomes the default it and `ubuntu-latest`
  will be the same image.
- **Not every job runs on every push.** A push or PR gets both ubuntu test
  legs, gtk and lint. That cost has not been measured since the second ubuntu
  leg was added; before it, the set was 15 billable minutes.
  The macOS leg and both sanitizer jobs are
  weekly (Mondays 09:00 UTC) plus `workflow_dispatch`, which runs the full set
  on any branch: `gh workflow run CI --ref <branch>`. macOS runners bill at 10x,
  so that one job was 40 of a full run's 71 minutes and 64% of a month's spend.
  A change to platform code, or anything you want a sanitizer verdict on before
  merging, needs the manual run — a green PR has not been near macOS or TSan.
- **Prose-only pushes do not run CI at all.** `paths-ignore` covers `plans/`,
  `docs/` and top-level `*.md`. `rules/` is deliberately not in that list: the
  ubuntu legs gate its records and generated views.
- **`actions/cache` carries two things.** ccache on the test and gtk jobs —
  each build job prints `ccache --show-stats`, which is where to look if a
  build step is slower than expected — and the 37 MB of face models on the
  ubuntu-26.04 leg, keyed by the model filenames so the key stays valid until a
  model is deliberately bumped. A models cache miss costs a download, not a
  failure. The action's save runs in a post step declared `post-if: success()`,
  so a red job never writes its cache back and the next run pays the miss
  again. The sanitizer jobs are deliberately excluded from ccache: at a weekly
  cadence their cache is always stale.

## Tech stack

- **Language:** C++20
- **Build:** CMake
- **Dev environment:** system-installed native libraries; `mise` (optional)
  pins `cmake`/`clang-format`/`clang-tidy` to the versions CI uses
- **Dependencies:** OpenCV (image decode + phash + video keyframes), libexif
  (EXIF extraction), libheif (HEIC/HEIF/AVIF decode and their EXIF block),
  libjxl (JPEG XL decode and its Exif box; libheif handles HEIF-family
  containers only) — on macOS decode goes through Apple's ImageIO instead,
  and the two libraries are used only for the EXIF block there —
  LibRaw (RAW decode), FFmpeg/libavformat (video
  metadata), SQLite (database), spdlog (diagnostic logging), GoogleTest
  (tests)

## Interface discipline

**Clients go through phig's defined interfaces only.** A client reads phig
through `libphig`'s C ABI (`<phig/phig.h>`) when the collection is local, and
through the phig wire protocol when it is remote — plan 052, `designing`;
`phigd` does not exist yet, so `libphig` is the only interface that works
today. No client reads phig's SQLite schema, table names, or thumbnail cache
layout directly — those are private and change. The two interfaces cover the
same capabilities deliberately: a client chooses a transport at connect time,
it does not learn a second vocabulary. When a client needs something an
interface lacks, the capability is added there and tracked in `plans/`.

**libphig should reach all of phig's features, not the subset it reaches
today.** A capability that exists only in `core/` and `cli/` is a gap with a
schedule, not a boundary on what a client may do, and no other design may
read the ABI's current coverage as the limit of what phig can offer.
Deliberate exclusions are few and are named in `libphig/CLAUDE.md`, which also
carries the inventory of what is missing; plan 053 tracks closing it.

**Non-GUI functionality belongs in libphig, not the clients.** Logic that
isn't specific to rendering a UI — filtering, formatting, business rules —
goes in `core/` behind a `libphig` entry point, not duplicated in `gtk/` or
any other client.

**`cli/` — and `phigd` when it exists — are peers of libphig over `core/`, not
clients of it.** Both link `phig-core` and include `core/` headers directly,
so both are entitled to whatever `core/` has the moment it lands. That
standing comes from being phig rather than a foreign caller; it is not a
licence for a client to do the same.

## Platform boundaries

**The engine is portable; the clients are not.** `core/` and `libphig/` must
compile cleanly and pass their tests on Linux, macOS and Windows. Code that
genuinely has to differ per platform belongs behind a narrow split in the one
file that owns that concern (`paths.cpp`, `lockfile.cpp`), never as `#ifdef`s
threaded through the engines.

Each desktop client targets exactly one platform and may use it fully,
including platform-specific optimizations — that is the point of having three
rather than one portable client.

| Path | Platform |
|---|---|
| `core/`, `libphig/` | Linux, macOS, Windows |
| `gtk/` | Linux — GTK4/libadwaita |
| `macos/` | macOS — SwiftUI/AppKit. Planned, see 047 |
| `windows/` | Windows — WinUI 3. Planned, see 047 |

Being platform-specific buys a client native UI and native performance work.
It does not buy it a private path into phig's data: every client still reads
phig through a defined interface only, per Interface discipline above.

Current state: macOS builds and tests green in CI. Windows is not yet a
supported target — plan 039 tracks the remaining port, where `lockfile.cpp` is
a rewrite and `paths.cpp` needs a platform split. `cli/` still has POSIX
surface (`unistd.h`) and has not been audited for portability.

## CLI command reference

Derived from `cli/main.cpp`, which parses 52 distinct flags. Check it against
the source rather than trusting this block:
`grep -oE '"--[a-z0-9-]+"' cli/main.cpp | tr -d '"' | sort -u`.

```bash
# Global — recognized anywhere in the argv, before the command parsers see it
phig [--config <path>] [--no-wait] <command> ...
phig --version | --help

# Scan a directory into the database
phig scan <directory> [--recursive] [--force [hash,thumbs,faces,all]] [--thumbs] [--faces] [--media image|video|all] [--hidden] [--ignore-mount-warning] [--on-error warn|fail] [--parallel N] [--db <path>]

# Generate thumbnails
phig thumbs rebuild [--match <glob>] [--exclude <glob>] [--force] [--parallel N] [--db <path>]
phig thumbs clean [--dry-run] [--db <path>]

# Find duplicate images
phig duplicates [--type exact|near|all] [--threshold 0-64] [--format text|csv|json] [--output <path>] [--match <glob>] [--exclude <glob>] [--media image|video|all] [--fav] [--db <path>]

# Copy files into YYYY/MM structure
phig cp <destination> [--format <string>] [--match <glob>] [--exclude <glob>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Move files into YYYY/MM structure
phig mv <destination> [--format <string>] [--match <glob>] [--exclude <glob>] [--dry-run] [--on-conflict skip|overwrite|rename] [--db <path>]

# Search the collection
phig search [--ext <ext>] [--after <date>] [--before <date>] [--camera <s>] [--make <s>] [--min-size <n>] [--max-size <n>] [--media image|video|all] [--min-duration <d>] [--max-duration <d>] [--similar <image>] [--similar-hash <hex>] [--threshold 0-64] [--face <image>] [--face-threshold <f>] [--person <name>] [--tag <name>] [--gallery <name>] [--fav] [--blurry] [--min-sharpness <n>] [--max-sharpness <n>] [--match <glob>] [--exclude <glob>] [--limit N] [--count] [--porcelain] [--print0] [--format text|csv|json] [--output <path>] [--db <path>]

# Remove images from the database and/or disk
phig rm [--match <glob>] [--exclude <glob>] [--hard|--db-only|--orphans] [--ignore-mount-warning] [--dry-run] [--yes] [--db <path>]
phig trash empty [--yes]

# Tags, galleries, favorites
phig tag add|remove <tag> [--match <glob>] [--exclude <glob>] [--path <dir>] [--db <path>]
phig tag list [--db <path>]
phig gallery set <name> [--no-overwrite] [--match <glob>] [--exclude <glob>] [--path <dir>] [--db <path>]
phig gallery clear [--match <glob>] [--exclude <glob>] [--path <dir>] [--db <path>]
phig gallery list [--db <path>]
phig fav add|remove [--match <glob>] [--exclude <glob>] [--path <dir>] [--db <path>]
phig fav list [--count] [--limit N] [--porcelain] [--print0] [--format text|csv|json] [--output <path>] [--db <path>]

# Faces
phig face name <image> <name> [--db <path>]
phig face identify <name> [--db <path>]
phig face rebuild [--match <glob>] [--exclude <glob>] [--parallel N] [--db <path>]
phig face list [--db <path>]

# Download the face detection/recognition models
phig models download
```

`--path <dir>` narrows by directory prefix on `tag`, `gallery` and `fav` only.
`duplicates`, `cp` and `mv` reject it — measured, `Unknown option: --path` and
exit 1 — even though `--path` reads as a natural fit for all three.

Paths follow XDG conventions (all resolved in `core/paths.cpp`): database at
`~/.local/share/phig/phig.db`, models at `~/.cache/phig/models`, thumbnails
at `~/.cache/phig/thumbs`, diagnostic log at `~/.local/state/phig/phig.log`,
config at `~/.config/phig/config.ini`. An optional INI config file (parsed
in `core/config.cpp` via vendored inih) sets defaults; a global `--config
<path>` flag overrides its location. Precedence: CLI flag > config >
default.

## Project structure

```
core/       C++ engine (internal, no stability promise). scanner, hasher,
            exif, database, selection, dates, metadata, models, raw,
            relink, sidecar, settings, parallel, log, lockfile, paths,
            config, and the five engines: ScanEngine, ThumbsEngine,
            FacesEngine, SearchEngine, plus the remove/organize executors.
libphig/    C ABI (see libphig/CLAUDE.md) — context, query, mutate,
            records, settings, thumbs entry points behind phig.h.
cli/        phig binary: main.cpp (argv parsing, usage text, output
            rendering, SIGINT handler, progress printer).
gtk/        GTK4/libadwaita desktop client (see gtk/CLAUDE.md).
tests/      GoogleTest suite: one file per core/libphig unit, plus
            perf_duplicates.cpp for the perf target.
vendor/     Vendored third-party code.
scripts/    Dev tooling.
docs/       Cross-project specs + execution artifacts (see Layout above).
plans/      Numbered plans, monorepo-wide (see Layout above).
```

## Key design decisions

- **Database is source of truth** — always reflects current file locations.
  Designed to be reusable by other tools (the GTK client, libphig callers).
- **EXIF stored as JSON blob** — no schema migrations needed, queryable via
  `json_extract()`.
- **Re-scan skips unchanged files** — matches on path + mtime. Use
  `--force` to reprocess.
- **Granular --force** — accepts `hash,thumbs,faces,all` (comma-separated
  or repeated). Bare `--force` = all.
- **Safety guard on scan** — warns if all DB entries for a directory are
  missing (possible unmounted drive). Requires `--ignore-mount-warning` to
  proceed with deletion.
- **Scan re-link** — when scan removes a stale row (its file is gone) and
  exactly one other tracked file with the identical sha256 exists on disk,
  it transfers the stale row's user-authored metadata (tags/favorites,
  gallery if the target has none, faces if the target has none) to that
  surviving row before deleting the stale one, logged as a `relink` entry.
  Ambiguous (two+ identical-content files) or no on-disk match: no
  transfer, row still deleted with a restore snapshot.
- **Thumbnails** — JPEGs cached in `~/.cache/phig/thumbs/<size>/ab/<sha>.jpg`,
  keyed by (SHA256, size). Deduplicates naturally, and sizes are independent:
  generating 768px never replaces or invalidates 512px, so a client can ask for
  what it needs without a cache-wide regeneration. 512 is the default that
  `scan --thumbs` and `thumbs rebuild` write, and libphig generates another size
  on demand when a lookup misses. `ThumbsOptions::size` (`core/thumbs_engine.h:18`)
  takes any size, but no CLI flag reaches it: `--size` is advertised by the help
  text (`cli/main.cpp:129`) and has no parser branch, so `thumbs rebuild --size
  768` prints `Unknown option: --size` and exits 1. Plan 043 owns wiring it.
- **Video support** — scans video files (MP4/MOV/MKV/AVI/WebM etc.),
  extracts keyframe phash via OpenCV VideoCapture, metadata via
  libavformat. Videos stored with `media_type = 'video'`,
  searchable/filterable.
- **Schema migrations** — `meta` table tracks schema version. Forward-only.
  Version check on startup bails if DB is newer than code. Current: v9.
  Migrations run with `PRAGMA foreign_keys=OFF` around each transaction, with
  `foreign_key_check` inside it before the commit: SQLite's `DROP TABLE`
  deletes every row first, so a rebuild of a table with `ON DELETE CASCADE`
  children would empty them and report success.
- **`exif` column renamed to `metadata`** — stores JSON blob for any media
  type (EXIF for images, container metadata for videos).
- **Separate cp/mv commands** — cp is non-destructive, mv updates DB paths.
- **Match/exclude system** — `--match` includes (repeatable), `--exclude`
  excludes (repeatable), exclude wins on overlap.
- **Duplicates compare against full DB** — `--match` selects which groups
  to show, not which files to compare.
- **Activity log** — `mv`/`cp`/`rm` record an operation plus per-file
  entries in `phig.db` (append-only; undo is a future logged operation).
  Each entry commits in the same transaction as the DB change it
  describes. Disable with `[log] enabled = false`; cap size with `[log]
  max_entries` (default 100000, `0` = unlimited, oldest operations pruned
  first). `--dry-run` writes nothing.
- **Diagnostic log** — one spdlog logger, two sinks: stderr
  (warnings/errors, `[warning]`/`[error]` prefix) and a rotating file at
  `~/.local/state/phig/phig.log` (UTC-timestamped; one line per entry with
  newlines escaped; 5 MB rotation to `phig.1.log`). Command invocations and
  scan summaries log at INFO (file only). `[log] file = false` drops the
  file sink; `[log] file_level = info|warn|error` sets its threshold. All
  logging goes through `core/log.h` — no spdlog includes elsewhere.
- **Sidecar files** — `mv`/`cp` carry a media file's metadata sidecars
  (xmp, pp3, dop, on1, aae, json, thm, lrv; both `IMG_1234.xmp` and
  `IMG_1234.CR2.xmp` naming) to the destination beside their image,
  renaming them to match a renamed parent, logged as
  `move_sidecar`/`copy_sidecar` entries linked to the parent image id.
  Sidecars are never indexed as media. Organize and search display also
  read a sidecar date (XMP `CreateDate`/`DateTimeOriginal`, Google Takeout
  `photoTakenTime`) as a date source after EXIF and before mtime, so
  EXIF-stripped photos file (and display) under their real date, not the
  copy date.
- **Concurrency** — SQLite is WAL with a 30s busy_timeout; write
  transactions use `BEGIN IMMEDIATE` so read-then-write paths don't hit
  BUSY_SNAPSHOT, and schema migrations re-check the version inside an
  IMMEDIATE transaction so concurrent first-runs don't re-apply DDL.
  Most mutating commands additionally take a process-wide advisory `flock`
  on `<db>.lock` (`core/lockfile.cpp`) so whole operations serialize; reads
  never take it. Per-machine only. The `face` subcommands are the exception
  — they run lock-free, matching libphig's faces operation, and defend
  themselves instead: `faces.id` is AUTOINCREMENT so a deleted id is never
  reissued and a face deleted mid-run matches nothing, `identify` writes under
  `person_id IS NULL` so it refuses a face another labeler named first, and
  `rebuild` reads each image id inside the transaction that writes its
  faces. A lock only excludes other lock-takers, so it was never protection
  against an ABI caller anyway.
- **RAW + JPEG pairing** — a RAW file (CR2, NEF, ARW, etc.) is the indexed
  shot; a same-basename JPEG beside it is not indexed as its own row.
  Instead it's an attachment: the RAW row's pixel source (for
  phash/faces/thumbs) and a file carried alongside the RAW on `mv`/`cp`,
  like a sidecar. Pixels come from a 3-tier LibRaw-backed source — sidecar
  JPEG, then embedded preview, then full RAW decode — replacing OpenCV's
  `cv::imread`, which fails on many RAW formats. On scan, a JPEG with a
  same-basename RAW on disk is demoted: skipped as its own row, and if it
  was already indexed from an earlier scan, its tags/faces/gallery
  transfer to the RAW row at cleanup before its row is dropped. A lone
  JPEG with no sibling RAW is indexed normally.

## Plans and specs

`plans/` is the single index of phig's work — one sequence across the whole
monorepo, covering `core/`, `libphig/`, `cli/`, `gtk/`, and anything added
later. There are no per-directory plan folders, and a plan may span several
directories. Every plan is `plans/NNN-name.md` with frontmatter (`name`,
`status`, `created`, `description`) and a row in `plans/000-INDEX.md`. If it
isn't in the index, it isn't tracked — the index is the only place to look
for what's outstanding.

`docs/` holds two things, neither of them tracked plans: cross-project specs
and ledgers from when phig, phig-gtk, and phig-web were separate repos, and
execution artifacts (superpowers plans, SDD ledgers, diagnoses) belonging to
a numbered plan. An artifact in `docs/` may be linked from a numbered plan;
it never carries the status itself.

**Plan location overrides superpowers.** The `writing-plans` skill defaults
to `docs/superpowers/plans/YYYY-MM-DD-name.md` and yields to project
preference. In this repo a plan goes to `plans/NNN-name.md` with an index
row. If the plan is an execution script — bite-sized TDD steps with
checkboxes — write it to `docs/plans/` as an artifact and open or update the
numbered plan that owns it.

Status is one of: `designing` (open questions remain), `planned` (designed,
not started), `in-progress` (started; the plan file says what's left),
`completed` (shipped, including pieces explicitly deferred to a later plan),
`deferred` (deliberately not now), `superseded` (replaced — the row and the
file's frontmatter both name the replacement).

**An index row is one sentence.** It says what the plan is, in enough words
to decide whether to open it — 15 to 30, the length of the 001-005 rows. It
carries no status, no progress, no measurements and no design reasoning: all
of that lives in the plan file, which is the only place it can be kept
current. A row that restates the plan makes the index unscannable, which is
the one thing it exists to be.

New cross-project or monorepo-wide design docs go in `docs/`. Completed
plans should not be edited unless the user explicitly asks — it's fine to
add cross-references to other plans. Changes to completed features get a new
plan rather than editing the original. Always write a plan and get approval
before implementing.

## Rules of record

`rules/` states what must be true when phig moves, deletes or overwrites —
conflict resolution, reconciliation, atomicity, recoverability, metadata
preservation, attachments, concurrency, cancellation, authorization. One TOML
record per rule, each naming the statement, why it is that way, where it is
enforced, which operations it binds, which asset it protects, and the test that
pins it. `rules/README.md` is the field reference. Plan 054 owns the mechanism.

**Read the relevant `rules/*.toml` before designing anything that moves,
deletes or overwrites** — do not reconstruct the behaviour from call sites.
That reconstruction is exactly what this exists to stop: designing plans 041
and 042 turned up five cases where the code did something other than what it
appeared to, each found by reading six files while designing something else.

Four working rules:

- **Changing a guard means updating its record in the same commit**, then
  running `python3 scripts/rules.py --check --emit` and committing the
  regenerated views alongside it. A rule whose `enforced_at` no longer matches
  the code is worse than no rule, because the next design will trust it.
- **Enforcement belongs in `core/` or `libphig/`, never `cli/`.** A guard that
  lives in `cli/` is unreachable by libphig, by `phigd`, and by every GUI — so
  it is a finding to record, not a record to file. This is Interface
  discipline above, applied to data safety. CI enforces it rather than trusting
  it: an anchor under `cli/` fails the build unless the rule's `status` is
  `violated`, where the misplacement is the thing being recorded and an open
  plan's `closes_rules` claims the rule. **Such a rule must also name a user
  asset the command can actually destroy** — the rule's whole value is that a
  plan moves it into `core/`, so a finding with nothing to move and nothing to
  protect is an ordinary bug and belongs in a plan's body. Without that bar the
  record drifts into a bug tracker for `cli/`.
- **Behaviour no rule states? Write the rule.** A new guard, a new operation,
  or a behaviour discovered while reading code each need a record. The
  discovered-while-reading case is the important one — that is how all five
  original findings surfaced, and none was ever reported as a bug.
- **A rule the code violates is recorded, not deleted.** Set `status =
  "violated"` and add the rule id to an open plan's `closes_rules`. Deleting
  the inconvenient rule turns the record into a description of the code, which
  is the one thing it must never become. The same applies to `test_gap` when a
  test is weaker than the claim it pins — that one needs no plan, because a
  weak test is not unsafe code.

An **operation** in `rules/operations.toml` is a `core/` primitive that
surfaces hang off (`surfaces_cli`, `surfaces_abi`, `surfaces_http`), not a CLI
command — so a rule bound to an operation is inherited by every surface,
including ones added later. A function that mutates but is internal to an
operation goes in `core_internal`; rules anchor at it and bind the operation
that covers it.

`rules/by-invariant.md`, `by-operation.md` and `by-asset.md` are **generated
from the TOML by `scripts/rules.py --emit` and are never hand-edited.** An edit
to one is discarded on the next run and fails CI in the meantime, because the
build-and-test job regenerates all three and diffs them against what was
committed. The same run checks the records' own schema, that anchors resolve,
that a `holds` rule's tests exist and pass, that every `violated` or `partial`
rule is claimed by an open plan's `closes_rules` and that no closed plan still
claims one, that every ABI symbol, CLI command and CLI flag is classified, and that the
ABI version macros agree with the CMake `VERSION`/`SOVERSION`.

Status: the enumeration phase is complete, reported in
`docs/plans/2026-08-09-054-enumeration-report.md` — for how many rules there
are now, run `python3 scripts/rules.py --check`, which prints the count from
the records themselves. The report's figure was three short within a week, so
this file does not carry one.

**CLI flags are covered.** Every long flag `cli/main.cpp` parses sits in either
`mutating_cli_flags` or `read_only_cli_flags` in `rules/operations.toml`, and
check 4 fails the build on one that is in neither or in both. A flag is
mutating when it can change what is written to disk or the database —
including which files an operation writes to or destroys, not only whether it
writes at all. Classify by what the flag reaches, never by whether its command
sounds read-only: `search --similar -` stages its query image through a
fixed-path truncating write, and was filed read-only until that was measured.

## Conventions

- **Comments describe the present.** What the code does and why it is that
  way now — not what it used to do, or which change introduced it. History
  belongs in the commit message.
- **Prose is plain.** No marketing tone, no three-part flourishes, no
  padding phrases. State the thing and stop. This applies to commit
  messages, specs, plans, and READMEs.
- **Performance claims are measured, not reasoned.** This has been
  load-bearing: a BK-tree specified for duplicate detection turned out 40x
  slower than its target, and three rounds of guard calibration each found
  a regression the previous round could not have predicted. Correctness
  questions yield to reasoning; performance questions have only yielded to
  measurement.
- **Data-safety changes ship with real-behavior tests**, not mocks.

## Known issues

Three `DatabaseTest` cases fail on master — `UpdatePathOntoStaleDestClearsItsFaces`,
`DeletePathsCascadesTagsAndFaces`, `PurgeSnapshotMarksOmittedFaces` — all with
`Failed to delete face vectors: SQL logic error` from
`delete_faces_for_image`'s DELETE against the `vec_faces` vec0 virtual table.
Reproducible from a clean build at `3177320`, so a change that leaves exactly
these three failing has not caused them. **Platform-dependent**: all three pass
on macOS at `7c59d2d` against Apple's system SQLite 3.51.0, so a clean run
proves the machine, not the fix. Diagnosis in progress: plan 046.

**They no longer reproduce on CI.** All three passed on both the ubuntu-latest
and ubuntu-26.04 legs on 2026-08-13 (run against `b69dae0`), where the whole
suite was green. That is a measurement of those runners, not a fix: nothing has
changed in `delete_faces_for_image` or the vendored vec0, so the likely
explanation is the environment rather than the code. Do not close 046 on this —
but do not spend time chasing a failure CI is not showing either.

`DatabaseConcurrency.ConcurrentMigrationIsSafe` was a genuine 10% flake
caused by `sqlite3_busy_timeout` being set after `PRAGMA
journal_mode=WAL`; fixed in `73d1e1a`. If a single unattributed test
failure appears, check whether it is that test before investigating a
change.
