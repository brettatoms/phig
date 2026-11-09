---
name: GTK Hardening
status: planned
created: 2026-11-08
description: >
  Make `gtk/` C++ throughout, including its collection seam and the remaining
  C files, and move the build to C++23 for `std::expected` and
  `std::move_only_function`. Also fix the startup race that frees app state
  under the startup worker, and close the loader leak and minor items 061 left
  open. Depends on 069, which stops `core/` redirecting the host's stderr.
  Lands before GTK M1, which adds many seam functions.
---

# 068 — GTK Hardening

## Problem

061 made gtk a peer of libphig over `core/` and left five things behind. Each
was found during 061's execution or final review.

1. **gtk is still partly C, and its seam is C because of it.** `collection.h`
   is a C header: `gboolean` results, `char**` errors, a `PhigRowPage` the
   caller must free, and C-string fields. It is C only because three files are:
   - `gallery-model.c`
   - `row-page.c`
   - `tests/test-gallery-model.c`

   061 kept `gallery-model.c` in C to limit scope. Its stated reason, that
   GObject zeroes instance memory, does not rule out C++. A GObject type can be
   defined in a `.cpp` file, and a member with a constructor is placement-new'd
   in `instance_init` and destroyed in `finalize`. The project's direction,
   recorded in root `CLAUDE.md` on 2026-11-08, is C++ throughout `core/` and
   `gtk/`, with C only in libphig's exported header.
2. **The startup race.**
   - `startup_thread_func` (`gtk/src/main.cpp:303`) reads
     `result->app->db_path` while `phig_collection_open` runs.
   - If the window closes first, `g_application_run` returns, and `main`
     frees `app` while the worker is still reading it.
   - The process can then also exit with that thread mid-open.
3. **core/ redirects the host's stderr** during every OpenCV decode, which
   hides GTK's warnings and serializes the loader's workers. Plan 069 removes
   the redirect, and this plan depends on it.
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

### 1. C++23

The root `CMakeLists.txt` moves from `CMAKE_CXX_STANDARD 20` to `23`. That
applies to every target. Two library features are what this plan wants from it:

- `std::expected`, for seam calls that fail with a reason;
- `std::move_only_function`, for callbacks that own move-only captures such
  as `GRef`.

Not every CI toolchain provides both. Measured and researched on 2026-11-08:

| Toolchain | Where | `-std=c++23` | `std::expected` | `std::move_only_function` |
|---|---|---|---|---|
| GCC 16.2, libstdc++ 16 | local | yes, measured | yes, measured | yes, measured |
| clang 23.1, libstdc++ 16 | local | yes, measured | yes, measured | yes, measured |
| GCC 13, libstdc++ 13 | ubuntu-latest: test and gtk jobs | yes | yes (GCC 12+) | yes (GCC 12+) |
| clang 18, libstdc++ 13 | ubuntu-latest: sanitizer jobs | yes | **no** | yes |
| Apple clang (Xcode 26.6), libc++ | macos-latest | yes | yes (libc++ 16+) | **no** |
| clang-tidy 21, libstdc++ 13 | lint | yes | yes | yes |

Why the two gaps:
- **clang 18 with libstdc++.** libstdc++ defines `std::expected` only when
  `__cpp_concepts >= 202002L`, and clang 18 reports an older value. Measured
  locally by forcing clang 23's `__cpp_concepts` to `201907L`: `<expected>`
  then declares nothing. A WebKit bug reports the same against clang 18.
- **Apple's libc++.** libc++ implemented `std::move_only_function` for LLVM 22
  (PR #94670, merged November 2025). That is after the LLVM base of Xcode 26's
  Apple clang, so macOS does not have it. This is inferred from release dates,
  not measured on a runner.

So the rule this plan adds to root `CLAUDE.md`:

- **`gtk/` may use both.** It builds only in the gtk job, with GCC 13, and
  never on macOS or in the sanitizer jobs.
- **`core/`, `libphig/` and `cli/` use neither** until the sanitizer jobs move
  to a clang at version 19 or later and Apple's libc++ ships
  `std::move_only_function`. Lifting the restriction is a CI change plus a
  line in `CLAUDE.md`.

Switching the standard itself was checked locally on 2026-11-08, on master at
`def166d` with only `CMAKE_CXX_STANDARD` changed. Every target, gtk included,
built with `-DPHIG_WERROR=ON` under GCC 16.2 and clang 23.1, and all 910 ctest
tests passed under both. GCC printed one warning, a `-Wstringop-overflow` in
`tests/test_live_photo.cpp`, which the C++20 build prints too. The CI toolchains
still need a `workflow_dispatch` run of every job, including macOS and both
sanitizers, before the switch merges.

### 2. A C++ seam

`collection.h` becomes a C++ header:

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

std::expected<CollectionPtr, std::string> open(const std::string& path) noexcept;
std::expected<uint32_t, std::string> count(Collection&) noexcept;
std::expected<std::vector<Row>, std::string> page(Collection&, uint32_t offset,
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
- **Every fallible call carries its reason.** The model's `page-fetch-failed`
  signal can then report why a page failed, which today it cannot.
- **`ThumbGenerate` mirrors `core/`'s `EnsureThumb`** instead of collapsing to
  a bool, so M1 can say "file missing" without another seam change.
- **`row-page.c` goes away**, because a page is a `std::vector<Row>`.
- **`fake-collection.cpp` implements the new header.**

### 3. gtk is C++ throughout

Converting the three C files is not limited to what the seam forces. Every gtk
file follows the same rules:

- **GObject state is C++.**
  - `PhigImageRow` holds a `phig::gtk::Row` in place of its three `char*`
    fields.
  - `PhigGalleryModel` holds a `struct Impl` with `std::unordered_map`s in
    place of its three `GHashTable`s, and an LRU `std::list` where it keeps a
    clock today.

  Both are placement-new'd in `instance_init` and destroyed in `finalize`.
- **Signals take typed lambdas.** A helper in `src/signal.h` replaces
  `G_CALLBACK` casts and `gpointer user_data`:

  ```cpp
  template <typename Sig, typename F>
  gulong connect(gpointer instance, const char* signal, F f);

  connect<void(GtkListItemFactory*, GtkListItem*)>(
      factory, "bind", [app](GtkListItemFactory*, GtkListItem* item) noexcept { ... });
  ```

  It heap-allocates `f`, passes a trampoline generated from `Sig` to
  `g_signal_connect_data`, and frees `f` in the closure's destroy notify. A
  `static_assert(std::is_nothrow_invocable_v<F, Args...>)` turns `gtk/`'s rule
  that no exception unwinds into GTK into a compile error rather than a review
  item. The signal's real signature still has to match `Sig`; GLib cannot
  check that at compile time, so each call names it explicitly.
- **GLib types stay where GTK requires them:** GObject types, `GListModel`,
  `GTask` for main-loop delivery, `GCancellable`, and anything a GTK function
  takes or returns.

### 4. The startup worker owns what it reads

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

### 5. The loader owns its callbacks

```cpp
using ThumbCallback = std::move_only_function<void(ThumbResult, GdkTexture*) noexcept>;
void thumb_loader_request(ThumbLoader&, int64_t image_id, std::string sha256,
                          guint32 max_dim, GCancellable*, ThumbCallback cb);
```

- **Every path releases the captures.** The loader destroys the callback when
  the request is delivered, cancelled, dropped at shutdown, or dropped because
  it arrived after shutdown.
- **The bind site captures a `GRef<GtkListItem>` directly.**
  `std::move_only_function` accepts move-only captures, so nothing needs a
  `shared_ptr` around it.

## Steps

Execution plan: `docs/plans/2026-11-09-068-gtk-hardening.md`.

Each step lands green, and the ASan gtk build runs the gtk tests at each one.

1. **C++23 (decision 1).**
   - Change the standard.
   - Add the library-feature rule to root `CLAUDE.md`.
   - Build every target with `-Werror` under GCC and clang, and run both suites.
   - Push a branch and run `gh workflow run CI --ref <branch>`, which covers
     macOS and both sanitizers. Pushing needs approval at the time.
   - Merge only when every job is green.
2. **Loader callbacks (decision 5).** Test first: a cancelled request, a
   request dropped at shutdown, and a request made after shutdown each destroy
   their callback exactly once, counted by a capture's destructor. The current
   API has no callback object to count, so the tests and the API change land
   together. Then update `main.cpp`'s bind site.
3. **Rename the C files (decision 3).** Rename the three C files to `.cpp` and
   fix only what the C++ compiler rejects. Commit that alone, so the rename and
   the rewrite review separately.
4. **C++ seam (decision 2).** Replace `collection.h`, `collection.cpp`, the
   fake and their callers, and delete `row-page.c`. `test-collection` keeps its
   six cases against the new signatures, and `test-gallery-model` keeps its
   ten.
5. **C++ internals and the signal helper (decision 3).**
   - Move `PhigImageRow` and `PhigGalleryModel` state to C++ members.
   - Add `src/signal.h` with a test: a connected lambda runs, and its captures
     are destroyed on disconnect and when the instance is finalized.
   - Move every `g_signal_connect` in gtk onto it.
6. **Startup race (decision 4).**
   - Move `startup_thread_func` and `StartupResult` into
     `gtk/src/startup.cpp` so a headless test can link them.
   - Add a fake `open` that blocks on a gate. The test frees the `PhigApp`
     equivalent, releases the gate, and passes under ASan.
   - Verify the hold/release path by hand under ASan with a display: break in
     `phig_collection_open` in gdb, close the window, continue, and check for
     a clean exit with status 0. GTK cannot run headless in CI.
7. **Minors.** Add the null check in `on_factory_bind`, the `thumb-loader.h`
   wording and the `rules/README.md` wording. Run `rules.py --check --emit`.
8. **Docs.** Update `gtk/CLAUDE.md` (the seam's signatures in the Rules list,
   the signal helper, and the `ThumbLoader` callback contract), remove the
   "three remain" sentence once the C files are gone, and update
   `gtk/README.md`.

## Relationship to other plans

- **061:** this plan closes the follow-ups its Outcome records.
- **069:** removes the stderr redirect. Land it first.
- **067:** changes `phig_collection_open`'s argument to a catalog directory.
  Whichever plan lands second carries that change into the seam.
- **GTK M1 (not yet written):** builds on the C++ seam and the signal helper.
