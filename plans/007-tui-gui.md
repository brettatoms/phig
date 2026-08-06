---
name: TUI / GUI
status: superseded
created: 2026-03-28
description: >
  Superseded by 035 (GTK client M0) and 031 (libphig operations surface). This
  plan assumed a GUI built on plan 006's HTTP API, which was deferred; the
  client instead reads phig through libphig's C ABI. Its open questions were
  answered by `docs/specs/2026-07-31-phig-gtk-design.md`: GTK4/libadwaita, a
  native desktop app, thumbnails cached by SHA256 (plan 014). A TUI is no
  longer planned.
---

# TUI / GUI

**Superseded.** The GUI shipped as `gtk/` — see plan 035 for M0 and plan 031
for the ABI surface it needs to go further. The framework and transport
questions below were settled in `docs/specs/2026-07-31-phig-gtk-design.md`;
duplicate review remains unbuilt but belongs to the GTK client's roadmap, not
to this plan.

## Goal

Interactive interface for browsing the image database, reviewing duplicate groups with visual comparison, and triggering organize/cleanup operations.

## Open Questions

- TUI framework: FTXUI (C++), or build in a higher-level language against the API
- GUI framework: web-based (htmx/Alpine), Tauri, Flutter, or native
- Whether TUI and GUI are separate tools or one adaptive interface
- Thumbnail generation and caching for visual review
- Duplicate review workflow — side-by-side comparison, bulk accept/reject
