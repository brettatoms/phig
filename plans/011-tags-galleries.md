---
name: Tags, Galleries & Favorites
status: planned
created: 2026-03-28
description: >
  Tag images with user-defined labels (N per image) and assign galleries
  (1 per image). Favorites are a protected built-in tag with convenience
  commands. All searchable and usable as filters/format tokens.
---

# Tags, Galleries & Favorites

## Goal

Allow users to annotate images with tags (many per image) and galleries (one per
image), both searchable and usable as filters across commands. Favorites are
implemented as a single protected built-in tag (`favorite`) with ergonomic
`phig fav` commands, rather than a separate mechanism — one annotation system, no
duplication. (Supersedes plan 016, which used a standalone boolean column.)

## Schema

```sql
CREATE TABLE tags (
    id       INTEGER PRIMARY KEY,
    name     TEXT NOT NULL UNIQUE
);

CREATE TABLE image_tags (
    image_id INTEGER NOT NULL REFERENCES images(id),
    tag_id   INTEGER NOT NULL REFERENCES tags(id),
    PRIMARY KEY (image_id, tag_id)
);

-- Favorites are just a built-in tag, seeded so it always exists.
INSERT OR IGNORE INTO tags(name) VALUES ('favorite');

-- Gallery is a simple column on the images table (0 or 1 per image)
ALTER TABLE images ADD COLUMN gallery TEXT;

CREATE INDEX idx_gallery ON images(gallery);
```

Tags use a junction table since images can have many tags. Galleries are a single
column since an image has at most one.

### Tag lifecycle

- Tags are created on first `tag add <name>` and **persist** — they are not
  garbage-collected when their last image association is removed. This keeps
  `tag list` stable and avoids surprising vocabulary changes.
- The `favorite` tag is **seeded at migration and protected**: it always exists
  and cannot be deleted from the vocabulary, so `--fav`/`%fav` always have a
  stable referent even when zero images are favorited. Removing `favorite` from an
  individual image (unfavoriting) is of course allowed — that is not "deleting the
  tag".
- This plan adds no `tag delete`/`tag rename` command; if one is added later, it
  must refuse to delete `favorite`.

## CLI

### Tags

```
phig tag add <tag> [flags]
  --match <glob>           Apply to files matching glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable)
  --path <directory>       Apply to files under this directory
  --db <path>

phig tag remove <tag> [flags]
  --match <glob>           Remove from files matching glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable)
  --path <directory>       Remove from files under this directory
  --db <path>

phig tag list [flags]
  --path <directory>       Only show tags for files under this directory
  --db <path>
```

Examples:
```bash
phig tag add vacation --match "IMG_20230815*"
phig tag add family --path ~/Photos/Family
phig tag remove vacation --match "IMG_20230815_001.jpg"
phig tag list                    # list all tags and their counts
```

`tag add`/`tag remove` report how many images were affected.

### Galleries

```
phig gallery set <gallery-name> [flags]
  --match <glob>           Apply to files matching glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable)
  --path <directory>       Apply to files under this directory
  --no-overwrite           Skip images that already have a gallery
  --db <path>

phig gallery clear [flags]
  --match <glob>           Clear from files matching glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable)
  --path <directory>       Clear from files under this directory
  --db <path>

phig gallery list [flags]
  --db <path>
```

Examples:
```bash
phig gallery set "Italy 2024" --path ~/Photos/Italy
phig gallery set "Sarah Birthday" --match "IMG_202308*" --no-overwrite
phig gallery clear --match "IMG_20230815_001.jpg"
phig gallery list                  # list all galleries and their counts
```

### Favorites (convenience layer over the `favorite` tag)

`phig fav` is ergonomic sugar; each command is exactly a tag operation on the
built-in `favorite` tag (so `phig fav add X` ≡ `phig tag add favorite X`).

```
phig fav add [flags]        # == tag add favorite
phig fav remove [flags]     # == tag remove favorite
phig fav list [flags]       # favorited images, with search-style output
  --match/--filter/--path   Selection flags (same as tag commands)
  --format text|csv|json    (fav list) output format
  --porcelain / --print0    (fav list) machine output
  --output <path>           (fav list) write to file
  --db <path>
```

Examples:
```bash
phig fav add --path ~/Photos/Best
phig fav remove --match "IMG_20230815_003.jpg"
phig fav list --print0 | xargs -0 open
```

`fav add`/`fav remove` report counts ("Favorited 12 images"). `fav list` lists
favorited images using the same output modes as `phig search`.

## Search Integration

Tags, galleries, and favorites are all searchable via the search command
(plan 008):

```bash
phig search --tag vacation
phig search --tag vacation --tag family     # images with both tags
phig search --gallery "Italy 2024"
phig search --fav                            # == --tag favorite
phig search --fav --camera "EOS R5"
```

`--fav` is an alias for `--tag favorite`.

## Organize Integration

```bash
phig cp ~/Organized --format "%gallery/%Y/%m/%original"
# → Italy 2024/2024/07/IMG_1234.jpg   (no gallery → "no-gallery")

phig cp ~/Organized --format "%fav/%Y/%m/%original"
# → favorites/2024/07/IMG_1234.jpg
# → unfavorited/2024/07/IMG_5678.jpg
```

- `%gallery` → the image's gallery, or `no-gallery`.
- `%fav` → `favorites` if the image has the `favorite` tag, else `unfavorited`.
- A general `%tag` token is intentionally omitted (images can have many tags).

## Duplicates Integration

```bash
phig duplicates --fav    # only duplicate groups where at least one is favorited
```

Helps find favorited photos that still have duplicates taking up space.

## Open Questions

- Should there be a `phig tag rename` command? (If so, `favorite` is not renamable.)
- Should tags/galleries appear in the porcelain output of search?
- Should `tag list` show the built-in `favorite` tag inline, or is that noise?
  (Leaning: show it — it's a normal tag.)

## Implementation

### Database
- Migration: `tags`, `image_tags` tables, `gallery` column, seed `favorite` tag.
- `set_tag(paths, tag, bool add)` bulk add/remove; auto-create tag row on add.
- `get_tag_counts()`, `get_gallery_counts()`, `set_gallery(...)`, `clear_gallery(...)`.
- Search criteria: `tags` (AND semantics), `gallery`; `--fav` maps to `tags=[favorite]`.
- Guard: reject deletion of the `favorite` tag if a tag-delete path is ever added.

### CLI (main.cpp)
- `phig tag add|remove|list`, `phig gallery set|clear|list`, `phig fav add|remove|list`.
- `fav` subcommands delegate to the tag code path with `tag = "favorite"`.
- `--tag` (repeatable), `--gallery`, `--fav` on search; `--fav` on duplicates.
- `%gallery`, `%fav` format tokens.

## Checklist

- [ ] Schema migration (tags, image_tags, gallery column, seed `favorite`)
- [ ] `phig tag add` / `tag remove` (report counts) / `tag list`
- [ ] `phig gallery set` (with `--no-overwrite`) / `gallery clear` / `gallery list`
- [ ] `phig fav add` / `fav remove` / `fav list` (sugar over `favorite` tag)
- [ ] `favorite` tag seeded + protected from deletion
- [ ] Search integration (`--tag`, `--gallery`, `--fav`)
- [ ] Duplicates `--fav`
- [ ] Organize format tokens `%gallery`, `%fav`
- [ ] Tests (tags CRUD, gallery, favorites-as-tag, protection, search/organize)

## Cross-References

- [008-search.md](008-search.md) — search command (`--tag`, `--gallery`, `--fav`)
- [003-organize.md](003-organize.md) — format tokens (`%gallery`, `%fav`)
- [016-favorites.md](016-favorites.md) — **superseded by this plan** (favorites are
  now a protected built-in tag, not a standalone boolean column)
