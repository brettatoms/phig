---
name: GTK Hardening
status: designing
created: 2026-11-08
description: >
  Turn `gtk/src/collection.h` into a C++ interface now that every caller can be
  C++, fix the startup race that frees app state under the startup worker, stop
  `core/` redirecting the host process's stderr, and close the loader leak and
  minor items 061 left open. Lands before GTK M1, which adds many seam
  functions.
---

# 068 — GTK Hardening

## Problem

061 made gtk a peer of libphig over `core/` and left five things behind. Each
was found during 061's execution or final review.

1. **The seam is C for no remaining reason.** `collection.h` is a C header:
   `gboolean` results, `char**` errors, a `PhigRowPage` the caller must free,
   and C-string fields. It is C only because `gallery-model.c`, `row-page.c`
   and `tests/test-gallery-model.c` are C. 061 kept `gallery-model.c` in C to
   limit scope. Its stated reason, that GObject zeroes instance memory, does
   not rule out C++: a GObject type can be defined in a `.cpp` file, and a
   member with a constructor is placement-new'd in `instance_init` and
   destroyed in `finalize`.
2. **The startup race.**
   - `startup_thread_func` (`gtk/src/main.cpp:303`) reads
     `result->app->db_path` while `phig_collection_open` runs.
   - If the window closes first, `g_application_run` returns, and `main`
     frees `app` while the worker is still reading it.
   - The process can then also exit with that thread mid-open.
3. **core/ redirects the host's stderr.** `StderrSuppressor` in
   `core/hasher.cpp` points file descriptor 2 at `/dev/null` while OpenCV
   decodes in `decode_image_with_warning` and `extract_video_keyframe`.
   - **It hides other output.** Thumbnail generation and the faces engine
     reach it, so in gtk it hides GTK warnings from every thread for the
     length of a decode. That includes the warnings the real-run check counts,
     and ASan reports. libphig hosts lose their stderr the same way.
   - **It serializes decoding.** It also holds one process-wide mutex across
     the decode, so concurrent decodes run one at a time. What that costs has
     not been measured.
4. **The loader leaks a cancelled request's data.** `thumb_loader_request`
   takes a function pointer and a `gpointer`. A request cancelled before
   delivery never calls back. Its `ThumbResultData`, which holds a
   `GtkListItem` reference, is therefore never freed (`main.cpp:604`, `:683`).
   061's Outcome records this.
5. **061's parked minors:**
   - `app->window` is never cleared when the window is destroyed. Item 2's
     fix needs it.
   - `on_factory_bind` has no null check on `app->loader`.
   - `thumb-loader.h`'s threading wording is unclear.
   - `rules/README.md`'s `core_internal` definition does not cover
     `ensure_thumb`'s callers.

## Decisions

### 1. A C++ seam

Convert `gallery-model.c`, `row-page.c` and `tests/test-gallery-model.c` to
C++, then make `collection.h` a C++ header:

```cpp
namespace phig::gtk {

struct Row {
    int64_t id;
    std::string path;
    std::string sha256;  // empty when phig has not hashed the row
    std::string filename;
};

struct Collection;  // defined by collection.cpp, or by the fake
struct CollectionClose { void operator()(Collection*) const noexcept; };
using CollectionPtr = std::unique_ptr<Collection, CollectionClose>;

struct OpenResult {
    CollectionPtr coll;  // null on failure
    std::string error;
};
OpenResult open(const std::string& path) noexcept;

std::optional<uint32_t> count(Collection&) noexcept;
std::optional<std::vector<Row>> page(Collection&, uint32_t offset,
                                     uint32_t limit) noexcept;

enum class ThumbLookup { Present, Absent, Invalid };
struct ThumbLocation {
    ThumbLookup state;
    std::string path;  // empty when Invalid
};
ThumbLocation thumb_lookup(Collection&, std::string_view sha256, int size) noexcept;

enum class ThumbGenerate { Ok, NoSuchRow, SourceMissing, Failed };
ThumbGenerate thumb_generate(Collection&, int64_t image_id, int size) noexcept;

}  // namespace phig::gtk
```

- **Functions stay `noexcept` and return values.** The loader's and the
  model's worker threads call them, and an exception escaping a `std::thread`
  terminates. Catching in one file keeps that rule in one place.
- **`ThumbGenerate` mirrors `core/`'s `EnsureThumb`** instead of collapsing to
  a bool, so M1 can say "file missing" without another seam change.
- **`PhigImageRow` holds a `phig::gtk::Row`**, placement-new'd in
  `instance_init` and destroyed in `finalize`, in place of its three `char*`
  fields.
- **The rest of `gallery-model` is untouched.** Its `GHashTable`s and fetch
  logic work, so converting the file changes only what the seam forces.
- **`row-page.c` goes away**, because a page is a `std::vector<Row>`.
- **`fake-collection.cpp` implements the new header.**

`gtk/CLAUDE.md`'s "GObjects only where GTK requires one" rule changes: it
names the placement-new pattern instead of implying C.

### 2. The startup worker owns what it reads

- **The worker copies its inputs.** `StartupResult` holds its own copy of the
  path, and the worker never dereferences `app`.
- **The process stays up until startup finishes.** `on_activate` calls
  `g_application_hold` before starting the task, and `startup_done` calls
  `g_application_release`. Closing the window during startup no longer ends
  the process with the worker mid-open.
- **`startup_done` checks for the window.** It looks at `app->window` before
  building the grid, and only releases if the window is gone. The window's
  `destroy` signal clears `app->window`, which also closes that minor.

The cost: closing the window during a slow schema migration delays exit until
the migration finishes. SQLite would roll an interrupted migration back
anyway, so the wait buys a clean exit, not safety.

### 3. Only the CLI suppresses stderr

`core/` gets a process-wide switch, `set_suppress_decoder_stderr(bool)`, off by
default. `cli/main.cpp` turns it on at startup. gtk and libphig hosts keep
their stderr. With the switch off, `StderrSuppressor` and its mutex are not
constructed at all, so gtk's and libphig's decodes also stop being serialized.

The CLI keeps today's behaviour, including the serialization. A measurement
decides whether that matters: `phig scan --parallel N --force hash` over a
fixed corpus, with the switch on and off. If the mutex costs a meaningful
share of a parallel scan, a later plan captures decoder messages where they are
produced instead: libjpeg-turbo with a per-call message handler, and FFmpeg's
log callback. That removes both the redirect and the mutex. Not this plan.

### 4. The loader owns its callbacks

```cpp
using ThumbCallback = std::function<void(ThumbResult, GdkTexture*)>;
void thumb_loader_request(ThumbLoader&, int64_t image_id, std::string sha256,
                          guint32 max_dim, GCancellable*, ThumbCallback cb);
```

The loader destroys the callback when the request is delivered, cancelled,
dropped at shutdown, or dropped because it arrived after shutdown. The
callback's captures are therefore released on every path. `std::function`
needs copyable captures, so the bind site captures a `GRef<GtkListItem>` in a
`shared_ptr`.

## Steps

Each step lands green, and the ASan gtk build runs the gtk tests at each one.

1. **Loader callbacks (decision 4).** Test first: a cancelled request, a
   request dropped at shutdown, and a request made after shutdown each destroy
   their callback exactly once, counted by a capture's destructor. The current
   API has no callback object to count, so the tests and the API change land
   together. Then update `main.cpp`'s bind site.
2. **C++ seam (decision 1).**
   - Rename the C files to `.cpp` and fix what the C++ compiler rejects.
     Commit that alone, so the conversion and the API change review
     separately.
   - Then replace `collection.h`, `collection.cpp`, the fake and their
     callers, and delete `row-page.c`.
   - `test-collection` keeps its six cases against the new signatures.
     `test-gallery-model` keeps its ten.
3. **Startup race (decision 2).**
   - Move `startup_thread_func` and `StartupResult` into
     `gtk/src/startup.cpp` so a headless test can link them.
   - Add a fake `open` that blocks on a gate. The test frees the `PhigApp`
     equivalent, releases the gate, and passes under ASan.
   - Verify the hold/release path by hand under ASan with a display: break in
     `phig_collection_open` in gdb, close the window, continue, and check for
     a clean exit with status 0. GTK cannot run headless in CI.
4. **Stderr switch (decision 3).** Add a test in `phig-tests`: with the switch
   off, a write to fd 2 from another thread during a decode of a corrupt JPEG
   fixture reaches a pipe. With it on, the same write is discarded, which is
   the CLI's behaviour today.
   Then run the measurement and record the numbers in this plan.
5. **Minors.** Add the null check in `on_factory_bind`, the `thumb-loader.h`
   wording and the `rules/README.md` wording. Run `rules.py --check --emit`.
6. **Docs.** Update `gtk/CLAUDE.md` (the seam's signatures in the Rules list,
   the GObject rule, and the `ThumbLoader` callback contract) and
   `gtk/README.md`.

## Open questions

1. **An error type instead of `OpenResult`.** C++20 has no `std::expected`.
   The seam has one fallible call that carries a message, so a struct is
   enough. A hand-written `Expected<T>` becomes worth it if M1 adds several
   more.
2. **Measure the mutex before or after 067?** The measurement is independent
   of 067, but 067 changes how a scan names its collection. Measuring after
   067 means the command recorded in this plan is the one that will still
   work.

## Relationship to other plans

- **061:** this plan closes the follow-ups its Outcome records.
- **067:** changes `phig_collection_open`'s argument to a catalog
  directory. Whichever plan lands second carries that change into the seam.
- **GTK M1 (not yet written):** builds on the C++ seam.
