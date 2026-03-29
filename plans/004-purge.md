---
name: Purge
status: completed
created: 2026-03-28
description: >
  Remove database entries by glob match/filter and path prefix. Dry-run mode.
---

# Purge

## Goal

Remove entries from the database without touching files on disk. Useful for cleaning up the DB after manually deleting files, or removing entries for files you no longer want tracked.

## CLI

```
phig purge [flags]
  --match <glob>           Remove entries matching filename glob (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable, wins over --match)
  --path <directory>       Remove entries under this directory
  --dry-run                Show what would be removed
  --db <path>              Database path
```

## Checklist

- [x] Purge by glob match (repeatable)
- [x] Purge by path prefix
- [x] Combined match + path filtering
- [x] Filter exclusion (repeatable, wins over match)
- [x] Dry-run mode
- [x] Safety: requires at least `--match` or `--path`
