# gtk/

GTK4/libadwaita desktop client. M0 status: a working thumbnail grid, not a
finished app — see `gtk/README.md` for the measured performance numbers and
what M0's criterion still misses.

Links `phig-core` directly, as `cli/` does, through one seam:
`src/collection.h`. Built from the repository root with
`cmake -B build -DPHIG_BUILD_GTK=ON`, then
`cmake --build build --target phig-gtk`.

## Rules

- **Only `src/collection.cpp` includes a `core/` header.** Widget code calls
  `collection.h`, never `Database` or an engine. The seam keeps reads paged.
  No call returns the whole collection, because a full read carries about
  3.8 KB of EXIF per row that the client never reads. A new capability is a
  new `collection.h` function.
- **The tests link `tests/fake-collection.cpp`, not `phig-core`.** They test
  the client's own behaviour and need no collection. A new `collection.h`
  function gets a fake implementation too. `test-collection` is the exception:
  it tests `collection.cpp` against a database it builds.
- **A `cv::Mat` never leaves `collection.cpp`.** Decoded pixels become a
  `GdkTexture` or a file path before widget code sees them.
- **No exception unwinds into GTK.** GTK calls the client's handlers through C
  frames. Every function passed to GLib or GTK is `noexcept`, and every
  `collection.h` function catches.
- **Never authorize a removal with `cmd_rm`'s `ConfirmFn`.** A destructive
  operation is authorized by a plan the caller inspects and then applies
  (R66). The Remove screen waits for that plan/apply pair in `core/`.
- **C++ throughout, against GTK's C API.** No new C files. Three remain,
  `src/gallery-model.c`, `src/row-page.c` and `tests/test-gallery-model.c`, and
  plan 068 converts them. Not gtkmm: its libadwaita support lags the widgets
  the client is built on. Prefer C++ types to GLib's where GTK does not
  require GLib's: `std::string` over `char*`, standard containers over
  `GHashTable`.
- **GObjects only where GTK requires one.** `PhigImageRow` and
  `PhigGalleryModel` are GObjects because GTK's list machinery needs them.
  GObject instance memory is zeroed, not C++-constructed, so a member with a
  constructor is placement-new'd in `instance_init` and destroyed in
  `finalize`. Everything else is plain C++ structs and free functions, with
  `GRef` (`src/gref.h`) owning GObject references.
- **Widgets and rendering stay on the main thread.** `ThumbLoader`'s worker
  threads call `phig_collection_thumb_lookup`/`phig_collection_thumb_generate`
  and load the file into a new `GdkTexture`, then post results back via
  `g_idle_add`. Creating a texture is thread-safe, and so is dropping one that
  never reached GTK. A worker never touches a `GtkWidget`, or a texture
  already handed to GTK.
- **`ThumbLoader` and `max_dim`:** `max_dim` is the largest edge in **physical**
  pixels — cell size times `gtk_widget_get_scale_factor`. The worker decodes to
  that size and the texture cache is keyed `(sha256, max_dim)`, so the same image
  at two zoom levels is two entries rather than one stale one. Quantise the value
  before passing it (`PHIG_TEXTURE_SIZE_QUANTUM` in `main.cpp`); passing raw pixel
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
