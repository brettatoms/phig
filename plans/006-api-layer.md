---
name: API Layer
status: superseded
created: 2026-03-28
description: >
  Superseded by 052 (phig Server — Protocol & Daemon), which builds the
  HTTP/JSON API this plan deferred. Every open question below is answered
  there: HTTP on libmicrohttpd rather than a higher-level language wrapping
  the DB, resource-oriented REST, a daemon linking `phig-core` as a peer of
  `cli/` rather than wrapping the CLI, bearer-token auth on a loopback bind
  behind a reverse proxy, and polled progress for long-running operations.
  Scope differs
  from what was sketched here: organize and remove are deliberately excluded,
  and media serving — thumbnails, originals, byte-range video — was not
  anticipated by this plan at all.
---

# API Layer

**Superseded by plan 052.** This sat deferred from 2026-03-28 while `libphig`
took over the client-interface role, and `docs/specs/2026-07-31-phig-gtk-design.md`
recorded the ABI as superseding it. That was true for local clients and never
covered remote ones. 052 builds the server properly, and the open questions
below are answered there rather than here.

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
