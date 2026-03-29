# phig

An image management power tool. Scan directories of images, build a metadata database, detect duplicates, and organize files into a clean folder structure.

## Features

- **Scan** — recursively scan directories, computing SHA256 + perceptual hashes, extracting EXIF data, and storing everything in SQLite
- **Duplicates** — find exact copies (same SHA256) and visually similar images (perceptual hash)
- **Organize** — copy or move images into a structured layout (e.g., `YYYY/MM/`) with deduplication, configurable format strings, and dry-run mode
- **Purge** — remove entries from the database by filename glob or directory
- **Match/filter** — repeatable `--match` and `--filter` globs across all commands, with filter taking precedence

## Requirements

- [Nix](https://nixos.org/download/) with [devenv](https://devenv.sh/) installed
- [direnv](https://direnv.net/) (optional, for automatic environment activation)

## Getting Started

```bash
git clone <repo-url> phig
cd phig
direnv allow          # auto-activates the dev environment (one-time)

cmake -B build        # configure
cmake --build build   # compile
```

The binary is at `./build/phig`.

If you don't use direnv, activate the environment manually:

```bash
devenv shell
cmake -B build && cmake --build build
```

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

# Organize into YYYY/MM structure
phig organize ~/Photos/Organized --dry-run
phig organize ~/Photos/Organized --format "%Y/%m/%d/%original"
phig organize ~/Photos/Organized --match "*.jpg" --filter "*_copy*" --action move

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

TBD
