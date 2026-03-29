# phig

An image management power tool. Scan directories of images, build a metadata database, detect duplicates, and organize files into a clean folder structure.

## Features

- **Scan** — recursively scan directories, computing SHA256 + perceptual hashes, extracting EXIF data, and storing everything in SQLite
- **Duplicates** — find exact copies (same SHA256) and visually similar images (perceptual hash)
- **Copy/Move** — copy or move images into a structured layout (e.g., `YYYY/MM/`) with configurable format strings, conflict resolution, and dry-run mode
- **Purge** — remove entries from the database by filename glob or directory
- **Match/filter** — repeatable `--match` and `--filter` globs across all commands, with filter taking precedence

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

Note: OpenCV's `contrib` modules (specifically `img_hash`) are required. On some distributions this is a separate package (e.g., `libopencv-contrib-dev` on Debian/Ubuntu).

## Usage

```bash
# Scan a directory
phig scan ~/Photos --recursive

# Scan with force re-hash
phig scan ~/Photos --recursive --force

# Find duplicates
phig duplicates
phig duplicates --type exact
phig duplicates --match "vacation*" --format json

# Copy into YYYY/MM structure
phig cp ~/Photos/Organized --dry-run
phig cp ~/Photos/Organized --format "%Y/%m/%d/%original"

# Move with filters
phig mv ~/Photos/Organized --match "*.jpg" --filter "*_copy*"

# Remove entries from the database
phig purge --match "*.tmp" --dry-run
phig purge --path ~/Photos/old-folder
```

## Database

The database is stored at `~/.local/share/phig/phig.db` by default. Override with `--db <path>` on any command.

The database is designed to be the single source of truth for your image collection — reusable by other tools, a future API, or a GUI.

## Running Tests

```bash
cmake --build build && ./build/phig-tests
```

## License

[MIT](LICENSE)
