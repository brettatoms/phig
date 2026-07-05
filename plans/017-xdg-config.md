---
name: XDG Paths & Config File
status: completed
created: 2026-07-05
description: >
  Centralize base-directory resolution behind XDG conventions (config/data/cache),
  move face-model downloads from data to cache, and add an INI config file
  (vendored inih) that can set default paths and safe command defaults. Config-aware
  --help. Models simply relocate to the cache dir (no shipped migration code).
---

# XDG Paths & Config File

## Goal

Give phig a proper configuration story. Today the database and face models both
live under `~/.local/share/phig/` via ad-hoc `XDG_DATA_HOME`/`HOME` logic
duplicated in `main.cpp` and `models.cpp`. Thumbnails (added in plan 014) already
live in the cache directory, but via yet another ad-hoc `XDG_CACHE_HOME`/`HOME`
block in `thumbs.cpp` (`get_thumb_cache_dir()`). The only way to configure anything
is command-line flags. This plan:

1. Centralizes all base-directory resolution in one module using full XDG
   conventions (config, data, cache).
2. Moves face-model downloads from the data directory to the **cache** directory
   (they are disposable, re-downloadable derived data).
3. Adds an optional INI config file so users can set default paths and a curated,
   safe set of command defaults.
4. Makes `--help` reflect the active configuration.

## Design Decisions

### XDG-style paths on all platforms

Each base directory honors its `XDG_*` environment variable when set and
non-empty; otherwise it falls back to the XDG default **on every platform,
including macOS** (no `~/Library` special-casing — consistency with Linux and
with how most CLI dev tools behave).

| Resolver       | Env var           | Fallback         | phig subdir |
|----------------|-------------------|------------------|-------------|
| `config_dir()` | `XDG_CONFIG_HOME` | `~/.config`      | `/phig`     |
| `data_dir()`   | `XDG_DATA_HOME`   | `~/.local/share` | `/phig`     |
| `cache_dir()`  | `XDG_CACHE_HOME`  | `~/.cache`       | `/phig`     |

Empty-string env vars are treated as unset (matches current behavior). A missing
`HOME` throws `std::runtime_error`, as today.

Derived paths:
- `default_db_path()` → `data_dir()/phig/phig.db` (unchanged location; logic moves here)
- `models_dir()` → `cache_dir()/phig/models` (**moved** from data to cache)
- `thumbs_dir()` → `cache_dir()/phig/thumbs` (absorbs `thumbs.cpp`'s resolver;
  location unchanged)
- `config_path()` → `config_dir()/phig/config.ini`

The **three** duplicated env-var blocks — `main.cpp:38`, `models.cpp:60`, and
`thumbs.cpp`'s `get_thumb_cache_dir()` (`thumbs.cpp:15`) — collapse into this one
module. `thumbs.cpp` calls `thumbs_dir()` instead of resolving the cache path
itself, so the cache layout stays identical. The `~`-expansion helper currently in
`filters.cpp:27` also moves here so config values and CLI path arguments share a
single expander.

### Config format: INI via vendored inih

INI fits phig's flat, path-and-defaults config; no nesting or typed arrays are
needed. `inih` (Ben Hoyt, BSD-3-Clause — compatible with phig's MIT) is vendored
as a single-file dependency exactly like `vendor/sqlite-vec/`.

Files vendored into `vendor/inih/`: `ini.c`, `ini.h`, `INIReader.h`,
`INIReader.cpp`. Added to the `phig-lib` target in CMake. The BSD-3-Clause notice
is retained in the vendored sources.

### Config schema

Flat sections; a commented example is documented in the README. Only the keys
below are recognized:

```ini
[paths]
db = ~/photos/phig.db
models_dir = ~/.cache/phig/models
thumbs_dir = ~/.cache/phig/thumbs

[general]
parallel = 4

[scan]
recursive = true
on_error = warn        # warn | fail
media = all            # image | video | all
thumbs = false         # generate thumbnails during scan
faces = false          # detect faces during scan

[organize]
format = %Y/%m/%original

[duplicates]
type = all             # exact | near | all
threshold = 5          # 0-64

[faces]
threshold = 0.6
```

The `[scan]` `media`/`thumbs`/`faces` keys correspond to the `--media`/`--thumbs`/
`--faces` flags added with video and thumbnail support. Granular `--force`
(`hash,thumbs,faces,all`) is **not** exposed — see excluded keys below.

Path values support `~` expansion via the shared expander.

### Excluded from config (safety)

These are **never** configurable and remain flag/built-in only:

- **`ignore_mount_warning`** — this flag (not `--force`, since the video/thumbnail
  work split them apart) is what overrides the unmounted-drive safety guard and lets
  scan delete DB entries. A persistent default would silently defeat that guard on
  every scan. Never config-overridable.
- **`force`** — now *granular reprocessing* (`hash,thumbs,faces,all`), not a safety
  bypass. Still excluded: a standing `force` default would silently reprocess
  everything on every scan (re-hash, regenerate thumbnails, re-run face detection),
  which is surprising and wasteful. Always per-command.
- **`dry_run`** — a persistent `dry_run=true` would make `cp`/`mv`/`purge` silently
  no-op forever.
- **`on_conflict`** — excluded entirely; `overwrite` as a standing default risks
  clobbering files. Always set per-command via `--on-conflict`.
- **All selection/query flags** — `--match`, `--filter`, `--path`, `--person`,
  `--face`, `--similar`, `--similar-hash`, `--before`, `--after`, `--min-size`,
  `--max-size`, `--make`, `--camera`, `--ext`, `--limit`, `--count`, `--output`,
  `--porcelain`. These describe a single query, not a standing preference.

Config validation is strict:
- If an excluded key (`ignore_mount_warning`, `force`, `dry_run`, `on_conflict`)
  appears → error naming the file and key, exit non-zero.
- Invalid value (unknown enum, out-of-range threshold, non-integer `parallel`) →
  error naming file, key, and offending value, exit non-zero.
- Unknown/unrecognized key → warn on stderr, ignore, continue.

### Precedence

Highest wins:

```
explicit CLI flag  >  config file value  >  built-in default
```

`XDG_*` env vars only shape the built-in default *paths*; they are not a separate
precedence layer for values. Each command's arg parser seeds `opts` from the loaded
`Config`, then CLI flags overwrite. `ignore_mount_warning`, `force`, `on_conflict`,
and `dry_run` are untouched by config.

### Config file selection: `--config <path>`

A global `--config <path>` flag (parsed before the subcommand) overrides which file
is loaded. Primary uses: pointing `test_porcelain.cpp` at a temp INI, and power-user
overrides. No `config` subcommand and no `config init` generator — the README
documents the format.

### Config-aware `--help`

`print_usage()` gains an **"Active configuration"** section printed at the top. It
is an additive block fed by the loaded `Config` and the path resolvers — the inline
`(default: X)` strings in the existing usage text are left as-is.

```
Active configuration:
  config file:    ~/.config/phig/config.ini  (loaded)
  db:             ~/photos/phig.db            [config]
  models_dir:     ~/.cache/phig/models        (default)
  scan.recursive: true                        [config, default: false]
```

- Resolved core paths (config file + whether found, db, models_dir) always shown.
- Each value the config overrides is listed and marked `[config, default: <builtin>]`.
- Values not touched by config are omitted (core paths show `(default)`).

### No automatic models migration

Earlier drafts included a one-time startup move of models from the old data-dir
location to the cache. This is dropped: phig has no released users, and the sole
current user has no models in the old location. `models_dir()` simply points at the
cache now, so a fresh `phig models download` writes there directly. Any pre-existing
models can be relocated with a one-off `mv` outside the program — no shipped
migration code.

## Implementation

### New Files

- `src/paths.h` / `src/paths.cpp`:
  - `std::string config_dir()`, `data_dir()`, `cache_dir()`
  - `std::string default_db_path()`, `models_dir()`, `thumbs_dir()`, `config_path()`
  - `std::string expand_tilde(const std::string&)` (moved from `filters.cpp`)
- `src/config.h` / `src/config.cpp`:
  - `struct Config` — optional-valued fields for each recognized key
  - `Config load_config(const std::string& path)` — returns defaults when absent,
    validates present keys, throws on excluded/invalid keys
- `vendor/inih/` — `ini.c`, `ini.h`, `INIReader.h`, `INIReader.cpp` (BSD-3-Clause)

### Modified Files

- `src/main.cpp`:
  - Remove local `default_db_path()`; use `paths.h`
  - Parse global `--config <path>` before subcommand dispatch
  - Load config at startup
  - Seed each command's `opts` from `Config` before applying CLI flags
  - Add "Active configuration" section to `print_usage()`
- `src/models.cpp`:
  - Remove local base-dir logic; `models_dir()` comes from `paths.h`
- `src/thumbs.cpp`:
  - Remove local `get_thumb_cache_dir()`; use `thumbs_dir()` from `paths.h`
- `src/filters.cpp`:
  - `~`-expansion helper moves to `paths.cpp`; call through to it
- `CMakeLists.txt`:
  - Add `src/paths.cpp`, `src/config.cpp`, `vendor/inih/ini.c`,
    `vendor/inih/INIReader.cpp` to `phig-lib`; add `vendor/inih` to includes
- `README.md`:
  - Document XDG paths, cache location for models, and the config file format
- `AGENTS.md`:
  - Update "Default database location" note to describe XDG resolution + config
    (done as part of this plan's completion, per plan rules)

### Config struct sketch

```cpp
struct Config {
    std::optional<std::string> db;
    std::optional<std::string> models_dir;
    std::optional<std::string> thumbs_dir;
    std::optional<int>         parallel;
    std::optional<bool>        scan_recursive;
    std::optional<std::string> scan_on_error;   // "warn" | "fail"
    std::optional<std::string> scan_media;       // "image" | "video" | "all"
    std::optional<bool>        scan_thumbs;
    std::optional<bool>        scan_faces;
    std::optional<std::string> organize_format;
    std::optional<std::string> duplicates_type;  // "exact" | "near" | "all"
    std::optional<int>         duplicates_threshold;
    std::optional<double>      faces_threshold;
};
```

## Testing

GoogleTest, following `tests/test_porcelain.cpp` patterns. All filesystem access is
isolated by setting `XDG_*` env vars and/or `--config` to temp dirs.

- Path resolution: each resolver with env var set / unset / empty string
- `~` expansion (leading `~` only; unchanged for non-`~` paths)
- Config parse: valid file, absent file (defaults), malformed INI, unknown key
  (warn + ignore), excluded-key rejection
  (`ignore_mount_warning`/`force`/`dry_run`/`on_conflict`), out-of-range/invalid values
- Precedence: flag beats config beats default (per command)

## Checklist

- [ ] `src/paths.h/cpp` — `config_dir`/`data_dir`/`cache_dir` resolvers
- [ ] `default_db_path`/`models_dir`/`thumbs_dir`/`config_path` derived helpers
- [ ] Move `~`-expansion from `filters.cpp` into `paths.cpp`
- [ ] Remove duplicated env-var logic from `main.cpp`, `models.cpp`, and `thumbs.cpp`
      (`get_thumb_cache_dir()` → `thumbs_dir()`)
- [ ] Vendor inih into `vendor/inih/` (BSD-3-Clause notice retained)
- [ ] `src/config.h/cpp` — `Config` struct + `load_config` with strict validation
- [ ] Reject excluded keys (`ignore_mount_warning`, `force`, `dry_run`, `on_conflict`)
      with clear errors
- [ ] Warn-and-ignore unknown keys
- [ ] Global `--config <path>` flag parsed before subcommand
- [ ] Seed each command's opts from config, then apply CLI flags (precedence)
- [ ] `models_dir()` points at cache; downloads land in cache
- [ ] "Active configuration" section in `print_usage()`
- [ ] CMakeLists: add new sources + inih + include dir
- [ ] Tests: path resolution (env set/unset/empty)
- [ ] Tests: `~` expansion
- [ ] Tests: config parse (valid/absent/malformed/unknown/excluded/invalid)
- [ ] Tests: precedence (flag > config > default)
- [ ] README: document XDG paths, model cache, config format
- [ ] Update AGENTS.md project description (paths + config)
