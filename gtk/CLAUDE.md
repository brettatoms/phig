# gtk/

GTK4/libadwaita desktop client. M1 status: browse, search, annotate and scan,
and nothing that moves, deletes or overwrites (plan 071). See
`gtk/README.md` for M0's measured performance numbers.

Links `phig-core` directly, as `cli/` does, through one seam:
`src/collection.h`. Built from the repository root with
`cmake -B build -DPHIG_BUILD_GTK=ON`, then
`cmake --build build --target phig-gtk`.

The client opens phig's default catalog (`phig::gtk::default_catalog`), so
`PHIG_CATALOG` and the config file apply. It takes no arguments and offers no
way to open another catalog.

## Rules

- **Only `src/collection.cpp` includes a `core/` header.** Widget code calls
  `collection.h`, never `Database` or an engine. The seam keeps reads paged.
  No call returns the whole collection's rows, because a full read carries
  about 3.8 KB of EXIF per row that the client never reads; `ids` returns every
  match of a query as ids alone. A new capability is a new `collection.h`
  function. Every function in it is `noexcept`; one that can fail returns
  `std::expected` carrying phig's reason.
- **The tests link `tests/fake-collection.cpp`, not `phig-core`.** They test
  the client's own behaviour and need no collection. A new `collection.h`
  function gets a fake implementation too. `test-collection` is the exception:
  it tests `collection.cpp` against a database it builds. Logic the window
  needs lives in units with their own tests — `query`, `format`, `scan-watch`,
  `gallery-selection` and `background.h` — because widgets need a display and
  the tests run without one; widget code has no automated test. The grid's
  selection is `PhigGallerySelection`, a model phig owns that keeps the
  selection on positions: `GtkMultiSelection` drops a selected position when a
  page of the gallery model loads and replaces its placeholder objects, and
  emits no selection-changed.
- **A `cv::Mat` never leaves `collection.cpp`.** Decoded pixels become a
  `GdkTexture` or a file path before widget code sees them.
- **No exception unwinds into GTK.** GTK calls the client's handlers through C
  frames. Every function passed to GLib or GTK is `noexcept`, and every
  `collection.h` function catches. Connect signals with `phig::gtk::connect`
  (`src/connect.h`), which rejects a handler that is not `noexcept` at compile
  time. It is not named `signal.h`: `src/` is on the include path, and a
  `signal.h` there shadows the system header GLib includes.
- **Every destructive action goes through a `core/` plan the user reviews and
  then applies.** Anything that deletes, moves or overwrites is authorized by
  a plan `core/` computes, which the client shows and the user approves before
  it is applied (R66): never a confirmation dialog around a one-shot call, and
  never `cmd_rm`'s `ConfirmFn`. The client shows no destructive action until
  that plan exists — not even a disabled menu item. Removal, organize, scan's
  reconcile, face rebuild and thumbnail clean each wait for the plan that makes
  them safe (plan 071's table).
- **Writes name their images and never wait for the lock.** A metadata write
  takes an explicit id list, resolved from the gallery model's ids
  (`phig_gallery_model_ids_in`), never from a query re-run at write time. It
  takes the writer lock without waiting and, while a CLI job holds it, changes
  nothing and reports busy (R55, R78).
- **The index is scan's walk and nothing else.** `index_start` runs
  `ScanEngine::run_workers`, never `run_reconcile`, so it cannot delete the
  row of a file it did not find. It is the only path that creates a catalog;
  `open` never does (R70).
- **C++ throughout, against GTK's C API.** No C files. `gtk/` may use
  `std::expected` and `std::move_only_function`; see root `CLAUDE.md` for why
  the rest of the tree may not. Not gtkmm: its libadwaita support lags the
  widgets the client is built on. Prefer C++ types to GLib's where GTK does not
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
- **Background work never reads `PhigApp`.** The window can close, and `main`
  frees `PhigApp`, while a worker runs. Startup's worker gets its input by
  value (`open_in_background`, `src/startup.h`). Every later read and write
  goes through `run_in_background` (`src/background.h`), which holds the
  application until it delivers, so its work may borrow `coll`; its callback
  checks `app->window`. The index runs on its own thread, which closing the
  window cancels and joins. The loader's and the model's
  workers borrow `coll`, which `thumb_loader_shutdown` and the model's dispose
  (which waits for a fetch in progress) keep alive; `~PhigApp` relies on both
  before it closes `coll`. The application is held until startup finishes, and
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
- **Do not add orientation handling here.** `core/`'s decoders apply EXIF
  orientation, and both thumbnail generation and the viewer's `image` decode
  through them. Adding a second correction here would double-apply it.
- **Real-run check:** a full-range fast-drag scroll must produce 0 GTK
  warnings (e.g. no "Duplicate item detected"). This is the check that
  caught `PHIG_MAX_PLACEHOLDERS` needing a real value instead of a guess —
  see `gtk/README.md`. Closing the window while a scan runs must exit
  cleanly, and cancelling a scan must leave the catalog consistent and the
  window responsive.
