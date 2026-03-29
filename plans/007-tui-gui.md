---
name: TUI / GUI
status: planned
created: 2026-03-28
description: >
  Interactive interface for browsing, reviewing duplicates, and organizing.
  Built on the API layer.
---

# TUI / GUI

## Goal

Interactive interface for browsing the image database, reviewing duplicate groups with visual comparison, and triggering organize/cleanup operations.

## Open Questions

- TUI framework: FTXUI (C++), or build in a higher-level language against the API
- GUI framework: web-based (htmx/Alpine), Tauri, Flutter, or native
- Whether TUI and GUI are separate tools or one adaptive interface
- Thumbnail generation and caching for visual review
- Duplicate review workflow — side-by-side comparison, bulk accept/reject
