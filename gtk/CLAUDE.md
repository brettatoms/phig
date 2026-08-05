# gtk/

GTK4/libadwaita desktop client. M0 status: a working thumbnail grid, not a
finished app — see `gtk/README.md` for the measured performance numbers and
what M0's criterion still misses.

Talks to phig only through `libphig`'s C ABI. **May only include
`<phig/phig.h>` — never a `core/` header, never link `phig-core`, and never
let a `cv::Mat` reach GUI code** (it can't cross a process boundary, which
matters once decode moves out-of-process).

## Rules

- **GTK calls happen on the main thread only.** `ThumbLoader`'s worker
  threads call `phig_thumb_lookup`/`phig_thumb_generate` off the main
  thread, then post results back via `g_idle_add` — never touch a
  `GtkWidget`, `GdkTexture`, or any GTK API from a worker thread directly.
- **`ThumbLoader` and `max_dim`:** `thumb_loader_request` takes a `max_dim`
  the current backend ignores (the cache holds one size), kept in the
  signature so a later backend — in-process at a requested size, or a
  `phig-decode` subprocess — needs no call-site change. Don't remove it
  because it looks unused today.
- **Cancel on unbind:** `on_factory_unbind` cancels any outstanding
  `ThumbLoader` request for a recycled cell before clearing its picture, so
  a cell reused for a different row never flashes the previous row's
  thumbnail. Any new async-load path bound to a list item needs the same
  cancel-on-unbind discipline.
- **Do not add orientation handling here.** Orientation is applied at
  thumbnail generation time (in `core/`), not when the GTK client displays
  the result. Adding a second correction here would double-apply it.
- **Real-run check:** a full-range fast-drag scroll must produce 0 GTK
  warnings (e.g. no "Duplicate item detected"). This is the check that
  caught `PHIG_MAX_PLACEHOLDERS` needing a real value instead of a guess —
  see `gtk/README.md`.
