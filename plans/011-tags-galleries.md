---
name: Tags & Galleries
status: planned
created: 2026-03-28
description: >
  Tag images with user-defined labels (N per image) and assign galleries
  (1 per image). Searchable via the search command.
---

# Tags & Galleries

## Goal

Allow users to annotate images with tags (many per image) and galleries (one per image). Both are searchable and usable as filters across commands.

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

-- Gallery is a simple column on the images table (0 or 1 per image)
ALTER TABLE images ADD COLUMN gallery TEXT;

CREATE INDEX idx_gallery ON images(gallery);
```

Tags use a junction table since images can have many tags. Galleries are a single column since an image has at most one.

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

## Search Integration

Both tags and galleries should be searchable via the search command (plan 008):

```bash
phig search --tag vacation
phig search --tag vacation --tag family     # images with both tags
phig search --gallery "Italy 2024"
phig search --tag family --after 2023-01-01
```

## Organize Integration

Galleries could be used as a format token in organize:

```bash
phig organize ~/Organized --format "%gallery/%Y/%m/%original"
# → Italy 2024/2024/07/IMG_1234.jpg
```

Images without a gallery would use `unsorted` or `no-gallery` as the folder name.

## Open Questions

- Should `phig tag add` report how many images were tagged?
- Should there be a `phig tag rename` command?
- Should `phig gallery set` report how many images were updated vs skipped (with `--no-overwrite`)?
- Should tags/galleries be included in the porcelain output of search?
- Should `%tag` be a format token for organize? (Tricky since images can have multiple tags)

## Checklist

- [ ] Schema migration (tags table, image_tags table, gallery column)
- [ ] `phig tag add` command
- [ ] `phig tag remove` command
- [ ] `phig tag list` command
- [ ] `phig gallery set` command (with `--no-overwrite`)
- [ ] `phig gallery clear` command
- [ ] `phig gallery list` command
- [ ] Search integration (`--tag`, `--gallery`)
- [ ] Organize format token `%gallery`
- [ ] Tests
