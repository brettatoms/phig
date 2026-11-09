# 068 — GTK Hardening: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `gtk/` is C++ throughout on C++23, reaches phig through a C++ seam
whose fallible calls carry their reason, and no longer leaks a cancelled
thumbnail request or lets the startup worker read freed state.

**Architecture:** The root build moves to C++23 so `gtk/` can use
`std::expected` and `std::move_only_function`. The thumbnail loader takes an
owning callback and destroys it exactly once, on the main thread. The three C
files become C++, the seam `collection.h` becomes a `phig::gtk` namespace that
returns values, and the two GObjects hold C++ state. Signals connect through a
typed helper, and the startup worker owns copies of what it reads while the
application is held until it finishes.

**Tech Stack:** C++23, GTK 4 / libadwaita, GLib's `g_test`, CMake 3.20.

**Spec:** `plans/068-gtk-hardening.md`. Read it before starting a task; it
explains why each piece is shaped the way it is.

## Global Constraints

- **Never write to or open `~/.local/share/phig/`, `~/.cache/phig/`,
  `~/.local/state/phig/`, `~/.config/phig/` or `/mnt/data/Pictures`**,
  read-only `sqlite3` included. Every test builds its own database and
  thumbnail directory in a temp dir. A hand-run `phig` or `phig-gtk` exports
  `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` and
  `XDG_STATE_HOME` to a `mktemp -d` directory and unsets `PHIG_CATALOG`.
- **`core/`, `libphig/` and `cli/` use neither `std::expected` nor
  `std::move_only_function`.** The sanitizer jobs build them with clang 18,
  whose libstdc++ `<expected>` declares nothing, and Apple's libc++ has no
  `std::move_only_function`. `gtk/` may use both.
- **Only `gtk/src/collection.cpp` includes a `core/` header.** Widget code
  calls `collection.h`.
- **No exception unwinds into GLib or GTK.** Every function passed to GLib or
  GTK (signal handlers, idle, timeout and GTask callbacks, destroy notifies,
  `GListModel` vfuncs) is `noexcept`, except the `class_init` and
  `instance_init` functions that `G_DEFINE_*` declares itself, which cannot
  be. Every `collection.h` function is `noexcept` and catches.
- **No class hierarchies in code phig writes.** No gtkmm. Plain structs, free
  functions, `GRef<T>` (`gtk/src/gref.h`).
- **A GObject instance's C++ members are placement-new'd in `instance_init`
  and destroyed in `finalize`.** GObject zeroes instance memory and runs no
  C++ constructor.
- **Do not run clang-format over a whole existing `gtk/` file you only edit.**
  `gtk/` is outside the lint gate and most of its files do not conform, so a
  reformat would hide the real diff. Format new files, and files a task
  rewrites whole, with the pinned clang-format. Files under `core/`,
  `libphig/` and `tests/` must pass the pinned clang-format and clang-tidy.
- **gtk build directories go under `build/`**, which is gitignored:
  `build/with-gtk` (GCC), `build/with-gtk-clang` and `build/with-gtk-asan`.
  Do not reconfigure the main `build/` with `PHIG_BUILD_GTK=ON`.
- **gtk tests run with no display:** `env -u DISPLAY -u WAYLAND_DISPLAY`.
- **gtk targets do not get `-Werror`.** `PHIG_WERROR` reaches only the
  `PHIG_WARNINGS` targets; gtk uses `PHIG_GTK_WARNINGS`. Each task's build
  step therefore greps its output for `warning`, and a task finishes with
  none.
- **Comments describe the present.** Plan numbers and history go in commit
  messages, not comments.
- **Commit messages are plain prose, with no AI attribution.**
- **Pushing needs the user's approval at the time.** The controller asks; an
  implementer never pushes.

Pinned tools:

```bash
CF=~/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
```

The full gtk check, used at the end of every task from Task 2 on. It builds
the client and every gtk test under GCC and ASan, and runs the gtk tests in
both:

```bash
gtk_check() {
  cmake --build build/with-gtk --target gtk-all -j 2>&1 | grep -E "warning|error"
  env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk --output-on-failure
  cmake --build build/with-gtk-asan --target gtk-all -j 2>&1 | grep -E "warning|error"
  ASAN_OPTIONS=detect_leaks=0 env -u DISPLAY -u WAYLAND_DISPLAY \
    ctest --test-dir build/with-gtk-asan -L gtk --output-on-failure
}
```

Expected: no `warning` or `error` lines from either build, and `100% tests
passed` from both ctest runs. Each shell command starts a fresh shell, so
define the function in the same command that calls it.

`gallery-model.cpp` is renamed C until Task 6, so its GLib callbacks are not
`noexcept` until then; the constraint below holds for every other file from
Task 2 on.

## Decisions this plan makes beyond 068

These are not in the spec. A reviewer should check each against it.

1. **The signal helper is `gtk/src/connect.h`, not `src/signal.h`.** The gtk
   targets put `src/` on the include path, and GLib includes `<signal.h>`.
   Measured while writing this plan: a `signal.h` in an include directory
   shadows the system header and every GLib include fails with `'GData' does
   not name a type`.
2. **The seam has `default_catalog()` and `open` takes a catalog directory.**
   Plan 067 landed after 068 was written and added both to the C seam. The
   C++ seam keeps them, and `default_catalog` returns `std::expected` like
   every other fallible call.
3. **A thumbnail request, and so its callback, is destroyed only on the main
   thread.** The spec requires that every path releases the callback. The
   callback holds a `GRef<GtkListItem>`, and dropping the last reference to a
   GTK object off the main thread is not safe, so a worker never destroys a
   request. It hands a cancelled request back through the same idle delivery
   as a result, and `thumb_loader_shutdown` takes the requests out of
   undelivered idles. Cost: one idle source per request cancelled after a
   worker took it.
4. **`thumb-loader.h`'s threading wording is fixed in Task 2,** where the
   comment it sits in is rewritten for the new callback, not in Task 8.
5. **The spec's step 5 is two tasks:** Task 5 adds the signal helper and moves
   every connection onto it; Task 6 moves the GObjects' state to C++. A
   reviewer could reject one and approve the other.
6. **`page-fetch-failed` gains a `const char*` argument** carrying the seam's
   reason, which is what the spec's "report why a page failed" needs. The
   window keeps its one generic toast; the model's warning carries the
   reason.
7. **A second activation during startup does not start a second open.** The
   application hold makes it reachable: close the window while the catalog
   opens, launch again, and a second open would replace `coll` under the
   first's loader. `startup_done` fills whichever window exists when it runs.
8. **`test-collection` has seven cases, not the six the spec counts.** Plan
   067 added one. Two are renamed because the seam now distinguishes
   `SourceMissing` from `NoSuchRow` and reports a default-catalog failure as
   a value rather than a warning.
9. **The tunables keep their `PHIG_*` names as `constexpr` values.**
   `gtk/README.md` and `gtk/CLAUDE.md` refer to them by name.

## File map

| File | Task | Responsibility |
|---|---|---|
| `CMakeLists.txt` | 1 | `CMAKE_CXX_STANDARD 23` |
| `CLAUDE.md` | 1, 9 | Language standard and the library-feature rule; gtk test count |
| `gtk/src/thumb-loader.h`, `.cpp` | 2, 4 | Owning callbacks; the C++ seam |
| `gtk/tests/test-thumb-loader.cpp` | 2, 4 | Callback destruction counted; the C++ fake |
| `gtk/tests/fake-collection.h`, `.cpp` | 2, 4, 7 | Generation hold; C++ rewrite; `open` with a hold |
| `gtk/src/gallery-model.c` → `.cpp` | 3, 4, 6 | Renamed; on the C++ seam; C++ state |
| `gtk/src/gallery-model.h` | 4, 6 | C++ header; `std::string` getters |
| `gtk/src/row-page.c` → `.cpp`, then deleted | 3, 4 | Gone once a page is a `std::vector<Row>` |
| `gtk/tests/test-gallery-model.c` → `.cpp` | 3, 4, 5, 6 | Renamed; the C++ fake; `connect`; `std::string` getters |
| `gtk/src/collection.h`, `.cpp` | 4 | The `phig::gtk` seam |
| `gtk/tests/test-collection.cpp` | 4 | The seam against a database it builds |
| `gtk/src/connect.h` | 5 | `phig::gtk::connect<Sig>` |
| `gtk/tests/test-connect.cpp` | 5 | The helper's tests |
| `gtk/src/startup.h`, `.cpp` | 7 | `open_in_background` |
| `gtk/tests/test-startup.cpp` | 7 | The worker reads its own copy |
| `gtk/src/main.cpp` | 2, 4, 5, 6, 7, 8 | Every caller |
| `gtk/CMakeLists.txt` | 3, 4, 5, 7 | Sources, test binaries, languages |
| `rules/README.md` | 8 | `core_internal` wording |
| `gtk/CLAUDE.md`, `gtk/README.md`, `plans/068-…`, `plans/000-INDEX.md` | 9 | Written down |

## Pre-flight

- [ ] **Confirm the start state.** The user approves creating the branch.

```bash
cd /home/brett/devel/phig
git status --short                      # expect: clean
git switch -c 068-gtk-hardening master
git rev-parse HEAD                      # record as BASE in the ledger
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON
cmake --build build/with-gtk -j 2>&1 | tail -1
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk --parallel 2>&1 | tail -3
python3 scripts/rules.py --check
```

Expected: `100% tests passed, 0 tests failed out of 1021` (1018 phig tests
and 3 gtk test binaries; ctest counts a gtk binary as one test) and `rules: 84 rules, 18 operations, OK`. Record both in the
ledger.

- [ ] **Configure the clang and ASan gtk builds** if they are absent or stale:

```bash
CC=clang CXX=clang++ cmake -B build/with-gtk-clang -DPHIG_BUILD_GTK=ON
cmake -B build/with-gtk-asan -DPHIG_BUILD_GTK=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
```

---

### Task 1: C++23

**Files:**
- Modify: `CMakeLists.txt:7`
- Modify: `CLAUDE.md` (Tech stack)

**Interfaces:**
- Produces: every target compiles as C++23. Later tasks use `<expected>` and
  `<functional>`'s `std::move_only_function` in `gtk/` only.

- [ ] **Step 1: Change the standard**

In `CMakeLists.txt`, replace:

```cmake
set(CMAKE_CXX_STANDARD 20)
```

with:

```cmake
set(CMAKE_CXX_STANDARD 23)
```

- [ ] **Step 2: Write the rule down**

In `CLAUDE.md`, under `## Tech stack`, replace the Language bullet's first
line, `- **Language:** C++20, in \`core/\`, \`cli/\`, \`gtk/\` and libphig's`,
with `- **Language:** C++23, in \`core/\`, \`cli/\`, \`gtk/\` and libphig's`.
Leave the rest of that bullet as it is. Directly after the bullet, insert:

```markdown
- **Library features newer than C++20 are `gtk/`-only for now.** `gtk/` may
  use `std::expected` and `std::move_only_function`; `core/`, `libphig/` and
  `cli/` use neither. The sanitizer jobs build those three with clang 18,
  whose libstdc++ `<expected>` declares nothing because clang 18 reports an
  older `__cpp_concepts`, and Apple's libc++ has no
  `std::move_only_function`. `gtk/` builds only in the gtk job, with GCC 13.
  Lifting this is a CI change plus this line. Plan 068 records the
  toolchain survey.
```

- [ ] **Step 3: Build and test every target under GCC with `-Werror`**

```bash
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON -DPHIG_WERROR=ON
cmake --build build/with-gtk -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk --parallel 2>&1 | tail -3
```

Expected: no output from the grep, and `100% tests passed, 0 tests failed out
of 1021`. If the build stops on a `-Wstringop-overflow` in
`tests/test_live_photo.cpp`, check out `master`, build it the same way under
C++20, and confirm the same warning stops it there. If it does, record it in
the task report as pre-existing and build this task with `-DPHIG_WERROR=OFF`;
do not fix it here. Any other warning is this task's to fix.

- [ ] **Step 4: Build and test every target under clang with `-Werror`**

```bash
CC=clang CXX=clang++ cmake -B build/with-gtk-clang -DPHIG_BUILD_GTK=ON -DPHIG_WERROR=ON
cmake --build build/with-gtk-clang -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk-clang --parallel 2>&1 | tail -3
```

Expected: as Step 3.

- [ ] **Step 5: Rebuild the main build and run clang-tidy**

clang-tidy reads `build/compile_commands.json`, so the main build must be
reconfigured to carry `-std=c++23`:

```bash
cmake -B build && cmake --build build -j 2>&1 | grep -E "warning|error"
grep -m1 -o "std=c++23\|std=gnu++23" build/compile_commands.json
ctest --test-dir build --parallel 2>&1 | tail -3
mise exec -- ./scripts/clang-tidy.sh > /tmp/068-tidy.log 2>&1; echo "exit $?"
grep -E "error:|warning:" /tmp/068-tidy.log | sort -u
```

Expected: the grep prints `std=gnu++23` or `std=c++23`, ctest reports `100%
tests passed, 0 tests failed out of 1018`, clang-tidy exits 0 and the last
grep prints nothing.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt CLAUDE.md
git commit -m "Build every target as C++23

gtk/ needs std::expected and std::move_only_function. core/, libphig/ and cli/
keep to C++20's library: the sanitizer jobs' clang 18 cannot see libstdc++'s
<expected>, and Apple's libc++ has no std::move_only_function."
```

- [ ] **Step 7 (controller): ask to run the CI toolchains**

The spec requires every CI job, macOS and both sanitizers included, to pass
before the switch merges. Ask the user for approval to push
`068-gtk-hardening` and run `gh workflow run CI --ref 068-gtk-hardening`.
With approval, push, start the run, and record its id in the ledger. Do not
wait on it: continue with Task 2. The final review checks its result.

---

### Task 2: The loader owns its callbacks

**Files:**
- Modify: `gtk/src/thumb-loader.h` (rewrite)
- Modify: `gtk/src/thumb-loader.cpp` (rewrite)
- Modify: `gtk/tests/test-thumb-loader.cpp` (rewrite)
- Modify: `gtk/tests/fake-collection.h`, `gtk/tests/fake-collection.cpp`
- Modify: `gtk/src/main.cpp:600-687` (the bind site)

**Interfaces:**
- Consumes: the C seam as it stands (`PhigCollection*`,
  `phig_collection_thumb_lookup`, `phig_collection_thumb_generate`).
- Produces:

```cpp
using ThumbCallback = std::move_only_function<void(ThumbResult, GdkTexture*) noexcept>;
void thumb_loader_request(ThumbLoader& loader, int64_t image_id, std::string sha256,
                          guint32 max_dim, GCancellable* cancellable, ThumbCallback cb);
void fake_collection_hold_generation(PhigCollection* coll, gboolean hold);  // tests only
```

The current API has no callback object to count, so the API change and its
tests land together. Steps 3 to 5 still produce a real failure first: they
swap the type without changing where requests are destroyed.

- [ ] **Step 1: Add a generation hold to the fake**

The test in Step 2 that checks which thread destroys a callback needs a
worker busy while a second request waits in the queue.

In `gtk/tests/fake-collection.h`, before `G_END_DECLS`, add:

```c
/* While hold is set, phig_collection_thumb_generate waits before generating. */
void fake_collection_hold_generation(PhigCollection* coll, gboolean hold);
```

In `gtk/tests/fake-collection.cpp`, add `#include <condition_variable>` and
`#include <mutex>` to the standard includes, and add three members to
`struct PhigCollection`, after `generate_count`:

```cpp
    std::mutex hold_mutex;
    std::condition_variable hold_cv;
    bool hold = false;  // guarded by hold_mutex
```

Inside `extern "C"`, after `fake_collection_generate_count`, add:

```cpp
void fake_collection_hold_generation(PhigCollection* coll, gboolean hold) {
    {
        std::lock_guard lock(coll->hold_mutex);
        coll->hold = hold != FALSE;
    }
    coll->hold_cv.notify_all();
}
```

In `phig_collection_thumb_generate`, directly after the first line (the
`image_id` and `size` check), add:

```cpp
    {
        std::unique_lock lock(coll->hold_mutex);
        coll->hold_cv.wait(lock, [coll] { return !coll->hold; });
    }
```

- [ ] **Step 2: Write the tests**

Replace `gtk/tests/test-thumb-loader.cpp` with:

```cpp
#include <glib.h>
#include <gtk/gtk.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "fake-collection.h"
#include "thumb-loader.h"

namespace {

// Every fake thumbnail is 64x64, charged at width * height * 4 bytes.
constexpr gsize kTextureBytes = 64 * 64 * 4;
constexpr gsize kRoomy = 64 * 1024 * 1024;

GThread* main_thread = nullptr;  // set by main()

std::string sha_of(guint index) {
    char sha[65];
    fake_collection_sha(index, sha);
    return sha;
}

// A collection of 8 rows and a loader over it. The loader is shut down before
// the collection closes: a delivery still queued on the main context holds a
// loader reference, so dropping this one alone may leave workers running.
struct Fixture {
    PhigCollection* coll;
    std::shared_ptr<ThumbLoader> loader;

    explicit Fixture(gsize budget = kRoomy, guint threads = 4)
        : coll(fake_collection_new(8)), loader(thumb_loader_new(coll, threads, budget)) {}
    ~Fixture() {
        thumb_loader_shutdown(*loader);
        loader.reset();
        phig_collection_close(coll);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

gboolean on_timeout(gpointer user_data) {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

// Runs the default main context for ms milliseconds.
void pump(guint ms) {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_timeout_add(ms, on_timeout, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

struct Capture {
    GMainLoop* loop = nullptr;
    bool called = false;
    bool synchronous = false;  // delivered inside thumb_loader_request
    ThumbResult result = THUMB_MISS;
    int width = -1;  // of the delivered texture; -1 if none
};

ThumbCallback capturing(Capture& capture) {
    return [&capture](ThumbResult result, GdkTexture* texture) noexcept {
        capture.called = true;
        capture.result = result;
        capture.width = texture != nullptr ? gdk_texture_get_width(texture) : -1;
        if (capture.loop != nullptr) g_main_loop_quit(capture.loop);
    };
}

// For a request that must never call back.
ThumbCallback must_not_run(bool& called) {
    return [&called](ThumbResult, GdkTexture*) noexcept { called = true; };
}

// How often, and on which threads, the callbacks holding one Probe were
// destroyed.
struct Destroyed {
    std::atomic<int> count{0};
    std::atomic<bool> off_main_thread{false};
};

// A move-only capture that reports its own destruction into a Destroyed.
class Probe {
  public:
    explicit Probe(Destroyed& into) : into_(&into) {}
    Probe(Probe&& other) noexcept : into_(std::exchange(other.into_, nullptr)) {}
    Probe& operator=(Probe&&) = delete;
    ~Probe() {
        if (into_ == nullptr) return;
        into_->count++;
        if (g_thread_self() != main_thread) into_->off_main_thread = true;
    }

  private:
    Destroyed* into_;
};

// Sets called when it runs, and reports its destruction into destroyed.
ThumbCallback probed(Destroyed& destroyed, bool& called) {
    return [probe = Probe(destroyed), &called](ThumbResult, GdkTexture*) noexcept {
        called = true;
    };
}

// Requests one thumbnail and runs the main loop until it is delivered, or for
// 5s. A texture-cache hit is delivered inside thumb_loader_request, before the
// loop would start, so the loop only runs when that did not happen.
Capture request_and_wait(ThumbLoader& loader, int64_t id, const std::string& sha, guint32 size) {
    Capture capture;
    capture.loop = g_main_loop_new(nullptr, FALSE);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, capture.loop);
    thumb_loader_request(loader, id, sha, size, nullptr, capturing(capture));
    capture.synchronous = capture.called;
    if (!capture.called) g_main_loop_run(capture.loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
    g_main_loop_unref(capture.loop);
    capture.loop = nullptr;
    return capture;
}

// Polls until a worker has posted a result for delivery, for up to 5s.
void wait_for_posted_result(ThumbLoader& loader) {
    for (int waited_ms = 0; thumb_loader_pending_idle_count(loader) == 0 && waited_ms < 5000;
         waited_ms++)
        g_usleep(1000);
    g_assert_cmpuint(thumb_loader_pending_idle_count(loader), >, 0);
}

// Polls until the workers have taken every queued request, for up to 5s.
void wait_for_empty_queue(ThumbLoader& loader) {
    for (int waited_ms = 0; thumb_loader_queue_length(loader) > 0 && waited_ms < 5000;
         waited_ms++)
        g_usleep(1000);
    g_assert_cmpint(thumb_loader_queue_length(loader), ==, 0);
}

void test_a_miss_is_generated_and_delivered() {
    Fixture f;
    Capture c = request_and_wait(*f.loader, 1, sha_of(0), PHIG_THUMB_SIZE_DEFAULT);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_cmpint(c.width, ==, 64);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 1);
}

// The second loader has an empty texture cache, so its request reaches the
// collection, which finds the file the first one generated.
void test_a_thumbnail_on_disk_is_not_regenerated() {
    Fixture f;
    g_assert_cmpint(request_and_wait(*f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);

    std::shared_ptr<ThumbLoader> second = thumb_loader_new(f.coll, 1, kRoomy);
    Capture c = request_and_wait(*second, 1, sha_of(0), 256);
    thumb_loader_shutdown(*second);
    second.reset();

    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_false(c.synchronous);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 1);
}

void test_a_repeat_request_is_served_from_memory() {
    Fixture f;
    g_assert_false(request_and_wait(*f.loader, 1, sha_of(0), 256).synchronous);
    Capture c = request_and_wait(*f.loader, 1, sha_of(0), 256);
    g_assert_true(c.synchronous);
    g_assert_cmpint(c.result, ==, THUMB_OK);
}

// A row phig has not hashed yet is data phig has not produced, not a failure.
void test_no_sha256_yields_thumb_miss() {
    Fixture f;
    Capture c = request_and_wait(*f.loader, 1, "", 256);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_MISS);
    g_assert_cmpint(c.width, ==, -1);
}

// The row has a sha256 but its thumbnail cannot be generated: a failure the
// user should be able to see.
void test_generation_failure_yields_thumb_failed() {
    Fixture f;
    Capture c = request_and_wait(*f.loader, -1, sha_of(0), 256);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_FAILED);
    g_assert_cmpint(c.width, ==, -1);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 0);
}

// No workers, so nothing pops the queue and its length shows whether the
// request was pushed at all.
void test_cancel_before_dispatch_never_calls_back() {
    Fixture f(kRoomy, 0);
    GCancellable* cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, must_not_run(called));
    g_assert_cmpint(thumb_loader_queue_length(*f.loader), ==, 0);
    pump(300);

    g_assert_false(called);
    g_object_unref(cancellable);
}

// Waits until a worker has posted the result before cancelling, so only the
// delivery-side check can see the cancel.
void test_cancel_after_dispatch_discards_the_result() {
    Fixture f;
    GCancellable* cancellable = g_cancellable_new();
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, must_not_run(called));
    wait_for_posted_result(*f.loader);
    g_cancellable_cancel(cancellable);
    pump(300);

    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
    g_object_unref(cancellable);
}

// A posted delivery holds a loader reference, so dropping the caller's last one
// leaves the loader alive until the delivery runs. It still calls back, and the
// loader is then destroyed on the main thread, before the collection closes.
void test_a_delivery_outlives_the_callers_reference() {
    PhigCollection* coll = fake_collection_new(8);
    std::shared_ptr<ThumbLoader> loader = thumb_loader_new(coll, 4, kRoomy);
    std::weak_ptr<ThumbLoader> watch = loader;
    Capture c;

    thumb_loader_request(*loader, 1, sha_of(0), 256, nullptr, capturing(c));
    wait_for_posted_result(*loader);
    loader.reset();
    g_assert_false(watch.expired());
    pump(300);

    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_true(watch.expired());
    phig_collection_close(coll);
}

void test_shutdown_discards_a_posted_result() {
    Fixture f;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, must_not_run(called));
    wait_for_posted_result(*f.loader);
    thumb_loader_shutdown(*f.loader);
    pump(300);

    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_pending_idle_count(*f.loader), ==, 0);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
}

// The texture is in memory, so without the shutdown check this would be a
// synchronous hit.
void test_a_request_after_shutdown_is_dropped() {
    Fixture f;
    g_assert_cmpint(request_and_wait(*f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);
    thumb_loader_shutdown(*f.loader);
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, must_not_run(called));
    pump(100);

    g_assert_false(called);
}

// Six textures against room for three and a half: the three most recent stay.
void test_the_cache_keeps_the_most_recent_within_budget() {
    Fixture f(kTextureBytes * 7 / 2);
    for (guint i = 0; i < 6; i++)
        g_assert_cmpint(request_and_wait(*f.loader, i + 1, sha_of(i), 256).result, ==, THUMB_OK);

    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 3 * kTextureBytes);
    g_assert_true(request_and_wait(*f.loader, 6, sha_of(5), 256).synchronous);
    g_assert_false(request_and_wait(*f.loader, 1, sha_of(0), 256).synchronous);
}

// A texture larger than the whole budget is evicted by its own insert. It is
// still delivered intact, and the cache ends empty.
void test_a_texture_over_budget_is_delivered_but_not_kept() {
    Fixture f(kTextureBytes / 2);
    Capture c = request_and_wait(*f.loader, 1, sha_of(0), 256);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_cmpint(c.width, ==, 64);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
}

void test_a_delivered_callback_is_destroyed_once() {
    Fixture f;
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));
    wait_for_posted_result(*f.loader);
    pump(300);

    g_assert_true(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_false(destroyed.off_main_thread.load());
}

void test_a_cache_hit_destroys_its_callback_before_returning() {
    Fixture f;
    g_assert_cmpint(request_and_wait(*f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));

    g_assert_true(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
}

void test_a_request_cancelled_before_dispatch_destroys_its_callback() {
    Fixture f(kRoomy, 0);
    GCancellable* cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, probed(destroyed, called));

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_object_unref(cancellable);
}

void test_a_request_cancelled_after_dispatch_destroys_its_callback() {
    Fixture f;
    GCancellable* cancellable = g_cancellable_new();
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, probed(destroyed, called));
    wait_for_posted_result(*f.loader);
    g_cancellable_cancel(cancellable);
    pump(300);

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_false(destroyed.off_main_thread.load());
    g_object_unref(cancellable);
}

// One worker, held inside the first request's generation, so the second
// request is still queued when it is cancelled. The worker then takes it
// already cancelled, and the callback must still be destroyed on the main
// thread: it may hold the last reference to a GTK object.
void test_a_callback_a_worker_drops_is_destroyed_on_the_main_thread() {
    Fixture f(kRoomy, 1);
    fake_collection_hold_generation(f.coll, TRUE);
    Destroyed first_destroyed;
    bool first_called = false;
    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr,
                         probed(first_destroyed, first_called));
    wait_for_empty_queue(*f.loader);

    GCancellable* cancellable = g_cancellable_new();
    Destroyed destroyed;
    bool called = false;
    thumb_loader_request(*f.loader, 2, sha_of(1), 256, cancellable, probed(destroyed, called));
    g_assert_cmpint(thumb_loader_queue_length(*f.loader), ==, 1);
    g_cancellable_cancel(cancellable);
    fake_collection_hold_generation(f.coll, FALSE);
    pump(300);

    g_assert_true(first_called);
    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_false(destroyed.off_main_thread.load());
    g_object_unref(cancellable);
}

// No workers, so the request is still queued when shutdown drops it.
void test_shutdown_destroys_a_queued_callback() {
    Fixture f(kRoomy, 0);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));
    g_assert_cmpint(thumb_loader_queue_length(*f.loader), ==, 1);
    thumb_loader_shutdown(*f.loader);

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
}

// No pump before the check: after shutdown the main context may never run
// again, as when g_application_run has returned.
void test_shutdown_destroys_a_posted_callback_without_the_main_loop() {
    Fixture f;
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));
    wait_for_posted_result(*f.loader);
    thumb_loader_shutdown(*f.loader);

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);

    pump(100);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_cmpuint(thumb_loader_pending_idle_count(*f.loader), ==, 0);
}

void test_a_request_after_shutdown_destroys_its_callback() {
    Fixture f;
    thumb_loader_shutdown(*f.loader);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    main_thread = g_thread_self();
    g_test_add_func("/thumb-loader/a-miss-is-generated-and-delivered",
                    test_a_miss_is_generated_and_delivered);
    g_test_add_func("/thumb-loader/a-thumbnail-on-disk-is-not-regenerated",
                    test_a_thumbnail_on_disk_is_not_regenerated);
    g_test_add_func("/thumb-loader/a-repeat-request-is-served-from-memory",
                    test_a_repeat_request_is_served_from_memory);
    g_test_add_func("/thumb-loader/no-sha256-yields-thumb-miss", test_no_sha256_yields_thumb_miss);
    g_test_add_func("/thumb-loader/generation-failure-yields-thumb-failed",
                    test_generation_failure_yields_thumb_failed);
    g_test_add_func("/thumb-loader/cancel-before-dispatch-never-calls-back",
                    test_cancel_before_dispatch_never_calls_back);
    g_test_add_func("/thumb-loader/cancel-after-dispatch-discards-the-result",
                    test_cancel_after_dispatch_discards_the_result);
    g_test_add_func("/thumb-loader/a-delivery-outlives-the-callers-reference",
                    test_a_delivery_outlives_the_callers_reference);
    g_test_add_func("/thumb-loader/shutdown-discards-a-posted-result",
                    test_shutdown_discards_a_posted_result);
    g_test_add_func("/thumb-loader/a-request-after-shutdown-is-dropped",
                    test_a_request_after_shutdown_is_dropped);
    g_test_add_func("/thumb-loader/the-cache-keeps-the-most-recent-within-budget",
                    test_the_cache_keeps_the_most_recent_within_budget);
    g_test_add_func("/thumb-loader/a-texture-over-budget-is-delivered-but-not-kept",
                    test_a_texture_over_budget_is_delivered_but_not_kept);
    g_test_add_func("/thumb-loader/a-delivered-callback-is-destroyed-once",
                    test_a_delivered_callback_is_destroyed_once);
    g_test_add_func("/thumb-loader/a-cache-hit-destroys-its-callback-before-returning",
                    test_a_cache_hit_destroys_its_callback_before_returning);
    g_test_add_func("/thumb-loader/a-request-cancelled-before-dispatch-destroys-its-callback",
                    test_a_request_cancelled_before_dispatch_destroys_its_callback);
    g_test_add_func("/thumb-loader/a-request-cancelled-after-dispatch-destroys-its-callback",
                    test_a_request_cancelled_after_dispatch_destroys_its_callback);
    g_test_add_func("/thumb-loader/a-callback-a-worker-drops-is-destroyed-on-the-main-thread",
                    test_a_callback_a_worker_drops_is_destroyed_on_the_main_thread);
    g_test_add_func("/thumb-loader/shutdown-destroys-a-queued-callback",
                    test_shutdown_destroys_a_queued_callback);
    g_test_add_func("/thumb-loader/shutdown-destroys-a-posted-callback-without-the-main-loop",
                    test_shutdown_destroys_a_posted_callback_without_the_main_loop);
    g_test_add_func("/thumb-loader/a-request-after-shutdown-destroys-its-callback",
                    test_a_request_after_shutdown_destroys_its_callback);
    return g_test_run();
}
```

- [ ] **Step 3: Run the tests to confirm they do not compile**

```bash
cmake --build build/with-gtk --target test-thumb-loader -j 2>&1 | grep -m3 "error"
```

Expected: errors at the `thumb_loader_request` calls, because the API still
takes a function pointer and a `gpointer`. Record the first in the task
report.

- [ ] **Step 4: Swap the callback type only**

In `gtk/src/thumb-loader.h`, add `#include <functional>` and
`#include <string>`, and replace the `ThumbCallback` alias and the
`thumb_loader_request` declaration with:

```cpp
using ThumbCallback = std::move_only_function<void(ThumbResult, GdkTexture*) noexcept>;

void thumb_loader_request(ThumbLoader& loader, int64_t image_id, std::string sha256,
                          guint32 max_dim, GCancellable* cancellable, ThumbCallback cb);
```

In `gtk/src/thumb-loader.cpp`, change only what that forces: `ThumbRequest`'s
`ThumbCallback cb = nullptr;` and `gpointer user_data = nullptr;` become
`ThumbCallback cb;`; both calls in `deliver` drop their `req.user_data`
argument; `thumb_loader_request` takes `std::string sha256` and
`ThumbCallback cb`, calls `cb(THUMB_OK, cached.get())` on a hit, and stores
`req->sha256 = std::move(sha256);` and `req->cb = std::move(cb);`. Leave the
worker, the delivery and `thumb_loader_shutdown` as they are.

In `gtk/src/main.cpp`, make the bind site compile the same minimal way: in
`on_factory_bind`, replace

```cpp
    auto* data = new ThumbResultData{gref(item), app};

    thumb_loader_request(*app->loader, phig_image_row_get_id(row), phig_image_row_get_sha256(row),
                          phig_texture_size_for(app), cancellable, on_thumb_result, data);
```

with

```cpp
    thumb_loader_request(*app->loader, phig_image_row_get_id(row), phig_image_row_get_sha256(row),
                         phig_texture_size_for(app), cancellable,
                         [data = std::make_unique<ThumbResultData>(ThumbResultData{gref(item), app})](
                             ThumbResult result, GdkTexture* texture) mutable noexcept {
                             on_thumb_result(result, texture, data.release());
                         });
```

Step 7 replaces it properly.

- [ ] **Step 5: Run the tests to confirm what fails**

```bash
cmake --build build/with-gtk --target test-thumb-loader -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-thumb-loader 2>&1 | grep -E "^(ok|not ok)|ERROR"
```

Expected: the run stops at
`/thumb-loader/a-callback-a-worker-drops-is-destroyed-on-the-main-thread`,
because the worker destroys the cancelled request on its own thread, or at
`/thumb-loader/shutdown-destroys-a-posted-callback-without-the-main-loop`,
because shutdown leaves posted requests to an idle that has not run. Run each
of the two alone with `-p <path>` and record both failures in the task report.
The first 12 tests and the other six new ones pass.

- [ ] **Step 6: Destroy every request on the main thread**

Replace `gtk/src/thumb-loader.h` with:

```cpp
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "collection.h"

// Size requested when a caller passes max_dim 0. Matches phig's own default, so
// an unsized request hits the cache the CLI populates.
inline constexpr guint32 PHIG_THUMB_SIZE_DEFAULT = 512;

enum ThumbResult {
    THUMB_OK = 0,  // texture is non-NULL
    THUMB_MISS,    // the row has no sha256 yet; nothing to generate from
    THUMB_FAILED   // generation failed, or the cached file could not be loaded
};

// Runs on the main thread. texture is valid for the call; ref it to keep it.
// Not called at all if the request is cancelled before delivery.
//
// The loader destroys the callback exactly once, on the main thread, whether or
// not it ran: after delivery, or when the request is cancelled, dropped by
// thumb_loader_shutdown, or made after it. A capture may therefore hold the
// last reference to a GTK object.
using ThumbCallback = std::move_only_function<void(ThumbResult, GdkTexture*) noexcept>;

// An async, cancellable queue that finds a thumbnail through the collection,
// generating it on a miss, and loads it into a GdkTexture, backed by a
// byte-bounded LRU cache of textures.
//
// n_threads workers pull requests off the queue, look up or generate the file,
// and load it off the main thread. Every request goes back to the main thread
// through an idle source, delivered or not, and each idle holds a reference to
// the loader, so the loader outlives every delivery it has posted. cache_bytes
// bounds the texture cache, charged at width * height * 4 per texture; the
// least recently used entry is evicted until the total fits.
struct ThumbLoader;

// Caller keeps ownership of coll and must not close it before calling
// thumb_loader_shutdown: workers call into it from their own threads.
std::shared_ptr<ThumbLoader> thumb_loader_new(PhigCollection* coll, guint n_threads,
                                              gsize cache_bytes);

// Stops and joins every worker, then destroys every queued request and every
// request posted for delivery but not yet delivered, callbacks included, before
// it returns. Once it returns no thread is inside the collection and none will
// be again. Call it on the main thread, before closing coll, however many
// references to the loader remain: a posted delivery holds one. Safe to call
// more than once; the last reference also calls it.
void thumb_loader_shutdown(ThumbLoader& loader);

// Requests a thumbnail for image_id/sha256 (64 hex digits, or "" if phig has not
// hashed the row). image_id is what generation needs; the caller has it on the
// PhigImageRow, which saves reversing it from the sha.
//
// Call it on the main thread. The callback runs on the main thread too: from an
// idle source, or before this returns on a texture-cache hit. A rebind onto a
// texture already in memory costs no worker round-trip and no second decode,
// which matters because zoom re-requests every visible cell at a new size.
//
// A request made after thumb_loader_shutdown, or with a cancellable already
// cancelled, is dropped without a callback.
//
// max_dim is the largest edge in PHYSICAL pixels (cell size times the scale
// factor); 0 means the cache's native size. Textures are cached under
// (sha256, max_dim), so callers should quantise it, or the cache grows a
// generation per distinct size.
//
// A miss generates the thumbnail before replying: hundreds of milliseconds
// against about one for a hit. Replies THUMB_MISS if the row has no sha256,
// THUMB_FAILED if generation or loading fails.
void thumb_loader_request(ThumbLoader& loader, int64_t image_id, std::string sha256,
                          guint32 max_dim, GCancellable* cancellable, ThumbCallback cb);

// Test-only: bytes currently charged against cache_bytes.
gsize thumb_loader_cache_bytes(ThumbLoader& loader);

// Test-only: requests queued and not yet taken by a worker.
gint thumb_loader_queue_length(ThumbLoader& loader);

// Test-only: results posted for delivery that have not yet run, or been
// destroyed undelivered, on the main thread.
guint thumb_loader_pending_idle_count(ThumbLoader& loader);
```

Replace `gtk/src/thumb-loader.cpp` with:

```cpp
#include "thumb-loader.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gref.h"

namespace {

// One request. Exactly one place owns it at a time: the queue, then the worker
// that took it, then the delivery that carries it back to the main thread. It
// is destroyed only on the main thread, because its callback may hold the last
// reference to a GTK object.
struct ThumbRequest {
    int64_t image_id = 0;
    std::string sha256;
    guint32 max_dim = 0;  // 0 = the cache's native size
    GRef<GCancellable> cancellable;
    ThumbCallback cb;

    // Set by the worker before it hands the request back.
    ThumbResult result = THUMB_MISS;
    GRef<GdkTexture> texture;
};

bool is_cancelled(const ThumbRequest& req) {
    return req.cancellable != nullptr && g_cancellable_is_cancelled(req.cancellable.get());
}

struct CacheEntry {
    GRef<GdkTexture> texture;
    gsize bytes = 0;
    std::list<std::string>::iterator lru;  // this entry's node in ThumbLoader::lru
};

// "<sha256>@<size>": the same image is cached at every size the grid has asked
// for, so a zoom is never served a texture built for the previous size.
std::string cache_key(const std::string& sha256, guint32 size) {
    return sha256 + "@" + std::to_string(size);
}

struct Delivery;

}  // namespace

struct ThumbLoader {
    PhigCollection* coll = nullptr;  // borrowed; see thumb_loader_new
    gsize cache_bytes_budget = 0;
    std::weak_ptr<ThumbLoader> self;  // what a worker hands to the idle source it posts

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::unique_ptr<ThumbRequest>> queue;
    bool stopping = false;  // guarded by queue_mutex
    // Requests a worker finished while the last loader reference was being
    // dropped, so it had no reference to post them with. thumb_loader_shutdown
    // destroys them after the join. Guarded by queue_mutex.
    std::vector<std::unique_ptr<ThumbRequest>> orphans;
    std::vector<std::thread> workers;

    std::mutex cache_mutex;
    std::unordered_map<std::string, CacheEntry> cache;
    std::list<std::string> lru;  // front = least recently used
    gsize cache_bytes = 0;

    // Deliveries posted to the main context and not yet destroyed.
    // thumb_loader_shutdown takes their requests, so a callback is destroyed
    // even if the main context never runs again.
    std::mutex posted_mutex;
    std::unordered_set<Delivery*> posted;

    // Diagnostic only; nothing waits on it.
    std::atomic<guint> pending_idle{0};

    // Set by thumb_loader_shutdown. A delivery already on the main context
    // checks it and delivers nothing, since the caller may be mid-teardown.
    std::atomic<bool> stopped{false};

    ~ThumbLoader() { thumb_loader_shutdown(*this); }
};

namespace {

// Caller holds cache_mutex.
void evict_locked(ThumbLoader& self) {
    while (self.cache_bytes > self.cache_bytes_budget && !self.lru.empty()) {
        auto victim = self.cache.find(self.lru.front());
        self.cache_bytes -= victim->second.bytes;
        self.cache.erase(victim);
        self.lru.pop_front();
    }
}

// Caches texture and returns the reference to deliver. That reference is taken
// before evicting: a texture larger than the whole budget is evicted by its own
// insert, and is still delivered.
GRef<GdkTexture> cache_insert(ThumbLoader& self, const std::string& sha256, guint32 size,
                              GRef<GdkTexture> texture) {
    std::lock_guard lock(self.cache_mutex);
    std::string key = cache_key(sha256, size);
    if (auto it = self.cache.find(key); it != self.cache.end()) {
        // A concurrent request for the same sha and size got here first.
        self.lru.splice(self.lru.end(), self.lru, it->second.lru);
        return gref(it->second.texture.get());
    }
    GRef<GdkTexture> delivered = gref(texture.get());
    gsize bytes = static_cast<gsize>(gdk_texture_get_width(texture.get())) *
                  static_cast<gsize>(gdk_texture_get_height(texture.get())) * 4;
    self.lru.push_back(key);
    self.cache.emplace(std::move(key),
                       CacheEntry{std::move(texture), bytes, std::prev(self.lru.end())});
    self.cache_bytes += bytes;
    evict_locked(self);
    return delivered;
}

// A reference to the cached texture for (sha256, size), or null. A hit becomes
// the most recently used.
GRef<GdkTexture> cache_probe(ThumbLoader& self, const std::string& sha256, guint32 size) {
    std::lock_guard lock(self.cache_mutex);
    auto it = self.cache.find(cache_key(sha256, size));
    if (it == self.cache.end()) return nullptr;
    self.lru.splice(self.lru.end(), self.lru, it->second.lru);
    return gref(it->second.texture.get());
}

// What an idle source carries to the main thread. Holding the loader is what
// keeps it alive until the delivery runs or is destroyed undelivered.
struct Delivery {
    std::shared_ptr<ThumbLoader> loader;
    std::unique_ptr<ThumbRequest> req;  // null once thumb_loader_shutdown took it
};

gboolean deliver(gpointer data) noexcept {
    auto& delivery = *static_cast<Delivery*>(data);
    if (!delivery.req) return G_SOURCE_REMOVE;
    ThumbRequest& req = *delivery.req;
    if (delivery.loader->stopped.load() || is_cancelled(req)) return G_SOURCE_REMOVE;

    if (req.result == THUMB_OK) {
        GRef<GdkTexture> texture =
            cache_insert(*delivery.loader, req.sha256, req.max_dim, std::move(req.texture));
        req.cb(THUMB_OK, texture.get());
    } else {
        req.cb(req.result, nullptr);
    }
    return G_SOURCE_REMOVE;
}

// Runs exactly once per posted request, after deliver or instead of it, on the
// main thread. Deleting the Delivery destroys the request, and may drop the
// last reference to the loader, which then joins its workers here.
void delivery_destroy(gpointer data) noexcept {
    auto* delivery = static_cast<Delivery*>(data);
    ThumbLoader& loader = *delivery->loader;
    {
        std::lock_guard lock(loader.posted_mutex);
        loader.posted.erase(delivery);
    }
    loader.pending_idle--;
    delete delivery;
}

void post_result(std::shared_ptr<ThumbLoader> loader, std::unique_ptr<ThumbRequest> req) {
    ThumbLoader& self = *loader;
    auto* delivery = new Delivery{std::move(loader), std::move(req)};
    {
        std::lock_guard lock(self.posted_mutex);
        self.posted.insert(delivery);
    }
    self.pending_idle++;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver, delivery, delivery_destroy);
}

// Every request a worker takes leaves through here, resolved or not, so it is
// destroyed on the main thread: by its delivery, or by thumb_loader_shutdown.
void hand_back(ThumbLoader& self, std::unique_ptr<ThumbRequest> req) {
    if (std::shared_ptr<ThumbLoader> loader = self.self.lock()) {
        post_result(std::move(loader), std::move(req));
        return;
    }
    std::lock_guard lock(self.queue_mutex);
    self.orphans.push_back(std::move(req));
}

PhigThumbLookup lookup(ThumbLoader& self, const ThumbRequest& req, int size, std::string& path) {
    char* raw = nullptr;
    PhigThumbLookup look = phig_collection_thumb_lookup(self.coll, req.sha256.c_str(), size, &raw);
    path = raw != nullptr ? raw : "";
    g_free(raw);
    return look;
}

// Looks the thumbnail up, generates it on a miss, and loads it, setting
// req.result and req.texture. Returns without generating if the request was
// cancelled first.
void resolve(ThumbLoader& self, ThumbRequest& req) {
    // Sizes are independent in phig's cache. The caller quantises max_dim, so
    // this is a small set of sizes rather than one per pixel.
    int want = static_cast<int>(req.max_dim > 0 ? req.max_dim : PHIG_THUMB_SIZE_DEFAULT);
    std::string path;
    PhigThumbLookup look = lookup(self, req, want, path);

    if (look == PHIG_THUMB_ABSENT) {
        // Generation costs hundreds of milliseconds against about one for a
        // load, so a request the user has scrolled past stops here.
        if (is_cancelled(req)) return;
        if (!phig_collection_thumb_generate(self.coll, req.image_id, want)) {
            req.result = THUMB_FAILED;
            return;
        }
        look = lookup(self, req, want, path);
    }

    if (look == PHIG_THUMB_ERROR) {
        req.result = THUMB_FAILED;
    } else if (look == PHIG_THUMB_ABSENT) {
        req.result = THUMB_MISS;
    } else {
        GError* error = nullptr;
        // phig wrote the file at the size asked for, so it loads without a
        // rescale, which keeps the texture as small as the budget assumes.
        GdkTexture* texture = gdk_texture_new_from_filename(path.c_str(), &error);
        if (texture == nullptr) {
            g_warning("thumb loader: failed to load %s: %s", path.c_str(),
                      error != nullptr ? error->message : "unknown error");
            g_clear_error(&error);
            req.result = THUMB_FAILED;
        } else {
            req.result = THUMB_OK;
            req.texture = adopt(texture);
        }
    }
}

void worker_main(ThumbLoader& self) {
    for (;;) {
        std::unique_ptr<ThumbRequest> req;
        {
            std::unique_lock lock(self.queue_mutex);
            self.queue_cv.wait(lock, [&self] { return self.stopping || !self.queue.empty(); });
            if (self.stopping) return;
            req = std::move(self.queue.front());
            self.queue.pop_front();
        }

        // A request cancelled before or during resolving goes back unresolved;
        // its delivery sees the cancel and does not call back. A row phig has
        // not hashed has no cache key: that is data phig has not produced, not
        // a failure, so it reports like a not-yet-generated miss.
        if (!is_cancelled(*req)) {
            if (req->sha256.empty())
                req->result = THUMB_MISS;
            else
                resolve(self, *req);
        }
        hand_back(self, std::move(req));
    }
}

}  // namespace

std::shared_ptr<ThumbLoader> thumb_loader_new(PhigCollection* coll, guint n_threads,
                                              gsize cache_bytes) {
    auto loader = std::make_shared<ThumbLoader>();
    loader->coll = coll;
    loader->cache_bytes_budget = cache_bytes;
    loader->self = loader;
    loader->workers.reserve(n_threads);
    for (guint i = 0; i < n_threads; i++)
        loader->workers.emplace_back(worker_main, std::ref(*loader));
    return loader;
}

void thumb_loader_shutdown(ThumbLoader& self) {
    self.stopped = true;
    // Requests are moved out under the locks and destroyed after this function's
    // last lock is released, so no callback's destructor runs under one.
    std::deque<std::unique_ptr<ThumbRequest>> queued;
    {
        std::lock_guard lock(self.queue_mutex);
        self.stopping = true;
        queued.swap(self.queue);
    }
    self.queue_cv.notify_all();
    for (std::thread& worker : self.workers) worker.join();
    self.workers.clear();

    std::vector<std::unique_ptr<ThumbRequest>> dropped;
    {
        std::lock_guard lock(self.queue_mutex);
        dropped.swap(self.orphans);
    }
    {
        std::lock_guard lock(self.posted_mutex);
        for (Delivery* delivery : self.posted)
            if (delivery->req) dropped.push_back(std::move(delivery->req));
    }
}

void thumb_loader_request(ThumbLoader& self, int64_t image_id, std::string sha256, guint32 max_dim,
                          GCancellable* cancellable, ThumbCallback cb) {
    if (self.stopped.load()) return;
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) return;

    if (GRef<GdkTexture> cached = cache_probe(self, sha256, max_dim)) {
        cb(THUMB_OK, cached.get());
        return;
    }

    auto req = std::make_unique<ThumbRequest>();
    req->image_id = image_id;
    req->sha256 = std::move(sha256);
    req->max_dim = max_dim;
    req->cancellable = gref(cancellable);
    req->cb = std::move(cb);
    {
        std::lock_guard lock(self.queue_mutex);
        // req is declared before the lock, so on this return it is destroyed
        // after the lock is released.
        if (self.stopping) return;
        self.queue.push_back(std::move(req));
    }
    self.queue_cv.notify_one();
}

gsize thumb_loader_cache_bytes(ThumbLoader& self) {
    std::lock_guard lock(self.cache_mutex);
    return self.cache_bytes;
}

gint thumb_loader_queue_length(ThumbLoader& self) {
    std::lock_guard lock(self.queue_mutex);
    return static_cast<gint>(self.queue.size());
}

guint thumb_loader_pending_idle_count(ThumbLoader& self) { return self.pending_idle.load(); }
```

- [ ] **Step 7: Capture the list item in the callback**

In `gtk/src/main.cpp`, delete the `ThumbResultData` struct and the comment
above it (`/* Bundles the two things on_thumb_result needs: …`). Replace the
start of `on_thumb_result`, from its comment through the line
`PhigApp* app = data->app;`, with:

```cpp
/* Delivered on the main thread by ThumbLoader. Guarded on the cancellable
 * stored on the item, not on the result: if the cell has since been unbound
 * (and its cancellable cancelled), a stale success for a now-different row
 * must not be shown. */
static void on_thumb_result(PhigApp* app, GtkListItem* item, ThumbResult result,
                            GdkTexture* texture) noexcept {
```

The body from `auto* cancellable = …` on is unchanged. In `on_factory_bind`,
replace the comment above it with:

```cpp
/* bind: a placeholder row (page not fetched yet) gets the grey state and
 * nothing else. A real row gets a cancellable stashed on the item and a
 * thumbnail request whose callback holds a reference to the item, so the item
 * outlives the hop to a worker thread and back whether or not GTK still has it
 * bound when the result arrives. */
```

and replace the Step 4 request with:

```cpp
    thumb_loader_request(*app->loader, phig_image_row_get_id(row), phig_image_row_get_sha256(row),
                         phig_texture_size_for(app), cancellable,
                         [app, item = gref(item)](ThumbResult result, GdkTexture* texture) noexcept {
                             on_thumb_result(app, item.get(), result, texture);
                         });
```

- [ ] **Step 8: Run the full gtk check**

Run `gtk_check` from Global Constraints. Expected: no warnings, `100% tests
passed` in both builds, and `test-thumb-loader` reports 20 `ok`.

- [ ] **Step 9: Commit**

```bash
git add gtk/src/thumb-loader.h gtk/src/thumb-loader.cpp gtk/src/main.cpp \
        gtk/tests/test-thumb-loader.cpp gtk/tests/fake-collection.h gtk/tests/fake-collection.cpp
git commit -m "Make the thumbnail loader own and destroy its callbacks

A request cancelled after dispatch never called back, so the ThumbResultData
and GtkListItem reference that on_factory_bind passed with it leaked. The
callback is now a std::move_only_function the loader destroys exactly once, on
the main thread: workers hand cancelled requests back through the delivery
idle, and shutdown takes the requests out of deliveries that have not run.
The bind site captures a GRef to the list item directly."
```

---

### Task 3: Rename the C files

**Files:**
- Rename: `gtk/src/gallery-model.c` → `gtk/src/gallery-model.cpp`
- Rename: `gtk/src/row-page.c` → `gtk/src/row-page.cpp`
- Rename: `gtk/tests/test-gallery-model.c` → `gtk/tests/test-gallery-model.cpp`
- Modify: `gtk/CMakeLists.txt`

**Interfaces:** none change. This task fixes only what the C++ compiler
rejects, so the rename and the rewrite review separately.

- [ ] **Step 1: Rename**

```bash
git mv gtk/src/gallery-model.c gtk/src/gallery-model.cpp
git mv gtk/src/row-page.c gtk/src/row-page.cpp
git mv gtk/tests/test-gallery-model.c gtk/tests/test-gallery-model.cpp
```

- [ ] **Step 2: Update the build**

In `gtk/CMakeLists.txt`, replace `LANGUAGES C CXX` with `LANGUAGES CXX`,
delete the two `CMAKE_C_STANDARD` lines and the blank line after them, and
replace every `src/gallery-model.c`, `src/row-page.c` and
`tests/test-gallery-model.c` with the `.cpp` name.

- [ ] **Step 3: See what the compiler rejects**

```bash
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON
cmake --build build/with-gtk --target gtk-all -j 2>&1 | grep -E "error|warning" | sed 's/^.*gtk\///'
```

Expected, measured while writing this plan (line numbers are the files'
current ones):

- `src/gallery-model.cpp`, lines 79, 86, 205, 221, 235, 315, 337, 360, 361,
  410, 475: `invalid conversion from 'gpointer' {aka 'void*'}`.
- `tests/test-gallery-model.cpp`, lines 45, 99, 100, 101, 132, 141, 182: the
  same error; line 156: three `missing initializer` warnings.
- `src/row-page.cpp`: nothing.

- [ ] **Step 4: Fix each with a cast or an initializer**

`gallery-model.cpp`:
- 79 and 86: `PhigImageRow* row = g_object_new(PHIG_TYPE_IMAGE_ROW, NULL);`
  becomes `PhigImageRow* row = PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, NULL));`
- 205: `PhigPage* page = value;` becomes `PhigPage* page = static_cast<PhigPage*>(value);`
- 221: `PhigPlaceholderEntry* entry =` gets `static_cast<PhigPlaceholderEntry*>(`
  around the `g_hash_table_lookup(...)` call.
- 235: `PhigPlaceholderEntry* candidate = value;` becomes
  `PhigPlaceholderEntry* candidate = static_cast<PhigPlaceholderEntry*>(value);`
- 315 and 361: `PhigGalleryModel* self = g_weak_ref_get(&data->model);` becomes
  `PhigGalleryModel* self = static_cast<PhigGalleryModel*>(g_weak_ref_get(&data->model));`
- 337: `FetchTaskData* data = task_data;` becomes
  `FetchTaskData* data = static_cast<FetchTaskData*>(task_data);`
- 360: `FetchTaskData* data = g_task_get_task_data(task);` becomes
  `FetchTaskData* data = static_cast<FetchTaskData*>(g_task_get_task_data(task));`
- 410: `PhigPage* page = g_hash_table_lookup(...)` gets
  `static_cast<PhigPage*>(` around the call.
- 475: `PhigGalleryModel* self = g_object_new(PHIG_TYPE_GALLERY_MODEL, NULL);`
  becomes `PhigGalleryModel* self = PHIG_GALLERY_MODEL(g_object_new(PHIG_TYPE_GALLERY_MODEL, NULL));`

`test-gallery-model.cpp`:
- 45, 99, 100, 101: `GObject* x = g_list_model_get_item(...)` gets
  `static_cast<GObject*>(` around the call.
- 132: `ItemsChangedCapture* capture = user_data;` becomes
  `ItemsChangedCapture* capture = static_cast<ItemsChangedCapture*>(user_data);`
- 141: `g_main_loop_quit(user_data);` becomes
  `g_main_loop_quit(static_cast<GMainLoop*>(user_data));`
- 156: `ItemsChangedCapture capture = {.loop = loop, .fired = FALSE};` becomes
  `ItemsChangedCapture capture = {.loop = loop, .fired = FALSE, .position = 0, .removed = 0, .added = 0};`
- 182: `FailedCapture* capture = user_data;` becomes
  `FailedCapture* capture = static_cast<FailedCapture*>(user_data);`

Nothing else changes. If the compiler rejects a line this list does not name,
fix it the same minimal way and list it in the task report.

- [ ] **Step 5: Run the full gtk check**

Reconfigure the ASan build first, since its source list changed:

```bash
cmake -B build/with-gtk-asan -DPHIG_BUILD_GTK=ON
```

Then run `gtk_check`. Expected: no warnings and `100% tests passed` in both.

- [ ] **Step 6: Commit**

```bash
git add gtk/CMakeLists.txt gtk/src/gallery-model.cpp gtk/src/row-page.cpp gtk/tests/test-gallery-model.cpp
git commit -m "Compile gtk's last three C files as C++

A rename plus the casts and initializers C++ requires and C did not. The
gtk project no longer enables C."
```

---

### Task 4: The C++ seam

**Files:**
- Modify: `gtk/src/collection.h` (rewrite), `gtk/src/collection.cpp` (rewrite)
- Delete: `gtk/src/row-page.cpp`
- Modify: `gtk/tests/fake-collection.h` (rewrite), `gtk/tests/fake-collection.cpp` (rewrite)
- Modify: `gtk/tests/test-collection.cpp` (rewrite)
- Modify: `gtk/src/gallery-model.h` (rewrite), `gtk/src/gallery-model.cpp`
- Modify: `gtk/src/thumb-loader.h`, `gtk/src/thumb-loader.cpp`
- Modify: `gtk/tests/test-gallery-model.cpp`, `gtk/tests/test-thumb-loader.cpp`
- Modify: `gtk/src/main.cpp`, `gtk/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2's `ThumbCallback` and `thumb_loader_request`.
- Produces: the `phig::gtk` seam below; `phig::gtk::fake::{make, sha,
  fail_pages, kPageError, generate_count, hold_generation}`;
  `thumb_loader_new(phig::gtk::Collection&, guint, gsize)`;
  `phig_gallery_model_new(phig::gtk::Collection&)`; and a
  `page-fetch-failed` signal with one `const char*` argument, the reason.

- [ ] **Step 1: Write the seam header**

Replace `gtk/src/collection.h` with:

```cpp
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The gtk client's only route into phig. src/collection.cpp implements it over
// core/; the tests link tests/fake-collection.cpp instead. Every function may be
// called from any thread, and none throws: a failure comes back as a value, with
// phig's reason where the call can fail.
//
// Reads are paged: no call returns the whole collection. A full-collection read
// through core/ carries every row's EXIF blob, about 3.8 KB a row that the
// client never reads.
namespace phig::gtk {

struct Row {
    int64_t id = 0;
    std::string path;
    std::string sha256;  // empty when phig has not hashed the row
    std::string filename;
};

struct Collection;  // defined by collection.cpp, or by the fake
struct CollectionClose {
    void operator()(Collection* coll) const noexcept;
};
using CollectionPtr = std::unique_ptr<Collection, CollectionClose>;

// Opens the catalog in catalog_dir read-only and installs it as the process's
// catalog, so thumbnail lookups resolve inside it. Fails if the catalog has no
// database or its schema is not the version this build reads. Never creates a
// file.
std::expected<CollectionPtr, std::string> open(const std::string& catalog_dir) noexcept;

// The catalog phig uses when none is named: PHIG_CATALOG, then the config
// file's [paths] catalog, then $XDG_DATA_HOME/phig. Fails if the config file
// cannot be read or names an empty catalog.
std::expected<std::string, std::string> default_catalog() noexcept;

// Rows are in gallery order: newest effective date first, id breaking ties.
std::expected<uint32_t, std::string> count(Collection& coll) noexcept;

// Rows [offset, offset + limit) in gallery order. A page past the end is empty.
std::expected<std::vector<Row>, std::string> page(Collection& coll, uint32_t offset,
                                                  uint32_t limit) noexcept;

enum class ThumbLookup {
    Present,  // the file at path exists
    Absent,   // path is where it would be; generate to create it
    Invalid   // sha256 or size is not valid; path is empty
};

struct ThumbLocation {
    ThumbLookup state = ThumbLookup::Invalid;
    std::string path;
};

// Where the cached thumbnail for (sha256, size) is.
ThumbLocation thumb_lookup(Collection& coll, std::string_view sha256, int size) noexcept;

// Makes sure row image_id has a cached thumbnail at size, generating it if
// absent. Mirrors core/'s EnsureThumb: NoSuchRow covers an unknown id and an
// unhashed row, SourceMissing a row whose file is gone.
enum class ThumbGenerate { Ok, NoSuchRow, SourceMissing, Failed };
ThumbGenerate thumb_generate(Collection& coll, int64_t image_id, int size) noexcept;

}  // namespace phig::gtk
```

- [ ] **Step 2: Rewrite the seam's test**

Replace `gtk/tests/test-collection.cpp` with:

```cpp
#include <glib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "collection.h"
#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A database of three rows dated 2021, 2023 and 2022 in insert order, so the
// gallery's newest-first order differs from insertion order. The 2023 row is a
// real JPEG, so a thumbnail can be generated from it. XDG_DATA_HOME needs no
// redirect: the catalog is the temp dir, so generated thumbnails land inside it.
struct Fixture {
    fs::path dir;
    std::string db_path;

    Fixture() {
        char* tmp = g_dir_make_tmp("phig-gtk-collection-XXXXXX", nullptr);
        g_assert_nonnull(tmp);
        dir = tmp;
        g_free(tmp);
        db_path = (dir / "phig.db").string();

        Database db(db_path);
        db.init_schema();
        insert(db, "/nowhere/a.jpg", "2021-01-01T00:00:00Z", std::string(64, 'a'));
        insert(db, PHIG_SOURCE_DIR "/tests/data/faces_small.jpg", "2023-01-01T00:00:00Z",
               std::string(64, 'b'));
        insert(db, "/nowhere/c.jpg", "2022-01-01T00:00:00Z", std::string(64, 'c'));
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    static void insert(Database& db, const std::string& path, const char* modified_at,
                       const std::string& sha) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.modified_at = modified_at;
        info.sha256 = sha;
        db.insert(info);
    }

    phig::gtk::CollectionPtr open() const {
        auto coll = phig::gtk::open(dir.string());
        if (!coll) g_error("open failed: %s", coll.error().c_str());
        return std::move(*coll);
    }
};

void test_open_reports_a_missing_catalog_without_creating_it() {
    Fixture f;
    fs::path absent = f.dir / "absent";
    auto coll = phig::gtk::open(absent.string());
    g_assert_false(coll.has_value());
    g_assert_false(coll.error().empty());
    g_assert_false(fs::exists(absent));
}

void test_pages_come_newest_first() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();

    auto n = phig::gtk::count(*coll);
    g_assert_true(n.has_value());
    g_assert_cmpuint(*n, ==, 3);

    const std::string sha_b(64, 'b');
    const std::string sha_c(64, 'c');
    auto page = phig::gtk::page(*coll, 0, 2);
    g_assert_true(page.has_value());
    g_assert_cmpuint(page->size(), ==, 2);
    g_assert_cmpstr((*page)[0].sha256.c_str(), ==, sha_b.c_str());
    g_assert_cmpstr((*page)[0].filename.c_str(), ==, "faces_small.jpg");
    g_assert_cmpstr((*page)[1].sha256.c_str(), ==, sha_c.c_str());

    page = phig::gtk::page(*coll, 2, 2);
    g_assert_true(page.has_value());
    g_assert_cmpuint(page->size(), ==, 1);
    g_assert_cmpstr((*page)[0].path.c_str(), ==, "/nowhere/a.jpg");

    page = phig::gtk::page(*coll, 3, 2);
    g_assert_true(page.has_value());
    g_assert_true(page->empty());
}

void test_a_thumbnail_is_generated_on_demand() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const std::string sha(64, 'b');

    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*coll, sha, 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Absent);
    g_assert_true(g_str_has_prefix(found.path.c_str(), f.dir.c_str()));

    const int64_t id = phig::gtk::page(*coll, 0, 1)->front().id;
    g_assert_true(phig::gtk::thumb_generate(*coll, id, 256) == phig::gtk::ThumbGenerate::Ok);

    found = phig::gtk::thumb_lookup(*coll, sha, 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Present);
    g_assert_true(g_file_test(found.path.c_str(), G_FILE_TEST_EXISTS));
    g_assert_true(found.path.rfind((f.dir / "thumbs").string(), 0) == 0);
}

void test_generation_names_a_missing_source_and_an_unknown_id() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const int64_t missing = phig::gtk::page(*coll, 2, 1)->front().id;  // /nowhere/a.jpg
    g_assert_true(phig::gtk::thumb_generate(*coll, missing, 256) ==
                  phig::gtk::ThumbGenerate::SourceMissing);
    g_assert_true(phig::gtk::thumb_generate(*coll, 999999, 256) ==
                  phig::gtk::ThumbGenerate::NoSuchRow);
}

void test_lookup_rejects_a_malformed_sha() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*coll, "abc", 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Invalid);
    g_assert_true(found.path.empty());
}

void test_default_catalog_follows_the_environment() {
    // A config directory with no config file, so a developer's real
    // config.ini cannot decide the answer.
    const std::string config_home =
        "/tmp/phig-gtk-xdg-config-" + std::to_string(static_cast<long>(getpid()));
    g_setenv("XDG_CONFIG_HOME", config_home.c_str(), TRUE);
    g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
    g_unsetenv("PHIG_CATALOG");

    auto dir = phig::gtk::default_catalog();
    g_assert_true(dir.has_value());
    g_assert_cmpstr(dir->c_str(), ==, "/tmp/phig-gtk-xdg-data/phig");

    g_setenv("PHIG_CATALOG", "/tmp/phig-gtk-elsewhere", TRUE);
    dir = phig::gtk::default_catalog();
    g_assert_true(dir.has_value());
    g_assert_cmpstr(dir->c_str(), ==, "/tmp/phig-gtk-elsewhere");

    g_unsetenv("PHIG_CATALOG");
    g_unsetenv("XDG_DATA_HOME");
    g_unsetenv("XDG_CONFIG_HOME");
}

void test_default_catalog_reports_why_it_cannot_be_resolved() {
    const fs::path config_home =
        "/tmp/phig-gtk-xdg-config-empty-" + std::to_string(static_cast<long>(getpid()));
    fs::create_directories(config_home / "phig");
    std::ofstream(config_home / "phig" / "config.ini") << "[paths]\ncatalog =\n";
    g_setenv("XDG_CONFIG_HOME", config_home.c_str(), TRUE);
    g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
    g_unsetenv("PHIG_CATALOG");

    auto dir = phig::gtk::default_catalog();
    g_assert_false(dir.has_value());
    g_assert_false(dir.error().empty());

    g_unsetenv("XDG_DATA_HOME");
    g_unsetenv("XDG_CONFIG_HOME");
    std::error_code ec;
    fs::remove_all(config_home, ec);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/collection/open-reports-a-missing-catalog-without-creating-it",
                    test_open_reports_a_missing_catalog_without_creating_it);
    g_test_add_func("/collection/pages-come-newest-first", test_pages_come_newest_first);
    g_test_add_func("/collection/a-thumbnail-is-generated-on-demand",
                    test_a_thumbnail_is_generated_on_demand);
    g_test_add_func("/collection/generation-names-a-missing-source-and-an-unknown-id",
                    test_generation_names_a_missing_source_and_an_unknown_id);
    g_test_add_func("/collection/lookup-rejects-a-malformed-sha",
                    test_lookup_rejects_a_malformed_sha);
    g_test_add_func("/collection/default-catalog-follows-the-environment",
                    test_default_catalog_follows_the_environment);
    g_test_add_func("/collection/default-catalog-reports-why-it-cannot-be-resolved",
                    test_default_catalog_reports_why_it_cannot_be_resolved);
    return g_test_run();
}
```

- [ ] **Step 3: Confirm it does not compile**

```bash
cmake --build build/with-gtk --target test-collection -j 2>&1 | grep -m3 "error"
```

Expected: errors naming `phig::gtk::open` or `phig::gtk::page`, since
`collection.cpp` still defines the C functions.

- [ ] **Step 4: Rewrite the implementation**

Replace `gtk/src/collection.cpp` with:

```cpp
#include "collection.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <utility>

#include "catalog.h"
#include "config.h"
#include "database.h"
#include "paths.h"
#include "thumbs.h"
#include "thumbs_engine.h"
#include "types.h"

namespace phig::gtk {

struct Collection {
    std::unique_ptr<Database> db;
};

void CollectionClose::operator()(Collection* coll) const noexcept { delete coll; }

namespace {

// The current exception's message, or fallback when it carries none. Call only
// inside a catch block.
std::string caught(const char* fallback) noexcept {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return fallback;
    }
}

// A thumbnail path is built from the hash, so it must be exactly 64 hex digits.
bool is_hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c)) != 0;
           });
}

}  // namespace

std::expected<CollectionPtr, std::string> open(const std::string& catalog_dir) noexcept {
    try {
        const CatalogPaths paths = catalog_paths(catalog_dir);
        CollectionPtr coll(new Collection{open_catalog_db(paths.db, CatalogAccess::Read)});
        set_catalog(paths);
        migrate_legacy_catalog_files(paths);
        return coll;
    } catch (...) {
        return std::unexpected(caught("Could not open the phig catalog."));
    }
}

std::expected<std::string, std::string> default_catalog() noexcept {
    try {
        const Config cfg = load_config(config_path());
        return resolve_catalog_dir(std::nullopt, cfg).dir;
    } catch (...) {
        return std::unexpected(caught("Could not resolve the default catalog."));
    }
}

std::expected<uint32_t, std::string> count(Collection& coll) noexcept {
    try {
        const int64_t n = coll.db->count_matching(Database::SearchCriteria{});
        return n > 0 ? static_cast<uint32_t>(n) : 0u;
    } catch (...) {
        return std::unexpected(caught("Could not count the catalog's rows."));
    }
}

std::expected<std::vector<Row>, std::string> page(Collection& coll, uint32_t offset,
                                                  uint32_t limit) noexcept {
    try {
        Database::PageCriteria c;
        c.sort = Database::SortKey::Date;
        c.descending = true;
        c.offset = offset;
        c.limit = static_cast<int32_t>(limit);
        std::vector<Row> rows;
        for (ImageInfo& info : coll.db->search_page(c))
            rows.push_back(Row{info.id, std::move(info.path), std::move(info.sha256),
                               std::move(info.filename)});
        return rows;
    } catch (...) {
        return std::unexpected(caught("Could not read a page of the catalog."));
    }
}

ThumbLocation thumb_lookup(Collection& coll, std::string_view sha256, int size) noexcept {
    (void)coll;  // the thumbnail cache is located by core/paths, not by the database
    try {
        if (size <= 0 || !is_hex64(sha256)) return {};
        const std::string sha(sha256);
        std::string path = get_thumb_path(sha, size);
        if (path.empty()) return {};
        const ThumbLookup state = thumb_exists(sha, size) ? ThumbLookup::Present : ThumbLookup::Absent;
        return {state, std::move(path)};
    } catch (...) {
        g_warning("phig collection: thumbnail lookup failed: %s", caught("unknown error").c_str());
    }
    return {};
}

ThumbGenerate thumb_generate(Collection& coll, int64_t image_id, int size) noexcept {
    if (size <= 0) return ThumbGenerate::Failed;
    try {
        switch (ensure_thumb(*coll.db, image_id, size)) {
            case EnsureThumb::Ok:
                return ThumbGenerate::Ok;
            case EnsureThumb::NoSuchRow:
                return ThumbGenerate::NoSuchRow;
            case EnsureThumb::SourceMissing:
                return ThumbGenerate::SourceMissing;
            case EnsureThumb::Failed:
                return ThumbGenerate::Failed;
        }
    } catch (...) {
        g_warning("phig collection: thumbnail generation failed: %s",
                  caught("unknown error").c_str());
    }
    return ThumbGenerate::Failed;
}

}  // namespace phig::gtk
```

Then delete `gtk/src/row-page.cpp`:

```bash
git rm gtk/src/row-page.cpp
```

In `gtk/CMakeLists.txt`, remove every `src/row-page.cpp`.

- [ ] **Step 5: Run the seam's test**

```bash
cmake --build build/with-gtk --target test-collection -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-collection
```

Expected: 7 `ok`. The other gtk targets do not build yet.

- [ ] **Step 6: Rewrite the fake**

Replace `gtk/tests/fake-collection.h` with:

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "collection.h"

// A Collection with synthetic rows and no database, for the gtk tests.
namespace phig::gtk::fake {

// n_rows rows. Row i (0-based) has id i + 1, path "/fake/<i>.jpg", filename
// "<i>.jpg" and the sha256 sha(i) gives. Thumbnails live in a private temp
// directory that closing the collection removes. Generating one writes a 64x64
// PNG; generation reports NoSuchRow for an id outside [1, n_rows].
CollectionPtr make(uint32_t n_rows);

// Row index's sha256: the index in hex, zero-padded to 64 digits.
std::string sha(uint32_t index);

// The reason page gives while fail_pages is set.
inline constexpr const char* kPageError = "fake: page fetch failed";

// While fail is set, page fails with kPageError.
void fail_pages(Collection& coll, bool fail);

// How many thumbnail files thumb_generate has written.
uint32_t generate_count(Collection& coll);

// While hold is set, thumb_generate waits before generating.
void hold_generation(Collection& coll, bool hold);

}  // namespace phig::gtk::fake
```

Replace `gtk/tests/fake-collection.cpp` with:

```cpp
#include "fake-collection.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string_view>

namespace phig::gtk {

struct Collection {
    uint32_t n_rows = 0;
    std::filesystem::path thumb_dir;
    std::atomic<bool> fail_pages{false};
    std::atomic<uint32_t> generate_count{0};
    std::mutex hold_mutex;
    std::condition_variable hold_cv;
    bool hold = false;  // guarded by hold_mutex
};

void CollectionClose::operator()(Collection* coll) const noexcept {
    std::error_code ec;
    std::filesystem::remove_all(coll->thumb_dir, ec);
    delete coll;
}

namespace {

constexpr int kThumbEdge = 64;

bool is_hex64(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return g_ascii_isxdigit(c) != FALSE;
           });
}

std::filesystem::path thumb_path(const Collection& coll, std::string_view sha256, int size) {
    return coll.thumb_dir / std::to_string(size) / (std::string(sha256) + ".png");
}

// Writes a solid width x height PNG to path, atomically.
bool write_png(const std::filesystem::path& path, int width, int height) {
    std::string pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, '\x80');
    GBytes* bytes = g_bytes_new(pixels.data(), pixels.size());
    GdkTexture* texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes,
                                                 static_cast<gsize>(width) * 4);
    GBytes* png = gdk_texture_save_to_png_bytes(texture);
    gsize size = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(png, &size));
    gboolean ok = g_file_set_contents(path.c_str(), data, static_cast<gssize>(size), nullptr);
    g_bytes_unref(png);
    g_object_unref(texture);
    g_bytes_unref(bytes);
    return ok;
}

}  // namespace

std::expected<uint32_t, std::string> count(Collection& coll) noexcept { return coll.n_rows; }

std::expected<std::vector<Row>, std::string> page(Collection& coll, uint32_t offset,
                                                  uint32_t limit) noexcept {
    if (coll.fail_pages.load()) return std::unexpected(std::string(fake::kPageError));
    const uint32_t end = offset >= coll.n_rows ? offset : std::min(coll.n_rows, offset + limit);
    std::vector<Row> rows;
    rows.reserve(end - offset);
    for (uint32_t i = offset; i < end; i++)
        rows.push_back(Row{static_cast<int64_t>(i) + 1, "/fake/" + std::to_string(i) + ".jpg",
                           fake::sha(i), std::to_string(i) + ".jpg"});
    return rows;
}

ThumbLocation thumb_lookup(Collection& coll, std::string_view sha256, int size) noexcept {
    if (!is_hex64(sha256) || size <= 0) return {};
    std::filesystem::path p = thumb_path(coll, sha256, size);
    std::error_code ec;
    const ThumbLookup state =
        std::filesystem::exists(p, ec) ? ThumbLookup::Present : ThumbLookup::Absent;
    return {state, p.string()};
}

ThumbGenerate thumb_generate(Collection& coll, int64_t image_id, int size) noexcept {
    if (size <= 0) return ThumbGenerate::Failed;
    if (image_id < 1 || image_id > static_cast<int64_t>(coll.n_rows)) return ThumbGenerate::NoSuchRow;
    {
        std::unique_lock lock(coll.hold_mutex);
        coll.hold_cv.wait(lock, [&coll] { return !coll.hold; });
    }
    std::filesystem::path p = thumb_path(coll, fake::sha(static_cast<uint32_t>(image_id - 1)), size);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return ThumbGenerate::Ok;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!write_png(p, kThumbEdge, kThumbEdge)) return ThumbGenerate::Failed;
    coll.generate_count++;
    return ThumbGenerate::Ok;
}

namespace fake {

CollectionPtr make(uint32_t n_rows) {
    CollectionPtr coll(new Collection());
    coll->n_rows = n_rows;
    char* dir = g_dir_make_tmp("phig-gtk-fake-XXXXXX", nullptr);
    g_assert_nonnull(dir);
    coll->thumb_dir = dir;
    g_free(dir);
    return coll;
}

std::string sha(uint32_t index) {
    char out[65];
    g_snprintf(out, sizeof out, "%064x", index);
    return out;
}

void fail_pages(Collection& coll, bool fail) { coll.fail_pages = fail; }

uint32_t generate_count(Collection& coll) { return coll.generate_count.load(); }

void hold_generation(Collection& coll, bool hold) {
    {
        std::lock_guard lock(coll.hold_mutex);
        coll.hold = hold;
    }
    coll.hold_cv.notify_all();
}

}  // namespace fake

}  // namespace phig::gtk
```

- [ ] **Step 7: Put the loader on the seam**

In `gtk/src/thumb-loader.h`, replace the `thumb_loader_new` declaration and
its comment with:

```cpp
// Caller keeps ownership of coll and must not close it before calling
// thumb_loader_shutdown: workers call into it from their own threads.
std::shared_ptr<ThumbLoader> thumb_loader_new(phig::gtk::Collection& coll, guint n_threads,
                                              gsize cache_bytes);
```

In `gtk/src/thumb-loader.cpp`:
- `PhigCollection* coll = nullptr;  // borrowed; see thumb_loader_new` becomes
  `phig::gtk::Collection* coll = nullptr;  // borrowed; see thumb_loader_new`.
- Delete the `lookup` helper.
- Replace `resolve` with:

```cpp
// Looks the thumbnail up, generates it on a miss, and loads it, setting
// req.result and req.texture. Returns without generating if the request was
// cancelled first.
void resolve(ThumbLoader& self, ThumbRequest& req) {
    // Sizes are independent in phig's cache. The caller quantises max_dim, so
    // this is a small set of sizes rather than one per pixel.
    const int want = static_cast<int>(req.max_dim > 0 ? req.max_dim : PHIG_THUMB_SIZE_DEFAULT);
    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*self.coll, req.sha256, want);

    if (found.state == phig::gtk::ThumbLookup::Absent) {
        // Generation costs hundreds of milliseconds against about one for a
        // load, so a request the user has scrolled past stops here.
        if (is_cancelled(req)) return;
        if (phig::gtk::thumb_generate(*self.coll, req.image_id, want) !=
            phig::gtk::ThumbGenerate::Ok) {
            req.result = THUMB_FAILED;
            return;
        }
        found = phig::gtk::thumb_lookup(*self.coll, req.sha256, want);
    }

    if (found.state == phig::gtk::ThumbLookup::Invalid) {
        req.result = THUMB_FAILED;
    } else if (found.state == phig::gtk::ThumbLookup::Absent) {
        req.result = THUMB_MISS;
    } else {
        GError* error = nullptr;
        // phig wrote the file at the size asked for, so it loads without a
        // rescale, which keeps the texture as small as the budget assumes.
        GdkTexture* texture = gdk_texture_new_from_filename(found.path.c_str(), &error);
        if (texture == nullptr) {
            g_warning("thumb loader: failed to load %s: %s", found.path.c_str(),
                      error != nullptr ? error->message : "unknown error");
            g_clear_error(&error);
            req.result = THUMB_FAILED;
        } else {
            req.result = THUMB_OK;
            req.texture = adopt(texture);
        }
    }
}
```

- `thumb_loader_new(PhigCollection* coll, …)` becomes
  `thumb_loader_new(phig::gtk::Collection& coll, …)`, with
  `loader->coll = &coll;`.

In `gtk/tests/test-thumb-loader.cpp`:
- `sha_of` becomes `std::string sha_of(guint index) { return phig::gtk::fake::sha(index); }`.
- `Fixture` becomes:

```cpp
struct Fixture {
    phig::gtk::CollectionPtr coll;
    std::shared_ptr<ThumbLoader> loader;

    explicit Fixture(gsize budget = kRoomy, guint threads = 4)
        : coll(phig::gtk::fake::make(8)), loader(thumb_loader_new(*coll, threads, budget)) {}
    ~Fixture() {
        thumb_loader_shutdown(*loader);
        loader.reset();
        coll.reset();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};
```

- Every `fake_collection_generate_count(f.coll)` becomes
  `phig::gtk::fake::generate_count(*f.coll)`.
- `fake_collection_hold_generation(f.coll, TRUE)` and `(…, FALSE)` become
  `phig::gtk::fake::hold_generation(*f.coll, true)` and `(…, false)`.
- In `test_a_thumbnail_on_disk_is_not_regenerated`, `thumb_loader_new(f.coll, 1, kRoomy)`
  becomes `thumb_loader_new(*f.coll, 1, kRoomy)`.
- In `test_a_delivery_outlives_the_callers_reference`, the first two lines
  become `phig::gtk::CollectionPtr coll = phig::gtk::fake::make(8);` and
  `std::shared_ptr<ThumbLoader> loader = thumb_loader_new(*coll, 4, kRoomy);`,
  and the last line becomes `coll.reset();`.

- [ ] **Step 8: Put the gallery model on the seam**

Replace `gtk/src/gallery-model.h` with:

```cpp
#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include <cstdint>

#include "collection.h"

/* PhigImageRow: one row of the gallery. It owns copies of its fields, so a row
 * stays valid after its page is evicted.
 *
 * A row can be a placeholder: returned by PhigGalleryModel for a page that
 * has not been fetched yet, so get_item never blocks. Check
 * phig_image_row_is_placeholder before trusting the other getters. */
#define PHIG_TYPE_IMAGE_ROW (phig_image_row_get_type())
G_DECLARE_FINAL_TYPE(PhigImageRow, phig_image_row, PHIG, IMAGE_ROW, GObject)

gboolean phig_image_row_is_placeholder(PhigImageRow* row);
int64_t phig_image_row_get_id(PhigImageRow* row);
const char* phig_image_row_get_path(PhigImageRow* row);
const char* phig_image_row_get_sha256(PhigImageRow* row);
const char* phig_image_row_get_filename(PhigImageRow* row);

/* PhigGalleryModel: a GListModel over the collection's gallery rows without holding
 * the collection in memory. It knows the total row count up front (from
 * phig::gtk::count) and keeps only an LRU of fetched pages resident.
 *
 * get_item never blocks: a resident page returns a ref to its cached row; a
 * non-resident page returns a placeholder row and schedules an async fetch
 * (a GTask running phig::gtk::page on a worker thread), which installs the
 * page and emits items-changed for its range from the GTask completion
 * callback on the main thread. Pages beyond the resident budget are evicted
 * least-recently-used; rows already handed out to callers stay valid because
 * they are refcounted GObjects owning their own copies.
 *
 * Signals:
 *   "page-fetch-failed" (const char* reason): a page fetch failed, for the
 *   reason the collection gave; the affected positions keep their placeholder
 *   rows (no items-changed is emitted for them, since nothing changed).
 *   Emitted once per failed fetch, on the main thread; a GUI is expected to
 *   coalesce this into one user-visible notification rather than one per
 *   occurrence. */
#define PHIG_TYPE_GALLERY_MODEL (phig_gallery_model_get_type())
G_DECLARE_FINAL_TYPE(PhigGalleryModel, phig_gallery_model, PHIG, GALLERY_MODEL, GObject)

/* Caller keeps ownership of coll and must not close it while any reference to
 * the model is outstanding: page fetches use it from a worker thread for as
 * long as the model exists. */
PhigGalleryModel* phig_gallery_model_new(phig::gtk::Collection& coll);

/* Test-only helpers. Production code goes through GListModel's get_item,
 * which schedules an async fetch; these exist so tests can force a page to
 * become resident, and inspect the resident-page budget, without a
 * GMainLoop. */
void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index);
guint phig_gallery_model_resident_page_count(PhigGalleryModel* self);
guint phig_gallery_model_get_max_resident_pages(PhigGalleryModel* self);
```

In `gtk/src/gallery-model.cpp`:

1. After `#include "gallery-model.h"`, add:

```cpp
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>
```

2. Replace `phig_image_row_new_from_data` and its comment with:

```cpp
/* Copies every field it exposes out of data. */
static PhigImageRow* phig_image_row_new_from_data(const phig::gtk::Row& data) {
    PhigImageRow* row = PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, NULL));
    row->placeholder = FALSE;
    row->id = data.id;
    row->path = g_strdup(data.path.c_str());
    row->sha256 = g_strdup(data.sha256.c_str());
    row->filename = g_strdup(data.filename.c_str());
    return row;
}
```

3. In `struct _PhigGalleryModel`, `PhigCollection* coll;` becomes
   `phig::gtk::Collection* coll;`.

4. Replace `phig_page_new_from_rows` and its comment with:

```cpp
/* Builds owned rows from a freshly fetched page. Touches no GTK or main-loop
 * state. */
static PhigPage* phig_page_new_from_rows(guint page_index,
                                         const std::vector<phig::gtk::Row>& fetched) {
    PhigPage* page = g_new0(PhigPage, 1);
    page->page_index = page_index;
    page->n_rows = static_cast<guint>(fetched.size());
    page->rows = g_new0(PhigImageRow*, page->n_rows);
    for (guint i = 0; i < page->n_rows; i++)
        page->rows[i] = phig_image_row_new_from_data(fetched[i]);
    return page;
}
```

5. Replace the `FetchTaskData` typedef and `fetch_task_data_free` with:

```cpp
struct FetchTaskData {
    GWeakRef model;
    guint page_index = 0;
    /* Set on the worker thread. Empty if the model was gone before the fetch
     * ran. */
    std::optional<std::expected<std::vector<phig::gtk::Row>, std::string>> fetched;
};

static void fetch_task_data_free(FetchTaskData* data) {
    g_weak_ref_clear(&data->model);
    delete data;
}
```

   Keep the long comment above the old typedef. The comment inside the old
   `fetch_task_data_free` about freeing `fetched` goes with it: a
   `std::vector` frees itself.

6. Replace the body of `fetch_page_thread_func` after the `(void)` lines
   with:

```cpp
    FetchTaskData* data = static_cast<FetchTaskData*>(task_data);
    PhigGalleryModel* self = phig_gallery_model_enter_in_flight(data);
    if (self == NULL) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    data->fetched.emplace(
        phig::gtk::page(*self->coll, data->page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE));
    phig_gallery_model_leave_in_flight(self);
    g_task_return_boolean(task, TRUE);
```

7. Replace the body of `fetch_page_done` after the `(void)` lines with:

```cpp
    GTask* task = G_TASK(result);
    FetchTaskData* data = static_cast<FetchTaskData*>(g_task_get_task_data(task));
    PhigGalleryModel* self = static_cast<PhigGalleryModel*>(g_weak_ref_get(&data->model));
    if (self == NULL)
        return; /* model was destroyed before this fetch's result landed */

    g_hash_table_remove(self->pending, GUINT_TO_POINTER(data->page_index));
    if (!data->fetched) {
        g_object_unref(self);
        return;
    }
    if (!*data->fetched) {
        const std::string& reason = data->fetched->error();
        g_warning("gallery model: page %u fetch failed: %s", data->page_index, reason.c_str());
        g_signal_emit(self, signals[SIGNAL_PAGE_FETCH_FAILED], 0, reason.c_str());
        g_object_unref(self);
        return;
    }

    phig_gallery_model_install_page(self, phig_page_new_from_rows(data->page_index, **data->fetched));
    g_object_unref(self);
```

8. In `phig_gallery_model_schedule_fetch`,
   `FetchTaskData* data = g_new0(FetchTaskData, 1);` becomes
   `FetchTaskData* data = new FetchTaskData{};`.

9. In `phig_gallery_model_class_init`, the `g_signal_new` call's last line
   `NULL, NULL, G_TYPE_NONE, 0);` becomes `NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);`.

10. Replace `phig_gallery_model_new` and `phig_gallery_model_fetch_page_sync`
    with:

```cpp
PhigGalleryModel* phig_gallery_model_new(phig::gtk::Collection& coll) {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(g_object_new(PHIG_TYPE_GALLERY_MODEL, NULL));
    self->coll = &coll;
    if (auto total = phig::gtk::count(coll))
        self->total = *total;
    else
        g_warning("gallery model: count failed: %s", total.error().c_str());
    return self;
}

void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index) {
    auto fetched = phig::gtk::page(*self->coll, page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    if (!fetched)
        return;
    phig_gallery_model_install_page(self, phig_page_new_from_rows(page_index, *fetched));
}
```

In `gtk/tests/test-gallery-model.cpp`:
- Add `#include <string>`.
- `typedef struct { PhigCollection* coll; PhigGalleryModel* model; } Fixture;`
  becomes a struct with `phig::gtk::Collection* coll;`. `g_test_add`
  allocates the fixture zeroed with no constructor, so it keeps a raw pointer.
- `fixture_set_up`: `f->coll = phig::gtk::fake::make(N_ROWS).release();` and
  `f->model = phig_gallery_model_new(*f->coll);`.
- `fixture_tear_down`: `phig_collection_close(f->coll);` becomes
  `phig::gtk::CollectionClose{}(f->coll);`.
- In `test_rows_carry_the_collections_fields`, replace the `char sha[65];`
  and `fake_collection_sha(250, sha);` lines with
  `const std::string sha = phig::gtk::fake::sha(250);`, and the sha assertion
  with `g_assert_cmpstr(phig_image_row_get_sha256(row), ==, sha.c_str());`.
- `FailedCapture` gains a member `std::string reason;` after `fired`, and its
  initializer in the test becomes `FailedCapture capture = {.loop = loop, .fired = FALSE, .reason = {}};`.
- `on_page_fetch_failed` becomes:

```cpp
static void on_page_fetch_failed(PhigGalleryModel* model, const char* reason,
                                 gpointer user_data) {
    (void)model;
    FailedCapture* capture = static_cast<FailedCapture*>(user_data);
    capture->fired = TRUE;
    capture->reason = reason;
    g_main_loop_quit(capture->loop);
}
```

- In `test_failed_fetch_emits_page_fetch_failed`:
  `fake_collection_fail_pages(f->coll, TRUE);` becomes
  `phig::gtk::fake::fail_pages(*f->coll, true);`, the expected message
  becomes `"gallery model: page 0 fetch failed: fake: page fetch failed"`, and
  after `g_assert_true(capture.fired);` add
  `g_assert_cmpstr(capture.reason.c_str(), ==, phig::gtk::fake::kPageError);`.

- [ ] **Step 9: Put the window on the seam**

In `gtk/src/main.cpp`:
- `CollectionPtr coll;` in `PhigApp` and in `StartupResult` becomes
  `phig::gtk::CollectionPtr coll;`.
- Replace the body of `startup_thread_func` after `auto* result = …;` with:

```cpp
    auto opened = phig::gtk::open(result->app->catalog_dir);
    if (!opened) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                                opened.error().c_str());
        return;
    }
    result->coll = std::move(*opened);
    g_task_return_boolean(task, TRUE);
```

  and change its comment `// The collection's error comes back as an owned copy.`
  to `// The collection's reason comes back as the task's error.` Task 7
  replaces this function; the race it has is unchanged here.
- In `startup_done`, `thumb_loader_new(app->coll.get(), …)` becomes
  `thumb_loader_new(*app->coll, …)` and `phig_gallery_model_new(app->coll.get())`
  becomes `phig_gallery_model_new(*app->coll)`.
- `on_page_fetch_failed` gains a parameter: its signature becomes
  `static void on_page_fetch_failed(PhigGalleryModel* model, const char* reason, gpointer user_data) noexcept`,
  with `(void)reason;` after `(void)model;`. The model has already logged the
  reason; the window shows one generic toast.
- Replace `resolve_default_catalog` with:

```cpp
// phig's own default, from core/ through the collection seam. Empty, after a
// warning naming the reason, when it cannot be resolved.
static std::string resolve_default_catalog() {
    auto dir = phig::gtk::default_catalog();
    if (dir) return std::move(*dir);
    g_warning("phig: could not resolve the default catalog: %s", dir.error().c_str());
    return {};
}
```

- [ ] **Step 10: Run the full gtk check**

Run `gtk_check`. Expected: no warnings; `100% tests passed` in both builds;
7 `ok` from `test-collection`, 10 from `test-gallery-model`, 20 from
`test-thumb-loader`. Then confirm nothing outside the seam's own file still
names the old seam:

```bash
grep -rn "PhigCollection\|PhigRowPage\|phig_collection_\|fake_collection_\|row-page" \
  gtk/src gtk/tests gtk/CMakeLists.txt
```

Expected: no output. `gtk/CLAUDE.md` and `gtk/README.md` still name the old
seam until Task 9.

- [ ] **Step 11: Commit**

```bash
git add -A gtk/
git commit -m "Make gtk's collection seam C++

collection.h becomes the phig::gtk namespace: rows are std::strings, a page is
a std::vector<Row>, and every fallible call returns std::expected carrying
phig's reason. thumb_generate mirrors core/'s EnsureThumb, so a missing source
and an unknown row are told apart. The gallery model's page-fetch-failed
signal carries the reason. row-page.cpp is gone."
```

---

### Task 5: Typed signal connections

**Files:**
- Create: `gtk/src/connect.h`
- Create: `gtk/tests/test-connect.cpp`
- Modify: `gtk/CMakeLists.txt`
- Modify: `gtk/src/main.cpp` (every `g_signal_connect`)
- Modify: `gtk/tests/test-gallery-model.cpp` (both `g_signal_connect`s)

**Interfaces:**
- Produces:

```cpp
namespace phig::gtk {
template <typename Sig, typename F>
gulong connect(gpointer instance, const char* signal, F f);
}
```

- [ ] **Step 1: Write the test**

Create `gtk/tests/test-connect.cpp`:

```cpp
#include <glib-object.h>

#include <utility>

#include "connect.h"

// A GObject with one signal, "ask", that takes an int and returns one, so the
// tests can check arguments, the return value and handler lifetime without GTK.
#define TEST_TYPE_EMITTER (test_emitter_get_type())
G_DECLARE_FINAL_TYPE(TestEmitter, test_emitter, TEST, EMITTER, GObject)

struct _TestEmitter {
    GObject parent_instance;
};

G_DEFINE_FINAL_TYPE(TestEmitter, test_emitter, G_TYPE_OBJECT)

static guint ask_signal;

static void test_emitter_class_init(TestEmitterClass* klass) {
    ask_signal = g_signal_new("ask", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                              nullptr, nullptr, G_TYPE_INT, 1, G_TYPE_INT);
}

static void test_emitter_init(TestEmitter* self) { (void)self; }

namespace {

TestEmitter* new_emitter() { return TEST_EMITTER(g_object_new(TEST_TYPE_EMITTER, nullptr)); }

int ask(TestEmitter* emitter, int question) {
    int answer = 0;
    g_signal_emit(emitter, ask_signal, 0, question, &answer);
    return answer;
}

// A move-only capture that counts its own destruction.
class Probe {
  public:
    explicit Probe(int& destroyed) : destroyed_(&destroyed) {}
    Probe(Probe&& other) noexcept : destroyed_(std::exchange(other.destroyed_, nullptr)) {}
    Probe& operator=(Probe&&) = delete;
    ~Probe() {
        if (destroyed_ != nullptr) ++*destroyed_;
    }

  private:
    int* destroyed_;
};

void test_a_connected_lambda_gets_the_arguments_and_returns_its_value() {
    TestEmitter* emitter = new_emitter();
    TestEmitter* seen = nullptr;
    phig::gtk::connect<int(TestEmitter*, int)>(emitter, "ask",
                                               [&seen](TestEmitter* self, int question) noexcept {
                                                   seen = self;
                                                   return question * 2;
                                               });
    g_assert_cmpint(ask(emitter, 21), ==, 42);
    g_assert_true(seen == emitter);
    g_object_unref(emitter);
}

void test_disconnecting_destroys_the_captures() {
    TestEmitter* emitter = new_emitter();
    int destroyed = 0;
    gulong id = phig::gtk::connect<int(TestEmitter*, int)>(
        emitter, "ask", [probe = Probe(destroyed)](TestEmitter*, int) noexcept { return 0; });
    g_assert_cmpint(destroyed, ==, 0);

    g_signal_handler_disconnect(emitter, id);
    g_assert_cmpint(destroyed, ==, 1);

    g_object_unref(emitter);
    g_assert_cmpint(destroyed, ==, 1);
}

void test_finalizing_the_instance_destroys_the_captures() {
    TestEmitter* emitter = new_emitter();
    int destroyed = 0;
    phig::gtk::connect<int(TestEmitter*, int)>(
        emitter, "ask", [probe = Probe(destroyed)](TestEmitter*, int) noexcept { return 0; });

    g_object_unref(emitter);
    g_assert_cmpint(destroyed, ==, 1);
}

void test_each_connection_owns_its_own_captures() {
    TestEmitter* emitter = new_emitter();
    int calls = 0;
    phig::gtk::connect<int(TestEmitter*, int)>(emitter, "ask",
                                               [&calls](TestEmitter*, int) noexcept {
                                                   calls++;
                                                   return 1;
                                               });
    phig::gtk::connect<int(TestEmitter*, int)>(emitter, "ask",
                                               [&calls](TestEmitter*, int) noexcept {
                                                   calls++;
                                                   return 2;
                                               });
    g_assert_cmpint(ask(emitter, 0), ==, 2);  // RUN_LAST: the last handler's value wins
    g_assert_cmpint(calls, ==, 2);
    g_object_unref(emitter);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/connect/a-connected-lambda-gets-the-arguments-and-returns-its-value",
                    test_a_connected_lambda_gets_the_arguments_and_returns_its_value);
    g_test_add_func("/connect/disconnecting-destroys-the-captures",
                    test_disconnecting_destroys_the_captures);
    g_test_add_func("/connect/finalizing-the-instance-destroys-the-captures",
                    test_finalizing_the_instance_destroys_the_captures);
    g_test_add_func("/connect/each-connection-owns-its-own-captures",
                    test_each_connection_owns_its_own_captures);
    return g_test_run();
}
```

In `gtk/CMakeLists.txt`, after the `thumb-loader` test, add:

```cmake
phig_gtk_test(connect tests/test-connect.cpp)
```

- [ ] **Step 2: Confirm it does not compile**

```bash
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON
cmake --build build/with-gtk --target test-connect -j 2>&1 | grep -m1 "connect.h"
```

Expected: `connect.h: No such file or directory`.

- [ ] **Step 3: Write the helper**

Create `gtk/src/connect.h`:

```cpp
#pragma once

#include <glib-object.h>

#include <type_traits>
#include <utility>

namespace phig::gtk {

namespace detail {

template <typename Sig>
struct Trampoline;

template <typename R, typename... Args>
struct Trampoline<R(Args...)> {
    template <typename F>
    static R call(Args... args, gpointer data) noexcept {
        return (*static_cast<F*>(data))(args...);
    }

    template <typename F>
    static void destroy(gpointer data, GClosure* closure) noexcept {
        (void)closure;
        delete static_cast<F*>(data);
    }

    template <typename F>
    static constexpr bool nothrow = std::is_nothrow_invocable_r_v<R, F&, Args...>;
};

}  // namespace detail

// Connects f to instance's signal and returns the handler id. Sig is the
// signal's C signature without the trailing user_data, such as
// void(GtkGridView*, guint) for GtkGridView::activate. GLib cannot check it
// against the signal, so it must match the signal's documentation.
//
// f moves to the heap and is destroyed when the handler is disconnected or the
// instance is finalized, so it may own its captures. f must be noexcept: no
// exception may unwind into GLib, and the static_assert makes that a compile
// error rather than a review item.
//
// Not named signal.h: src/ is on the include path, and a signal.h there would
// shadow the system header GLib includes.
template <typename Sig, typename F>
gulong connect(gpointer instance, const char* signal, F f) {
    using T = detail::Trampoline<Sig>;
    static_assert(T::template nothrow<F>, "a signal handler must be noexcept");
    auto* owned = new F(std::move(f));
    return g_signal_connect_data(instance, signal, G_CALLBACK(&T::template call<F>), owned,
                                 &T::template destroy<F>, GConnectFlags{});
}

}  // namespace phig::gtk
```

- [ ] **Step 4: Run the test**

```bash
cmake --build build/with-gtk --target test-connect -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-connect
```

Expected: 4 `ok`.

- [ ] **Step 5: Confirm a throwing handler does not compile**

```bash
cat > /tmp/068-connect-throws.cpp <<'EOF'
#include "connect.h"
void f(GObject* o) { phig::gtk::connect<void(GObject*)>(o, "notify", [](GObject*) {}); }
EOF
c++ -std=c++23 -fsyntax-only -Igtk/src $(pkg-config --cflags gobject-2.0) /tmp/068-connect-throws.cpp 2>&1 \
  | grep -m1 "must be noexcept"
```

Expected: one line containing `a signal handler must be noexcept`.

- [ ] **Step 6: Move every connection in `main.cpp` onto it**

Add `#include "connect.h"` to `main.cpp`'s local includes. Change these
handlers' signatures as listed; each body keeps its lines except the
`(void)` casts and the `static_cast` of `user_data` that the new parameters
make unnecessary:

| Handler | New signature |
|---|---|
| `on_zoom_begin` | `static void on_zoom_begin(PhigApp* app) noexcept` |
| `on_zoom_scale_changed` | `static void on_zoom_scale_changed(PhigApp* app, double scale) noexcept` |
| `on_zoom_end` | `static void on_zoom_end(PhigApp* app) noexcept` |
| `on_scroll` | `static gboolean on_scroll(PhigApp* app, GtkEventControllerScroll* controller, double dy) noexcept` |
| `on_factory_setup` | `static void on_factory_setup(GtkListItem* item) noexcept` |
| `on_factory_bind` | `static void on_factory_bind(PhigApp* app, GtkListItem* item) noexcept` |
| `on_factory_unbind` | `static void on_factory_unbind(GtkListItem* item) noexcept` |
| `on_cell_activate` | `static void on_cell_activate(PhigApp* app, guint position) noexcept` |
| `on_page_fetch_failed` | `static void on_page_fetch_failed(PhigApp* app) noexcept` |
| `on_activate` | `static void on_activate(GtkApplication* gtk_app, PhigApp* app) noexcept` |

Delete `on_grid_width_changed` and `on_scale_factor_changed`; their
connections call `phig_queue_relayout` directly. Keep
`on_scale_factor_changed`'s comment, moved to its connection.

Replace each connection:

```cpp
    // startup_done
    phig::gtk::connect<void(PhigGalleryModel*, const char*)>(
        model, "page-fetch-failed",
        [app](PhigGalleryModel*, const char*) noexcept { on_page_fetch_failed(app); });

    phig::gtk::connect<void(GtkSignalListItemFactory*, GtkListItem*)>(
        factory, "setup",
        [](GtkSignalListItemFactory*, GtkListItem* item) noexcept { on_factory_setup(item); });
    phig::gtk::connect<void(GtkSignalListItemFactory*, GtkListItem*)>(
        factory, "bind",
        [app](GtkSignalListItemFactory*, GtkListItem* item) noexcept { on_factory_bind(app, item); });
    phig::gtk::connect<void(GtkSignalListItemFactory*, GtkListItem*)>(
        factory, "unbind",
        [](GtkSignalListItemFactory*, GtkListItem* item) noexcept { on_factory_unbind(item); });
```

```cpp
    // on_activate
    phig::gtk::connect<void(GtkGridView*, guint)>(
        app->grid, "activate",
        [app](GtkGridView*, guint position) noexcept { on_cell_activate(app, position); });

    phig::gtk::connect<void(GObject*, GParamSpec*)>(
        gtk_scrolled_window_get_hadjustment(app->scroller), "notify::page-size",
        [app](GObject*, GParamSpec*) noexcept { phig_queue_relayout(app); });
    /* Moving between monitors changes how many physical pixels a cell needs, so
     * the sharpness clamp and the requested texture size both change. */
    phig::gtk::connect<void(GObject*, GParamSpec*)>(
        app->grid, "notify::scale-factor",
        [app](GObject*, GParamSpec*) noexcept { phig_queue_relayout(app); });

    phig::gtk::connect<void(GtkGesture*, GdkEventSequence*)>(
        zoom, "begin", [app](GtkGesture*, GdkEventSequence*) noexcept { on_zoom_begin(app); });
    phig::gtk::connect<void(GtkGestureZoom*, double)>(
        zoom, "scale-changed",
        [app](GtkGestureZoom*, double scale) noexcept { on_zoom_scale_changed(app, scale); });
    phig::gtk::connect<void(GtkGesture*, GdkEventSequence*)>(
        zoom, "end", [app](GtkGesture*, GdkEventSequence*) noexcept { on_zoom_end(app); });

    phig::gtk::connect<gboolean(GtkEventControllerScroll*, double, double)>(
        scroll, "scroll",
        [app](GtkEventControllerScroll* controller, double, double dy) noexcept {
            return on_scroll(app, controller, dy);
        });
```

```cpp
    // main
    phig::gtk::connect<void(GtkApplication*)>(
        gtk_app.get(), "activate",
        [app = app.get()](GtkApplication* application) noexcept { on_activate(application, app); });
```

`on_scroll`'s body drops its `(void)dx;`, and `on_factory_bind`'s drops
`(void)factory;`.

- [ ] **Step 7: Move the test's connections onto it**

In `gtk/tests/test-gallery-model.cpp`, add `#include "connect.h"`. Delete
`on_items_changed` and `on_page_fetch_failed`. In
`test_async_fetch_emits_items_changed`, replace the `g_signal_connect` with:

```cpp
    gulong handler_id = phig::gtk::connect<void(GListModel*, guint, guint, guint)>(
        f->model, "items-changed",
        [&capture](GListModel*, guint position, guint removed, guint added) noexcept {
            capture.fired = TRUE;
            capture.position = position;
            capture.removed = removed;
            capture.added = added;
            g_main_loop_quit(capture.loop);
        });
```

In `test_failed_fetch_emits_page_fetch_failed`, replace it with:

```cpp
    gulong handler_id = phig::gtk::connect<void(PhigGalleryModel*, const char*)>(
        f->model, "page-fetch-failed", [&capture](PhigGalleryModel*, const char* reason) noexcept {
            capture.fired = TRUE;
            capture.reason = reason;
            g_main_loop_quit(capture.loop);
        });
```

- [ ] **Step 8: Run the full gtk check**

Reconfigure the ASan build for the new test binary
(`cmake -B build/with-gtk-asan -DPHIG_BUILD_GTK=ON`), then run `gtk_check`.
Expected: no warnings and `100% tests passed` in both. Then:

```bash
grep -rn "g_signal_connect\|G_CALLBACK" gtk/src gtk/tests | grep -v "gtk/src/connect.h"
```

Expected: no output.

- [ ] **Step 9: Commit**

```bash
git add gtk/src/connect.h gtk/tests/test-connect.cpp gtk/CMakeLists.txt gtk/src/main.cpp \
        gtk/tests/test-gallery-model.cpp
git commit -m "Connect gtk's signals through a typed, owning helper

phig::gtk::connect<Sig> takes a lambda in place of a G_CALLBACK cast and a
gpointer, owns it until the handler is disconnected or the instance
finalized, and refuses at compile time a handler that is not noexcept. Every
connection in gtk uses it."
```

---

### Task 6: The gallery model's state in C++

**Files:**
- Modify: `gtk/src/gallery-model.h`
- Modify: `gtk/src/gallery-model.cpp` (rewrite)
- Modify: `gtk/src/main.cpp` (the two row getters it calls)
- Modify: `gtk/tests/test-gallery-model.cpp` (`std::string` getters)

**Interfaces:**
- Consumes: the seam from Task 4.
- Produces:

```cpp
bool phig_image_row_is_placeholder(PhigImageRow* row);
int64_t phig_image_row_get_id(PhigImageRow* row);
const std::string& phig_image_row_get_path(PhigImageRow* row);
const std::string& phig_image_row_get_sha256(PhigImageRow* row);
const std::string& phig_image_row_get_filename(PhigImageRow* row);
```

The ten `test-gallery-model` cases are this task's test: they pin every
behaviour the rewrite must keep, and none changes except for the getter
types.

- [ ] **Step 1: Change the getters in the header**

In `gtk/src/gallery-model.h`, add `#include <string>`, and replace the five
`phig_image_row_*` declarations with the five in Interfaces above.

- [ ] **Step 2: Rewrite the implementation**

Replace `gtk/src/gallery-model.cpp` with:

```cpp
#include "gallery-model.h"

#include <condition_variable>
#include <expected>
#include <iterator>
#include <list>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gref.h"

namespace {

/* One page holds this many rows. Not varied in measurement (task-7-report.md):
 * page-fetch latency (0.85ms warm / 19ms cold per page fetch) is never
 * the bottleneck a full-range scroll hits, so there was no result to tune
 * this against. */
constexpr guint PHIG_PAGE_SIZE = 200;

/* Pages resident at once: the requested page plus this many either side,
 * rounded up to an odd budget so eviction has a clear least-recently-used
 * end. Measured at 1, 2, and 4 against the full 29,953-row scroll
 * (task-7-report.md): all three produce the same frame timings and
 * comparable RSS. Stubbing thumbnail loading out entirely leaves frame timing
 * unchanged, so page-fetch radius was never going to move it either. 2 is kept
 * as a starting point that measured no worse than the alternatives. */
constexpr guint PHIG_PAGE_RADIUS = 2;
constexpr guint PHIG_MAX_RESIDENT_PAGES = 2 * PHIG_PAGE_RADIUS + 1;

/* Cached per-position placeholders, capped independently of resident pages:
 * a scroll that outruns fetches touches positions across many more pages
 * than PHIG_MAX_RESIDENT_PAGES bounds, so this needs its own budget rather
 * than inheriting that one. At its current derived value (1000 with
 * PHIG_PAGE_RADIUS=2), a full-range fast-drag scroll, both warm and
 * miss-heavy, produced zero "Duplicate item detected" warnings
 * (task-7-report.md), so this stays derived rather than pinned to a
 * separately-measured constant. */
constexpr guint PHIG_MAX_PLACEHOLDERS = PHIG_MAX_RESIDENT_PAGES * PHIG_PAGE_SIZE;

}  // namespace

struct _PhigImageRow {
    GObject parent_instance;
    bool placeholder;
    phig::gtk::Row row;  // constructed in phig_image_row_init, destroyed in finalize
};

G_DEFINE_FINAL_TYPE(PhigImageRow, phig_image_row, G_TYPE_OBJECT)

static void phig_image_row_finalize(GObject* obj) noexcept {
    PHIG_IMAGE_ROW(obj)->row.~Row();
    G_OBJECT_CLASS(phig_image_row_parent_class)->finalize(obj);
}

static void phig_image_row_class_init(PhigImageRowClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = phig_image_row_finalize;
}

static void phig_image_row_init(PhigImageRow* self) { new (&self->row) phig::gtk::Row(); }

bool phig_image_row_is_placeholder(PhigImageRow* row) { return row->placeholder; }

int64_t phig_image_row_get_id(PhigImageRow* row) { return row->row.id; }

const std::string& phig_image_row_get_path(PhigImageRow* row) { return row->row.path; }

const std::string& phig_image_row_get_sha256(PhigImageRow* row) { return row->row.sha256; }

const std::string& phig_image_row_get_filename(PhigImageRow* row) { return row->row.filename; }

namespace {

GRef<PhigImageRow> new_placeholder() {
    GRef<PhigImageRow> row = adopt(PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, nullptr)));
    row->placeholder = true;
    return row;
}

GRef<PhigImageRow> new_row(phig::gtk::Row data) {
    GRef<PhigImageRow> row = adopt(PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, nullptr)));
    row->placeholder = false;
    row->row = std::move(data);
    return row;
}

// One resident page. Rows already handed to callers survive its eviction: they
// are separately refcounted.
struct Page {
    std::vector<GRef<PhigImageRow>> rows;
    std::list<guint>::iterator lru;  // this page's node in Impl::page_lru
};

// The cached placeholder for one non-resident position. GTK's list-item
// manager keys recycled widgets by item pointer identity: a single object
// shared across positions makes it pick a widget for the wrong position at
// random (glib#3030), and a fresh object per call breaks GtkSelectionModel's
// own identity tracking for repeated queries of the same position. One cached
// object per position satisfies both.
struct Placeholder {
    GRef<PhigImageRow> row;
    std::list<guint>::iterator lru;  // this entry's node in Impl::placeholder_lru
};

struct Impl {
    phig::gtk::Collection* coll = nullptr;  // borrowed; see phig_gallery_model_new
    guint total = 0;

    std::unordered_map<guint, Page> pages;  // by page index
    std::list<guint> page_lru;              // front = least recently used
    std::unordered_set<guint> pending;      // page indices being fetched

    // Bounded like pages, but separately, at PHIG_MAX_PLACEHOLDERS, so a fast
    // scroll that outruns fetches cannot grow it however many distinct pages
    // it touches. A position's entry is also dropped once its page lands.
    std::unordered_map<guint, Placeholder> placeholders;  // by position
    std::list<guint> placeholder_lru;                     // front = least recently used

    // coll is borrowed from the caller, not owned, so pinning the model alive
    // (the GWeakRef in FetchTask) does not keep coll alive. in_flight counts
    // worker threads between resolving that weak ref and dropping it, that is,
    // inside phig::gtk::page. dispose blocks until it reaches zero, which is
    // what makes the header's "caller must not close coll while any reference
    // to the model is outstanding" contract true: the caller's final unref does
    // not return while a worker still uses it.
    std::mutex in_flight_mutex;
    std::condition_variable in_flight_cv;
    guint in_flight = 0;  // guarded by in_flight_mutex
};

}  // namespace

struct _PhigGalleryModel {
    GObject parent_instance;
    Impl impl;  // constructed in phig_gallery_model_init, destroyed in finalize
};

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    PhigGalleryModel, phig_gallery_model, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, phig_gallery_model_list_model_iface_init))

/* Emitted on the main thread when a page fetch fails, after the affected
 * positions are left as placeholders. Carries the collection's reason. */
enum { SIGNAL_PAGE_FETCH_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

namespace {

void touch(std::list<guint>& lru, std::list<guint>::iterator node) {
    lru.splice(lru.end(), lru, node);
}

// Evicts least-recently-used pages until the resident count is within budget.
void evict_pages(Impl& impl) {
    while (impl.pages.size() > PHIG_MAX_RESIDENT_PAGES) {
        impl.pages.erase(impl.page_lru.front());
        impl.page_lru.pop_front();
    }
}

// The cached placeholder for position, created if absent. Evicts the least
// recently used placeholder first if the table is at its cap.
PhigImageRow* placeholder_for(Impl& impl, guint position) {
    if (auto it = impl.placeholders.find(position); it != impl.placeholders.end()) {
        touch(impl.placeholder_lru, it->second.lru);
        return it->second.row.get();
    }
    if (impl.placeholders.size() >= PHIG_MAX_PLACEHOLDERS) {
        impl.placeholders.erase(impl.placeholder_lru.front());
        impl.placeholder_lru.pop_front();
    }
    impl.placeholder_lru.push_back(position);
    auto [it, inserted] = impl.placeholders.emplace(
        position, Placeholder{new_placeholder(), std::prev(impl.placeholder_lru.end())});
    return it->second.row.get();
}

// Drops cached placeholders for [position, position + n_rows): those positions
// now have real rows.
void drop_placeholders(Impl& impl, guint position, guint n_rows) {
    for (guint i = 0; i < n_rows; i++) {
        auto it = impl.placeholders.find(position + i);
        if (it == impl.placeholders.end()) continue;
        impl.placeholder_lru.erase(it->second.lru);
        impl.placeholders.erase(it);
    }
}

// Installs a fetched page as the most recently used, evicts if over budget,
// and emits items-changed for the range it covers.
void install_page(PhigGalleryModel* self, guint page_index, std::vector<phig::gtk::Row> rows) {
    Impl& impl = self->impl;
    Page page;
    page.rows.reserve(rows.size());
    for (phig::gtk::Row& row : rows) page.rows.push_back(new_row(std::move(row)));
    const guint n_rows = static_cast<guint>(page.rows.size());
    const guint position = page_index * PHIG_PAGE_SIZE;

    if (auto old = impl.pages.find(page_index); old != impl.pages.end()) {
        impl.page_lru.erase(old->second.lru);
        impl.pages.erase(old);
    }
    impl.page_lru.push_back(page_index);
    page.lru = std::prev(impl.page_lru.end());
    impl.pages.emplace(page_index, std::move(page));
    evict_pages(impl);
    drop_placeholders(impl, position, n_rows);
    /* removed == added == n_rows: an in-place swap of placeholders for real
     * rows at the same positions. The total item count never changes after
     * construction, so this is not an insertion or a deletion. GTK treats
     * this as a no-op unless removed/added are nonzero, so the view would
     * never re-query get_item for this range without it. */
    g_list_model_items_changed(G_LIST_MODEL(self), position, n_rows, n_rows);
}

/* GTask state for the async fetch. model is a weak reference, not a strong
 * one: a GTask only releases its task data once its completion callback has
 * been dispatched, which needs a main-loop iteration that a caller destroying
 * the model synchronously (as the sync test helper's caller does, and as app
 * shutdown may do) never provides. A strong ref here would keep the model, and
 * the collection it points at, alive past when the caller expects to free them.
 * Each thread-side use resolves a temporary strong ref and drops it right
 * after, so the model cannot be finalized under a call in progress, but an
 * idle callback that never runs cannot pin it forever either. */
struct FetchTask {
    GWeakRef model;
    guint page_index = 0;
    // Set on the worker thread. Empty if the model was gone before the fetch ran.
    std::optional<std::expected<std::vector<phig::gtk::Row>, std::string>> fetched;
};

// Registers as in-flight, then returns a strong ref, or null if the model is
// gone. While this returns non-null, dispose blocks until leave_in_flight.
PhigGalleryModel* enter_in_flight(FetchTask& task) {
    auto* self = static_cast<PhigGalleryModel*>(g_weak_ref_get(&task.model));
    if (self == nullptr) return nullptr;
    std::lock_guard lock(self->impl.in_flight_mutex);
    self->impl.in_flight++;
    return self;
}

// The notify happens while self still holds its strong ref, so dispose cannot
// finish, and finalize destroy the condition variable, before it returns.
void leave_in_flight(PhigGalleryModel* self) {
    {
        std::lock_guard lock(self->impl.in_flight_mutex);
        self->impl.in_flight--;
    }
    self->impl.in_flight_cv.notify_all();
    g_object_unref(self);
}

void fetch_thread(GTask* task, gpointer source_object, gpointer task_data,
                  GCancellable* cancellable) noexcept {
    (void)source_object;
    (void)cancellable;
    auto& data = *static_cast<FetchTask*>(task_data);
    PhigGalleryModel* self = enter_in_flight(data);
    if (self == nullptr) {
        g_task_return_boolean(task, FALSE);
        return;
    }
    data.fetched.emplace(phig::gtk::page(*self->impl.coll, data.page_index * PHIG_PAGE_SIZE,
                                         PHIG_PAGE_SIZE));
    leave_in_flight(self);
    g_task_return_boolean(task, TRUE);
}

void fetch_done(GObject* source, GAsyncResult* result, gpointer user_data) noexcept {
    (void)source;
    (void)user_data;
    auto& data = *static_cast<FetchTask*>(g_task_get_task_data(G_TASK(result)));
    GRef<PhigGalleryModel> self =
        adopt(static_cast<PhigGalleryModel*>(g_weak_ref_get(&data.model)));
    if (!self) return;  // the model was destroyed before this fetch's result landed

    self->impl.pending.erase(data.page_index);
    if (!data.fetched) return;
    if (!*data.fetched) {
        const std::string& reason = data.fetched->error();
        g_warning("gallery model: page %u fetch failed: %s", data.page_index, reason.c_str());
        g_signal_emit(self.get(), signals[SIGNAL_PAGE_FETCH_FAILED], 0, reason.c_str());
        return;
    }
    install_page(self.get(), data.page_index, std::move(**data.fetched));
}

void schedule_fetch(PhigGalleryModel* self, guint page_index) {
    if (!self->impl.pending.insert(page_index).second) return;

    auto* data = new FetchTask{};
    g_weak_ref_init(&data->model, self);
    data->page_index = page_index;

    /* No source_object: a GTask refs its source_object for the task's own
     * lifetime, which would recreate the strong-ref hazard the weak
     * reference avoids. */
    GTask* task = g_task_new(nullptr, nullptr, fetch_done, nullptr);
    g_task_set_task_data(task, data, [](gpointer p) noexcept {
        auto* t = static_cast<FetchTask*>(p);
        g_weak_ref_clear(&t->model);
        delete t;
    });
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

GType get_item_type(GListModel* list) noexcept {
    (void)list;
    return PHIG_TYPE_IMAGE_ROW;
}

guint get_n_items(GListModel* list) noexcept { return PHIG_GALLERY_MODEL(list)->impl.total; }

gpointer get_item(GListModel* list, guint position) noexcept {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(list);
    Impl& impl = self->impl;
    if (position >= impl.total) return nullptr;

    const guint page_index = position / PHIG_PAGE_SIZE;
    const guint offset_in_page = position % PHIG_PAGE_SIZE;
    if (auto it = impl.pages.find(page_index); it != impl.pages.end()) {
        if (offset_in_page >= it->second.rows.size())
            return nullptr;  // last page, past its actual row count
        touch(impl.page_lru, it->second.lru);
        return g_object_ref(it->second.rows[offset_in_page].get());
    }

    schedule_fetch(self, page_index);
    return g_object_ref(placeholder_for(impl, position));
}

}  // namespace

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface) {
    iface->get_item_type = get_item_type;
    iface->get_n_items = get_n_items;
    iface->get_item = get_item;
}

/* The caller may close coll as soon as its last reference to this model is
 * released, so dispose must not return while a worker is still inside
 * phig::gtk::page. */
static void phig_gallery_model_dispose(GObject* obj) noexcept {
    Impl& impl = PHIG_GALLERY_MODEL(obj)->impl;
    {
        std::unique_lock lock(impl.in_flight_mutex);
        impl.in_flight_cv.wait(lock, [&impl] { return impl.in_flight == 0; });
    }
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->dispose(obj);
}

static void phig_gallery_model_finalize(GObject* obj) noexcept {
    PHIG_GALLERY_MODEL(obj)->impl.~Impl();
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->finalize(obj);
}

static void phig_gallery_model_class_init(PhigGalleryModelClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = phig_gallery_model_dispose;
    G_OBJECT_CLASS(klass)->finalize = phig_gallery_model_finalize;

    signals[SIGNAL_PAGE_FETCH_FAILED] =
        g_signal_new("page-fetch-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                     nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void phig_gallery_model_init(PhigGalleryModel* self) { new (&self->impl) Impl(); }

PhigGalleryModel* phig_gallery_model_new(phig::gtk::Collection& coll) {
    auto* self = PHIG_GALLERY_MODEL(g_object_new(PHIG_TYPE_GALLERY_MODEL, nullptr));
    self->impl.coll = &coll;
    if (auto total = phig::gtk::count(coll))
        self->impl.total = *total;
    else
        g_warning("gallery model: count failed: %s", total.error().c_str());
    return self;
}

void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index) {
    auto rows = phig::gtk::page(*self->impl.coll, page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    if (rows) install_page(self, page_index, std::move(*rows));
}

guint phig_gallery_model_resident_page_count(PhigGalleryModel* self) {
    return static_cast<guint>(self->impl.pages.size());
}

guint phig_gallery_model_get_max_resident_pages(PhigGalleryModel* self) {
    (void)self;
    return PHIG_MAX_RESIDENT_PAGES;
}
```

Format it, since the task rewrites it whole:

```bash
$CF -i gtk/src/gallery-model.cpp
```

- [ ] **Step 3: Update the getters' callers**

In `gtk/src/main.cpp`, `on_cell_activate`:
`const char* path = phig_image_row_get_path(row);` and
`GFile* file = g_file_new_for_path(path);` become
`GFile* file = g_file_new_for_path(phig_image_row_get_path(row).c_str());`.
The bind site's `phig_image_row_get_sha256(row)` already converts to
`std::string`.

In `gtk/tests/test-gallery-model.cpp`: each `g_assert_cmpstr` over a getter
appends `.c_str()` to the getter call, and in
`test_row_strings_survive_page_eviction`,
`char* sha_copy = g_strdup(phig_image_row_get_sha256(row));` becomes
`const std::string sha_copy = phig_image_row_get_sha256(row);`, its assertion
becomes `g_assert_cmpstr(phig_image_row_get_sha256(row).c_str(), ==, sha_copy.c_str());`,
and `g_free(sha_copy);` is deleted.

- [ ] **Step 4: Run the full gtk check**

Run `gtk_check`. Expected: no warnings, `100% tests passed` in both, and 10
`ok` from `test-gallery-model` under ASan with no report. Then:

```bash
grep -n "GHashTable\|g_hash_table\|g_new0\|g_free\|g_strdup" gtk/src/gallery-model.cpp
```

Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add gtk/src/gallery-model.h gtk/src/gallery-model.cpp gtk/src/main.cpp gtk/tests/test-gallery-model.cpp
git commit -m "Hold the gallery model's state in C++ members

PhigImageRow holds a phig::gtk::Row, and PhigGalleryModel an Impl with
standard containers and list-ordered LRUs in place of three GHashTables and
two logical clocks. Both are placement-new'd in instance_init and destroyed
in finalize. Behaviour is unchanged; the ten gallery-model tests pin it."
```

---

### Task 7: The startup worker owns what it reads

**Files:**
- Create: `gtk/src/startup.h`, `gtk/src/startup.cpp`
- Create: `gtk/tests/test-startup.cpp`
- Modify: `gtk/tests/fake-collection.h`, `gtk/tests/fake-collection.cpp` (`open`, `default_catalog`)
- Modify: `gtk/src/main.cpp`
- Modify: `gtk/CMakeLists.txt`

**Interfaces:**
- Consumes: `phig::gtk::open`, `phig::gtk::connect`.
- Produces:

```cpp
namespace phig::gtk {
using OpenResult = std::expected<CollectionPtr, std::string>;
using OpenDone = std::move_only_function<void(OpenResult) noexcept>;
void open_in_background(std::string catalog_dir, OpenDone done);
}
namespace phig::gtk::fake {
void hold_open(bool hold);
std::string last_opened();
inline constexpr const char* kMissingCatalog = "/fake/missing";
}
```

- [ ] **Step 1: Give the fake an `open` that can be held**

In `gtk/tests/fake-collection.h`, before the closing namespace line, add:

```cpp
// open fails for this directory with "fake: no catalog at /fake/missing", and
// succeeds with a 3-row collection for any other.
inline constexpr const char* kMissingCatalog = "/fake/missing";

// While hold is set, open waits before it reads its argument.
void hold_open(bool hold);

// The catalog_dir that open last read.
std::string last_opened();
```

In `gtk/tests/fake-collection.cpp`, add these before `namespace fake {`:

```cpp
namespace {

std::mutex open_mutex;
std::condition_variable open_cv;
bool open_held = false;    // guarded by open_mutex
std::string open_last;     // guarded by open_mutex

}  // namespace

std::expected<CollectionPtr, std::string> open(const std::string& catalog_dir) noexcept {
    std::unique_lock lock(open_mutex);
    open_cv.wait(lock, [] { return !open_held; });
    // Read only after the hold, so a caller that passed a reference to storage
    // it then freed is caught under ASan.
    open_last = catalog_dir;
    if (open_last == fake::kMissingCatalog)
        return std::unexpected("fake: no catalog at " + open_last);
    lock.unlock();
    return fake::make(3);
}

std::expected<std::string, std::string> default_catalog() noexcept {
    return std::string("/fake/default");
}
```

and inside `namespace fake`, after `hold_generation`:

```cpp
void hold_open(bool hold) {
    {
        std::lock_guard lock(open_mutex);
        open_held = hold;
    }
    open_cv.notify_all();
}

std::string last_opened() {
    std::lock_guard lock(open_mutex);
    return open_last;
}
```

- [ ] **Step 2: Write the test**

Create `gtk/tests/test-startup.cpp`:

```cpp
#include <glib.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "fake-collection.h"
#include "startup.h"

namespace {

gboolean on_timeout(gpointer user_data) {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

struct Outcome {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    std::optional<phig::gtk::OpenResult> result;
    GThread* delivered_on = nullptr;

    Outcome() = default;
    Outcome(const Outcome&) = delete;
    Outcome& operator=(const Outcome&) = delete;
    ~Outcome() { g_main_loop_unref(loop); }
};

phig::gtk::OpenDone record_into(Outcome& outcome) {
    return [&outcome](phig::gtk::OpenResult result) noexcept {
        outcome.result.emplace(std::move(result));
        outcome.delivered_on = g_thread_self();
        g_main_loop_quit(outcome.loop);
    };
}

// Runs the main loop until the open is delivered, or for 5s.
void wait_for(Outcome& outcome) {
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, outcome.loop);
    if (!outcome.result) g_main_loop_run(outcome.loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
    g_assert_true(outcome.result.has_value());
}

// What closing the window during startup does: the caller's state is freed
// while the worker is still inside open. The worker must read only its copy.
void test_the_worker_reads_only_its_own_copy() {
    phig::gtk::fake::hold_open(true);
    Outcome outcome;
    auto caller_state = std::make_unique<std::string>("/fake/catalog");

    phig::gtk::open_in_background(*caller_state, record_into(outcome));
    caller_state.reset();
    phig::gtk::fake::hold_open(false);
    wait_for(outcome);

    g_assert_true(outcome.result->has_value());
    g_assert_cmpstr(phig::gtk::fake::last_opened().c_str(), ==, "/fake/catalog");
}

void test_a_failed_open_carries_its_reason() {
    Outcome outcome;
    phig::gtk::open_in_background(phig::gtk::fake::kMissingCatalog, record_into(outcome));
    wait_for(outcome);

    g_assert_false(outcome.result->has_value());
    g_assert_cmpstr(outcome.result->error().c_str(), ==, "fake: no catalog at /fake/missing");
}

void test_the_result_is_delivered_on_the_calling_thread() {
    Outcome outcome;
    phig::gtk::open_in_background("/fake/catalog", record_into(outcome));
    wait_for(outcome);

    g_assert_true(outcome.delivered_on == g_thread_self());
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/startup/the-worker-reads-only-its-own-copy",
                    test_the_worker_reads_only_its_own_copy);
    g_test_add_func("/startup/a-failed-open-carries-its-reason",
                    test_a_failed_open_carries_its_reason);
    g_test_add_func("/startup/the-result-is-delivered-on-the-calling-thread",
                    test_the_result_is_delivered_on_the_calling_thread);
    return g_test_run();
}
```

In `gtk/CMakeLists.txt`, after the `connect` test, add:

```cmake
phig_gtk_test(startup tests/test-startup.cpp src/startup.cpp tests/fake-collection.cpp)
```

- [ ] **Step 3: Confirm it does not compile**

```bash
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON
cmake --build build/with-gtk --target test-startup -j 2>&1 | grep -m1 "startup.h"
```

Expected: `startup.h: No such file or directory`.

- [ ] **Step 4: Write `open_in_background`**

Create `gtk/src/startup.h`:

```cpp
#pragma once

#include <expected>
#include <functional>
#include <string>

#include "collection.h"

namespace phig::gtk {

using OpenResult = std::expected<CollectionPtr, std::string>;
using OpenDone = std::move_only_function<void(OpenResult) noexcept>;

// Opens catalog_dir on a worker thread, then calls done with the result on the
// calling thread's default main context. The worker reads only its own copy of
// catalog_dir, so the caller's state may be freed while it runs. Keeping alive
// whatever done refers to is the caller's job. done may be destroyed on either
// thread.
void open_in_background(std::string catalog_dir, OpenDone done);

}  // namespace phig::gtk
```

Create `gtk/src/startup.cpp`:

```cpp
#include "startup.h"

#include <gio/gio.h>

#include <optional>
#include <utility>

namespace phig::gtk {

namespace {

struct OpenTask {
    std::string catalog_dir;
    std::optional<OpenResult> result;  // set on the worker thread
    OpenDone done;
};

void open_thread(GTask* task, gpointer source_object, gpointer task_data,
                 GCancellable* cancellable) noexcept {
    (void)source_object;
    (void)cancellable;
    auto& data = *static_cast<OpenTask*>(task_data);
    data.result.emplace(open(data.catalog_dir));
    g_task_return_boolean(task, TRUE);
}

void open_finished(GObject* source, GAsyncResult* result, gpointer user_data) noexcept {
    (void)source;
    (void)user_data;
    auto& data = *static_cast<OpenTask*>(g_task_get_task_data(G_TASK(result)));
    data.done(std::move(*data.result));
}

}  // namespace

void open_in_background(std::string catalog_dir, OpenDone done) {
    auto* data = new OpenTask{std::move(catalog_dir), std::nullopt, std::move(done)};
    GTask* task = g_task_new(nullptr, nullptr, open_finished, nullptr);
    g_task_set_task_data(task, data,
                         [](gpointer p) noexcept { delete static_cast<OpenTask*>(p); });
    g_task_run_in_thread(task, open_thread);
    g_object_unref(task);
}

}  // namespace phig::gtk
```

- [ ] **Step 5: Run the test**

```bash
cmake --build build/with-gtk --target test-startup -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-startup
cmake -B build/with-gtk-asan -DPHIG_BUILD_GTK=ON
cmake --build build/with-gtk-asan --target test-startup -j 2>&1 | grep -E "warning|error"
ASAN_OPTIONS=detect_leaks=0 env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk-asan/gtk/test-startup
```

Expected: 3 `ok` in both, and no AddressSanitizer report.

- [ ] **Step 6: Show the test catches a worker that reads the caller's state**

Temporarily change `OpenTask`'s first member to `const std::string& catalog_dir;`
and `open_in_background`'s parameter to `const std::string& catalog_dir`, so
the worker reads the caller's string. Rebuild `test-startup` in
`build/with-gtk-asan` and run
`-p /startup/the-worker-reads-only-its-own-copy`. Expected:
`ERROR: AddressSanitizer: heap-use-after-free` with `open` in the stack. Record
the first lines in the task report, then restore both lines and confirm the
test passes again.

- [ ] **Step 7: Use it in `main.cpp`**

1. Add `#include "startup.h"` to the local includes.
2. Delete `StartupResult`, its comment, and `startup_thread_func`.
3. Add a member to `PhigApp`, after `page_fetch_toast_shown`:

```cpp
    // An open is running. A second activation then builds a window and leaves
    // startup_done to fill it, rather than starting a second open whose result
    // would replace coll under the first's loader.
    bool opening = false;
```

4. Replace `startup_done`'s signature and its first lines, through the end of
   the `if (!g_task_propagate_boolean(…)) { … }` block, with:

```cpp
// Runs on the main thread when the open finishes. The window may have closed
// while the worker ran; the result then closes with opened.
static void startup_done(PhigApp* app, phig::gtk::OpenResult opened) noexcept {
    if (app->window == nullptr) return;
    if (!opened) {
        show_startup_error(app, opened.error().c_str());
        return;
    }
```

   and `app->coll = std::move(startup->coll);` becomes
   `app->coll = std::move(*opened);`. The rest of the body is unchanged.

5. In `on_activate`, directly after the line that creates `app->window`, add:

```cpp
    /* Cleared on destroy, so startup_done can tell the window closed while the
     * catalog was opening. */
    phig::gtk::connect<void(GtkWidget*)>(app->window, "destroy",
                                         [app](GtkWidget*) noexcept { app->window = nullptr; });
```

6. In `on_activate`, directly before the `if (app->catalog_dir.empty())` check,
   add:

```cpp
    if (app->opening) return;  // startup_done fills this window
```

7. Replace the end of `on_activate`, from `auto* startup = new StartupResult{};`
   through `g_object_unref(task);`, with:

```cpp
    /* Held until startup_done has run, so closing the window during a slow open
     * keeps the process up until the worker finishes, rather than exiting with
     * it mid-open. */
    GApplication* gapp = G_APPLICATION(gtk_app);
    g_application_hold(gapp);
    app->opening = true;
    phig::gtk::open_in_background(app->catalog_dir,
                                  [app, gapp](phig::gtk::OpenResult opened) noexcept {
                                      app->opening = false;
                                      startup_done(app, std::move(opened));
                                      g_application_release(gapp);
                                  });
```

In `gtk/CMakeLists.txt`, add `src/startup.cpp` to `phig-gtk`'s sources.

- [ ] **Step 8: Run the full gtk check**

Run `gtk_check`. Expected: no warnings and `100% tests passed` in both, with
3 `ok` from `test-startup`. Then confirm the worker no longer reaches `app`:

```bash
grep -n "result->app\|StartupResult\|startup_thread_func" gtk/src/main.cpp
```

Expected: no output.

- [ ] **Step 9 (controller): ask the user for the by-hand check**

GTK cannot run headless in CI, and this check opens a window on the user's
screen, so the controller asks before running it. With approval, build a
scratch catalog from public files and run the ASan client under gdb:

```bash
SCRATCH=$(mktemp -d)
export HOME=$SCRATCH XDG_CONFIG_HOME=$SCRATCH/config XDG_DATA_HOME=$SCRATCH/data \
       XDG_CACHE_HOME=$SCRATCH/cache XDG_STATE_HOME=$SCRATCH/state
unset PHIG_CATALOG
./build/phig --catalog "$SCRATCH/catalog" scan tests/data --recursive
ASAN_OPTIONS=detect_leaks=0 gdb -q -ex "break phig::gtk::open" -ex run \
  --args ./build/with-gtk-asan/gtk/phig-gtk "$SCRATCH/catalog"
```

When gdb stops in `phig::gtk::open`, close the window, then `continue`.
Expected: the process exits with status 0 and no AddressSanitizer report.
Record gdb's last lines in the ledger, and remove `$SCRATCH`.

- [ ] **Step 10: Commit**

```bash
git add gtk/src/startup.h gtk/src/startup.cpp gtk/tests/test-startup.cpp gtk/src/main.cpp \
        gtk/tests/fake-collection.h gtk/tests/fake-collection.cpp gtk/CMakeLists.txt
git commit -m "Keep the startup worker off PhigApp and the process up until it finishes

The worker read app->catalog_dir while main could free app once the window
closed, and the process could exit with the worker mid-open. open_in_background
gives the worker its own copy, the application is held until startup_done
runs, and startup_done skips building a grid when the window's destroy signal
has cleared app->window. A second activation during startup builds a window
and does not start a second open."
```

---

### Task 8: 061's remaining minors

**Files:**
- Modify: `gtk/src/main.cpp` (`on_factory_bind`)
- Modify: `rules/README.md`

**Interfaces:** none.

`app->window` is cleared on destroy by Task 7, and `thumb-loader.h`'s
threading wording was rewritten in Task 2. Confirm both before starting:

```bash
grep -n "destroy" gtk/src/main.cpp | head -3
grep -n "whichever thread" gtk/src/thumb-loader.h
```

Expected: the `destroy` connection from Task 7, and no output from the second
grep.

- [ ] **Step 1: Guard the bind on the loader**

In `gtk/src/main.cpp`'s `on_factory_bind`, directly after the placeholder
branch's closing brace, add:

```cpp
    /* startup_done creates the loader before it attaches the model, so this
     * only guards a later reordering. */
    if (!app->loader)
        return;
```

- [ ] **Step 2: Fix the `core_internal` definition**

In `rules/README.md`, replace:

```markdown
- **`core_internal`** — the name mutates real data but is the *inside* of
  an operation already declared, with no caller outside `core/`. A rule
```

with:

```markdown
- **`core_internal`** — the name mutates real data but is the *inside* of
  an operation already declared. Its callers are in `core/`, or are thin
  surfaces that reach the operation through it: `ensure_thumb` is called by
  libphig's `phig_thumb_generate` and by gtk's collection seam, and is
  still `thumbs.rebuild`'s inside, not an operation of its own. A rule
```

- [ ] **Step 3: Check the rules**

```bash
python3 scripts/rules.py --check --emit
git status --short rules/
```

Expected: `rules: 84 rules, 18 operations, OK`, and only `rules/README.md`
modified (it is not generated, so `--emit` changes nothing else).

- [ ] **Step 4: Run the full gtk check and commit**

Run `gtk_check`, then:

```bash
git add gtk/src/main.cpp rules/README.md
git commit -m "Guard factory bind on the loader and widen core_internal's definition

core_internal said no caller outside core/, which ensure_thumb's two client
callers contradicted."
```

---

### Task 9: Write it down

**Files:**
- Modify: `gtk/CLAUDE.md`, `gtk/README.md`, `CLAUDE.md`
- Modify: `plans/068-gtk-hardening.md`, `plans/000-INDEX.md`

**Interfaces:** none.

- [ ] **Step 1: Count the gtk tests**

```bash
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk 2>&1 | tail -3
for t in collection gallery-model thumb-loader connect startup; do
  printf "%s " $t; env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-$t 2>&1 | grep -c "^ok"
done
```

Expected: 5 ctest tests, and 7, 10, 20, 4 and 3 cases: 44 in all. Use the
measured numbers below if they differ.

- [ ] **Step 2: Update `gtk/CLAUDE.md`**

- The paragraph about the client's argument: `phig_collection_default_catalog`
  becomes `phig::gtk::default_catalog`.
- In the first rule, after "A new capability is a new `collection.h`
  function.", add: "Every function in it is `noexcept`; one that can fail
  returns `std::expected` carrying phig's reason."
- In the "No exception unwinds into GTK" rule, replace "and every
  `collection.h` function catches." with "and every `collection.h` function
  catches. Connect signals with `phig::gtk::connect` (`src/connect.h`), which
  rejects a handler that is not `noexcept` at compile time. It is not named
  `signal.h`: `src/` is on the include path, and a `signal.h` there shadows
  the system header GLib includes."
- Replace the "C++ throughout" rule's second and third sentences, "No new C
  files. Three remain, `src/gallery-model.c`, `src/row-page.c` and
  `tests/test-gallery-model.c`, and plan 068 converts them.", with "No C
  files. `gtk/` may use `std::expected` and `std::move_only_function`; see
  root `CLAUDE.md` for why the rest of the tree may not."
- In the "Widgets and rendering stay on the main thread" rule, replace
  `phig_collection_thumb_lookup`/`phig_collection_thumb_generate` with
  `phig::gtk::thumb_lookup`/`phig::gtk::thumb_generate`, and append: "A
  `ThumbCallback` is destroyed exactly once, on the main thread, whether or
  not it ran, so it may hold the last reference to a GTK object. Capture what
  the result needs in it, such as a `GRef` to the list item."
- Add a rule after it:

```markdown
- **Background work owns its inputs.** A worker thread reads only what it was
  handed by value, never `PhigApp`: the window can close, and `main` free
  `PhigApp`, while it runs. `open_in_background` (`src/startup.h`) is the
  pattern. The application is held until startup finishes, and
  `startup_done` checks `app->window` before touching any widget.
```

- [ ] **Step 3: Update `gtk/README.md`**

- `phig_collection_default_catalog` becomes `phig::gtk::default_catalog`.
- `src/gallery-model.c` becomes `src/gallery-model.cpp`.

- [ ] **Step 4: Update root `CLAUDE.md`**

Replace "Its 28 tests across three binaries" with "Its 44 tests across five
binaries", using the numbers Step 1 measured.

- [ ] **Step 5: Close the plan**

In `plans/068-gtk-hardening.md`'s frontmatter, change `status: planned` to
`status: completed` and add `completed: <today's date>` after `created`.
Append:

```markdown
## Outcome

Shipped on branch `068-gtk-hardening`; execution plan
`docs/plans/2026-11-09-068-gtk-hardening.md`.

- The build is C++23. `gtk/` uses `std::expected` and
  `std::move_only_function`; root `CLAUDE.md` keeps the rest of the tree off
  both until the sanitizer jobs and Apple's libc++ catch up.
- `collection.h` is the `phig::gtk` namespace. Fallible calls return
  `std::expected` with phig's reason, and `thumb_generate` mirrors
  `EnsureThumb`. `row-page` is gone.
- `gtk/` has no C files. `PhigImageRow` and `PhigGalleryModel` hold C++
  state, placement-new'd in `instance_init`.
- Signals connect through `phig::gtk::connect` in `src/connect.h`. The spec
  named it `signal.h`, which would shadow the system header.
- The loader destroys every callback exactly once, on the main thread, which
  closes 061's cancelled-request leak.
- The startup worker reads its own copy of the catalog path, the application
  is held until it finishes, and a closed window clears `app->window`.
- <the CI run's result for the toolchains in Decision 1, with its run id>
```

In `plans/000-INDEX.md`, change 068's status cell from `planned` to
`completed`. Re-read both and confirm they agree.

- [ ] **Step 6: Commit**

```bash
git add gtk/CLAUDE.md gtk/README.md CLAUDE.md plans/068-gtk-hardening.md plans/000-INDEX.md
git commit -m "Document gtk's C++ seam, signal helper and ownership rules; close plan 068"
```

---

## Final verification

- [ ] Every build and suite, from clean configure:

```bash
cmake -B build && cmake --build build -j 2>&1 | grep -E "warning|error"
ctest --test-dir build --parallel 2>&1 | tail -3
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON -DPHIG_WERROR=ON
cmake --build build/with-gtk -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk --parallel 2>&1 | tail -3
CC=clang CXX=clang++ cmake -B build/with-gtk-clang -DPHIG_BUILD_GTK=ON -DPHIG_WERROR=ON
cmake --build build/with-gtk-clang -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk-clang -L gtk 2>&1 | tail -3
cmake --build build/with-gtk-asan --target gtk-all -j 2>&1 | grep -E "warning|error"
ASAN_OPTIONS=detect_leaks=0 env -u DISPLAY -u WAYLAND_DISPLAY \
  ctest --test-dir build/with-gtk-asan -L gtk 2>&1 | tail -3
python3 scripts/rules.py --check --emit && git status --short
mise exec -- ./scripts/clang-tidy.sh > /tmp/068-tidy.log 2>&1; echo "exit $?"
$CF --dry-run --Werror gtk/src/connect.h gtk/src/startup.h gtk/src/startup.cpp \
  gtk/src/collection.h gtk/src/collection.cpp gtk/src/gallery-model.cpp \
  gtk/tests/test-connect.cpp gtk/tests/test-startup.cpp gtk/tests/test-collection.cpp \
  gtk/tests/fake-collection.h gtk/tests/fake-collection.cpp
```

Expected: no warning lines; `1018` phig tests and `1023` with gtk (1018 +
five gtk binaries), all passing; the clang and ASan gtk runs at `100%`; rules OK with a
clean tree; clang-tidy exit 0; clang-format silent.

- [ ] Nothing outside `gtk/` uses the gtk-only library features:

```bash
grep -rn "std::expected\|std::move_only_function\|<expected>" core libphig cli tests
```

Expected: no output.

- [ ] The CI run from Task 1 Step 7, or a fresh `gh workflow run CI --ref
  068-gtk-hardening` with the user's approval, is green on every job,
  macOS and both sanitizers included. The branch does not merge until it is.

- [ ] With the user's approval (it opens a window), the real-run check from
  `gtk/CLAUDE.md`: a full-range scroll over a scratch catalog of public
  images, built as in Task 7 Step 9 from `/home/brett/phig-corpus`, run with
  `PHIG_MEASURE=1`, prints 0 GTK warnings.
