---
name: Duplicate Detection
status: completed
created: 2026-03-28
description: >
  Find exact (SHA256) and near (phash hamming distance) duplicates.
  Text/CSV/JSON output. Path prefix filtering. Match/filter globs with
  full-DB comparison.
---

# Duplicate Detection

## Goal

Query the database for duplicate and similar images. Three categories:

| Type | Detection | Example |
|---|---|---|
| Exact duplicates | Same SHA256 | Same file copied twice |
| Near duplicates | Same phash, different SHA256 | Same photo re-saved, resized, re-compressed |
| Similar images | Close phash (small hamming distance) | Burst shots, slight crops |

## CLI

```
phig duplicates [flags]
  --type exact|near|all    Duplicate type to find (default: all)
  --threshold [0-64]       Hamming distance for near duplicates (default: 5)
  --format text|csv|json   Output format (default: text)
  --output <path>          Write to file instead of stdout
  --match <glob>           Show groups containing matching files (repeatable)
  --filter <glob>          Exclude files matching glob (repeatable, wins over --match)
  --path <directory>       Only check files under this directory
  --db <path>              Database path
```

## Checklist

- [x] Exact duplicate detection (GROUP BY sha256)
- [x] Near duplicate detection (O(n²) phash hamming distance with union-find grouping)
- [x] Text output format
- [x] CSV output format
- [x] JSON output format
- [x] `--output` file redirect
- [x] `--path` prefix filtering
- [x] `--match` / `--filter` glob support (repeatable, filter wins)
- [x] Full-DB comparison with match/filter applied to results (not inputs)

## Design Decisions

- **Full-DB comparison:** `--match` filters which groups to *show*, not which files to *compare*. This means "do any of my vacation photos have duplicates anywhere?" works correctly.
- **Union-find for near duplicates:** Groups transitively similar images. If A~B and B~C, all three end up in the same group.
- **O(n²) comparison:** Fine for tens of thousands of images. Would need optimization (VP-tree, BK-tree) for millions.
