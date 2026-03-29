---
name: Track Copied Files
status: planned
created: 2026-03-28
description: >
  Add copied files to the database during organize --action copy so the DB
  tracks all known image locations.
---

# Track Copied Files

## Goal

When `phig organize --action copy` copies a file to a new location, add the destination as a new entry in the database. The DB should track every known image location, not just the originals.

## Motivation

The database is intended as the central source of truth for all known images — potentially reusable by a GUI or API. If copies aren't tracked, the DB has blind spots.

## Checklist

- [ ] On copy, insert new DB row for the destination file (same sha256/phash/exif, new path/filename)
- [ ] On re-run, skip files already in DB (existing path+mtime logic handles this)
- [ ] Verify duplicate detection still works correctly with both original and copy in DB
