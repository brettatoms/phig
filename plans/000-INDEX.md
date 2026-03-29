# Plan Index

| # | Name | Status | File | Description |
|---|------|--------|------|-------------|
| 001 | Scan & Database | completed | [001-scan-database.md](001-scan-database.md) | Directory scanning, image decoding, phash + SHA256 hashing, EXIF extraction, SQLite storage with WAL mode, mtime-based skip, stale entry cleanup with unmount safety guard. |
| 002 | Duplicate Detection | completed | [002-duplicate-detection.md](002-duplicate-detection.md) | Find exact (SHA256) and near (phash hamming distance) duplicates. Text/CSV/JSON output. Path prefix filtering. Match/filter globs with full-DB comparison. |
| 003 | Copy & Move | completed | [003-organize.md](003-organize.md) | `phig cp` and `phig mv` — copy/move files into structured folder layout using format strings (%Y/%m/%d/%camera/%original). Glob match/filter. Conflict resolution (skip/overwrite/rename). Dry-run mode. DB path updates on move. |
| 004 | Purge | completed | [004-purge.md](004-purge.md) | Remove database entries by glob match/filter and path prefix. Dry-run mode. |
| 005 | Track Copied Files | planned | [005-track-copies.md](005-track-copies.md) | Add copied files to the database during organize --action copy so the DB tracks all known image locations. |
| 006 | API Layer | planned | [006-api-layer.md](006-api-layer.md) | HTTP/JSON API exposing scan, duplicates, organize, and query operations. Foundation for TUI/GUI clients. |
| 007 | TUI / GUI | planned | [007-tui-gui.md](007-tui-gui.md) | Interactive interface for browsing, reviewing duplicates, and organizing. Built on the API layer. |
| 008 | Search | completed | [008-search.md](008-search.md) | General-purpose query command with filename, date, EXIF, size, and visual similarity search. Human-readable and porcelain output modes. |
| 009 | Watch Mode | planned | [009-watch.md](009-watch.md) | Watch a directory for changes and auto-scan new/modified/deleted images. Flag on scan command using efsw for cross-platform file watching. |
| 010 | Face Recognition | completed | [010-face-recognition.md](010-face-recognition.md) | Detect faces via dlib, store 128D embeddings in SQLite with sqlite-vec for fast KNN search. Search by providing a reference photo of a person. |
| 011 | Tags & Events | planned | [011-tags-events.md](011-tags-events.md) | Tag images with user-defined labels (N per image) and assign events (1 per image). Searchable and usable as organize format tokens. |
| 012 | Blur Detection | planned | [012-blur-detection.md](012-blur-detection.md) | Compute sharpness score via Laplacian variance during scan. Store in DB. Search for blurry images to clean up. |
