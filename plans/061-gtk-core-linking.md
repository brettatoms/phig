---
name: GTK Links core/ Directly
status: completed
created: 2026-08-10
completed: 2026-11-07
description: >
  Make `gtk/` a peer of `libphig` over `core/` rather than a client of it, the
  same standing `cli/` and `phigd` have. Removes plan 053 from the GTK
  client's critical path, moves `gtk/` to a mixed C/C++ build, and hands
  libphig's schedule to the macOS and Windows clients that actually need a C
  ABI.
---

# 061 — GTK Links `core/` Directly

## What this is

A boundary decision and the work that follows it. `gtk/` currently reads phig
only through `libphig`'s C ABI and may include no `core/` header. This plan
makes it a peer of `libphig` over `core/` instead — the standing root
`CLAUDE.md` already grants `cli/`, and plan 052 claims for `phigd`.

Settled: the boundary itself, how far the C++ conversion goes
(`thumb-loader.c` and `main.c` convert, `gallery-model.c` stays C), the build
topology (`gtk/` joins the root build behind an off-by-default option), what
gtk's tests link (a fake seam, not the engine), what happens to libphig
(nothing, until the macOS and Windows clients need a symbol), and how plan
054's rules reach the client (through the 62 that bind a `core/` operation,
not through a new surface value). What is left is dependencies rather than
questions — see the end.

**Confirmed 2026-11-07.** GTK on Linux is the next client phig builds, ahead of
the macOS client, which is what decides the boundary question in this plan's
favour. Order: plan 060's R60 fix, then this plan, then a GTK M1 plan scoped to
browse and curate (Gallery with its sidebar, the Viewer, and tag, gallery and
favorite edits), all of which `core/` already supports.

Execution plan: `docs/plans/2026-11-07-061-gtk-core-linking.md`.

## Why

**The "foreign caller" framing does not describe `gtk/`.** Root `CLAUDE.md`
grants `cli/` direct access to `core/` because "that standing comes from being
phig rather than a foreign caller." `gtk/` is in the same repository, built by
the same people, shipped as part of phig. The distinction the rule draws does
not separate it from `cli/`.

**libphig's founding consumer is not going to use it.** Root `CLAUDE.md:8`
describes libphig as the ABI "for foreign callers such as a future Elixir web
client." Plan 052 then decided the web tier is an HTTP client of `phigd`, not
an FFI caller — no permanent colocation with the collection, and no
thread-local `phig_last_error` read from a migrating virtual thread. Those two
statements disagree, and 052 is the later decision. libphig's remaining
consumers are the macOS client (Swift, whose interop layer plan 049 builds) and
the Windows client (C#/C++) — both unbuilt.

**The capabilities already exist in `core/`.** Every item on plan 053's missing
list is a callable function today:

| Capability | In `core/` |
|---|---|
| Duplicate groups | `Database::get_exact_duplicates` (`core/database.h:132`) |
| One image's tags | `Database::get_tags_for_image` (`core/database.h:205`) |
| Tag counts | `Database::get_tag_counts` (`core/database.h:206`) |
| Gallery counts | `Database::get_gallery_counts` (`core/database.h:232`) |
| People, read | `Database::list_people` (`core/database.h:296`) |
| Photos of a person | `Database::get_images_for_person` (`core/database.h:299`) |
| Face model download | `download_face_models` (`core/models.h:9`) |
| Person / face / similar search | `SearchEngine`'s other three `SearchMode`s |
| Glob and path selection | `Database::ids_matching`, `resolve_named_selection` (`core/selection.h`) |
| Remove | `cmd_rm` (`core/remove_exec.h`) |
| Full-resolution decode | `core/raw.h`, `core/image_codec.h` |

**What the boundary costs.** 053 gates six of plan 047's nine screens:
Duplicates and People have no data source, Remove and the Activity Log have no
action or reader, and the Viewer cannot list an image's tags
or populate the sidebar's tag and gallery sections. Pixel decoding for the
Viewer is worse than gated: 053 excludes it and no numbered plan owns it, so
the Viewer has no route to a full-resolution image through the ABI at all.

## What this does not change

Four things survive the switch, and none of them is what the boundary was
costing:

- **041 (scan reconcile plan/apply).** `relink_moved_rows` and
  `demote_jpeg_rows` each decide *and* write inside the logged operation and
  return only counts. There is no decide-only phase in `core/` to call, so a
  scan dialog that shows "12 files are gone — remove them?" needs 041 whichever
  surface it links.
- **The activity log reader.** `core/database.h:61` has
  `log_begin_operation` and no getter anywhere. Screen 7 is core work before it
  is client work.
- **046.** The three `vec_faces` delete failures are Linux-specific and reached
  through `delete_faces_for_image`, which face rebuild goes through. The People
  screen inherits it on this machine.
- **A GTK M1 plan.** 035 is complete, 047 is a design document. macOS has 050
  for its first browsing milestone; GTK has no counterpart, and this plan does
  not become one.

## Language and structure

**Mixed C/C++ target, not a big-bang port.** `gtk/CMakeLists.txt:2` becomes
`LANGUAGES C CXX`. The split is not a compromise between two styles: it falls
on whether GTK requires the thing in a file to be a GObject, which is always
answerable and so survives contact with new files.

**`gallery-model.c` stays C permanently.** Both its GObjects are mandatory.
`PhigImageRow` must be one because `GListModel::get_item` returns a `GObject*`
and GTK's list-item manager keys recycled widgets by item pointer identity — a
single shared object makes it pick a widget for the wrong position (glib#3030,
recorded at `gallery-model.c:132-143`). `PhigGalleryModel` must be one because
it implements `GListModel` and emits `page-fetch-failed`.

C++ inside those structs is worse, not neutral: **GObject instance memory is
not C++-constructed.** `g_object_new` allocates and zeroes, then calls `_init`,
so a `std::string` member is not a constructed `std::string`. Turning
`PhigImageRow`'s three `char*` fields into `std::string` means placement-new in
`phig_image_row_init` and explicit destructor calls in `_finalize` — more
ceremony than the three `g_free` calls at `gallery-model.c:44-46`, and a
footgun for whoever adds a fourth field. The two hand-rolled LRU scans
(`gallery-model.c:199-216` and `:230-245`) are near-identical and do want
factoring out, but that is a shared helper in C, not a language argument.

**`thumb-loader.c` and `main.c` convert.** `ThumbLoader` is a GObject and
nothing in GTK requires it to be — it is a GObject so idle sources can hold
refcounts on it (`thumb_loader_post_result`, `thumb-loader.c:225-229`), which
is `shared_ptr`/`weak_ptr` spelled in C. Dropping it removes
`G_DECLARE_FINAL_TYPE`, `G_DEFINE_FINAL_TYPE`, `class_init`, `init`, `dispose`,
`finalize` and every `PHIG_THUMB_LOADER()` cast: about 30 lines carrying no
logic. The larger gain is ownership moving from comments into types —
`cache_insert`'s "takes ownership of the reference passed in, returns a new ref"
contract becomes a by-value `GRef` parameter and return type, its two unlock
paths become one `lock_guard`, and the aliasing where `entry->key` *is* the hash
table's key (freed by the value destructor, because the table is created with a
NULL key-destroy at `thumb-loader.c:398-399`) disappears into
`unordered_map<std::string, Entry>`. `ThumbRequest`'s three-way ownership
between queue, worker and idle source is a `unique_ptr` moved through, which is
what 41 lines of comment across `thumb-loader.c:11-22`, `:172-176`, `:200-216`
and `:218-224` currently explain.

`main.c` has no `G_DEFINE` at all — one `PhigApp` plain struct, free functions,
12 `g_signal_connect` handlers. It is already composition, so converting is
cheap and structural nothing: RAII replaces four manual `*_free` functions, and
`std::sort` replaces `qsort` with `phig_measure_cmp_us`.

**No inheritance is required in code phig writes.** GTK4's list and grid views
are composition-based: `GtkSignalListItemFactory` supplies bind and unbind
callbacks, which is why `on_factory_unbind` is a function rather than an
override, and layout goes through `gtk_widget_set_layout_manager`. Using GTK at
all means using GObject's own hierarchy through casts (`GTK_WIDGET(x)`), which
is unavoidable and opaque — phig never writes a hierarchy. gtkmm remains
rejected, and its inheritance-first design (subclassing `Gtk::Window`) is a
second reason beyond the one already recorded.

The C++ this buys is value-semantic:

- `template <class T> using GRef = std::unique_ptr<T, GObjectUnref>` for
  GObject references — move-only, no base class.
- Plain structs plus free functions, the shape `main.c` already has, with
  `std::string`, `std::vector` and `std::optional` replacing manual lifetime
  tracking.
- Captureless lambdas for signal handlers with state carried in the
  `user_data` struct. A capturing lambda does not convert to the function
  pointer `g_signal_connect` needs; that is a constraint to work with, not a
  reason to introduce classes.

**Core-facing code lives in its own translation units.** Widget callbacks do
not call `Database` directly. This is not a purity rule: plan 044 measured
`get_all_images` returning 111.6 MB of EXIF that no caller reads (~3.8 KB/row
over 29,925 rows, extrapolating to ~3.7 GB at 1M), and libphig's paged read
surface was preventing that class of mistake by construction. The seam keeps
the paging discipline without keeping the ABI.

**The seam is what gtk's tests substitute.** They test gtk's own behaviour —
page LRU eviction, one placeholder object per position, cancel-on-unbind, the
texture byte budget — so they link a fake implementation of the seam rather
than `phig-core`, and need no collection at all. That fixes what they do today:
both binaries read a collection path from `PHIG_TEST_DB` and are marked
`SKIP_RETURN_CODE 77`, so on a machine with no collection all 13 tests skip
rather than run, and `test_cache_respects_byte_budget` needs six real
thumbnails to exercise a byte counter. The interface earns its second
implementation here — the fake is the reason it exists, not a speculative
abstraction.

**`cv::Mat` stays inside that seam.** `gtk/CLAUDE.md`'s rule was written
against a process boundary decode would cross; measurement (0 crashes in 12,684
probes) already argues in-process decode is defensible, so the rule narrows
rather than disappears — a `cv::Mat` may exist in a bridge translation unit and
must be a `GdkTexture` before any widget sees it.

**The `thumb-loader` conversion carries one bug fix.**
`thumb_loader_evict_locked` pops from the LRU head while over budget, and the
just-inserted entry is at the tail, so it is evicted only when it is the last
one standing — which happens when a single texture exceeds the whole budget.
`thumb-loader.c:155` then calls `g_object_ref(entry->texture)` on an entry the
hash table's `GDestroyNotify` has already freed. Reachable from the shipped
binary: `PHIG_THUMB_CACHE_BYTES` is an env override (`main.c:840`) and
`env_override_uint` accepts any positive value. The case is untested by design
— `test_cache_respects_byte_budget` sizes its budget for about three entries
and says in a comment that a single-entry budget "never exercises LRU
*ordering*" — so a test for it is part of the fix.

The fix is to take the caller's reference before evicting, and it is not
something the conversion produces for free: the same structure in C++ has the
same defect. It rides along because the file is being rewritten anyway, and
because phig has no released client whose behaviour a change here could
disturb.

## Build integration

`gtk/` joins the root build as `add_subdirectory(gtk)` inside
`option(PHIG_BUILD_GTK OFF)`, so it is neither built nor configured unless
asked — a plain `cmake -B build && cmake --build build` never touches it and
needs no gtk4 or libadwaita. With the option on, `cmake --build build --target
phig-gtk` builds `phig-core` and `phig-gtk` and nothing else, so the gtk-only
loop survives. gtk keeps its own `CMakeLists.txt`, targets, warning flags and
two test binaries; the change there is `pkg_check_modules(PHIG REQUIRED phig)`
becoming a link against `phig-core`.

Two costs against the standalone build it replaces. `CMAKE_BUILD_TYPE` comes
from the parent, where `gtk/CMakeLists.txt:6-8` currently defaults itself to
Release independently — defensible, but a behaviour change. And one build
directory serves both, so removing it removes gtk's too.

**Keeping `gtk/` fully out-of-tree was rejected on its cost, not its merits.**
The independence is real and is exactly what the libphig boundary was buying:
one versioned `.so`, one include directory, one `phig.pc`. `phig-core` is not
that. Making `find_package(phig-core)` work from outside the tree needs
`core/CMakeLists.txt:45-55` rewritten, because
`target_include_directories(phig-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} …
${CMAKE_BINARY_DIR})` holds absolute paths and `install(EXPORT)` refuses
source-directory paths; both `phig-core` and the `phig-vendor` it links
`PUBLIC` in the export set; an install rule for core's headers, none of which
are installed today (the root build installs exactly one file, `phig.pc`, at
`CMakeLists.txt:110`); and a `phig-coreConfig.cmake` calling `find_dependency`
for OpenCV, libexif, LibRaw, FFmpeg's four libraries, SQLite and spdlog, since
a static library's link dependencies are resolved again by its consumer. That
last file is ongoing cost: a new native dependency in `core/` that is not added
to it breaks gtk's link. And it buys build-system independence only — gtk
recompiles when a `core/` header changes either way, which is what linking a
static engine means.

CI's gtk job loses its install step — build libphig, install to a prefix, point
`PKG_CONFIG_PATH` at it (`ci.yml:188-198`) — and becomes a configure of the
root build with `-DPHIG_BUILD_GTK=ON`.

## Rule coverage

**`gtk/` does not become a new `binds_surface` value.** 62 of plan 054's 70
rules bind a `core/` operation and 8 bind a surface, which is 054's design
working as intended: a rule binds the operation and every surface inherits it.
A client that consumes `core/` primitives therefore inherits those 62 by
construction, exactly as `cli/` does, and gaining a surface value would add
nothing it does not already carry.

Seven of the eight surface rules exist *because* there is a C ABI, and have no
meaning for an in-tree C++ caller: R62 and R63 are C-to-C++ translation
mechanics (exception conversion, thread-local error storage), R64 and R65 are
ABI versioning and struct layout, R67 is the `phig_op` lifecycle, and R69 and
R70 bind `cli`.

**R66 is the one whose substance transfers, and it is an operation property
rather than a surface one.** "A destructive operation is authorized by a plan
the caller inspects and then applies with a second call, never by a callback the
engine invokes mid-run" is a statement about what removal *is*, not about how a
caller reaches it. `remove.trash`, `remove.hard`, `remove.db-only` and
`remove.orphans` are already core operations (`rules/operations.toml:177-208`),
and none has a plan phase anywhere in `core/`:
`cmd_rm(const RemoveOptions&, const ConfirmFn& confirm = nullptr)`
(`core/remove_exec.h:66`) is the CLI's stand-in for the plan core does not have.

That gap is already owned without this plan touching it. R66 binds `abi`, and
053 owns reaching remove through the ABI, so 053 must expose it as plan/apply,
which means `core/` has to grow one. The GTK client then uses it because it is
the only authorized removal path there is. This plan records the dependency and
claims none of the work.

What `gtk/` needs instead is client discipline, which belongs in
`gtk/CLAUDE.md` rather than in the record set: do not reach for `cmd_rm`'s
`ConfirmFn`, and do not let an exception unwind out of a GLib signal handler,
since GTK enters phig's handlers through C frames with no unwind guarantee.
Neither protects the collection — they protect the client's own correctness —
which is why neither is a data-integrity rule.

## Consequences for libphig and 053

libphig stops having a shipping consumer until the macOS client exists. It is
left as it is: nothing about a monorepo needs a maintainer assigned to a
directory, so it stays at ABI 1.4 with its tests green in CI, and it is
updated when the macOS and Windows clients need a symbol — which is when 049
builds the Swift interop layer against it. The cost of that position is real
and is that an ABI with no caller is infrastructure whose gaps nobody finds.
Two things follow.

**053's driver changes.** Its ordering is currently justified by 047's screens
reached through the ABI. That argument moves to 049/050, which need a C ABI
because Swift cannot include `core/` headers comfortably, and to 052's parity
requirement that both interfaces cover the same capabilities so a client picks a
transport rather than learning a second vocabulary. 053 stays open — root
`CLAUDE.md`'s rule that libphig should reach all of phig's features is
independent of who is currently calling it — but it is no longer on the GTK
client's critical path.

**053 loses its best requirements probe.** A client written against `core/`
will answer 053's open design questions — how a caller names its subject,
whether a whole-collection read is a call or a `phig_op*` — by discovering what
the nine screens actually need. That is worth capturing as the GTK work
proceeds rather than rediscovering when the Swift client arrives.

## Documentation changes

The decision contradicts four documents, and this plan owns fixing them:

- **Root `CLAUDE.md`** — Interface discipline names `gtk/` as a client that may
  only use defined interfaces, and the peer paragraph names only `cli/` and
  `phigd`. The Layout table calls libphig the ABI "for foreign callers such as
  a future Elixir web client", which 052 already made wrong. "Non-GUI
  functionality belongs in libphig, not the clients" needs restating: the logic
  still belongs in `core/`, it just no longer needs an ABI entry point to be
  reachable.
- **`gtk/CLAUDE.md`** — the "may only include `<phig/phig.h>`, never link
  `phig-core`" rule inverts; the `cv::Mat` rule narrows to the seam; the paging
  discipline is added; and the two client-discipline rules from Rule coverage
  above go in — no `cmd_rm` `ConfirmFn`, no exception unwinding out of a signal
  handler.
- **`libphig/CLAUDE.md`** — "the boundary for foreign callers (`gtk/`, and
  later Elixir)" is no longer accurate about either.
- **047** — its dependency list cites 041, 042 and 043 for the GTK client;
  043 was never a GTK dependency and 053 is not one now.

Per the repo's convention, completed plans are not edited: 035's description of
M0 reading phig only through libphig stays as the record of what shipped.

## Dependencies

- **041** — reconcile plan/apply, for a scan dialog that removes stale rows.
  Core-side work with no substitute.
- **064** — a removal plan/apply pair in `core/`, which R66 already requires of
  the ABI, for the Remove screen. 053 handed remove to 064.
- **A GTK M1 plan** — this plan opens the boundary and does not schedule the
  screens behind it.

## Cross-references

- `plans/035-gtk-m0.md` — what shipped in M0, read-only through libphig.
- `plans/041-scan-reconcile-plan-apply.md` — still required; core-side work.
- `plans/047-gui-functional-design.md` — the nine screens this unblocks.
- `plans/049-photos-import.md`, `plans/050-macos-app-m0.md` — libphig's first
  genuine foreign caller and the Swift interop layer.
- `plans/052-phig-server.md` — the decision that the web tier is an HTTP client.
- `plans/053-libphig-feature-parity.md` — stays open, loses its GTK driver.
- `plans/044-large-collection-scale.md` — the measured reason for the paging
  discipline the seam preserves.
- `rules/boundary.toml` — R62 and R66, the two rules a third surface would
  otherwise escape.

## Outcome

Shipped in `6e60ea7`..`dd4667c` and the documentation commit that closes this
plan; execution plan `docs/plans/2026-11-07-061-gtk-core-linking.md`.

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
