# phig

A command-line tool for managing image and video collections. Scan directories, build a metadata database, detect duplicates, recognize faces, and organize files into a folder structure.

## Features

- **Scan** — recursively scan directories, computing SHA256 + perceptual hashes, extracting EXIF/container metadata, and storing everything in SQLite. Handles images and video.
- **Duplicates** — find exact copies (same SHA256) and visually similar files (perceptual hash)
- **Copy/Move** — copy or move files into a structured layout (e.g., `YYYY/MM/`) with configurable format strings, conflict resolution, and dry-run mode
- **Search** — query the database by filename, date, EXIF data, file size, video duration, sharpness, tags, gallery, visual similarity, or face
- **Face recognition** — detect faces, compute embeddings, name people, and search by person
- **Tags, galleries, favorites** — label files with tags, assign a gallery, mark favorites, and use them in search and organize
- **Thumbnails** — generate JPEG thumbnails (512px default) cached by SHA256 and size
- **Activity log** — record `mv`/`cp`/`rm` in the database for an audit trail
- **Remove** — remove images by glob pattern: trash (default, recoverable), permanently delete, drop the DB row only, or drop rows for missing files
- **Match/exclude** — repeatable `--match` and `--exclude` globs across commands, matching against full paths, with exclude taking precedence

## Building

phig builds against stock system libraries — OpenCV (core modules only; no
`contrib` needed), SQLite3, libexif, libcurl, FFmpeg, spdlog, LibRaw,
libheif, and libjxl.
Install the dependencies with your package manager, then optionally install
[mise](https://mise.jdx.dev/) and run `mise install` from the repo root to
get the exact `cmake`, `clang-format`, and `clang-tidy` versions CI uses
(`mise.toml`) — matching `clang-format`'s version matters, since a
different version reformats code differently and would make the lint gate
disagree between your machine and CI. mise is optional: any `cmake` >= 3.20
works, and lint-tool version drift only shows up as noise in
`clang-format --dry-run` output, not a broken build.

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libopencv-dev libsqlite3-dev \
    libexif-dev libcurl4-openssl-dev libavformat-dev libavcodec-dev \
    libavutil-dev libswscale-dev libgtest-dev libspdlog-dev libraw-dev \
    libheif-dev libheif-plugin-libde265 libjxl-dev

# Debian/Ubuntu split libheif's codecs into plugins: libheif-dev alone decodes
# AVIF but not HEIC, because HEVC decoding lives in libheif-plugin-libde265.
# Installing libde265 by itself is not enough — libheif loads the plugin, not
# the codec library directly.

# Fedora
sudo dnf install gcc-c++ cmake make pkgconf-pkg-config opencv-devel sqlite-devel \
    libexif-devel libcurl-devel ffmpeg-free-devel gtest-devel spdlog-devel LibRaw-devel \
    libheif-devel libjxl-devel

# Arch Linux
sudo pacman -S base-devel cmake opencv sqlite libexif curl ffmpeg gtest spdlog libraw \
    libheif libjxl

# macOS (Homebrew)
brew install cmake pkg-config opencv sqlite libexif ffmpeg googletest spdlog libraw libheif jpeg-xl
```

Then build and install:

```bash
cmake -B build
cmake --build build
sudo cmake --install build                     # installs to /usr/local/bin/phig
# or
cmake --install build --prefix ~/.local        # installs to ~/.local/bin/phig
```

Notes:
- **OpenCV** — two different numbers, and one does not cover the other.
  - **≥ 4.5.4 to build.** `cv::FaceDetectorYN::create` and
    `cv::FaceRecognizerSF::create` are called unconditionally, so CMake fails
    below that version.
  - **≥ 4.8.0 to detect faces.** YuNet 2023mar, the model `phig models
    download` fetches, has an ONNX graph that older importers accept and then
    cannot run: `create()` succeeds and the first `detect()` throws
    `(-204:Requested object was not found) Layer with requested id=-1 not
    found`. Measured against that model file — 4.6.0 and 4.7.0 both throw,
    4.8.0 and 4.10.0 both run it clean.

  Between the two, phig builds and everything that does not detect a face
  works. What fails, with a message naming the version needed and the version
  installed: `scan --faces`, `face rebuild`, `face name`, and `search --face`.
  What still works: `face list`, `face identify`, and `search --person`, which
  read stored embeddings and labels and build no detector. Ubuntu 22.04 ships
  4.5.4 and 24.04 ships 4.6.0, so both are below the face floor; 25.10 and
  26.04 ship 4.10.0. Only core modules are used — no `opencv_contrib`.
- **FFmpeg on Fedora**: the official repos ship `ffmpeg-free-devel`, which is
  sufficient. The fully-featured `ffmpeg-devel` lives in the third-party RPM Fusion
  repo; you only need it if `ffmpeg-free-devel` is missing a codec you require.
- Check your build with `phig --version`.

## Configuration

### File locations

User-level files follow the XDG base directory convention on all platforms
(honoring `XDG_CONFIG_HOME`/`XDG_DATA_HOME`/`XDG_CACHE_HOME` when set). A
catalog is not split across them: it is one directory, by default
`$XDG_DATA_HOME/phig` (`~/.local/share/phig/` when that is unset).

| What | Default location |
|------|------------------|
| Catalog (database, lock, trash, thumbnails, diagnostic log) | `~/.local/share/phig/` |
| Face-recognition models | `~/.cache/phig/models/` |
| Config file | `~/.config/phig/config.ini` |

A catalog is one directory holding `phig.db`, `phig.db.lock`, `trash/`,
`thumbs/` and `phig.log`. Choose one with `--catalog <dir>` (a global flag),
the `PHIG_CATALOG` environment variable, or `[paths] catalog` in the config
file, in that order. For development, set `PHIG_CATALOG` in a `.envrc` to keep
a scratch catalog apart from your real one.

Earlier versions kept thumbnails in `~/.cache/phig/thumbs/` and the log in
`~/.local/state/phig/`. The first command run against an existing default
catalog moves both into it, once. A destination that already exists is left
alone, and so is the old copy.

The database is the source of truth for your collection; it always reflects the
current file locations so other tools can read it.

### Config file

If `~/.config/phig/config.ini` exists it is loaded automatically; point at a
different file with the global `--config <path>` flag. Values set defaults;
an explicit CLI flag always overrides the config, which overrides the built-in
default. Leading `~` in path values is expanded.

```ini
[paths]
catalog = ~/photos/phig-catalog
models_dir = ~/.cache/phig/models

[general]
parallel = 4

[scan]
recursive = true
on_error = warn        # warn | fail
media = all            # image | video | all
thumbs = false
faces = false

[organize]
format = %Y/%m/%original

[duplicates]
type = all             # exact | near | all
threshold = 5          # 0-64

[faces]
threshold = 0.6

[log]
enabled = true         # record mv/cp/rm in an activity log for audit/rollback
max_entries = 100000   # cap log size; 0 = unlimited, oldest operations pruned first
file = true            # write the diagnostic log to <catalog>/phig.log
file_level = info      # info | warn | error — minimum level written to the file
```

`enabled` and `max_entries` control the activity log (`mv`/`cp`/`rm` history
in the database); `file` and `file_level` control the diagnostic log — a
separate spdlog logger that writes warnings/errors to stderr and, unless
`file = false`, also to a rotating file at `<catalog>/phig.log`
(5 MB rotation to `phig.1.log`).

Destructive or per-invocation options are deliberately **not** configurable and
must be given on the command line: `--force`, `--dry-run`, `--on-conflict`,
`--ignore-mount-warning`, and the selection/query flags (`--match`, `--exclude`,
etc.). Unknown keys produce a warning; invalid values are a hard error.

`phig --help` prints an "Active configuration" summary showing the resolved
paths and any values your config overrides.

### Concurrency

Mutating commands (`scan`, `mv`, `cp`, `rm`, `tag`/`gallery`/`fav` writes,
`face` writes, `thumbs rebuild`/`clean`, `models download`) take a process-wide
advisory lock (the catalog's `phig.db.lock`) so two of them never run at once
and interleave. If another mutating command is already running, phig prints
`Another phig operation is running (pid N); waiting...` and waits. Pass
`--no-wait` to fail immediately instead. Read commands (`search`,
`duplicates`, `log`, the `list` subcommands) never take the lock and are never
blocked.

The lock is per-machine (POSIX `flock`) and released automatically if phig
crashes or is killed. It does not coordinate phig instances on different hosts
sharing one database over a network filesystem.

## libphig

phig's functionality is available to other programs through `libphig`, a C ABI
shared library installed alongside the CLI (`libphig.so` plus
`include/phig/phig.h`). It covers paginated queries, per-image metadata,
thumbnail path lookup, and tag/gallery/favorite writes.

Clients link it instead of reading phig's database directly: the SQLite schema
and the thumbnail cache layout are private and may change, while the header is
a versioned contract (`phig_abi_version()`).

Long-running operations — scan, organize, remove, face detection — are not yet
in the ABI. Run them through the CLI, which holds the writer lock and records
the activity log. The engines live in `core/`; giving them ABI surface, with
progress reporting and cancellation, is plan 031.

libphig's own metadata writes take the same writer lock as the CLI (see
Concurrency, above), scoped to the database the caller opened, so they cannot
interleave with a running CLI job. The call is non-blocking: a contended lock
returns `PHIG_ERR_DB` immediately rather than freezing the caller.

Pixel decoding is deliberately absent: phig decodes arbitrary user files with
OpenCV, FFmpeg, and LibRaw, which can crash on malformed input, so a client
that needs decoded images should run it out-of-process.

### Settings

`libphig` can read and write phig's config file, so a client uses the same
settings the CLI does instead of parsing `config.ini` itself:

```c
phig_settings *s = NULL;
phig_settings_load(NULL, &s);           /* phig's config.ini */

phig_config cfg = {0};
phig_settings_to_config(s, &cfg);       /* paths from the config file */
cfg.mode = PHIG_READ_ONLY;              /* per-context, never from config */

phig_ctx *ctx = NULL;
phig_init(&cfg, &ctx);
```

Settings are addressed as `"section.name"` strings rather than struct fields,
so a setting phig adds later is readable without recompiling. `PHIG_KEY_*`
macros cover the documented keys. Keys are matched the way the CLI matches
them — lowercased, with `-` mapped to `_` — so a config written as `[General]`
resolves to `general.parallel` and is rewritten in place rather than
duplicated.

`phig_settings_get` expands a leading `~`; `phig_settings_get_raw` returns the
value exactly as stored. A settings dialog wants the raw form, so displaying a
path and writing it back keeps the portable `~` instead of hard-coding one
machine's home directory.

Writes preserve the file: comments, ordering, and keys libphig does not model
survive a `phig_settings_save`. The save writes a temp sibling and renames, so
a crash cannot leave a truncated config. Loading is a separate call from
`phig_init`, so opening a collection does not touch the filesystem beyond the
database.

## Usage

### Scanning

Scan directories to build the image database:

```bash
phig scan ~/Photos --recursive
phig scan ~/Photos --recursive --force          # re-process everything
phig scan ~/Photos --recursive --faces          # also detect faces
phig scan ~/Photos --recursive --thumbs         # also generate thumbnails
phig scan ~/Photos --media image                # skip video files
phig scan ~/Photos --faces --parallel 2         # control parallelism
```

Scanning handles images and video. `--force` accepts `hash`, `thumbs`, `faces`,
or `all` (bare `--force` means all) to re-process only what you name.

If you move or rename files outside phig, a rescan re-links their metadata: when
a tracked file is gone but a byte-identical file exists at a new path, phig
transfers the old file's tags, favorites, gallery, and faces to it before
dropping the stale entry (reported as `Relinked: N`).

RAW files (CR2, NEF, ARW, etc.) are indexed directly. If a same-basename JPEG
sits beside a RAW, it isn't indexed separately — it's used as the RAW's pixel
source (via LibRaw, falling back to the embedded preview or a full decode) and
is carried alongside the RAW on `mv`/`cp`, the same way sidecars are.

### Finding Duplicates

```bash
phig duplicates                                 # all duplicates
phig duplicates --type exact                    # exact copies only
phig duplicates --type near --threshold 3       # very similar images
phig duplicates --match "*/Photos/*"            # only in Photos directory
phig duplicates --format json --output dupes.json
```

`--output` refuses a path that already exists unless you add `--overwrite`; the
same holds for `search` and `fav list`. `--count` with `--output` is an error.

### Searching

```bash
# By metadata
phig search --ext jpg
phig search --after 2023-01-01 --before 2024-01-01
phig search --camera "iPhone"
phig search --min-size 5MB
phig search --match "*/Vacation/*"

# By media type and video duration
phig search --media video --min-duration 30s --max-duration 5m

# By sharpness (blur detection)
phig search --blurry                            # blur_score <= 100
phig search --min-sharpness 500                 # only sharp images

# By tag, gallery, or favorite
phig search --tag landscape --tag edited        # must have all listed tags
phig search --gallery "Trip 2024"
phig search --fav

# Visual similarity (provide a reference image)
phig search --similar reference.jpg
phig search --similar reference.jpg --threshold 3
cat photo.jpg | phig search --similar -         # from stdin

# Count results
phig search --ext jpg --count

# Machine-readable output
phig search --porcelain                         # tab-separated
phig search --print0                            # null-separated (for xargs -0)
phig search --format json
phig search --format csv --output results.csv            # refuses an existing file
phig search --format csv --output results.csv --overwrite

# View results in an image viewer
phig search --match "*/Vacation/*" --print0 | xargs -0 eog
```

### Face Recognition

Download face detection models (one-time):

```bash
phig models download
```

Scan with face detection:

```bash
phig scan ~/Photos --recursive --faces
```

Name people and search:

```bash
# Name a person using a clear headshot (single face)
phig face name headshot.jpg "Brett"

# Auto-label all matching faces in the database
phig face identify "Brett"

# Search for images of a person
phig search --person "Brett"
phig search --person "Brett" --print0 | xargs -0 eog

# Search by face (provide any photo of the person)
phig search --face photo.jpg
cat photo.jpg | phig search --face -

# List known people
phig face list

# Re-detect faces (e.g., after model update)
phig face rebuild
phig face rebuild --match "*/2024/*"
```

New scans with `--faces` automatically label detected faces that match known people.

### Thumbnails

Thumbnails are JPEGs cached under `<catalog>/thumbs/<size>/`, keyed by
SHA256 and size, so identical files share one thumbnail and several sizes can
coexist. 512px is the default; `--size` writes another without disturbing it.

```bash
phig thumbs rebuild                             # generate missing thumbnails (512px)
phig thumbs rebuild --size 768                  # fill another size; leaves 512px alone
phig thumbs rebuild --match "*/2024/*"          # only for matching files
phig thumbs clean                               # remove orphaned thumbnails
```

You can also generate thumbnails during a scan with `phig scan --thumbs`.

### Tags, Galleries, and Favorites

Tag files with any number of labels, assign each file to at most one gallery, and
mark favorites. All three are searchable (`--tag`, `--gallery`, `--fav`) and
usable as organize format tokens (`%gallery`, `%fav`).

```bash
# Tags (any number per file)
phig tag add landscape --match "*/Nature/*"
phig tag remove landscape --match "*/City/*"
phig tag list                                   # tags and counts

# Galleries (one per file)
phig gallery set "Trip 2024" --match "*/June/*"
phig gallery clear --match "*/June/*"
phig gallery list

# Favorites (a built-in tag)
phig fav add --match "*/Best/*"
phig fav remove --match "*/Best/*"
phig fav list
```

### Organizing Files

Copy or move images into a structured folder layout:

```bash
# Preview what would happen
phig cp ~/Photos/Organized --dry-run
phig cp ~/Photos/Organized --format "%Y/%m/%original"
phig cp ~/Photos/Organized --format "%Y/%m/%d/%original"
phig cp ~/Photos/Organized --format "%Y/%m/%camera/%original"

# Move with match/exclude
phig mv ~/Photos/Organized --match "*.jpg" --exclude "*_copy*"

# Handle conflicts (default: skip)
phig cp ~/Photos/Organized --on-conflict skip     # skip if the path is taken
phig cp ~/Photos/Organized --on-conflict rename   # auto-rename around conflicts
```

Format tokens: `%Y` (year), `%m` (month), `%d` (day), `%camera` (EXIF model), `%make` (EXIF make), `%gallery` (assigned gallery), `%fav` (favorite state), `%original` (original filename).

`cp` and `mv` exit 1 when any file or attachment was not carried, and print
the number of attachments that were not.

`--on-conflict overwrite` will not destroy an existing file: if the destination
path is already taken on disk, it renames around the conflict rather than
overwriting.

`mv`, `cp`, and `rm` are recorded in an activity log in the database for an
audit trail. Disable it with `[log] enabled = false`; cap its size with
`[log] max_entries`.

`mv`/`cp` also carry a file's metadata sidecars (xmp, pp3, dop, on1, aae, json,
thm, lrv) along with it, renaming them to match; organize and search also fall
back to a sidecar's date when a file has no EXIF date, before falling back to
its modified time.

### Removing Files

```bash
phig rm --match "*/old-folder/*"            # trash (recoverable)
phig rm --match "*.tmp" --hard --yes        # permanent delete
phig rm --match "*/Trash/*" --db-only       # forget rows, keep files
phig rm --orphans                            # drop rows for missing files
phig trash empty --yes                       # empty the trash
```

### Match and Filter

All commands support `--match` and `--exclude` for glob-based path filtering:

```bash
# Match includes files (repeatable)
phig search --match "*/Vacation/*" --match "*/Beach/*"

# Filter excludes files (repeatable, wins over match)
phig search --match "*.jpg" --exclude "*_thumb*"

# Tilde expansion works
phig search --match "~/Photos/*"

# Globs match against the full path
phig duplicates --match "*/2024/*"
phig rm --match "*/old-backup/*"
```

## Running Tests

```bash
cmake --build build && ./build/phig-tests
```
