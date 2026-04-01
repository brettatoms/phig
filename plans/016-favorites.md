---
name: Favorites
status: planned
created: 2026-04-01
description: >
  Mark images as favorites. Simple boolean flag per image with CLI commands
  to add/remove/list. Searchable and usable as an organize format token.
---

# Favorites

## Goal

Let users mark images as favorites — a quick, lightweight way to flag the best
shots without the overhead of a full tagging system. Favorites are searchable,
filterable across commands, and usable in organize format strings.

## Schema

```sql
ALTER TABLE images ADD COLUMN favorite INTEGER NOT NULL DEFAULT 0;
CREATE INDEX idx_favorite ON images(favorite);
```

Simple boolean column (`0`/`1`). Migration sets all existing rows to `0`.

## CLI

```
phig fav add [flags]
  --match <glob>           Favorite files matching glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable)
  --path <directory>       Favorite files under this directory
  --db <path>

phig fav remove [flags]
  --match <glob>           Unfavorite files matching glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable)
  --path <directory>       Unfavorite files under this directory
  --db <path>

phig fav list [flags]
  --path <directory>       Only show favorites under this directory
  --match <glob>           Include filter (repeatable)
  --filter <glob>          Exclude filter (repeatable)
  --format text|csv|json   Output format (default: text)
  --porcelain              Machine-parseable tab-separated output
  --print0                 Null-separated paths
  --output <path>          Write to file
  --db <path>
```

### Examples

```bash
# Favorite specific files
phig fav add --match "IMG_20230815_001.jpg"

# Favorite everything in a directory
phig fav add --path ~/Photos/Best

# Unfavorite some files
phig fav remove --match "IMG_20230815_003.jpg"

# List all favorites
phig fav list

# List favorites as null-separated paths (pipe to other tools)
phig fav list --print0 | xargs -0 open
```

### Output

All commands report how many images were affected:

```
Favorited 12 images
```

```
Unfavorited 3 images
```

`phig fav list` uses the same output modes as `phig search` (text/csv/json,
porcelain, print0).

## Search Integration

Add `--fav` flag to the search command:

```bash
phig search --fav                          # all favorites
phig search --fav --after 2024-01-01       # favorites from 2024+
phig search --fav --camera "EOS R5"        # favorites from a specific camera
```

Internally, `--fav` adds `WHERE favorite = 1` to the search query.

## Organize Integration

Add `%fav` format token that resolves to `favorites` or `unfavorited`:

```bash
phig cp ~/Organized --format "%fav/%Y/%m/%original"
# → favorites/2024/07/IMG_1234.jpg
# → unfavorited/2024/07/IMG_5678.jpg
```

## Duplicates Integration

Add `--fav` flag to duplicates to only show groups containing at least one
favorite:

```bash
phig duplicates --fav    # duplicate groups where at least one is favorited
```

This helps find cases where you've favorited a photo but have duplicates
taking up space.

## Implementation

### Database

- Schema migration: add `favorite` column
- `Database::set_favorite(paths, bool)` — bulk update
- `Database::get_favorites(path_prefix)` — query favorites
- Add `favorite` field to `SearchCriteria`
- Include `favorite` in `ImageInfo` struct and read/write paths

### CLI (main.cpp)

- Parse `phig fav add|remove|list` subcommands
- Reuse existing match/filter/path flag infrastructure
- `fav list` shares output formatting with search

### Changes to Existing Code

- `types.h`: add `bool favorite = false` to `ImageInfo`
- `database.cpp`: read/write `favorite` column in insert/query paths
- `main.cpp`: add `--fav` to search and duplicates argument parsing
- Format string handling: add `%fav` token

## Checklist

- [ ] Schema migration (add `favorite` column + index)
- [ ] `ImageInfo.favorite` field
- [ ] Database read/write for favorite column
- [ ] `Database::set_favorite()` bulk update method
- [ ] `phig fav add` command
- [ ] `phig fav remove` command
- [ ] `phig fav list` command (with all output modes)
- [ ] `--fav` flag on `phig search`
- [ ] `--fav` flag on `phig duplicates`
- [ ] `%fav` organize format token
- [ ] Tests

## Cross-References

- [008-search.md](008-search.md) — search command, `--fav` integration
- [003-organize.md](003-organize.md) — format tokens, `%fav`
- [011-tags-events.md](011-tags-events.md) — tags are the heavier-weight annotation system
