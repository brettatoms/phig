# phig

A command-line tool for managing image and video collections. Scan directories, build a metadata database, detect duplicates, recognize faces, and organize files into a folder structure.

## Features

- **Scan** — recursively scan directories, computing SHA256 + perceptual hashes, extracting EXIF/container metadata, and storing everything in SQLite. Handles images and video.
- **Duplicates** — find exact copies (same SHA256) and visually similar files (perceptual hash)
- **Copy/Move** — copy or move files into a structured layout (e.g., `YYYY/MM/`) with configurable format strings, conflict resolution, and dry-run mode
- **Search** — query the database by filename, date, EXIF data, file size, video duration, sharpness, tags, gallery, visual similarity, or face
- **Face recognition** — detect faces, compute embeddings, name people, and search by person
- **Tags, galleries, favorites** — label files with tags, assign a gallery, mark favorites, and use them in search and organize
- **Thumbnails** — generate 512px JPEG thumbnails cached by SHA256
- **Activity log** — record `mv`/`cp`/`purge` in the database for an audit trail
- **Purge** — remove entries from the database by glob pattern
- **Match/filter** — repeatable `--match` and `--filter` globs across commands, matching against full paths, with filter taking precedence

## Building

### With devenv (development)

Requires [Nix](https://nixos.org/download/) with [devenv](https://devenv.sh/) and optionally [direnv](https://direnv.net/).

```bash
cd phig
direnv allow          # auto-activates the dev environment (one-time)

cmake -B build
cmake --build build
```

The binary is at `./build/phig`. It links against Nix store libraries and is intended for local development use.

### With system packages (distribution)

phig builds against stock system libraries — OpenCV (core modules only; no
`contrib` needed), SQLite3, libexif, libcurl, FFmpeg, and spdlog. Install the
dependencies with your package manager:

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libopencv-dev libsqlite3-dev \
    libexif-dev libcurl4-openssl-dev libavformat-dev libavcodec-dev \
    libavutil-dev libswscale-dev libgtest-dev libspdlog-dev

# Fedora
sudo dnf install gcc-c++ cmake make pkgconf-pkg-config opencv-devel sqlite-devel \
    libexif-devel libcurl-devel ffmpeg-free-devel gtest-devel spdlog-devel

# Arch Linux
sudo pacman -S base-devel cmake opencv sqlite libexif curl ffmpeg gtest spdlog

# macOS (Homebrew)
brew install cmake pkg-config opencv sqlite libexif ffmpeg googletest spdlog
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
- **OpenCV** ≥ 4.5 for core features; face recognition (the YuNet/SFace models)
  needs ≥ 4.7. Only core modules are used — no `opencv_contrib`.
- **FFmpeg on Fedora**: the official repos ship `ffmpeg-free-devel`, which is
  sufficient. The fully-featured `ffmpeg-devel` lives in the third-party RPM Fusion
  repo; you only need it if `ffmpeg-free-devel` is missing a codec you require.
- Check your build with `phig --version`.

## Configuration

### File locations

phig follows the XDG base directory convention on all platforms (honoring
`XDG_CONFIG_HOME`/`XDG_DATA_HOME`/`XDG_CACHE_HOME`/`XDG_STATE_HOME` when set):

| What | Default location |
|------|------------------|
| Database | `~/.local/share/phig/phig.db` |
| Face-recognition models | `~/.cache/phig/models/` |
| Thumbnails | `~/.cache/phig/thumbs/` |
| Diagnostic log | `~/.local/state/phig/phig.log` |
| Config file | `~/.config/phig/config.ini` |

The database is the source of truth for your collection; it always reflects the
current file locations so other tools can read it.

### Config file

If `~/.config/phig/config.ini` exists it is loaded automatically; point at a
different file with the global `--config <path>` flag. Values set defaults;
an explicit CLI flag always overrides the config, which overrides the built-in
default. Leading `~` in path values is expanded.

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
enabled = true         # record mv/cp/purge in an activity log for audit/rollback
max_entries = 100000   # cap log size; 0 = unlimited, oldest operations pruned first
file = true            # write the diagnostic log to ~/.local/state/phig/phig.log
file_level = info      # info | warn | error — minimum level written to the file
```

`enabled` and `max_entries` control the activity log (`mv`/`cp`/`purge` history
in the database); `file` and `file_level` control the diagnostic log — a
separate spdlog logger that writes warnings/errors to stderr and, unless
`file = false`, also to a rotating file at `~/.local/state/phig/phig.log`
(5 MB rotation to `phig.1.log`).

Destructive or per-invocation options are deliberately **not** configurable and
must be given on the command line: `--force`, `--dry-run`, `--on-conflict`,
`--ignore-mount-warning`, and the selection/query flags (`--match`, `--filter`,
etc.). Unknown keys produce a warning; invalid values are a hard error.

`phig --help` prints an "Active configuration" summary showing the resolved
paths and any values your config overrides.

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

### Finding Duplicates

```bash
phig duplicates                                 # all duplicates
phig duplicates --type exact                    # exact copies only
phig duplicates --type near --threshold 3       # very similar images
phig duplicates --match "*/Photos/*"            # only in Photos directory
phig duplicates --format json --output dupes.json
```

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
phig search --format csv --output results.csv

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

Thumbnails are 512px JPEGs cached under `~/.cache/phig/thumbs/`, keyed by SHA256
so identical files share one thumbnail.

```bash
phig thumbs rebuild                             # generate missing thumbnails
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

# Move with filters
phig mv ~/Photos/Organized --match "*.jpg" --filter "*_copy*"

# Handle conflicts (default: skip)
phig cp ~/Photos/Organized --on-conflict skip     # skip if the path is taken
phig cp ~/Photos/Organized --on-conflict rename   # auto-rename around conflicts
```

Format tokens: `%Y` (year), `%m` (month), `%d` (day), `%camera` (EXIF model), `%make` (EXIF make), `%gallery` (assigned gallery), `%fav` (favorite state), `%original` (original filename).

`--on-conflict overwrite` will not destroy an existing file: if the destination
path is already taken on disk, it renames around the conflict rather than
overwriting.

`mv`, `cp`, and `purge` are recorded in an activity log in the database for an
audit trail. Disable it with `[log] enabled = false`; cap its size with
`[log] max_entries`.

`mv`/`cp` also carry a file's metadata sidecars (xmp, pp3, dop, on1, aae, json,
thm, lrv) along with it, renaming them to match; organize and search also fall
back to a sidecar's date when a file has no EXIF date, before falling back to
its modified time.

### Purging Database Entries

```bash
phig purge --match "*/old-folder/*" --dry-run   # preview
phig purge --match "*.tmp"                       # remove by pattern
phig purge --match "*/Trash/*"                   # remove by directory
```

### Match and Filter

All commands support `--match` and `--filter` for glob-based path filtering:

```bash
# Match includes files (repeatable)
phig search --match "*/Vacation/*" --match "*/Beach/*"

# Filter excludes files (repeatable, wins over match)
phig search --match "*.jpg" --filter "*_thumb*"

# Tilde expansion works
phig search --match "~/Photos/*"

# Globs match against the full path
phig duplicates --match "*/2024/*"
phig purge --match "*/old-backup/*"
```

## Running Tests

```bash
cmake --build build && ./build/phig-tests
```

## License

[MIT](LICENSE)
