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
- **`ThumbLoader` and `max_dim`:** `max_dim` is the largest edge in **physical**
  pixels — cell size times `gtk_widget_get_scale_factor`. The worker decodes to
  that size and the texture cache is keyed `(sha256, max_dim)`, so the same image
  at two zoom levels is two entries rather than one stale one. Quantise the value
  before passing it (`PHIG_TEXTURE_SIZE_QUANTUM` in `main.c`); passing raw pixel
  sizes grows a cache generation per distinct size.
- **Cell size is derived, never set:** the grid divides the viewport width by the
  column count, so it always fills the width exactly with no horizontal scroll.
  Zoom changes the column count; resizing changes the cell size. Don't add a
  fixed cell-size constant back.
- **Never resize cells synchronously from a size-allocation notification.**
  Resizing requests another layout pass, and during a window-open or drag
  animation the width changes every frame, so GTK warns "layout continuously
  requested, giving up after 4 tries". Coalesce through `phig_queue_relayout`.
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
