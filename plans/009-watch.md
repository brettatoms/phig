---
name: Watch Mode
status: planned
created: 2026-03-28
description: >
  Watch a directory for file changes and automatically scan new/modified/deleted
  images. Implemented as a flag on the scan command using efsw for cross-platform
  file system watching.
---

# Watch Mode

## Goal

Continuously watch a directory for changes and automatically update the database when images are added, modified, or deleted. Implemented as a `--watch` flag on the existing scan command.

## CLI

```
phig scan <directory> --watch [flags]
  --recursive         Watch subdirectories (default: non-recursive)
  --on-error warn|fail
  --db <path>
```

## Behavior

- On startup, performs a normal scan of the directory
- Then enters watch mode, listening for filesystem events
- **File created/modified:** Process the file (hash, phash, EXIF) and insert/update in DB
- **File deleted:** Remove from DB (subject to the same unmount safety guard — if ALL files disappear at once, warn instead of deleting)
- **File renamed/moved:** Update path in DB
- Ctrl-C exits cleanly

## Library

**efsw** (Entropia File System Watcher) — MIT licensed, C++, CMake native.

- Linux: inotify
- macOS: FSEvents / kqueue
- Windows: ReadDirectoryChangesW
- Fallback: stat()-based polling

## Debouncing

File writes often generate multiple events (create, modify, modify, close). Need to debounce:
- Batch events per file over a short window (e.g., 500ms)
- Only process once the file has settled

## Open Questions

- Should watch mode output a live log of changes, or be quiet by default?
- Should there be a `--debounce <ms>` flag?
- How to handle large batch additions (e.g., copying 1000 photos into a watched folder) — queue and process with the same thread pool as scan?
- Should we add efsw via CMake FetchContent, git submodule, or expect it in the Nix environment?
- Is the unmount safety guard needed in watch mode, or is it only relevant for full re-scans?

## Checklist

- [ ] Add efsw dependency to devenv.nix and CMakeLists.txt
- [ ] Implement `--watch` flag on scan command
- [ ] File created → process and insert
- [ ] File modified → reprocess and update
- [ ] File deleted → remove from DB
- [ ] File renamed → update path in DB
- [ ] Event debouncing
- [ ] Clean shutdown on Ctrl-C
- [ ] Tests for watch event handling logic
