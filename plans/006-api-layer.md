---
name: API Layer
status: deferred
created: 2026-03-28
description: >
  HTTP/JSON API exposing scan, duplicates, organize, and query operations.
  Foundation for TUI/GUI clients.
---

# API Layer

## Goal

Expose phig's functionality as an HTTP/JSON API so that TUI, GUI, and other tools can interact with the image database programmatically.

## Inspiration

Inspired by [beets](https://beets.io/) — a CLI-first music manager with a plugin ecosystem and web UI built on top.

## Open Questions

- HTTP framework choice (C++ options: cpp-httplib, Crow, Drogon; or build API in a higher-level language wrapping the SQLite DB directly)
- REST vs simpler RPC-style endpoints
- Whether the API wraps the CLI or shares the core library
- Authentication / local-only binding
- Streaming progress for long-running operations (scan)
