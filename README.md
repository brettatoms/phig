# phig

An image management power tool. Scan directories of images, build a metadata database, detect duplicates, recognize faces, and organize files into a clean folder structure.

## Features

- **Scan** — recursively scan directories, computing SHA256 + perceptual hashes, extracting EXIF data, and storing everything in SQLite
- **Duplicates** — find exact copies (same SHA256) and visually similar images (perceptual hash)
- **Copy/Move** — copy or move images into a structured layout (e.g., `YYYY/MM/`) with configurable format strings, conflict resolution, and dry-run mode
- **Search** — query the database by filename, date, EXIF data, file size, visual similarity, or face matching
- **Face Recognition** — detect faces, compute embeddings, name people, and search by person
- **Purge** — remove entries from the database by glob pattern
- **Match/filter** — repeatable `--match` and `--filter` globs across all commands, matching against full paths, with filter taking precedence

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

Install dependencies with your system package manager:

```bash
# Arch Linux
sudo pacman -S opencv libexif sqlite cmake base-devel

# Debian / Ubuntu
sudo apt install libopencv-dev libopencv-contrib-dev libexif-dev libsqlite3-dev cmake g++

# macOS (Homebrew)
brew install opencv libexif sqlite cmake
```

Then build and install:

```bash
cmake -B build
cmake --build build
sudo cmake --install build                     # installs to /usr/local/bin/phig
# or
cmake --install build --prefix ~/.local        # installs to ~/.local/bin/phig
```

Note: OpenCV's `contrib` modules (specifically `img_hash`) and `objdetect` module are required. On some distributions this is a separate package (e.g., `libopencv-contrib-dev` on Debian/Ubuntu).

## Database

The database is stored at `~/.local/share/phig/phig.db` by default. Override with `--db <path>` on any command.

The database is designed to be the single source of truth for your image collection — reusable by other tools, a future API, or a GUI.

## Usage

### Scanning

Scan directories to build the image database:

```bash
phig scan ~/Photos --recursive
phig scan ~/Photos --recursive --force          # re-hash everything
phig scan ~/Photos --recursive --faces          # also detect faces
phig scan ~/Photos --faces --parallel 2         # control parallelism
```

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

# Handle conflicts
phig cp ~/Photos/Organized --on-conflict rename   # auto-rename duplicates
phig cp ~/Photos/Organized --on-conflict overwrite
```

Format tokens: `%Y` (year), `%m` (month), `%d` (day), `%camera` (EXIF model), `%make` (EXIF make), `%original` (original filename).

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
