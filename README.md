# phig

phig manages image and video collections. It scans directories into a catalog
of metadata, hashes and thumbnails, finds duplicates, recognizes faces, and
copies or moves files into a dated folder layout.

The engine is a C++ library with three front ends:

- `phig`, the command-line tool. The [Usage](#usage) section covers it.
- `phig-gtk`, a GTK4 and libadwaita desktop client for Linux. It is at an
  early stage: it shows a catalog as a thumbnail grid. See
  [gtk/README.md](gtk/README.md).
- libphig, a C library for programs that cannot link the C++ engine. See
  [libphig](#libphig).

Native macOS and Windows clients are planned. They will read phig through
libphig.

## Features

- **Scan:** Scan directories recursively, compute SHA256 and perceptual hashes,
  extract EXIF or container metadata, and store it all in SQLite. Images and
  video are both supported.
- **Duplicates:** Find exact copies by SHA256 and visually similar files by
  perceptual hash.
- **Copy and move:** Copy or move files into a layout such as `YYYY/MM/`, with
  format strings, conflict handling, and a dry-run mode.
- **Search:** Query by filename, date, EXIF data, file size, video duration,
  sharpness, tags, gallery, visual similarity, or face.
- **Face recognition:** Detect faces, compute embeddings, name people, and
  search by person.
- **Tags, galleries, and favorites:** Label files with tags, assign each a
  gallery, mark favorites, and use all three in search and organize.
- **Thumbnails:** Generate JPEG thumbnails, 512px by default, cached by SHA256
  and size.
- **Activity log:** Record `mv`, `cp`, and `rm` in the database as an audit
  trail.
- **Remove:** Remove files by glob pattern. You can move them to the trash
  (the default, and recoverable), delete them permanently, drop only the
  database row, or drop the rows for missing files.
- **Match and exclude:** Use repeatable `--match` and `--exclude` globs across
  commands. Globs match full paths, and an exclude wins over a match.

## Building

phig builds against the system packages of OpenCV (core modules only, no
`opencv_contrib`), libjpeg-turbo, SQLite3, libexif, libcurl, FFmpeg, spdlog,
LibRaw, libheif, and libjxl. Install them with your package manager.

[mise](https://mise.jdx.dev/) is optional. Run `mise install` from the
repository root to get the `cmake`, `clang-format`, and `clang-tidy` versions
that CI uses (`mise.toml`). The `clang-format` version matters for
contributors, because another version formats code differently and the lint
check then disagrees with CI. Without mise, any `cmake` 3.20 or later builds
phig.

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libopencv-dev libjpeg-dev \
    libsqlite3-dev libexif-dev libcurl4-openssl-dev libavformat-dev libavcodec-dev \
    libavutil-dev libswscale-dev libgtest-dev libspdlog-dev libraw-dev \
    libheif-dev libheif-plugin-libde265 libjxl-dev

# Debian/Ubuntu split libheif's codecs into plugins: libheif-dev alone decodes
# AVIF but not HEIC, because HEVC decoding lives in libheif-plugin-libde265.
# Installing libde265 by itself is not enough, because libheif loads the
# plugin, not the codec library.

# Fedora
sudo dnf install gcc-c++ cmake make pkgconf-pkg-config opencv-devel libjpeg-turbo-devel \
    sqlite-devel libexif-devel libcurl-devel ffmpeg-free-devel gtest-devel spdlog-devel \
    LibRaw-devel libheif-devel libjxl-devel

# Arch Linux
sudo pacman -S base-devel cmake opencv libjpeg-turbo sqlite libexif curl ffmpeg gtest spdlog \
    libraw libheif libjxl

# macOS (Homebrew)
brew install cmake pkg-config opencv jpeg-turbo sqlite libexif ffmpeg googletest spdlog libraw libheif jpeg-xl
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
- **OpenCV:** phig has two minimum versions.
  - **4.5.4 to build.** phig calls `cv::FaceDetectorYN::create` and
    `cv::FaceRecognizerSF::create` unconditionally, so CMake fails below that
    version.
  - **4.8.0 to detect faces.** YuNet 2023mar, the model that `phig models
    download` fetches, has an ONNX graph that older importers accept but cannot
    run. `create()` succeeds and the first `detect()` throws
    `(-204:Requested object was not found) Layer with requested id=-1 not
    found`. Measured against that model file, 4.6.0 and 4.7.0 both throw, and
    4.8.0 and 4.10.0 both run it.

  Between the two versions, phig builds and everything that does not detect a
  face works. `scan --faces`, `face rebuild`, `face name`, and `search --face`
  fail with a message that names the version needed and the version installed.
  `face list`, `face identify`, and `search --person` still work, because they
  read stored embeddings and labels and build no detector. Ubuntu 22.04 ships
  4.5.4 and 24.04 ships 4.6.0, which are both below 4.8.0. Ubuntu 25.10 and
  26.04 ship 4.10.0.
- **FFmpeg on Fedora:** the official repositories ship `ffmpeg-free-devel`,
  which is enough. The full `ffmpeg-devel` is in the third-party RPM Fusion
  repository. You need it only if `ffmpeg-free-devel` lacks a codec that you
  use.
- Check your build with `phig --version`.

## Configuration

### File locations

User-level files follow the XDG base directory convention on all platforms,
and honor `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, and `XDG_CACHE_HOME` when they
are set. A catalog is one directory, not split across them. By default it is
`$XDG_DATA_HOME/phig`, or `~/.local/share/phig/` when that variable is unset.

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

The database is the record of your collection and always reflects current
file locations.

### Config file

If `~/.config/phig/config.ini` exists, phig loads it. To use a different
file, pass the global `--config <path>` flag. Config values set defaults. A
command-line flag overrides the config, and the config overrides the built-in
default. A leading `~` in a path value is expanded.

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
file_level = info      # minimum level written to the file: info | warn | error
```

`enabled` and `max_entries` control the activity log, the history of `mv`,
`cp`, and `rm` in the database. `file` and `file_level` control the diagnostic
log. That is a separate spdlog logger that writes warnings and errors to
stderr. Unless `file = false` is set, it also writes to a rotating file at
`<catalog>/phig.log`, which rotates to `phig.1.log` at 5 MB.

Destructive and per-invocation options cannot be set in the config. Give
them on the command line: `--force`, `--dry-run`, `--on-conflict`,
`--ignore-mount-warning`, and the selection and query flags such as `--match`
and `--exclude`. An unknown key produces a warning, and an invalid value is an
error.

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

Settings are addressed as `"section.name"` strings, not struct fields, so a
setting that phig adds later is readable without recompiling. `PHIG_KEY_*`
macros cover the documented keys. Keys are matched the way the CLI matches
them: lowercased, with `-` mapped to `_`. A config written as `[General]`
therefore resolves to `general.parallel`, and a save rewrites it in place
instead of adding a duplicate.

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

RAW files such as CR2, NEF, and ARW are indexed directly. A JPEG with the same
basename beside a RAW file is not indexed separately. It becomes the RAW
file's pixel source, and `mv` and `cp` carry it with the RAW file the way they
carry sidecars. Without that JPEG, phig uses the RAW file's embedded preview,
then a full decode through LibRaw.

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

# Re-detect faces, for example after a model update
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

`mv` and `cp` also carry a file's metadata sidecars (xmp, pp3, dop, on1, aae,
json, thm, and lrv) and rename them to match. When a file has no EXIF date,
organize and search use a sidecar's date, then the file's modified time.

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
