# gtk/

GTK4/libadwaita desktop client. M0 status: a working thumbnail grid, not a
finished app — see `gtk/README.md` for the measured performance numbers and
what M0's criterion still misses.

Links `phig-core` directly, as `cli/` does, through one seam:
`src/collection.h`. Built from the repository root with
`cmake -B build -DPHIG_BUILD_GTK=ON`, then
`cmake --build build --target phig-gtk`.

The client's argument is a catalog directory. Without one it uses
`phig::gtk::default_catalog`, so `PHIG_CATALOG` and the config file apply.

## Rules

- **Only `src/collection.cpp` includes a `core/` header.** Widget code calls
  `collection.h`, never `Database` or an engine. The seam keeps reads paged.
  No call returns the whole collection, because a full read carries about
  3.8 KB of EXIF per row that the client never reads. A new capability is a
  new `collection.h` function. Every function in it is `noexcept`; one that can
  fail returns `std::expected` carrying phig's reason.
- **The tests link `tests/fake-collection.cpp`, not `phig-core`.** They test
  the client's own behaviour and need no collection. A new `collection.h`
  function gets a fake implementation too. `test-collection` is the exception:
  it tests `collection.cpp` against a database it builds.
- **A `cv::Mat` never leaves `collection.cpp`.** Decoded pixels become a
  `GdkTexture` or a file path before widget code sees them.
- **No exception unwinds into GTK.** GTK calls the client's handlers through C
  frames. Every function passed to GLib or GTK is `noexcept`, and every
  `collection.h` function catches. Connect signals with `phig::gtk::connect`
  (`src/connect.h`), which rejects a handler that is not `noexcept` at compile
  time. It is not named `signal.h`: `src/` is on the include path, and a
  `signal.h` there shadows the system header GLib includes.
- **Never authorize a removal with `cmd_rm`'s `ConfirmFn`.** A destructive
  operation is authorized by a plan the caller inspects and then applies
  (R66). The Remove screen waits for that plan/apply pair in `core/`.
- **C++ throughout, against GTK's C API.** No C
  files. `gtk/` may use `std::expected` and `std::move_only_function`; see
  root `CLAUDE.md` for why the rest of the tree may not. Not gtkmm: its libadwaita support lags the widgets
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
  threads call `phig::gtk::thumb_lookup`/`phig::gtk::thumb_generate`
  and load the file into a new `GdkTexture`, then post results back via
  `g_idle_add`. Creating a texture is thread-safe, and so is dropping one that
  never reached GTK. A worker never touches a `GtkWidget`, or a texture
  already handed to GTK. A `ThumbCallback` is destroyed exactly once, on the
  main thread, whether or not it ran, so it may hold the last reference to a
  GTK object. Capture what the result needs in it, such as a `GRef` to the list
  item.
- **Background work owns its inputs.** A worker thread reads only what it was
  handed by value, never `PhigApp`: the window can close, and `main` free
  `PhigApp`, while it runs. `open_in_background` (`src/startup.h`) is the
  pattern. The application is held until startup finishes, and
  `startup_done` checks `app->window` before touching any widget.
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
