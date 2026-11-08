# 061 — GTK Links `core/` Directly: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `gtk/` reads phig through `core/` instead of libphig's C ABI, joins
the root CMake build behind `PHIG_BUILD_GTK`, and its tests run without a
collection.

**Architecture:** One C-callable seam, `gtk/src/collection.h`, is the only way
gtk reaches phig. `collection.cpp` implements it over `phig-core`;
`tests/fake-collection.cpp` implements it for the tests, which then link no
engine. `gallery-model.c` stays C. `thumb-loader` and `main` become C++ built
from plain structs, `std::` containers and a `GRef` owning pointer. The
thumbnail-generation logic libphig holds today moves into `core/` first, so
gtk and libphig call one function.

**Tech Stack:** C11 and C++20, GTK 4 / libadwaita, GLib's `g_test`, CMake
3.20, GoogleTest for the `core/` change.

**Spec:** `plans/061-gtk-core-linking.md`. Read it before starting a task; it
explains why each piece is shaped the way it is.

## Global Constraints

- **Never write to `~/.local/share/phig/` or `/mnt/data/Pictures`.** Every test
  builds its own database and thumbnail directory in a temp dir. No step points
  a test at a real collection.
- **No `phig_*` ABI behaviour changes.** `phig_thumb_generate` keeps returning
  `PHIG_OK`, `PHIG_ERR_DB`, `PHIG_ERR_NOT_FOUND` and `PHIG_ERR_INTERNAL` for the
  same inputs. The existing `AbiThumbGenerate.*` tests must pass unmodified.
- **A default build is unchanged.** `cmake -B build && cmake --build build`
  must not configure `gtk/` or need gtk4 or libadwaita.
- **Only `gtk/src/collection.cpp` includes a `core/` header.** Widget code calls
  `collection.h`.
- **`gallery-model.c` stays C.** Its two GObjects are required by GTK, and
  GObject instance memory is not C++-constructed.
- **No class hierarchies in code phig writes.** No gtkmm. Use plain structs,
  free functions, and `GRef<T> = std::unique_ptr<T, GObjectUnref>`.
- **No exception unwinds into GLib or GTK.** Every C++ function passed to GLib
  or GTK (signal handlers, idle, timeout and GTask callbacks, destroy notifies)
  is `noexcept`. Every `collection.h` function catches everything.
- **Do not run clang-format on existing `gtk/` files.** `gtk/` is outside the
  lint gate and its files do not conform (136 findings at the start of this
  plan). Reformatting them would hide the real diff. New files may be formatted
  with the pinned clang-format. Files under `core/`, `libphig/` and `tests/`
  must pass the pinned clang-format and clang-tidy as usual.
- **gtk build directories go under `build/`**, which is gitignored: use
  `build/with-gtk` and, for the one sanitizer step, `build/with-gtk-asan`. Do
  not reconfigure the main `build/` with `PHIG_BUILD_GTK=ON`.
- **Comments describe the present.** Plan numbers and history go in commit
  messages, not comments.
- **Commit messages are plain prose, with no AI attribution.**

Pinned tools:

```bash
CF=~/.local/share/mise/installs/pipx-clang-format/21.1.8/bin/clang-format
CT_PATH=~/.local/share/mise/installs/pipx-clang-tidy/21.1.6/bin
```

## Decisions this plan makes beyond 061

These are not in the spec. A reviewer should check each against it.

1. **`ensure_thumb` moves into `core/` (Task 1).** Generating one row's
   thumbnail on demand needs a row lookup, a cached check, a source check, and
   a video-keyframe branch. That logic lives in `libphig/thumbs.cpp` today.
   Root `CLAUDE.md` puts non-GUI logic in `core/`, and without the move gtk
   would hold a second copy.
2. **The seam bakes in the gallery's one query:** newest effective date first,
   with id as the tiebreak. That is the query `main.c` builds today. GTK M1's
   sidebar filters will extend `collection.h`; nothing here anticipates them.
3. **gtk tests carry the ctest label `gtk`, and a `gtk-all` target builds them
   with the client.** CI builds `gtk-all` and runs `-L gtk`, so adding a test
   binary needs no CI edit.
4. **The fake writes real PNG files.** `ThumbLoader` loads textures from disk
   paths, so a fake that only returned paths could not exercise it.
5. **One GTK-side leak is recorded, not fixed.** When a request is cancelled
   after dispatch, `on_thumb_result` never runs, so its `ThumbResultData` and
   the `GtkListItem` reference it holds are never released. Fixing this needs
   a destroy-notify on `thumb_loader_request`. That is an API change 061 does
   not ask for, so Task 8 records it in 061's outcome as a follow-up.

## File map

| File | Task | Responsibility |
|---|---|---|
| `core/thumbs_engine.h`, `core/thumbs_engine.cpp` | 1 | `ensure_thumb`: one row's thumbnail, generated if absent |
| `libphig/thumbs.cpp` | 1 | `phig_thumb_generate` delegates to `ensure_thumb` |
| `tests/test_thumbs_engine.cpp` | 1 | `EnsureThumb.*` tests |
| `CMakeLists.txt` | 2 | `PHIG_BUILD_GTK` option, `add_subdirectory(gtk)` |
| `gtk/CMakeLists.txt` | 2–7 | gtk targets inside the root build |
| `.github/workflows/ci.yml` | 2 | gtk job configures the root build |
| `gtk/src/collection.h` | 3 | The seam: C interface, paged reads, thumbnail lookup and generation |
| `gtk/src/row-page.c` | 3 | `PhigRowPage` allocation, shared by both implementations |
| `gtk/src/collection.cpp` | 3 | The seam over `phig-core` |
| `gtk/tests/test-collection.cpp` | 3 | `collection.cpp` against a database it builds |
| `gtk/tests/fake-collection.h`, `.cpp` | 4 | The seam for tests: synthetic rows, PNG thumbnails |
| `gtk/src/gallery-model.h`, `.c` | 4 | Reads through the seam (stays C) |
| `gtk/tests/test-gallery-model.c` | 4 | Against the fake; no skip |
| `gtk/src/thumb-loader.h`, `.c` | 5 | Reads through the seam; eviction fix |
| `gtk/tests/test-thumb-loader.cpp` | 5, 7 | Against the fake; no skip |
| `gtk/src/main.c` → `main.cpp` | 4, 5, 6, 7 | Opens the collection; becomes C++ in 6 |
| `gtk/src/gref.h` | 6 | `GRef<T>`, `gref`, `adopt` |
| `gtk/src/thumb-loader.c` → `.cpp` | 7 | C++ rewrite: `shared_ptr` loader, `unique_ptr` requests |
| Root `CLAUDE.md`, `gtk/CLAUDE.md`, `gtk/README.md`, `libphig/CLAUDE.md`, `plans/047-…`, `plans/061-…`, `plans/000-INDEX.md` | 8 | The boundary change, written down |

## Pre-flight

- [ ] Confirm the start state:

```bash
cd /home/brett/devel/phig
git status --short          # expect: clean
git rev-parse HEAD          # record as BASE; Task 8 diffs against it
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -3
python3 scripts/rules.py --check
```

Expected: `100% tests passed` and `rules: … OK`. Write BASE and the passing
test count in the ledger.

---

### Task 1: `ensure_thumb` in `core/`

**Files:**
- Modify: `core/thumbs_engine.h` (append after `thumb_needs_generating`)
- Modify: `core/thumbs_engine.cpp`
- Modify: `libphig/thumbs.cpp:1-61`
- Test: `tests/test_thumbs_engine.cpp`

**Interfaces:**
- Produces: `enum class EnsureThumb { Ok, NoSuchRow, SourceMissing, Failed };`
  and `EnsureThumb ensure_thumb(Database& db, int64_t image_id, int size);` in
  `core/thumbs_engine.h`. Task 3 calls it.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_thumbs_engine.cpp`. The fixture uses `XDG_CACHE_HOME`, as
`ThumbsEngineTest` does, so thumbnails land in the temp dir.

```cpp
// ---- ensure_thumb: one row's thumbnail, generated on demand ----

class EnsureThumbTest : public ::testing::Test {
   protected:
    std::string orig_cache;
    fs::path temp_dir;
    std::unique_ptr<Database> db;

    void SetUp() override {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg) orig_cache = xdg;
        temp_dir = make_temp_dir("ensure-thumb");
        setenv("XDG_CACHE_HOME", temp_dir.c_str(), 1);
        db = std::make_unique<Database>((temp_dir / "phig.db").string());
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        if (orig_cache.empty()) {
            unsetenv("XDG_CACHE_HOME");
        } else {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        }
        fs::remove_all(temp_dir);
    }

    int64_t add_row(const std::string& path, const std::string& sha,
                    const std::string& media_type = "image") {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = fs::path(path).extension().string().substr(1);
        info.modified_at = "2021-01-01T00:00:00Z";
        info.sha256 = sha;
        info.media_type = media_type;
        db->insert(info);
        return db->get_image_id(path);
    }
};

TEST_F(EnsureThumbTest, GeneratesAMissingThumbnail) {
    std::string sha(64, 'a');
    int64_t id = add_row("tests/data/faces_small.jpg", sha);
    ASSERT_FALSE(thumb_exists(sha, 256));

    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Ok);
    EXPECT_TRUE(thumb_exists(sha, 256));
    EXPECT_EQ(get_thumb_path(sha, 256).rfind(temp_dir.string(), 0), 0u);
}

TEST_F(EnsureThumbTest, ACachedThumbnailNeedsNoSourceFile) {
    // The cached check comes before the source check, so a row whose file has
    // gone still reports its cached thumbnail.
    std::string sha(64, 'b');
    int64_t id = add_row("/nonexistent/b.jpg", sha);
    fs::path thumb = get_thumb_path(sha, 256);
    fs::create_directories(thumb.parent_path());
    std::ofstream(thumb) << "cached";

    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Ok);
}

TEST_F(EnsureThumbTest, AnUnknownIdOrAnUnhashedRowIsNoSuchRow) {
    int64_t unhashed = add_row("tests/data/faces_small.jpg", "");
    EXPECT_EQ(ensure_thumb(*db, 999999, 256), EnsureThumb::NoSuchRow);
    EXPECT_EQ(ensure_thumb(*db, unhashed, 256), EnsureThumb::NoSuchRow);
}

TEST_F(EnsureThumbTest, AMissingSourceIsSourceMissing) {
    int64_t id = add_row("/nonexistent/c.jpg", std::string(64, 'c'));
    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::SourceMissing);
}

TEST_F(EnsureThumbTest, AnUndecodableSourceIsFailed) {
    fs::path junk = temp_dir / "junk.jpg";
    std::ofstream(junk) << "not a jpeg";
    int64_t id = add_row(junk.string(), std::string(64, 'd'));
    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Failed);
    EXPECT_FALSE(thumb_exists(std::string(64, 'd'), 256));
}

TEST_F(EnsureThumbTest, AVideoRowIsGeneratedFromItsKeyframe) {
    std::string sha(64, 'e');
    int64_t id = add_row("tests/data/test_red.mov", sha, "video");
    EXPECT_EQ(ensure_thumb(*db, id, 256), EnsureThumb::Ok);
    EXPECT_TRUE(thumb_exists(sha, 256));
}
```

Add `#include <memory>` to the file's includes if it is not already there.

- [ ] **Step 2: Run them and confirm they fail**

```bash
cmake --build build -j 2>&1 | grep -E "error|ensure_thumb" | head
```

Expected: compile errors naming `ensure_thumb` and `EnsureThumb`.

- [ ] **Step 3: Declare `ensure_thumb`**

Append to `core/thumbs_engine.h`, after `thumb_needs_generating`:

```cpp
// The outcome of ensure_thumb. NoSuchRow covers an unknown id and a row with no
// sha256: neither has a cache key to write to.
enum class EnsureThumb { Ok, NoSuchRow, SourceMissing, Failed };

// Makes sure one row's thumbnail exists at `size` (> 0), generating it from the
// row's file if it is not cached. A video is generated from its keyframe. This
// is the single-row, on-demand counterpart to ThumbsEngine::run, for a client
// that generates as cells scroll into view. Safe to call concurrently,
// including for the same row.
EnsureThumb ensure_thumb(Database& db, int64_t image_id, int size);
```

- [ ] **Step 4: Implement it**

In `core/thumbs_engine.cpp`, add `#include <filesystem>` to the includes and
append:

```cpp
EnsureThumb ensure_thumb(Database& db, int64_t image_id, int size) {
    ImageInfo img;
    if (!db.get_image_for_thumb(image_id, img) || img.sha256.empty()) {
        return EnsureThumb::NoSuchRow;
    }
    if (thumb_exists(img.sha256, size)) return EnsureThumb::Ok;

    std::error_code ec;
    if (!std::filesystem::exists(img.path, ec)) return EnsureThumb::SourceMissing;

    bool ok = false;
    if (img.media_type == "video") {
        auto kf = extract_video_keyframe(img.path);
        if (!kf.frame.empty()) ok = generate_thumb(kf.frame, img.sha256, size);
    } else {
        ok = generate_thumb(img.path, img.sha256, size);
    }
    return ok ? EnsureThumb::Ok : EnsureThumb::Failed;
}
```

- [ ] **Step 5: Make `phig_thumb_generate` delegate**

In `libphig/thumbs.cpp`, replace the body of `phig_thumb_generate` (from
`PHIG_TRY(ctx)` to `PHIG_CATCH(ctx)`) with:

```cpp
    PHIG_TRY(ctx)
    switch (ensure_thumb(*ctx->db, image_id, size)) {
        case EnsureThumb::Ok:
            return PHIG_OK;
        case EnsureThumb::NoSuchRow:
            return PHIG_ERR_DB;
        case EnsureThumb::SourceMissing:
            return PHIG_ERR_NOT_FOUND;
        case EnsureThumb::Failed:
            return PHIG_ERR_INTERNAL;
    }
    return PHIG_ERR_INTERNAL;
    PHIG_CATCH(ctx)
```

Keep the `if (size <= 0) return PHIG_ERR_INVALID_ARG;` guard above it. Remove
the includes this leaves unused: `<filesystem>`, `"hasher.h"` and `"types.h"`.
`internal.h` already includes `thumbs_engine.h`.

- [ ] **Step 6: Run the new tests and the ABI tests**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='EnsureThumbTest.*:AbiThumbGenerate.*:AbiThumb*'
```

Expected: all pass, including the six `AbiThumbGenerate` cases, which are unmodified.

- [ ] **Step 7: Full suite, rules, lint**

```bash
ctest --test-dir build --parallel 2>&1 | tail -3
python3 scripts/rules.py --check
$CF --dry-run --Werror core/thumbs_engine.h core/thumbs_engine.cpp libphig/thumbs.cpp tests/test_thumbs_engine.cpp
PATH="$CT_PATH:$PATH" ./scripts/clang-tidy.sh 2>&1 | tail -5
```

Expected: `100% tests passed`, `rules: … OK`, no clang-format output, no
clang-tidy findings. If `rules.py` reports an unclassified name, it means
`ensure_thumb` needs a record. Add it to `core_internal` on the
`thumbs.rebuild` operation in `rules/operations.toml`, then rerun with `--emit`
and commit the regenerated views.

- [ ] **Step 8: Commit**

```bash
git add core/thumbs_engine.h core/thumbs_engine.cpp libphig/thumbs.cpp tests/test_thumbs_engine.cpp
git commit -m "Move on-demand thumbnail generation into core as ensure_thumb

libphig's phig_thumb_generate held the only copy of the single-row path:
look the row up, return early if cached, check the source exists, take a
video's keyframe. The GTK client needs the same thing without the ABI, so
it now lives in core and phig_thumb_generate maps its result to the same
status codes as before."
```

---

### Task 2: `gtk/` joins the root build

**Files:**
- Modify: `CMakeLists.txt` (append at the end)
- Replace: `gtk/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml:372-424`

**Interfaces:**
- Produces: the `PHIG_BUILD_GTK` option. gtk targets are `phig-gtk` and
  `test-<name>`, and gtk tests are labelled `gtk`. Output goes to
  `<build>/gtk/`.

The code is unchanged in this task. gtk still links libphig, now as the
in-tree `phig_abi` target instead of through pkg-config.

- [ ] **Step 1: Add the option to the root build**

Append to the end of `CMakeLists.txt`, after the perf tests. It must come after
`enable_testing()`, so that ctest sees gtk's tests:

```cmake
# ---- GTK client ----
# Off by default, so a plain build needs neither gtk4 nor libadwaita. With it
# on, `cmake --build build --target phig-gtk` builds the client and the phig
# libraries it links, and nothing else.
option(PHIG_BUILD_GTK "Build the GTK4/libadwaita client in gtk/" OFF)
if(PHIG_BUILD_GTK)
    add_subdirectory(gtk)
endif()
```

- [ ] **Step 2: Replace `gtk/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)

if(NOT TARGET phig-core)
    message(FATAL_ERROR "gtk/ is configured from the repository root: "
                        "cmake -B build -DPHIG_BUILD_GTK=ON")
endif()

project(phig-gtk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PHIG_GTK_WARNINGS -Wall -Wextra)

pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4 libadwaita-1)

add_executable(phig-gtk src/main.c src/gallery-model.c src/thumb-loader.c)
target_compile_options(phig-gtk PRIVATE ${PHIG_GTK_WARNINGS})
target_link_libraries(phig-gtk PRIVATE phig_abi PkgConfig::GTK)

# `--target gtk-all` builds the client and every gtk test binary.
add_custom_target(gtk-all)
add_dependencies(gtk-all phig-gtk)

# One test binary per source under test, labelled `gtk` so CI and
# `ctest -L gtk` select them without naming each one.
function(phig_gtk_test name)
    add_executable(test-${name} ${ARGN})
    target_compile_options(test-${name} PRIVATE ${PHIG_GTK_WARNINGS})
    target_include_directories(test-${name} PRIVATE src tests)
    target_link_libraries(test-${name} PRIVATE PkgConfig::GTK)
    add_test(NAME ${name} COMMAND test-${name})
    set_tests_properties(${name} PROPERTIES LABELS gtk)
    add_dependencies(gtk-all test-${name})
endfunction()

phig_gtk_test(gallery-model tests/test-gallery-model.c src/gallery-model.c)
target_link_libraries(test-gallery-model PRIVATE phig_abi)
set_tests_properties(gallery-model PROPERTIES SKIP_RETURN_CODE 77)

phig_gtk_test(thumb-loader tests/test-thumb-loader.c src/thumb-loader.c)
target_link_libraries(test-thumb-loader PRIVATE phig_abi)
set_tests_properties(thumb-loader PROPERTIES SKIP_RETURN_CODE 77)
```

- [ ] **Step 3: Build it and run its tests**

```bash
cmake -B build/with-gtk -DPHIG_BUILD_GTK=ON
cmake --build build/with-gtk --target gtk-all -j
ctest --test-dir build/with-gtk -L gtk
```

Expected: `phig-gtk`, `test-gallery-model` and `test-thumb-loader` build, and
ctest reports 2 tests, both `***Skipped` (no `PHIG_TEST_DB`).

- [ ] **Step 4: Confirm that a default build is untouched and that a standalone gtk configure fails clearly**

```bash
cmake --build build --target help | grep -c gtk      # main build dir, option off
cmake -S gtk -B /tmp/phig-gtk-standalone 2>&1 | grep -A1 "configured from the repository root"
rm -rf /tmp/phig-gtk-standalone
```

Expected: `0`, then the FATAL_ERROR text.

- [ ] **Step 5: Update the CI gtk job**

In `.github/workflows/ci.yml`, replace the comment above `gtk:` (lines 372–375)
with:

```yaml
  # gtk/ builds inside the root build with -DPHIG_BUILD_GTK=ON. Building only
  # the gtk targets keeps phig-tests and phig-perf-tests, which the ubuntu
  # test job owns, out of this job. Linux only — gtk/ is the Linux client (see
  # "Platform boundaries" in CLAUDE.md).
```

Then replace the three steps from `- name: Build and install libphig` to the
end of `- name: Test gtk client` with:

```yaml
      - name: Build gtk client
        run: |
          cmake -B build -DPHIG_WERROR=ON -DPHIG_BUILD_GTK=ON \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
          cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN)" --target gtk-all
          ccache --show-stats

      - name: Test gtk client
        run: ctest --test-dir build -L gtk --output-on-failure --parallel
```

`gtk-all` depends on every test binary `phig_gtk_test` creates, so a gtk test
added later is built without another CI edit.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt gtk/CMakeLists.txt .github/workflows/ci.yml
git commit -m "Build gtk/ inside the root build behind PHIG_BUILD_GTK

gtk/ was a separate CMake project that found libphig through pkg-config,
so CI installed libphig to a prefix before building it. It is now a
subdirectory of the root build, off by default, linking the in-tree
phig_abi target. Its tests carry the gtk label so CI selects them with
-L gtk."
```

---

### Task 3: the seam, over `core/`

**Files:**
- Create: `gtk/src/collection.h`
- Create: `gtk/src/row-page.c`
- Create: `gtk/src/collection.cpp`
- Create: `gtk/tests/test-collection.cpp`
- Modify: `gtk/CMakeLists.txt`

**Interfaces:**
- Consumes: `ensure_thumb` and `EnsureThumb` (Task 1); `Database`,
  `get_thumb_path`, `thumb_exists` and `default_db_path` from `core/`.
- Produces: everything in `collection.h` below. Tasks 4–7 use these names and
  types exactly.

- [ ] **Step 1: Write `gtk/src/collection.h`**

```c
#ifndef PHIG_GTK_COLLECTION_H
#define PHIG_GTK_COLLECTION_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* The gtk client's only route into phig. src/collection.cpp implements it over
 * core/; the tests link tests/fake-collection.cpp instead. Every function may
 * be called from any thread and never lets an exception escape.
 *
 * Reads are paged: no call returns the whole collection. A full-collection
 * read through core/ carries every row's EXIF blob, about 3.8 KB a row that
 * the client never reads. */
typedef struct PhigCollection PhigCollection;

/* One gallery row. Strings belong to the PhigRowPage holding the row. */
typedef struct {
    int64_t id;
    char* path;
    char* sha256; /* "" when phig has not hashed the row */
    char* filename;
} PhigRowData;

typedef struct {
    PhigRowData* rows;
    guint n_rows;
} PhigRowPage;

/* For implementations of this interface: a page of n_rows zeroed rows. */
PhigRowPage* phig_row_page_new(guint n_rows);
void phig_row_page_free(PhigRowPage* page);

/* Opens the database at db_path read-only. Returns NULL and sets *error (free
 * with g_free) if the file is missing or unreadable, or is at a schema version
 * this build does not read. Never creates a file. */
PhigCollection* phig_collection_open(const char* db_path, char** error);
void phig_collection_close(PhigCollection* coll);

/* phig's default database path ($XDG_DATA_HOME/phig/phig.db). Free with g_free. */
char* phig_collection_default_path(void);

/* Rows are in gallery order: newest effective date first, id breaking ties. */
gboolean phig_collection_count(PhigCollection* coll, guint* out);

/* Rows [offset, offset + limit) in gallery order. NULL on failure; a page past
 * the end has n_rows == 0. */
PhigRowPage* phig_collection_page(PhigCollection* coll, guint offset, guint limit);

typedef enum {
    PHIG_THUMB_PRESENT, /* the file at *path exists */
    PHIG_THUMB_ABSENT,  /* *path is where it would be; generate to create it */
    PHIG_THUMB_ERROR    /* sha256 or size is not valid; *path is NULL */
} PhigThumbLookup;

/* Where the cached thumbnail for (sha256, size) is. Sets *path (free with
 * g_free) unless the result is PHIG_THUMB_ERROR. */
PhigThumbLookup phig_collection_thumb_lookup(PhigCollection* coll, const char* sha256, int size,
                                             char** path);

/* Makes sure row image_id has a cached thumbnail at size, generating it if
 * absent. FALSE if the row is unknown or unhashed, or its file is missing or
 * cannot be decoded. */
gboolean phig_collection_thumb_generate(PhigCollection* coll, int64_t image_id, int size);

G_END_DECLS

#endif /* PHIG_GTK_COLLECTION_H */
```

- [ ] **Step 2: Write `gtk/src/row-page.c`**

```c
#include "collection.h"

PhigRowPage* phig_row_page_new(guint n_rows) {
    PhigRowPage* page = g_new0(PhigRowPage, 1);
    page->n_rows = n_rows;
    page->rows = g_new0(PhigRowData, n_rows);
    return page;
}

void phig_row_page_free(PhigRowPage* page) {
    if (page == NULL)
        return;
    for (guint i = 0; i < page->n_rows; i++) {
        g_free(page->rows[i].path);
        g_free(page->rows[i].sha256);
        g_free(page->rows[i].filename);
    }
    g_free(page->rows);
    g_free(page);
}
```

- [ ] **Step 3: Write the failing test, `gtk/tests/test-collection.cpp`**

```cpp
#include <glib.h>

#include <filesystem>
#include <string>

#include "collection.h"
#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A database of three rows dated 2021, 2023 and 2022 in insert order, so the
// gallery's newest-first order differs from insertion order. The 2023 row is a
// real JPEG, so a thumbnail can be generated from it. XDG_CACHE_HOME points
// into the temp dir, so generated thumbnails never reach the real cache.
struct Fixture {
    fs::path dir;
    std::string db_path;

    Fixture() {
        char* tmp = g_dir_make_tmp("phig-gtk-collection-XXXXXX", nullptr);
        g_assert_nonnull(tmp);
        dir = tmp;
        g_free(tmp);
        g_setenv("XDG_CACHE_HOME", (dir / "cache").c_str(), TRUE);
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
        g_unsetenv("XDG_CACHE_HOME");
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

    PhigCollection* open() const {
        char* error = nullptr;
        PhigCollection* coll = phig_collection_open(db_path.c_str(), &error);
        if (coll == nullptr) g_error("open failed: %s", error);
        return coll;
    }
};

void test_open_reports_a_missing_file_without_creating_it() {
    Fixture f;
    fs::path absent = f.dir / "absent.db";
    char* error = nullptr;
    PhigCollection* coll = phig_collection_open(absent.c_str(), &error);
    g_assert_null(coll);
    g_assert_nonnull(error);
    g_assert_cmpstr(error, !=, "");
    g_free(error);
    g_assert_false(fs::exists(absent));
}

void test_pages_come_newest_first() {
    Fixture f;
    PhigCollection* coll = f.open();

    guint n = 0;
    g_assert_true(phig_collection_count(coll, &n));
    g_assert_cmpuint(n, ==, 3);

    PhigRowPage* page = phig_collection_page(coll, 0, 2);
    g_assert_nonnull(page);
    g_assert_cmpuint(page->n_rows, ==, 2);
    g_assert_cmpstr(page->rows[0].sha256, ==, std::string(64, 'b').c_str());
    g_assert_cmpstr(page->rows[0].filename, ==, "faces_small.jpg");
    g_assert_cmpstr(page->rows[1].sha256, ==, std::string(64, 'c').c_str());
    phig_row_page_free(page);

    page = phig_collection_page(coll, 2, 2);
    g_assert_cmpuint(page->n_rows, ==, 1);
    g_assert_cmpstr(page->rows[0].path, ==, "/nowhere/a.jpg");
    phig_row_page_free(page);

    page = phig_collection_page(coll, 3, 2);
    g_assert_nonnull(page);
    g_assert_cmpuint(page->n_rows, ==, 0);
    phig_row_page_free(page);

    phig_collection_close(coll);
}

void test_a_thumbnail_is_generated_on_demand() {
    Fixture f;
    PhigCollection* coll = f.open();
    std::string sha(64, 'b');

    char* path = nullptr;
    g_assert_cmpint(phig_collection_thumb_lookup(coll, sha.c_str(), 256, &path), ==,
                    PHIG_THUMB_ABSENT);
    g_assert_nonnull(path);
    g_assert_true(g_str_has_prefix(path, f.dir.c_str()));
    g_free(path);

    PhigRowPage* page = phig_collection_page(coll, 0, 1);
    int64_t id = page->rows[0].id;
    phig_row_page_free(page);

    g_assert_true(phig_collection_thumb_generate(coll, id, 256));
    g_assert_cmpint(phig_collection_thumb_lookup(coll, sha.c_str(), 256, &path), ==,
                    PHIG_THUMB_PRESENT);
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    g_free(path);

    phig_collection_close(coll);
}

void test_generation_fails_for_a_missing_source_or_an_unknown_id() {
    Fixture f;
    PhigCollection* coll = f.open();
    PhigRowPage* page = phig_collection_page(coll, 2, 1);  // the /nowhere/a.jpg row
    g_assert_false(phig_collection_thumb_generate(coll, page->rows[0].id, 256));
    phig_row_page_free(page);
    g_assert_false(phig_collection_thumb_generate(coll, 999999, 256));
    phig_collection_close(coll);
}

void test_lookup_rejects_a_malformed_sha() {
    Fixture f;
    PhigCollection* coll = f.open();
    char* path = nullptr;
    g_assert_cmpint(phig_collection_thumb_lookup(coll, "abc", 256, &path), ==, PHIG_THUMB_ERROR);
    g_assert_null(path);
    phig_collection_close(coll);
}

void test_default_path_follows_xdg_data_home() {
    g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
    char* path = phig_collection_default_path();
    g_assert_cmpstr(path, ==, "/tmp/phig-gtk-xdg-data/phig/phig.db");
    g_free(path);
    g_unsetenv("XDG_DATA_HOME");
    g_assert_false(g_file_test("/tmp/phig-gtk-xdg-data", G_FILE_TEST_EXISTS));
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/collection/open-reports-a-missing-file-without-creating-it",
                    test_open_reports_a_missing_file_without_creating_it);
    g_test_add_func("/collection/pages-come-newest-first", test_pages_come_newest_first);
    g_test_add_func("/collection/a-thumbnail-is-generated-on-demand",
                    test_a_thumbnail_is_generated_on_demand);
    g_test_add_func("/collection/generation-fails-for-a-missing-source-or-an-unknown-id",
                    test_generation_fails_for_a_missing_source_or_an_unknown_id);
    g_test_add_func("/collection/lookup-rejects-a-malformed-sha",
                    test_lookup_rejects_a_malformed_sha);
    g_test_add_func("/collection/default-path-follows-xdg-data-home",
                    test_default_path_follows_xdg_data_home);
    return g_test_run();
}
```

Add to the end of `gtk/CMakeLists.txt`:

```cmake
# Tests collection.cpp itself, against a database it builds, so unlike the
# other gtk tests it links phig-core.
phig_gtk_test(collection tests/test-collection.cpp src/collection.cpp src/row-page.c)
target_link_libraries(test-collection PRIVATE phig-core)
target_compile_definitions(test-collection PRIVATE PHIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
```

- [ ] **Step 4: Run it and confirm it fails**

```bash
cmake --build build/with-gtk --target test-collection -j 2>&1 | grep -E "error|undefined" | head
```

Expected: a link failure (`undefined reference to phig_collection_open`, among
others), or "No such file" for `collection.cpp`.

- [ ] **Step 5: Write `gtk/src/collection.cpp`**

```cpp
#include "collection.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "database.h"
#include "paths.h"
#include "thumbs.h"
#include "thumbs_engine.h"
#include "types.h"

struct PhigCollection {
    std::unique_ptr<Database> db;
};

namespace {

// GTK reaches this file through C frames, which an exception must not unwind
// through, so every entry point catches and reports here.
void warn_caught(const char* what) noexcept {
    try {
        throw;
    } catch (const std::exception& e) {
        g_warning("phig collection: %s failed: %s", what, e.what());
    } catch (...) {
        g_warning("phig collection: %s failed", what);
    }
}

}  // namespace

extern "C" {

PhigCollection* phig_collection_open(const char* db_path, char** error) {
    *error = nullptr;
    try {
        auto coll = std::make_unique<PhigCollection>();
        coll->db = std::make_unique<Database>(db_path, /*read_only=*/true);
        // A read-only connection cannot migrate, and every query below assumes
        // this build's schema.
        int have = coll->db->schema_version();
        int want = Database::expected_schema_version();
        if (have != want) {
            *error = g_strdup_printf(
                "The collection is at schema version %d and this build reads version %d.", have,
                want);
            return nullptr;
        }
        return coll.release();
    } catch (const std::exception& e) {
        *error = g_strdup(e.what());
    } catch (...) {
        *error = g_strdup("Could not open the phig database.");
    }
    return nullptr;
}

void phig_collection_close(PhigCollection* coll) { delete coll; }

char* phig_collection_default_path(void) {
    try {
        return g_strdup(default_db_path().c_str());
    } catch (...) {
        warn_caught("default path");
    }
    return nullptr;
}

gboolean phig_collection_count(PhigCollection* coll, guint* out) {
    try {
        int64_t n = coll->db->count_matching(Database::SearchCriteria{});
        *out = n > 0 ? static_cast<guint>(n) : 0;
        return TRUE;
    } catch (...) {
        warn_caught("count");
    }
    return FALSE;
}

PhigRowPage* phig_collection_page(PhigCollection* coll, guint offset, guint limit) {
    try {
        Database::PageCriteria c;
        c.sort = Database::SortKey::Date;
        c.descending = true;
        c.offset = offset;
        c.limit = static_cast<int32_t>(limit);
        std::vector<ImageInfo> rows = coll->db->search_page(c);

        PhigRowPage* page = phig_row_page_new(static_cast<guint>(rows.size()));
        for (size_t i = 0; i < rows.size(); i++) {
            page->rows[i].id = rows[i].id;
            page->rows[i].path = g_strdup(rows[i].path.c_str());
            page->rows[i].sha256 = g_strdup(rows[i].sha256.c_str());
            page->rows[i].filename = g_strdup(rows[i].filename.c_str());
        }
        return page;
    } catch (...) {
        warn_caught("page");
    }
    return nullptr;
}

PhigThumbLookup phig_collection_thumb_lookup(PhigCollection* coll, const char* sha256, int size,
                                             char** path) {
    (void)coll;  // the thumbnail cache is located by core/paths, not by the database
    *path = nullptr;
    try {
        if (sha256 == nullptr || size <= 0) return PHIG_THUMB_ERROR;
        std::string p = get_thumb_path(sha256, size);
        if (p.empty()) return PHIG_THUMB_ERROR;
        *path = g_strdup(p.c_str());
        return thumb_exists(sha256, size) ? PHIG_THUMB_PRESENT : PHIG_THUMB_ABSENT;
    } catch (...) {
        warn_caught("thumbnail lookup");
    }
    g_free(*path);
    *path = nullptr;
    return PHIG_THUMB_ERROR;
}

gboolean phig_collection_thumb_generate(PhigCollection* coll, int64_t image_id, int size) {
    if (size <= 0) return FALSE;
    try {
        return ensure_thumb(*coll->db, image_id, size) == EnsureThumb::Ok;
    } catch (...) {
        warn_caught("thumbnail generation");
    }
    return FALSE;
}

}  // extern "C"
```

If `get_thumb_path("abc", 256)` does not return `""`, the malformed-sha test
fails. In that case, add a 64-hex-digit check before calling it. Copy
`is_hex64` from `libphig/thumbs.cpp`; don't call into libphig.

- [ ] **Step 6: Run the test**

```bash
cmake --build build/with-gtk --target test-collection -j && ctest --test-dir build/with-gtk -R '^collection$' --output-on-failure
```

Expected: `1/1 Test #…: collection ……… Passed`. Run
`./build/with-gtk/gtk/test-collection` directly as well, and confirm the
output shows all 6 cases as `ok`.

- [ ] **Step 7: Commit**

```bash
git add gtk/src/collection.h gtk/src/row-page.c gtk/src/collection.cpp gtk/tests/test-collection.cpp gtk/CMakeLists.txt
git commit -m "Add the gtk client's seam into core: collection.h

A C interface the client reaches phig through, implemented over phig-core:
open read-only, count and page the gallery query, look up and generate one
thumbnail. Reads stay paged and every entry point catches, since GTK calls
in through C frames. Tested against a database the test builds."
```

---

### Task 4: the fake, and `gallery-model.c` on the seam

**Files:**
- Create: `gtk/tests/fake-collection.h`, `gtk/tests/fake-collection.cpp`
- Modify: `gtk/src/gallery-model.h`, `gtk/src/gallery-model.c`
- Replace: `gtk/tests/test-gallery-model.c`
- Modify: `gtk/src/main.c` (startup and shutdown only)
- Modify: `gtk/CMakeLists.txt`

**Interfaces:**
- Consumes: `collection.h` (Task 3).
- Produces: `PhigGalleryModel* phig_gallery_model_new(PhigCollection* coll);`.
  Also these fake functions, which Task 5 uses: `fake_collection_new(guint)`,
  `fake_collection_sha(guint, char[65])`, `fake_collection_fail_pages`,
  `fake_collection_set_thumb_size`, `fake_collection_generate_count`.

- [ ] **Step 1: Write `gtk/tests/fake-collection.h`**

```c
#ifndef PHIG_GTK_FAKE_COLLECTION_H
#define PHIG_GTK_FAKE_COLLECTION_H

#include "collection.h"

G_BEGIN_DECLS

/* A PhigCollection with n_rows synthetic rows and no database. Row i (0-based)
 * has id i + 1, path "/fake/<i>.jpg", filename "<i>.jpg" and the sha256
 * fake_collection_sha gives for i. Thumbnails live in a private temp directory
 * that phig_collection_close removes. Generating one writes a 64x64 PNG
 * unless fake_collection_set_thumb_size says otherwise. Generation fails for
 * an id outside [1, n_rows]. */
PhigCollection* fake_collection_new(guint n_rows);

/* Row index's sha256: the index in hex, zero-padded to 64 digits. */
void fake_collection_sha(guint index, char out[65]);

/* While fail is set, phig_collection_page returns NULL. */
void fake_collection_fail_pages(PhigCollection* coll, gboolean fail);

void fake_collection_set_thumb_size(PhigCollection* coll, int width, int height);

/* How many thumbnail files phig_collection_thumb_generate has written. */
guint fake_collection_generate_count(PhigCollection* coll);

G_END_DECLS

#endif /* PHIG_GTK_FAKE_COLLECTION_H */
```

- [ ] **Step 2: Write `gtk/tests/fake-collection.cpp`**

`phig_collection_open` and `phig_collection_default_path` are not defined here.
No test that links the fake calls them.

```cpp
#include "fake-collection.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

struct PhigCollection {
    guint n_rows = 0;
    std::filesystem::path thumb_dir;
    std::atomic<bool> fail_pages{false};
    std::atomic<int> thumb_width{64};
    std::atomic<int> thumb_height{64};
    std::atomic<guint> generate_count{0};
};

namespace {

std::string sha_of(guint index) {
    char sha[65];
    fake_collection_sha(index, sha);
    return sha;
}

bool is_hex64(const char* s) {
    if (s == nullptr) return false;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        if (n >= 64 || !g_ascii_isxdigit(s[n])) return false;
    }
    return n == 64;
}

std::filesystem::path thumb_path(const PhigCollection& coll, const char* sha256, int size) {
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

extern "C" {

PhigCollection* fake_collection_new(guint n_rows) {
    auto* coll = new PhigCollection();
    coll->n_rows = n_rows;
    char* dir = g_dir_make_tmp("phig-gtk-fake-XXXXXX", nullptr);
    g_assert_nonnull(dir);
    coll->thumb_dir = dir;
    g_free(dir);
    return coll;
}

void fake_collection_sha(guint index, char out[65]) { g_snprintf(out, 65, "%064x", index); }

void fake_collection_fail_pages(PhigCollection* coll, gboolean fail) {
    coll->fail_pages = fail != FALSE;
}

void fake_collection_set_thumb_size(PhigCollection* coll, int width, int height) {
    coll->thumb_width = width;
    coll->thumb_height = height;
}

guint fake_collection_generate_count(PhigCollection* coll) { return coll->generate_count.load(); }

void phig_collection_close(PhigCollection* coll) {
    if (coll == nullptr) return;
    std::error_code ec;
    std::filesystem::remove_all(coll->thumb_dir, ec);
    delete coll;
}

gboolean phig_collection_count(PhigCollection* coll, guint* out) {
    *out = coll->n_rows;
    return TRUE;
}

PhigRowPage* phig_collection_page(PhigCollection* coll, guint offset, guint limit) {
    if (coll->fail_pages.load()) return nullptr;
    guint end = offset >= coll->n_rows ? offset : std::min(coll->n_rows, offset + limit);
    PhigRowPage* page = phig_row_page_new(end - offset);
    for (guint i = offset; i < end; i++) {
        PhigRowData* row = &page->rows[i - offset];
        row->id = static_cast<int64_t>(i) + 1;
        row->path = g_strdup_printf("/fake/%u.jpg", i);
        row->filename = g_strdup_printf("%u.jpg", i);
        row->sha256 = g_strdup(sha_of(i).c_str());
    }
    return page;
}

PhigThumbLookup phig_collection_thumb_lookup(PhigCollection* coll, const char* sha256, int size,
                                             char** path) {
    *path = nullptr;
    if (!is_hex64(sha256) || size <= 0) return PHIG_THUMB_ERROR;
    std::filesystem::path p = thumb_path(*coll, sha256, size);
    *path = g_strdup(p.c_str());
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? PHIG_THUMB_PRESENT : PHIG_THUMB_ABSENT;
}

gboolean phig_collection_thumb_generate(PhigCollection* coll, int64_t image_id, int size) {
    if (image_id < 1 || image_id > static_cast<int64_t>(coll->n_rows) || size <= 0) return FALSE;
    std::string sha = sha_of(static_cast<guint>(image_id - 1));
    std::filesystem::path p = thumb_path(*coll, sha.c_str(), size);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return TRUE;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!write_png(p, coll->thumb_width.load(), coll->thumb_height.load())) return FALSE;
    coll->generate_count++;
    return TRUE;
}

}  // extern "C"
```

- [ ] **Step 3: Replace `gtk/tests/test-gallery-model.c` with the fake-backed version**

```c
#include <glib.h>

#include "fake-collection.h"
#include "gallery-model.h"

/* Five full pages of 200 and a short sixth of 50. */
#define N_ROWS 1050

typedef struct {
    PhigCollection* coll;
    PhigGalleryModel* model;
} Fixture;

static void fixture_set_up(Fixture* f, gconstpointer unused) {
    (void)unused;
    f->coll = fake_collection_new(N_ROWS);
    f->model = phig_gallery_model_new(f->coll);
}

/* The model goes first: its dispose waits for any page fetch still using the
 * collection. */
static void fixture_tear_down(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_clear_object(&f->model);
    phig_collection_close(f->coll);
}

static void test_n_items_matches_count(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f->model)), ==, N_ROWS);
}

static void test_out_of_range_returns_null(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_assert_null(g_list_model_get_item(G_LIST_MODEL(f->model), N_ROWS));
    g_assert_null(g_list_model_get_item(G_LIST_MODEL(f->model), N_ROWS + 1000));
}

static void test_get_item_returns_a_row_without_blocking(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* A cold model has no resident page, so this takes the placeholder path. A
     * synchronous implementation would have installed the page by the time
     * get_item returns, so the page still being non-resident is what shows a
     * placeholder came back rather than a blocking fetch. */
    GObject* item = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
    g_assert_nonnull(item);
    g_assert_true(PHIG_IS_IMAGE_ROW(item));
    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(item)));
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 0);
    g_object_unref(item);
}

static void test_rows_carry_the_collections_fields(Fixture* f, gconstpointer unused) {
    (void)unused;
    phig_gallery_model_fetch_page_sync(f->model, 1);
    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 250));
    g_assert_false(phig_image_row_is_placeholder(row));

    char sha[65];
    fake_collection_sha(250, sha);
    g_assert_cmpint(phig_image_row_get_id(row), ==, 251);
    g_assert_cmpstr(phig_image_row_get_sha256(row), ==, sha);
    g_assert_cmpstr(phig_image_row_get_path(row), ==, "/fake/250.jpg");
    g_assert_cmpstr(phig_image_row_get_filename(row), ==, "250.jpg");
    g_object_unref(row);
}

static void test_last_page_is_short(Fixture* f, gconstpointer unused) {
    (void)unused;
    phig_gallery_model_fetch_page_sync(f->model, 5);
    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), N_ROWS - 1));
    g_assert_false(phig_image_row_is_placeholder(row));
    g_assert_cmpint(phig_image_row_get_id(row), ==, N_ROWS);
    g_object_unref(row);
}

static void test_row_strings_survive_page_eviction(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* A row owns copies of its strings, so it stays valid after its page is
     * freed and evicted. */
    phig_gallery_model_fetch_page_sync(f->model, 0);
    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    char* sha_copy = g_strdup(phig_image_row_get_sha256(row));

    for (guint page = 0; page * 200 < N_ROWS; page++)
        phig_gallery_model_fetch_page_sync(f->model, page);

    g_assert_cmpstr(phig_image_row_get_sha256(row), ==, sha_copy);
    g_free(sha_copy);
    g_object_unref(row);
}

static void test_placeholders_are_unique_per_position(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* GTK's list-item manager keys recycled widgets by item pointer identity
     * (glib#3030): one placeholder shared across positions makes it pick a
     * widget for the wrong position. GtkSelectionModel separately requires the
     * same object for repeated queries of one position. Both must hold. */
    GObject* a1 = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
    GObject* b1 = g_list_model_get_item(G_LIST_MODEL(f->model), 400);
    GObject* a2 = g_list_model_get_item(G_LIST_MODEL(f->model), 0);

    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(a1)));
    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(b1)));
    g_assert_true(a1 != b1);
    g_assert_true(a1 == a2);

    g_object_unref(a1);
    g_object_unref(b1);
    g_object_unref(a2);
}

static void test_resident_pages_stay_bounded(Fixture* f, gconstpointer unused) {
    (void)unused;
    for (guint page = 0; page * 200 < N_ROWS; page++)
        phig_gallery_model_fetch_page_sync(f->model, page);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), <=,
                     phig_gallery_model_get_max_resident_pages(f->model));
}

typedef struct {
    GMainLoop* loop;
    gboolean fired;
    guint position;
    guint removed;
    guint added;
} ItemsChangedCapture;

static void on_items_changed(GListModel* model, guint position, guint removed, guint added,
                             gpointer user_data) {
    (void)model;
    ItemsChangedCapture* capture = user_data;
    capture->fired = TRUE;
    capture->position = position;
    capture->removed = removed;
    capture->added = added;
    g_main_loop_quit(capture->loop);
}

static gboolean on_timeout(gpointer user_data) {
    g_main_loop_quit(user_data);
    return G_SOURCE_REMOVE;
}

static void disarm_timeout(guint timeout_id) {
    if (g_main_context_find_source_by_id(NULL, timeout_id) != NULL)
        g_source_remove(timeout_id);
}

static void test_async_fetch_emits_items_changed(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* The real path: get_item schedules a GTask fetch and returns a
     * placeholder. The completion callback runs on the main loop and emits the
     * items-changed a GtkGridView needs to re-query the range. */
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    ItemsChangedCapture capture = {.loop = loop, .fired = FALSE};
    gulong handler_id =
        g_signal_connect(f->model, "items-changed", G_CALLBACK(on_items_changed), &capture);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, loop);

    g_object_unref(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_signal_handler_disconnect(f->model, handler_id);
    g_main_loop_unref(loop);

    g_assert_true(capture.fired);
    g_assert_cmpuint(capture.position, ==, 0);
    g_assert_cmpuint(capture.removed, ==, 200);
    g_assert_cmpuint(capture.added, ==, 200);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 1);
}

typedef struct {
    GMainLoop* loop;
    gboolean fired;
} FailedCapture;

static void on_page_fetch_failed(PhigGalleryModel* model, gpointer user_data) {
    (void)model;
    FailedCapture* capture = user_data;
    capture->fired = TRUE;
    g_main_loop_quit(capture->loop);
}

static void test_failed_fetch_emits_page_fetch_failed(Fixture* f, gconstpointer unused) {
    (void)unused;
    fake_collection_fail_pages(f->coll, TRUE);
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    FailedCapture capture = {.loop = loop, .fired = FALSE};
    gulong handler_id = g_signal_connect(f->model, "page-fetch-failed",
                                         G_CALLBACK(on_page_fetch_failed), &capture);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, loop);
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "gallery model: page 0 fetch failed*");

    g_object_unref(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_signal_handler_disconnect(f->model, handler_id);
    g_main_loop_unref(loop);

    g_test_assert_expected_messages();
    g_assert_true(capture.fired);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 0);
}

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, NULL);
#define T(name, fn) \
    g_test_add("/gallery-model/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)
    T("n-items-matches-count", test_n_items_matches_count);
    T("out-of-range-returns-null", test_out_of_range_returns_null);
    T("get-item-no-block", test_get_item_returns_a_row_without_blocking);
    T("rows-carry-the-collections-fields", test_rows_carry_the_collections_fields);
    T("last-page-is-short", test_last_page_is_short);
    T("row-strings-survive-eviction", test_row_strings_survive_page_eviction);
    T("resident-pages-bounded", test_resident_pages_stay_bounded);
    T("placeholders-unique-per-position", test_placeholders_are_unique_per_position);
    T("async-fetch-emits-items-changed", test_async_fetch_emits_items_changed);
    T("failed-fetch-emits-page-fetch-failed", test_failed_fetch_emits_page_fetch_failed);
#undef T
    return g_test_run();
}
```

In `gtk/CMakeLists.txt`, replace the gallery-model test block with:

```cmake
# Linked against tests/fake-collection.cpp in place of src/collection.cpp, so
# it needs no collection and no phig-core.
phig_gtk_test(gallery-model tests/test-gallery-model.c src/gallery-model.c src/row-page.c
              tests/fake-collection.cpp)
```

Remove its `phig_abi` link and its `SKIP_RETURN_CODE 77`.

- [ ] **Step 4: Run it and confirm it fails to build**

```bash
cmake --build build/with-gtk --target test-gallery-model -j 2>&1 | grep error | head
```

Expected: errors such as `phig_gallery_model_new` taking `phig_ctx*` rather than
`PhigCollection*`.

- [ ] **Step 5: Move `gallery-model` onto the seam**

`gtk/src/gallery-model.h`:
- Replace `#include <phig/phig.h>` with `#include "collection.h"`.
- Replace the `PhigImageRow` comment's first paragraph with: "PhigImageRow: one
  row of the gallery. Its strings are owned copies taken out of a PhigRowPage
  before the page is freed, so a row stays valid after its page is evicted."
- In the `PhigGalleryModel` comment, change "over a phig_query's results" to
  "over the collection's gallery rows", change "(from phig_query_count)" to
  "(from phig_collection_count)", and change "running phig_query_page" to
  "running phig_collection_page".
- Replace the constructor's comment and declaration with:

```c
/* Caller keeps ownership of coll and must not close it while any reference to
 * the model is outstanding: page fetches use it from a worker thread for as
 * long as the model exists. */
PhigGalleryModel* phig_gallery_model_new(PhigCollection* coll);
```

`gtk/src/gallery-model.c`:
- In the `PHIG_PAGE_SIZE` comment, change "per phig_query_page" to "per page fetch".
- Replace `phig_image_row_new_from_image` (with its comment) with:

```c
/* Copies every field it exposes out of data, whose strings belong to its page. */
static PhigImageRow* phig_image_row_new_from_data(const PhigRowData* data) {
    PhigImageRow* row = g_object_new(PHIG_TYPE_IMAGE_ROW, NULL);
    row->placeholder = FALSE;
    row->id = data->id;
    row->path = g_strdup(data->path);
    row->sha256 = g_strdup(data->sha256);
    row->filename = g_strdup(data->filename);
    return row;
}
```

- In `struct _PhigGalleryModel`, replace `phig_ctx* ctx;` and
  `phig_query* query;` with `PhigCollection* coll;`. In the in-flight comment,
  replace "ctx and query are borrowed" with "coll is borrowed", "the ctx/query
  it points at" with "the collection", "inside phig_query_page on ctx/query"
  with "inside phig_collection_page", and "must not free ctx/query" with "must
  not close coll". Apply the same wording to every comment in the file that
  mentions `ctx`, `query`, `phig_query_page` or `phig_image_list`.
- Replace `phig_page_new_from_list` (with its comment) with:

```c
/* Builds owned rows from a freshly fetched page and frees it. Called on
 * whichever thread fetched the page (worker for async, the caller's for the
 * sync test helper); it touches no GTK or main-loop state. */
static PhigPage* phig_page_new_from_rows(guint page_index, PhigRowPage* fetched) {
    PhigPage* page = g_new0(PhigPage, 1);
    page->page_index = page_index;
    page->n_rows = fetched->n_rows;
    page->rows = g_new0(PhigImageRow*, page->n_rows);
    for (guint i = 0; i < page->n_rows; i++)
        page->rows[i] = phig_image_row_new_from_data(&fetched->rows[i]);
    phig_row_page_free(fetched);
    return page;
}
```

- In `FetchTaskData`, change `phig_image_list* fetched;` to `PhigRowPage* fetched;`.
  In `fetch_task_data_free`, change `phig_image_list_free` to `phig_row_page_free`.
- In `fetch_page_thread_func`, replace everything from `phig_image_list* list = NULL;`
  to `data->fetched = list;` with:

```c
    PhigRowPage* fetched =
        phig_collection_page(self->coll, data->page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    phig_gallery_model_leave_in_flight(self);

    if (fetched == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "phig_collection_page failed");
        return;
    }
    data->fetched = fetched;
```

- In `fetch_page_done`, change `phig_page_new_from_list` to `phig_page_new_from_rows`
  and the trailing comment to `/* consumed: phig_page_new_from_rows freed it */`.
- Replace `phig_gallery_model_new` and `phig_gallery_model_fetch_page_sync` with:

```c
PhigGalleryModel* phig_gallery_model_new(PhigCollection* coll) {
    PhigGalleryModel* self = g_object_new(PHIG_TYPE_GALLERY_MODEL, NULL);
    self->coll = coll;
    guint total = 0;
    self->total = phig_collection_count(coll, &total) ? total : 0;
    return self;
}

void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index) {
    PhigRowPage* fetched =
        phig_collection_page(self->coll, page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    if (fetched == NULL)
        return;
    phig_gallery_model_install_page(self, phig_page_new_from_rows(page_index, fetched));
}
```

Confirm nothing is left behind:

```bash
grep -nE "phig_ctx|phig_query|phig_image_list|phig_image\b|ctx" gtk/src/gallery-model.[ch]
```

Expected: no output.

- [ ] **Step 6: Open the collection in `main.c` for the model**

The thumbnail loader still uses `phig_ctx` until Task 5, so for one commit
`main.c` opens both.

- Add `#include "collection.h"` after `#include "gallery-model.h"`.
- In `PhigApp`, after `phig_ctx* ctx;`, add `PhigCollection* coll;`. Change the
  comment above to "Owned once startup finishes; NULL until then. Freed in
  order by phig_app_free: the loader and the model both use them from worker
  threads."
- In `phig_app_free`, after `gtk_grid_view_set_model(app->grid, NULL);`, add:

```c
    if (app->coll != NULL)
        phig_collection_close(app->coll);
```

- In `StartupResult`, replace `phig_query* query;` with `PhigCollection* coll;`
  and the struct comment with "On failure ctx and coll are NULL or closed by
  startup_done."
- In `startup_thread_func`, replace everything from
  `status = phig_query_new(...)` to `phig_query_set_sort(...);` with:

```c
    char* error = NULL;
    result->coll = phig_collection_open(result->app->db_path, &error);
    if (result->coll == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error);
        g_free(error);
        return;
    }
```

  Shorten the comment above the function to say that `phig_init`'s failure has
  no message to carry, and that the collection's error comes back as an owned
  copy.
- In `startup_done`'s failure branch, replace the `phig_query_free` lines with:

```c
        if (startup->coll != NULL)
            phig_collection_close(startup->coll);
```

- In `startup_done`'s success path, add `app->coll = startup->coll;` after
  `app->ctx = startup->ctx;`. Change
  `phig_gallery_model_new(app->ctx, startup->query)` to
  `phig_gallery_model_new(app->coll)`. Replace the comment above it with
  "The model borrows coll, which app owns until phig_app_free."
- Replace `startup_result_free`'s comment with "Owns nothing: on success
  startup_done moves ctx and coll onto app, and on failure it closes them."

`gtk/CMakeLists.txt`: add `src/collection.cpp src/row-page.c` to `phig-gtk`'s
sources and `phig-core` to its link libraries. Keep `phig_abi` for now.

- [ ] **Step 7: Build everything and run the gtk tests headless**

```bash
cmake --build build/with-gtk --target phig-gtk test-gallery-model test-thumb-loader test-collection -j
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk --output-on-failure
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-gallery-model
```

Expected: `gallery-model` and `collection` pass, and `thumb-loader` is still
skipped. The direct run lists 10 `ok` lines. Running headless matters because
CI has no display.

- [ ] **Step 8: Commit**

```bash
git add gtk/tests/fake-collection.h gtk/tests/fake-collection.cpp gtk/src/gallery-model.h gtk/src/gallery-model.c gtk/tests/test-gallery-model.c gtk/src/main.c gtk/CMakeLists.txt
git commit -m "Read gallery pages through collection.h; test against a fake

PhigGalleryModel takes a PhigCollection in place of a libphig context and
query. Its tests link a fake collection of synthetic rows, so they run
everywhere instead of skipping without PHIG_TEST_DB, and gain cases for
row contents, the short last page and the page-fetch-failed signal."
```

---

### Task 5: `thumb-loader.c` on the seam, and the eviction fix

**Files:**
- Modify: `gtk/src/thumb-loader.h`, `gtk/src/thumb-loader.c`
- Delete: `gtk/tests/test-thumb-loader.c`
- Create: `gtk/tests/test-thumb-loader.cpp`
- Modify: `gtk/src/main.c`
- Modify: `gtk/CMakeLists.txt`

**Interfaces:**
- Consumes: `collection.h`, plus `fake_collection_*` (Task 4).
- Produces: `ThumbLoader* thumb_loader_new(PhigCollection* coll, guint n_threads, gsize cache_bytes);`.
  The rest of the `thumb-loader.h` API is unchanged. `phig-gtk` no longer links
  `phig_abi`.

- [ ] **Step 1: Write the new test file, `gtk/tests/test-thumb-loader.cpp`**

`git rm gtk/tests/test-thumb-loader.c`, then create:

```cpp
#include <glib.h>
#include <gtk/gtk.h>

#include <string>

#include "fake-collection.h"
#include "thumb-loader.h"

namespace {

// Every fake thumbnail is 64x64, charged at width * height * 4 bytes.
constexpr gsize kTextureBytes = 64 * 64 * 4;
constexpr gsize kRoomy = 64 * 1024 * 1024;

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
    ThumbLoader* loader;

    explicit Fixture(gsize budget = kRoomy, guint threads = 4)
        : coll(fake_collection_new(8)), loader(thumb_loader_new(coll, threads, budget)) {}
    ~Fixture() {
        thumb_loader_shutdown(loader);
        g_object_unref(loader);
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

void on_capture(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    auto* capture = static_cast<Capture*>(user_data);
    capture->called = true;
    capture->result = result;
    capture->width = texture != nullptr ? gdk_texture_get_width(texture) : -1;
    if (capture->loop != nullptr) g_main_loop_quit(capture->loop);
}

void on_must_not_run(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    (void)result;
    (void)texture;
    *static_cast<bool*>(user_data) = true;
}

// Requests one thumbnail and runs the main loop until it is delivered, or for
// 5s. A texture-cache hit is delivered inside thumb_loader_request, before the
// loop would start, so the loop only runs when that did not happen.
Capture request_and_wait(ThumbLoader* loader, int64_t id, const std::string& sha, guint32 size) {
    Capture capture;
    capture.loop = g_main_loop_new(nullptr, FALSE);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, capture.loop);
    thumb_loader_request(loader, id, sha.c_str(), size, nullptr, on_capture, &capture);
    capture.synchronous = capture.called;
    if (!capture.called) g_main_loop_run(capture.loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
    g_main_loop_unref(capture.loop);
    capture.loop = nullptr;
    return capture;
}

void test_a_miss_is_generated_and_delivered() {
    Fixture f;
    Capture c = request_and_wait(f.loader, 1, sha_of(0), PHIG_THUMB_SIZE_DEFAULT);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_cmpint(c.width, ==, 64);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 1);
}

// The second loader has an empty texture cache, so its request reaches the
// collection, which finds the file the first one generated.
void test_a_thumbnail_on_disk_is_not_regenerated() {
    Fixture f;
    g_assert_cmpint(request_and_wait(f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);

    ThumbLoader* second = thumb_loader_new(f.coll, 1, kRoomy);
    Capture c = request_and_wait(second, 1, sha_of(0), 256);
    thumb_loader_shutdown(second);
    g_object_unref(second);

    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_false(c.synchronous);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 1);
}

void test_a_repeat_request_is_served_from_memory() {
    Fixture f;
    g_assert_false(request_and_wait(f.loader, 1, sha_of(0), 256).synchronous);
    Capture c = request_and_wait(f.loader, 1, sha_of(0), 256);
    g_assert_true(c.synchronous);
    g_assert_cmpint(c.result, ==, THUMB_OK);
}

// A row phig has not hashed yet is data phig has not produced, not a failure.
void test_no_sha256_yields_thumb_miss() {
    Fixture f;
    Capture c = request_and_wait(f.loader, 1, "", 256);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_MISS);
    g_assert_cmpint(c.width, ==, -1);
}

// The row has a sha256 but its thumbnail cannot be generated: a failure the
// user should be able to see.
void test_generation_failure_yields_thumb_failed() {
    Fixture f;
    Capture c = request_and_wait(f.loader, -1, sha_of(0), 256);
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

    thumb_loader_request(f.loader, 1, sha_of(0).c_str(), 256, cancellable, on_must_not_run,
                         &called);
    g_assert_cmpint(thumb_loader_queue_length(f.loader), ==, 0);
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

    thumb_loader_request(f.loader, 1, sha_of(0).c_str(), 256, cancellable, on_must_not_run,
                         &called);
    for (int waited_ms = 0; thumb_loader_pending_idle_count(f.loader) == 0 && waited_ms < 5000;
         waited_ms++)
        g_usleep(1000);
    g_assert_cmpuint(thumb_loader_pending_idle_count(f.loader), >, 0);
    g_cancellable_cancel(cancellable);
    pump(300);

    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_cache_bytes(f.loader), ==, 0);
    g_object_unref(cancellable);
}

// Six textures against room for three and a half: the three most recent stay.
void test_the_cache_keeps_the_most_recent_within_budget() {
    Fixture f(kTextureBytes * 7 / 2);
    for (guint i = 0; i < 6; i++)
        g_assert_cmpint(request_and_wait(f.loader, i + 1, sha_of(i), 256).result, ==, THUMB_OK);

    g_assert_cmpuint(thumb_loader_cache_bytes(f.loader), ==, 3 * kTextureBytes);
    g_assert_true(request_and_wait(f.loader, 6, sha_of(5), 256).synchronous);
    g_assert_false(request_and_wait(f.loader, 1, sha_of(0), 256).synchronous);
}

// A texture larger than the whole budget is evicted by its own insert. It is
// still delivered intact, and the cache ends empty.
void test_a_texture_over_budget_is_delivered_but_not_kept() {
    Fixture f(kTextureBytes / 2);
    Capture c = request_and_wait(f.loader, 1, sha_of(0), 256);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_cmpint(c.width, ==, 64);
    g_assert_cmpuint(thumb_loader_cache_bytes(f.loader), ==, 0);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
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
    g_test_add_func("/thumb-loader/the-cache-keeps-the-most-recent-within-budget",
                    test_the_cache_keeps_the_most_recent_within_budget);
    g_test_add_func("/thumb-loader/a-texture-over-budget-is-delivered-but-not-kept",
                    test_a_texture_over_budget_is_delivered_but_not_kept);
    return g_test_run();
}
```

In `gtk/CMakeLists.txt`, replace the thumb-loader test block with:

```cmake
phig_gtk_test(thumb-loader tests/test-thumb-loader.cpp src/thumb-loader.c src/row-page.c
              tests/fake-collection.cpp)
```

- [ ] **Step 2: Move `thumb-loader` onto the seam (no fix yet)**

`gtk/src/thumb-loader.h`:
- Replace `#include <phig/phig.h>` with `#include "collection.h"`.
- Replace `thumb_loader_new`'s comment and declaration with:

```c
/* Caller keeps ownership of coll and must not close it while any reference to
 * the loader is outstanding, or before thumb_loader_shutdown: workers call into
 * it from their own threads. */
ThumbLoader* thumb_loader_new(PhigCollection* coll, guint n_threads, gsize cache_bytes);
```

- In the other comments, change `phig_thumb_lookup`/`phig_thumb_generate` to
  `phig_collection_thumb_lookup`/`phig_collection_thumb_generate`, and "on ctx"
  or "freeing ctx" to "on coll" or "closing coll".

`gtk/src/thumb-loader.c`:
- Remove `#include <limits.h>`.
- In `struct _ThumbLoader`, change `phig_ctx* ctx;` to `PhigCollection* coll;`.
- In `thumb_loader_worker`, replace everything from `char path[PATH_MAX];` to the
  closing brace of the `else { … gdk_texture_new_from_filename … }` block with:

```c
        /* Sizes are independent in phig's cache, so a 768 lookup does not see a
         * cached 512. The caller quantises max_dim, so this is a small set of
         * sizes rather than one per pixel. */
        int want = req->max_dim > 0 ? (int)req->max_dim : PHIG_THUMB_SIZE_DEFAULT;
        char* path = NULL;
        PhigThumbLookup look = phig_collection_thumb_lookup(self->coll, req->sha256, want, &path);
        gboolean generation_failed = FALSE;

        if (look == PHIG_THUMB_ABSENT) {
            /* Generation costs hundreds of milliseconds against about one for a
             * load, so a request the user has scrolled past stops here. */
            if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable)) {
                g_free(path);
                thumb_request_free(req);
                continue;
            }
            if (phig_collection_thumb_generate(self->coll, req->image_id, want)) {
                g_free(path);
                path = NULL;
                look = phig_collection_thumb_lookup(self->coll, req->sha256, want, &path);
            } else {
                generation_failed = TRUE;
            }
        }

        if (look == PHIG_THUMB_ERROR || generation_failed) {
            /* A failed generation is a real failure: the row has a sha256 and
             * phig could not decode the image. */
            req->result = THUMB_FAILED;
        } else if (look == PHIG_THUMB_ABSENT) {
            req->result = THUMB_MISS;
        } else {
            GError* error = NULL;
            /* phig wrote the file at the size asked for, so it loads without a
             * rescale, which keeps the texture as small as the budget assumes. */
            GdkTexture* texture = gdk_texture_new_from_filename(path, &error);
            if (texture == NULL) {
                g_warning("thumb loader: failed to load %s: %s", path,
                          error ? error->message : "unknown error");
                g_clear_error(&error);
                req->result = THUMB_FAILED;
            } else {
                req->result = THUMB_OK;
                req->texture = texture;
            }
        }
        g_free(path);
```

- In `thumb_loader_new`, change the parameter and `self->ctx = ctx;` to `coll`.
- Change every remaining comment that mentions `ctx`,
  `phig_thumb_lookup` or `phig_thumb_generate` in the same way as in the header.
  Then check:

```bash
grep -nE "phig_ctx|ctx\b|phig_thumb_(lookup|generate)|PATH_MAX" gtk/src/thumb-loader.[ch]
```

Expected: no output.

`gtk/src/main.c`: this drops libphig.
- Remove `#include <phig/phig.h>`.
- Remove `phig_ctx* ctx;` from `PhigApp` and `StartupResult`. Remove the
  `phig_init` block from `startup_thread_func`, including the `phig_config`
  setup. The function then opens only the collection. Update its comment so it
  mentions only the collection's error.
- Remove both `phig_shutdown` calls: one in `phig_app_free`, one in
  `startup_done`'s failure branch. Also remove `app->ctx = startup->ctx;`.
- Change `thumb_loader_new(app->ctx, …)` to `thumb_loader_new(app->coll, …)`.
- Update `phig_app_free`'s comment: the loader's workers and the model's
  in-flight page fetches both use the collection, so the loader is shut down
  and the model dropped before `phig_collection_close`.

```bash
grep -nE "phig_(ctx|init|shutdown|query|config|last_error|status|OK)\b|PHIG_(OK|READ_ONLY|SORT)" gtk/src/main.c
```

Expected: no output.

`gtk/CMakeLists.txt`: remove `phig_abi` from `phig-gtk`'s link libraries.

- [ ] **Step 3: Run the tests; all but the over-budget case should pass**

```bash
cmake --build build/with-gtk --target phig-gtk test-thumb-loader -j
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-thumb-loader
```

Expected: the first eight cases print `ok`. The over-budget case either
aborts on a `GLib-GObject-CRITICAL` (`g_test_init` makes criticals fatal) or
crashes. If it happens to pass, Step 4 still shows the defect.

- [ ] **Step 4: Show the defect under ASan**

```bash
cmake -B build/with-gtk-asan -DPHIG_BUILD_GTK=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build/with-gtk-asan --target test-thumb-loader -j
ASAN_OPTIONS=detect_leaks=0 env -u DISPLAY -u WAYLAND_DISPLAY \
  ./build/with-gtk-asan/gtk/test-thumb-loader -p /thumb-loader/a-texture-over-budget-is-delivered-but-not-kept 2>&1 | grep -m3 -E "AddressSanitizer|thumb_loader_cache_insert"
```

Expected: `ERROR: AddressSanitizer: heap-use-after-free`, with
`thumb_loader_cache_insert` in the stack. Record the first lines in the task
report as the RED evidence.

- [ ] **Step 5: Fix `thumb_loader_cache_insert`**

Replace the end of `thumb_loader_cache_insert`, from `thumb_loader_evict_locked(self);`
to `return ref;`, with:

```c
    /* The caller's reference is taken before evicting: a texture larger than
     * the whole budget is evicted by its own insert, and the entry is freed
     * with it. */
    GdkTexture* ref = g_object_ref(entry->texture);
    thumb_loader_evict_locked(self);
    g_mutex_unlock(&self->cache_lock);
    return ref;
```

Update the function's header comment so it says the returned reference is
taken before eviction.

- [ ] **Step 6: Run the tests, plain and under ASan**

```bash
cmake --build build/with-gtk --target test-thumb-loader -j
env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-thumb-loader
cmake --build build/with-gtk-asan --target test-thumb-loader -j
ASAN_OPTIONS=detect_leaks=0 env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk-asan/gtk/test-thumb-loader
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk --output-on-failure
```

Expected: 9 `ok` in both builds, no ASan report, and ctest `100% tests passed,
0 tests failed out of 3`.

- [ ] **Step 7: Confirm `phig-gtk` no longer reaches libphig**

```bash
grep -rn "phig/phig.h\|phig_abi" gtk/
ldd build/with-gtk/gtk/phig-gtk | grep -c "libphig\.so"
```

Expected: no grep output, and `0`.

- [ ] **Step 8: Commit**

```bash
git add -A gtk/src/thumb-loader.h gtk/src/thumb-loader.c gtk/tests/test-thumb-loader.c gtk/tests/test-thumb-loader.cpp gtk/src/main.c gtk/CMakeLists.txt
git commit -m "Load thumbnails through collection.h; fix over-budget eviction

ThumbLoader looks up and generates through the collection seam, so
phig-gtk no longer links libphig, and its tests run against the fake
instead of six thumbnails in one machine's cache.

thumb_loader_cache_insert took the caller's texture reference after
evicting. A texture larger than the whole budget is evicted by its own
insert, so that read a freed entry. Reachable through
PHIG_THUMB_CACHE_BYTES. The reference is now taken first, and a test
covers a texture over budget."
```

---

### Task 6: `main.c` becomes C++

**Files:**
- Rename: `gtk/src/main.c` → `gtk/src/main.cpp`
- Create: `gtk/src/gref.h`
- Modify: `gtk/src/collection.h` (append a C++ section)
- Modify: `gtk/CMakeLists.txt`

**Interfaces:**
- Produces: `GRef<T>`, `gref(T*)` and `adopt(T*)` in `gref.h`, and
  `CollectionPtr` in `collection.h`. Task 7 uses all of them.

The behaviour is unchanged. The task is done when the code compiles as C++,
uses RAII where the C version freed things by hand, and passes the same checks.

- [ ] **Step 1: Write `gtk/src/gref.h`**

```cpp
#pragma once

#include <glib-object.h>

#include <memory>

// An owning reference to a GObject. Move-only; destruction drops the ref.
struct GObjectUnref {
    void operator()(gpointer object) const noexcept { g_object_unref(object); }
};

template <class T>
using GRef = std::unique_ptr<T, GObjectUnref>;

// Takes a new reference on object, which may be NULL.
template <class T>
GRef<T> gref(T* object) {
    return GRef<T>(object != nullptr ? static_cast<T*>(g_object_ref(object)) : nullptr);
}

// Adopts a reference the caller already owns, such as a constructor's result.
template <class T>
GRef<T> adopt(T* object) {
    return GRef<T>(object);
}
```

- [ ] **Step 2: Add `CollectionPtr` to `collection.h`**

Insert before the final `#endif`:

```c
#ifdef __cplusplus
#include <memory>

struct PhigCollectionClose {
    void operator()(PhigCollection* coll) const noexcept { phig_collection_close(coll); }
};
using CollectionPtr = std::unique_ptr<PhigCollection, PhigCollectionClose>;
#endif
```

- [ ] **Step 3: Rename and switch the build**

```bash
git mv gtk/src/main.c gtk/src/main.cpp
```

In `gtk/CMakeLists.txt`, change `src/main.c` to `src/main.cpp` in `phig-gtk`'s
sources.

- [ ] **Step 4: Convert `main.cpp`**

Make these changes, and no others:

1. **Includes.** Replace `<math.h>` and `<stdlib.h>` with `<algorithm>`,
   `<cmath>`, `<cstdlib>`, `<memory>`, `<string>` and `<vector>`. Add
   `#include "gref.h"`.
2. **`PhigApp`** becomes a struct with member initialisers and a destructor
   that takes over `phig_app_free`:

```cpp
struct PhigApp {
    AdwApplicationWindow* window = nullptr;
    AdwToolbarView* toolbar_view = nullptr;
    GtkGridView* grid = nullptr;
    GtkScrolledWindow* scroller = nullptr;
    AdwToastOverlay* toast_overlay = nullptr;
    GtkLabel* failed_label = nullptr;  // header-bar counter for THUMB_FAILED results
    std::string db_path;

    // Set once startup finishes. The loader's workers and the model's page
    // fetches both use coll from worker threads, so ~PhigApp stops both before
    // coll, the last member, closes.
    CollectionPtr coll;
    ThumbLoader* loader = nullptr;

    guint thumb_failed_count = 0;
    bool page_fetch_toast_shown = false;  // one toast per session, not one per failed page

    gint64 startup_start_us = 0;  // g_get_monotonic_time() at process start, for PHIG_MEASURE

    // Zoom state. columns is the resting column count; pinch_cell tracks the
    // live size during a pinch so the snap can see which way it is going.
    guint target_cell_px = 0;     // what zoom sets; held across window resizes
    guint columns = 0;            // derived from target_cell_px and the viewport width
    guint relayout_id = 0;        // pending phig_relayout_idle, 0 if none
    bool relayout_needs_rebind = false;  // geometry changed: re-request textures
    double pinch_cell = 0.0;      // live target size during a pinch
    guint pinch_base_cell = 0;    // target size when the gesture began

    PhigApp() = default;
    PhigApp(const PhigApp&) = delete;
    PhigApp& operator=(const PhigApp&) = delete;
    ~PhigApp();
};

// Dropping the grid's model drops the model's last reference, so its dispose
// waits for in-flight page fetches before coll closes.
PhigApp::~PhigApp() {
    if (relayout_id != 0) g_source_remove(relayout_id);
    if (loader != nullptr) {
        thumb_loader_shutdown(loader);
        g_object_unref(loader);
    }
    if (grid != nullptr) gtk_grid_view_set_model(grid, nullptr);
}
```

   Delete `phig_app_free`.
3. **Measurement harness.** `PhigMeasureState` gets member initialisers and
   `std::vector<gint64> frame_deltas_us`. Allocate it with `new` in
   `phig_measure_maybe_start` and `delete` it where
   `phig_measure_state_free` was called. Delete `phig_measure_state_free` and
   `phig_measure_cmp_us`. In `phig_measure_tick`, use
   `state->frame_deltas_us.push_back(delta);`. Change `phig_measure_report` to
   take `const PhigMeasureState& state`, with this body before the `g_printerr`:

```cpp
    const std::vector<gint64>& deltas = state.frame_deltas_us;
    if (deltas.empty()) {
        g_printerr("PHIG_MEASURE scroll frames=0\n");
        return;
    }
    gint64 interval_us = state.refresh_interval_us > 0 ? state.refresh_interval_us : 16667;
    // Late = missed the next vsync deadline. A small tolerance keeps jitter in
    // the timestamp itself from reading as a drop.
    const gint64 tolerance_us = interval_us / 10;
    auto late = static_cast<guint>(std::count_if(
        deltas.begin(), deltas.end(), [&](gint64 d) { return d > interval_us + tolerance_us; }));

    std::vector<gint64> sorted = deltas;
    std::sort(sorted.begin(), sorted.end());
    gint64 p50 = sorted[sorted.size() / 2];
    gint64 p90 = sorted[static_cast<size_t>(sorted.size() * 0.90)];
    gint64 p99 = sorted[static_cast<size_t>(sorted.size() * 0.99)];
    gint64 worst = sorted.back();
```

   The `g_printerr` then uses `static_cast<guint>(deltas.size())` where it used
   `deltas->len`. Keep the comment above the function.
4. **Startup.** Replace `StartupResult` and `startup_result_free` with:

```cpp
// Handed from the startup worker to startup_done. coll is moved onto app on
// success, and closes with this struct otherwise.
struct StartupResult {
    PhigApp* app = nullptr;  // borrowed; app outlives the task
    CollectionPtr coll;
};
```

   In `startup_thread_func`, open with
   `result->coll.reset(phig_collection_open(result->app->db_path.c_str(), &error));`
   and test `if (!result->coll)`. In `startup_done`, remove the failure branch's
   close calls (the struct closes `coll`), and change `app->coll = startup->coll;`
   to `app->coll = std::move(startup->coll);`. Pass `app->coll.get()` to
   `thumb_loader_new` and `phig_gallery_model_new`. In `on_activate`, create the
   struct with `new StartupResult{}` and pass this as the destroy notify:

```cpp
    g_task_set_task_data(task, startup,
                         [](gpointer data) noexcept { delete static_cast<StartupResult*>(data); });
```

5. **`ThumbResultData`** becomes `struct ThumbResultData { GRef<GtkListItem> item; PhigApp* app; };`.
   In `on_factory_bind`, use `auto* data = new ThumbResultData{gref(item), app};`.
   In `on_thumb_result`, take ownership on entry with
   `std::unique_ptr<ThumbResultData> data(static_cast<ThumbResultData*>(user_data));`,
   use `data->item.get()` and `data->app`, and delete the two
   `g_object_unref(item)` calls.
6. **The default path.** Replace `default_db_path` with:

```cpp
// phig's own default, from core/ through the collection seam.
std::string default_db_path() {
    char* path = phig_collection_default_path();
    std::string out = path != nullptr ? path : "";
    g_free(path);
    return out;
}
```

7. **`main`:**

```cpp
int main(int argc, char** argv) {
    auto app = std::make_unique<PhigApp>();
    app->startup_start_us = g_get_monotonic_time();
    app->db_path = argc > 1 ? argv[1] : default_db_path();

    // argv is parsed above, not handed to GApplication: G_APPLICATION_DEFAULT_FLAGS
    // treats a bare positional argument as a file to open, which a db path is not.
    GRef<AdwApplication> gtk_app =
        adopt(adw_application_new("org.phig.Gtk", G_APPLICATION_DEFAULT_FLAGS));
    g_signal_connect(gtk_app.get(), "activate", G_CALLBACK(on_activate), app.get());
    int status = g_application_run(G_APPLICATION(gtk_app.get()), 1, argv);

    // g_application_run stops pumping the main context when it returns, so an
    // undispatched delivery never will be. ~PhigApp stops the loader's workers
    // and tears down the model explicitly rather than waiting on them.
    gtk_app.reset();
    app.reset();
    return status;
}
```

8. **Mechanical C-to-C++ fixes** (the compiler finds each of these):
   - Every implicit `void*` conversion becomes a `static_cast`. Examples:
     `PhigApp* app = user_data;` becomes `auto* app = static_cast<PhigApp*>(user_data);`,
     and `PhigImageRow* row = gtk_list_item_get_item(item);` becomes
     `auto* row = PHIG_IMAGE_ROW(gtk_list_item_get_item(item));`.
     `g_object_get_data` results are cast to `GCancellable*`.
   - The `|` of the two `GtkEventControllerScrollFlags` values is wrapped in
     `static_cast<GtkEventControllerScrollFlags>(…)`.
   - `TRUE`/`FALSE` assigned to members that are now `bool` become `true`/`false`.
   - The `(void)param;` lines may stay.
9. **`noexcept`.** Add `noexcept` to every function passed to `G_CALLBACK`,
   `g_idle_add`, `g_timeout_add`, `gtk_widget_add_tick_callback`,
   `g_task_new`, `g_task_run_in_thread` and `gtk_file_launcher_launch`, and to
   `on_thumb_result`.

- [ ] **Step 5: Build with no new warnings, then run**

```bash
cmake --build build/with-gtk --target phig-gtk -j 2>&1 | grep -E "warning|error" | grep main.cpp
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk --output-on-failure
grep -nE "g_free\(app\)|phig_app_free|qsort|startup_result_free|phig_measure_state_free" gtk/src/main.cpp
```

Expected: no warnings or errors from `main.cpp`, 3/3 tests pass, and the last
grep prints nothing.

Then run the app for a smoke test against a database the step builds. A
display is needed; if there is none, record the step as skipped in the
report.

```bash
SMOKE=$(mktemp -d)
export XDG_CACHE_HOME=$SMOKE/cache XDG_DATA_HOME=$SMOKE/data XDG_STATE_HOME=$SMOKE/state XDG_CONFIG_HOME=$SMOKE/config
./build/phig scan tests/data --db $SMOKE/phig.db >/dev/null
timeout 5 ./build/with-gtk/gtk/phig-gtk $SMOKE/phig.db; echo "exit=$?"
timeout 5 ./build/with-gtk/gtk/phig-gtk $SMOKE/absent.db; echo "exit=$?"
test -e $SMOKE/absent.db && echo "CREATED absent.db" || echo "absent.db not created"
unset XDG_CACHE_HOME XDG_DATA_HOME XDG_STATE_HOME XDG_CONFIG_HOME
rm -rf "$SMOKE"
```

The XDG exports keep the scan's log, lock and thumbnails out of the real
`~/.local` and `~/.cache`. Expected: both runs exit through the timeout
(`exit=124`) with no `CRITICAL` or `WARNING` lines on stderr. The first shows a
grid of `tests/data`. The second shows the "Could Not Open Collection" page,
and the check prints `absent.db not created`.

- [ ] **Step 6: Commit**

```bash
git add gtk/src/main.cpp gtk/src/gref.h gtk/src/collection.h gtk/CMakeLists.txt
git commit -m "Convert the gtk client's main to C++

PhigApp, the measurement state and the startup result now own what they
hold, so the four hand-written free functions are gone. std::sort replaces
qsort. Every function GLib or GTK calls into is noexcept, since those
calls arrive through C frames. Adds GRef, a unique_ptr that owns a GObject
reference, and CollectionPtr."
```

---

### Task 7: `thumb-loader` becomes C++

**Files:**
- Rename: `gtk/src/thumb-loader.c` → `gtk/src/thumb-loader.cpp` (rewritten)
- Replace: `gtk/src/thumb-loader.h`
- Modify: `gtk/tests/test-thumb-loader.cpp`, `gtk/src/main.cpp`, `gtk/CMakeLists.txt`

**Interfaces:**
- Consumes: `GRef`, `gref`, `adopt` (Task 6); `collection.h`.
- Produces: `std::shared_ptr<ThumbLoader> thumb_loader_new(PhigCollection*, guint, gsize)`.
  The other functions take `ThumbLoader&`. `ThumbResult`, `ThumbCallback` and
  `PHIG_THUMB_SIZE_DEFAULT` keep their names.

- [ ] **Step 1: Point the tests at the new API (they will not compile yet)**

In `gtk/tests/test-thumb-loader.cpp`:
- `Fixture::loader` becomes `std::shared_ptr<ThumbLoader> loader;`, and
  `~Fixture` becomes `{ thumb_loader_shutdown(*loader); loader.reset(); phig_collection_close(coll); }`.
- `request_and_wait` takes `ThumbLoader& loader` and calls
  `thumb_loader_request(loader, …)`.
- Every `f.loader` argument becomes `*f.loader`.
- In `test_a_thumbnail_on_disk_is_not_regenerated`, `second` is
  `std::shared_ptr<ThumbLoader>`. Call `request_and_wait(*second, …)` and
  `thumb_loader_shutdown(*second);`, then `second.reset();` in place of
  `g_object_unref`.
- Add `#include <memory>`.

In `gtk/CMakeLists.txt`, change `src/thumb-loader.c` to `src/thumb-loader.cpp`
in both `phig-gtk` and the thumb-loader test. Then:

```bash
git mv gtk/src/thumb-loader.c gtk/src/thumb-loader.cpp
cmake --build build/with-gtk --target test-thumb-loader -j 2>&1 | grep -m3 error
```

Expected: errors, because `thumb_loader_new` still returns `ThumbLoader*`.

- [ ] **Step 2: Replace `gtk/src/thumb-loader.h`**

```cpp
#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <memory>

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
using ThumbCallback = void (*)(ThumbResult result, GdkTexture* texture, gpointer user_data);

// An async, cancellable queue that finds a thumbnail through the collection,
// generating it on a miss, and loads it into a GdkTexture, backed by a
// byte-bounded LRU cache of textures.
//
// n_threads workers pull requests off the queue, look up or generate the file,
// and load it off the main thread. Results come back to the main thread
// through idle sources, each holding a reference to the loader, so the loader
// outlives every delivery it has posted. cache_bytes bounds the texture cache,
// charged at width * height * 4 per texture; the least recently used entry is
// evicted until the total fits.
struct ThumbLoader;

// Caller keeps ownership of coll and must not close it before calling
// thumb_loader_shutdown: workers call into it from their own threads.
std::shared_ptr<ThumbLoader> thumb_loader_new(PhigCollection* coll, guint n_threads,
                                              gsize cache_bytes);

// Stops and joins every worker and drops queued requests, so once this returns
// no thread is inside the collection and none will be again. Call it before
// closing coll, however many references to the loader remain — a posted
// delivery holds one. Deliveries still queued on the main context then deliver
// nothing. Safe to call more than once; the last reference also calls it.
void thumb_loader_shutdown(ThumbLoader& loader);

// Requests a thumbnail for image_id/sha256 (64 hex digits, or "" if phig has not
// hashed the row). image_id is what generation needs; the caller has it on the
// PhigImageRow, which saves reversing it from the sha.
//
// Asynchronous and cancellable, except on a texture-cache hit, which is
// delivered before this returns. A rebind onto a texture already in memory
// costs no worker round-trip and no second decode, which matters because zoom
// re-requests every visible cell at a new size.
//
// max_dim is the largest edge in PHYSICAL pixels (cell size times the scale
// factor); 0 means the cache's native size. Textures are cached under
// (sha256, max_dim), so callers should quantise it, or the cache grows a
// generation per distinct size.
//
// A miss generates the thumbnail before replying: hundreds of milliseconds
// against about one for a hit. Replies THUMB_MISS if the row has no sha256,
// THUMB_FAILED if generation or loading fails.
void thumb_loader_request(ThumbLoader& loader, int64_t image_id, const char* sha256,
                          guint32 max_dim, GCancellable* cancellable, ThumbCallback cb,
                          gpointer user_data);

// Test-only: bytes currently charged against cache_bytes.
gsize thumb_loader_cache_bytes(ThumbLoader& loader);

// Test-only: requests queued and not yet taken by a worker.
gint thumb_loader_queue_length(ThumbLoader& loader);

// Test-only: results posted for delivery that have not yet run, or been
// destroyed undelivered, on the main thread.
guint thumb_loader_pending_idle_count(ThumbLoader& loader);
```

- [ ] **Step 3: Write `gtk/src/thumb-loader.cpp`**

Replace the renamed file's contents with:

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
#include <utility>
#include <vector>

#include "gref.h"

namespace {

// One request. Exactly one place owns it at a time: the queue, then the worker
// that took it, then the idle source that delivers it.
struct ThumbRequest {
    int64_t image_id = 0;
    std::string sha256;
    guint32 max_dim = 0;  // 0 = the cache's native size
    GRef<GCancellable> cancellable;
    ThumbCallback cb = nullptr;
    gpointer user_data = nullptr;

    // Set by the worker before it posts the request to the main thread.
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

}  // namespace

struct ThumbLoader {
    PhigCollection* coll = nullptr;  // borrowed; see thumb_loader_new
    gsize cache_bytes_budget = 0;
    std::weak_ptr<ThumbLoader> self;  // what a worker hands to the idle source it posts

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::unique_ptr<ThumbRequest>> queue;
    bool stopping = false;  // guarded by queue_mutex
    std::vector<std::thread> workers;

    std::mutex cache_mutex;
    std::unordered_map<std::string, CacheEntry> cache;
    std::list<std::string> lru;  // front = least recently used
    gsize cache_bytes = 0;

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
    std::unique_ptr<ThumbRequest> req;
};

gboolean deliver(gpointer data) noexcept {
    auto& delivery = *static_cast<Delivery*>(data);
    ThumbRequest& req = *delivery.req;
    if (delivery.loader->stopped.load() || is_cancelled(req)) return G_SOURCE_REMOVE;

    if (req.result == THUMB_OK) {
        GRef<GdkTexture> texture =
            cache_insert(*delivery.loader, req.sha256, req.max_dim, std::move(req.texture));
        req.cb(THUMB_OK, texture.get(), req.user_data);
    } else {
        req.cb(req.result, nullptr, req.user_data);
    }
    return G_SOURCE_REMOVE;
}

// Runs exactly once per posted result, after deliver or instead of it. Deleting
// the Delivery may drop the last reference to the loader, which then joins its
// workers here, on the main thread.
void delivery_destroy(gpointer data) noexcept {
    auto* delivery = static_cast<Delivery*>(data);
    delivery->loader->pending_idle--;
    delete delivery;
}

void post_result(std::shared_ptr<ThumbLoader> loader, std::unique_ptr<ThumbRequest> req) {
    loader->pending_idle++;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver,
                    new Delivery{std::move(loader), std::move(req)}, delivery_destroy);
}

PhigThumbLookup lookup(ThumbLoader& self, const ThumbRequest& req, int size, std::string& path) {
    char* raw = nullptr;
    PhigThumbLookup look = phig_collection_thumb_lookup(self.coll, req.sha256.c_str(), size, &raw);
    path = raw != nullptr ? raw : "";
    g_free(raw);
    return look;
}

// Looks the thumbnail up, generates it on a miss, and loads it, setting
// req.result and req.texture. Returns false if the request was cancelled
// before generating.
bool resolve(ThumbLoader& self, ThumbRequest& req) {
    // Sizes are independent in phig's cache. The caller quantises max_dim, so
    // this is a small set of sizes rather than one per pixel.
    int want = static_cast<int>(req.max_dim > 0 ? req.max_dim : PHIG_THUMB_SIZE_DEFAULT);
    std::string path;
    PhigThumbLookup look = lookup(self, req, want, path);

    if (look == PHIG_THUMB_ABSENT) {
        // Generation costs hundreds of milliseconds against about one for a
        // load, so a request the user has scrolled past stops here.
        if (is_cancelled(req)) return false;
        if (!phig_collection_thumb_generate(self.coll, req.image_id, want)) {
            req.result = THUMB_FAILED;
            return true;
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
    return true;
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
        if (is_cancelled(*req)) continue;

        // A row phig has not hashed has no cache key. That is data phig has not
        // produced, not a failure, so it reports like a not-yet-generated miss.
        if (req->sha256.empty()) {
            req->result = THUMB_MISS;
        } else if (!resolve(self, *req) || is_cancelled(*req)) {
            continue;
        }

        // Empty only while the last owner is destroying the loader, which
        // delivers nothing anyway.
        if (std::shared_ptr<ThumbLoader> loader = self.self.lock())
            post_result(std::move(loader), std::move(req));
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
    for (guint i = 0; i < n_threads; i++) loader->workers.emplace_back(worker_main, std::ref(*loader));
    return loader;
}

void thumb_loader_shutdown(ThumbLoader& self) {
    self.stopped = true;
    {
        std::lock_guard lock(self.queue_mutex);
        self.stopping = true;
        self.queue.clear();
    }
    self.queue_cv.notify_all();
    for (std::thread& worker : self.workers) worker.join();
    self.workers.clear();
}

void thumb_loader_request(ThumbLoader& self, int64_t image_id, const char* sha256,
                          guint32 max_dim, GCancellable* cancellable, ThumbCallback cb,
                          gpointer user_data) {
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) return;

    if (GRef<GdkTexture> cached = cache_probe(self, sha256, max_dim)) {
        cb(THUMB_OK, cached.get(), user_data);
        return;
    }

    auto req = std::make_unique<ThumbRequest>();
    req->image_id = image_id;
    req->sha256 = sha256;
    req->max_dim = max_dim;
    req->cancellable = gref(cancellable);
    req->cb = cb;
    req->user_data = user_data;
    {
        std::lock_guard lock(self.queue_mutex);
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

- [ ] **Step 4: Update `main.cpp`**

- In `PhigApp`, change `ThumbLoader* loader = nullptr;` to
  `std::shared_ptr<ThumbLoader> loader;`.
- In `~PhigApp`, replace the loader block with:

```cpp
    if (loader) {
        thumb_loader_shutdown(*loader);
        loader.reset();
    }
```

- In `on_factory_bind`, change `thumb_loader_request(app->loader, …)` to
  `thumb_loader_request(*app->loader, …)`.
- `thumb_loader_new`'s call needs no change: it returns a `shared_ptr` and is
  assigned to `app->loader`.

- [ ] **Step 5: Build, then test plain, under ASan, and headless**

```bash
cmake --build build/with-gtk --target phig-gtk test-thumb-loader -j 2>&1 | grep -E "warning|error"
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk --output-on-failure
cmake --build build/with-gtk-asan --target test-thumb-loader -j
ASAN_OPTIONS=detect_leaks=0 env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk-asan/gtk/test-thumb-loader
for i in $(seq 20); do env -u DISPLAY -u WAYLAND_DISPLAY ./build/with-gtk/gtk/test-thumb-loader >/dev/null || echo "run $i failed"; done
```

Expected: no warnings, 3/3 tests pass, 9 `ok` under ASan with no report, and
none of the 20 repeat runs fails. The repeats are there because threading
changed.

Then repeat Task 6 Step 5's smoke run (display permitting).

```bash
grep -nE "G_DECLARE|G_DEFINE|GAsyncQueue|g_mutex|PHIG_THUMB_LOADER\(" gtk/src/thumb-loader.*
```

Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add -A gtk/src/thumb-loader.h gtk/src/thumb-loader.c gtk/src/thumb-loader.cpp gtk/tests/test-thumb-loader.cpp gtk/src/main.cpp gtk/CMakeLists.txt
git commit -m "Rewrite ThumbLoader in C++ without GObject

ThumbLoader was a GObject only so idle sources could hold references to
it. It is now a plain struct behind a shared_ptr: each posted delivery
holds one, and requests move through the queue, the worker and the idle
source as unique_ptrs. The texture cache is an unordered_map with an LRU
list, so the key aliasing and the separate GDestroyNotify go away."
```

---

### Task 8: write the boundary down

**Files:**
- Modify: `CLAUDE.md`, `gtk/CLAUDE.md`, `gtk/README.md`, `libphig/CLAUDE.md`,
  `plans/047-gui-functional-design.md`, `plans/061-gtk-core-linking.md`,
  `plans/000-INDEX.md`

Apply root `CLAUDE.md` §6 (Plain Writing) to every passage below.

- [ ] **Step 1: Root `CLAUDE.md`**

1. In the opening paragraph, change "and `libphig/`, the versioned C ABI for
   foreign callers such as a future Elixir web client." to "and `libphig/`, the
   versioned C ABI for clients that cannot link `core/`, such as the planned
   macOS and Windows clients."
2. In the Layout table, change the libphig row's description to "Versioned C
   ABI (`libphig.so`) for clients that cannot link `core/`."
3. Replace "`gtk/` has its own build (`cmake -B build && cmake --build build -j`
   from inside `gtk/`) and 13 tests across two binaries — see `gtk/CLAUDE.md`."
   with the text below. Fill in N from `./build/with-gtk/gtk/test-* | grep -c '^ok'`
   summed across the three binaries.

   > `gtk/` builds inside the root build when configured with
   > `-DPHIG_BUILD_GTK=ON`. The option is off by default, so a plain build needs
   > neither gtk4 nor libadwaita. `cmake --build build --target phig-gtk` then
   > builds the client and `phig-core` only. Its N tests across three binaries
   > carry the ctest label `gtk` (`ctest --test-dir build -L gtk`) and need no
   > collection. See `gtk/CLAUDE.md`.

4. In the CI bullet, replace "a gtk job that installs libphig and then builds
   and tests `gtk/` against it through pkg-config" with "a gtk job that
   configures the root build with `-DPHIG_BUILD_GTK=ON` and builds and tests
   the client".
5. In Interface discipline, replace the first paragraph's opening sentence and
   its next sentence ("**Clients go through phig's defined interfaces only.** A
   client reads phig through `libphig`'s C ABI (`<phig/phig.h>`) when the
   collection is local, …") with "**Foreign clients go through phig's defined
   interfaces only.** A client that cannot link `core/` — the planned macOS
   (Swift) and Windows (C#/C++) clients — reads phig through `libphig`'s C ABI
   (`<phig/phig.h>`) when the collection is local, …". Keep the rest of the
   paragraph.
6. Replace the "**Non-GUI functionality belongs in libphig, not the clients.**"
   paragraph with:

   > **Non-GUI functionality belongs in `core/`, not the clients.** Logic that
   > isn't specific to rendering a UI — filtering, formatting, business rules —
   > goes in `core/`, where `cli/` and `gtk/` call it directly and libphig
   > exposes it to the clients that cannot. A client never holds its own copy.

7. Replace the "**`cli/` — and `phigd` when it exists — are peers…**" paragraph
   with:

   > **`cli/`, `gtk/` — and `phigd` when it exists — are peers of libphig over
   > `core/`, not clients of it.** Each links `phig-core` and includes `core/`
   > headers directly, so each is entitled to whatever `core/` has the moment it
   > lands. That standing comes from being phig, built in this repository,
   > rather than a foreign caller. `gtk/` makes every `core/` call from one
   > file, `gtk/src/collection.cpp`; see `gtk/CLAUDE.md`.

8. In Platform boundaries, replace "every client still reads phig through a
   defined interface only, per Interface discipline above." with "a client reads
   phig through `core/`'s headers or a defined interface, never the database or
   the thumbnail cache directly, per Interface discipline above."

- [ ] **Step 2: `gtk/CLAUDE.md`**

Replace the paragraph starting "Talks to phig only through `libphig`'s C ABI"
with:

> Links `phig-core` directly, as `cli/` does, through one seam:
> `src/collection.h`. Built from the repository root with
> `cmake -B build -DPHIG_BUILD_GTK=ON`, then
> `cmake --build build --target phig-gtk`.

At the top of `## Rules`, insert:

> - **Only `src/collection.cpp` includes a `core/` header.** Widget code calls
>   `collection.h`, never `Database` or an engine. The seam keeps reads paged.
>   No call returns the whole collection, because a full read carries about
>   3.8 KB of EXIF per row that the client never reads. A new capability is a
>   new `collection.h` function.
> - **The tests link `tests/fake-collection.cpp`, not `phig-core`.** They test
>   the client's own behaviour and need no collection. A new `collection.h`
>   function gets a fake implementation too. `test-collection` is the exception:
>   it tests `collection.cpp` against a database it builds.
> - **A `cv::Mat` never leaves `collection.cpp`.** Decoded pixels become a
>   `GdkTexture` or a file path before widget code sees them.
> - **No exception unwinds into GTK.** GTK calls the client's handlers through C
>   frames. Every function passed to GLib or GTK is `noexcept`, and every
>   `collection.h` function catches.
> - **Never authorize a removal with `cmd_rm`'s `ConfirmFn`.** A destructive
>   operation is authorized by a plan the caller inspects and then applies
>   (R66). The Remove screen waits for that plan/apply pair in `core/`.
> - **GObjects only where GTK requires one.** `PhigImageRow` and
>   `PhigGalleryModel` are GObjects because GTK's list machinery needs them, and
>   `gallery-model.c` stays C. GObject instance memory is zeroed, not
>   C++-constructed, so a `std::string` member would need placement-new.
>   Everything else is plain C++ structs and free functions, with `GRef`
>   (`src/gref.h`) owning GObject references.

In the existing "GTK calls happen on the main thread only" rule, change
"`ThumbLoader`'s worker threads call `phig_thumb_lookup`/`phig_thumb_generate`"
to "`ThumbLoader`'s worker threads call
`phig_collection_thumb_lookup`/`phig_collection_thumb_generate`".

- [ ] **Step 3: `gtk/README.md`**

- In the first paragraph, replace the sentence "Talks to phig only through
  `libphig`'s C ABI (`<phig/phig.h>`) — never a `core/` header, never a
  `cv::Mat`." with "Links `phig-core` directly through one seam,
  `src/collection.h`."
- In Setup, keep the gtk4/libadwaita package lists. Replace the paragraph and
  code block about installing phig to `~/.local` and `PKG_CONFIG_PATH` with
  "phig's own native libraries are needed too; see the root `README.md`."
- Replace the Build block with:

```bash
cd ..   # the repository root
cmake -B build -DPHIG_BUILD_GTK=ON
cmake --build build --target phig-gtk -j
./build/gtk/phig-gtk [path/to/phig.db]
ctest --test-dir build -L gtk
```

- Leave the measured-performance section as it is: it records M0.

- [ ] **Step 4: `libphig/CLAUDE.md`**

- Lines 3–4: change "the boundary for foreign callers (`gtk/`, and later Elixir)"
  to "the boundary for clients that cannot link `core/`: the planned macOS and
  Windows clients. `gtk/` links `core/` directly, and the web tier is an HTTP
  client of `phigd` (plan 052)".
- Lines 9–10: change "This is the only header a client may include — see
  `gtk/CLAUDE.md`." to "This is the only header a foreign client may include."
- Lines 103–105: replace the paragraph with "Pixel decoding is also absent.
  `gtk/` decodes through `core/` in-process, on the measurement of 0 crashes in
  12,684 probes. Whether the ABI exposes decode is open, and out of scope for
  plan 031."

- [ ] **Step 5: Plan 047's dependencies**

In `plans/047-gui-functional-design.md`, under `## Dependencies`:
- Change the 043 bullet to "**043** — CLI primitive split (validates that
  scan/thumbs/faces are separable operations a GUI can call independently).
  Not a dependency of the GTK client, which calls the engines through `core/`."
- Add a bullet: "**061** — the GTK client links `core/` directly, so it does
  not wait on libphig coverage (053). The macOS and Windows clients still do."

- [ ] **Step 6: Close plan 061**

In `plans/061-gtk-core-linking.md`:
- Change the frontmatter to `status: completed` and add
  `completed: <today, YYYY-MM-DD>` after `created`.
- Append the following section, filling in the commit range:

```markdown
## Outcome

Shipped in <first-sha>..<last-sha>; execution plan
`docs/plans/2026-11-07-061-gtk-core-linking.md`.

- `gtk/` reaches phig through `src/collection.h`, implemented over `phig-core`
  in `src/collection.cpp` and by `tests/fake-collection.cpp` for the tests.
  `phig-gtk` no longer links libphig.
- On-demand thumbnail generation moved from `libphig/thumbs.cpp` into `core/`
  as `ensure_thumb`, which `phig_thumb_generate` and the GTK client both call.
- `gtk/` builds inside the root build behind `PHIG_BUILD_GTK` (off by
  default). Its three test binaries carry the `gtk` label and need no
  collection.
- `main` and `thumb-loader` are C++. `gallery-model.c` stays C.
- The over-budget eviction defect is fixed, with a test.

Found and not fixed: when a thumbnail request is cancelled after dispatch, its
callback never runs, so the `ThumbResultData` and the `GtkListItem` reference
`on_factory_bind` passed with it are never released. Fixing it needs a
destroy-notify on `thumb_loader_request`.
```

In `plans/000-INDEX.md`, change 061's status cell to `completed`. Re-read both
files and confirm they agree.

- [ ] **Step 7: Final verification**

```bash
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -3
cmake --build build --target help | grep -c gtk
python3 scripts/rules.py --check
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build/with-gtk -L gtk --output-on-failure
git diff --stat "$BASE" -- core libphig cli tests
```

Expected: the main suite passes, with the six new `EnsureThumbTest` cases on
top of the starting count. `0` gtk targets in the main build. Rules report OK.
The gtk tests pass 3/3. The diff touches only the four Task 1 files under
`core/`, `libphig/` and `tests/`.

- [ ] **Step 8: Commit**

```bash
git add CLAUDE.md gtk/CLAUDE.md gtk/README.md libphig/CLAUDE.md plans/047-gui-functional-design.md plans/061-gtk-core-linking.md plans/000-INDEX.md
git commit -m "Record that gtk/ links core/ directly

Root CLAUDE.md, gtk/CLAUDE.md, gtk/README.md and libphig/CLAUDE.md described
gtk/ as a libphig client. It is now a peer of libphig over core/, like
cli/, with rules for the seam that keeps that access in one file. Plan
061 is completed. 047 notes that the GTK client does not depend on 053."
```

---

## Not in this plan

- The GTK M1 screens: Gallery sidebar, Viewer, tag, gallery and favorite edits.
  They get their own plan once this lands.
- Any change to libphig's exported surface.
- Adding `gtk/` to the clang-format or clang-tidy gates.
- The `ThumbResultData` leak (Decision 5).
