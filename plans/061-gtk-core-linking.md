---
name: GTK Links core/ Directly
status: designing
created: 2026-08-10
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

The boundary decision is settled, and so is how far the C++ conversion goes —
`thumb-loader.c` and `main.c` convert, `gallery-model.c` stays C, for the
reasons in Language and structure below. What remains open is the build
topology and who owns libphig's schedule afterwards; those are the open
questions at the end.

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
| Duplicate groups | `Database::get_exact_duplicates` (`core/database.h:125`) |
| One image's tags | `Database::get_tags_for_image` (`core/database.h:173`) |
| Tag counts | `Database::get_tag_counts` (`core/database.h:174`) |
| Gallery counts | `Database::get_gallery_counts` (`core/database.h:197`) |
| People, read | `Database::list_people` (`core/database.h:261`) |
| Photos of a person | `Database::get_images_for_person` (`core/database.h:264`) |
| Face model download | `download_face_models` (`core/models.h:9`) |
| Person / face / similar search | `SearchEngine`'s other three `SearchMode`s |
| Glob and path selection | `SearchQuery::matches` / `excludes` |
| Remove | `cmd_rm` (`core/remove_exec.h`) |
| Full-resolution decode | `core/raw.h`, `core/image_codec.h` |

**What the boundary costs.** 053 gates six of plan 047's nine screens:
Duplicates and People have no data source, Remove and the Activity Log have no
action or reader, the Gallery's search field cannot filter by filename or path
(`phig_query` has no such predicate — the thumbs and faces operations take
`match`/`exclude`, queries do not), and the Viewer cannot list an image's tags
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

`phig-core` is `STATIC` and is not installed — only `phig_abi`, its headers and
`phig.pc` are (`libphig/CMakeLists.txt:50-51`), and `phig-core`'s `PUBLIC`
include directories pull in OpenCV, libexif, LibRaw and libavformat. Installing
it for an out-of-tree consumer means exporting a static target plus a
`phig-coreConfig.cmake` and having gtk repeat every transitive link dependency.

So `gtk/` joins the root build as `add_subdirectory(gtk)`, guarded by an option
(default off, or on only when `gtk4`/`libadwaita-1` are found) because the root
build runs on macOS in CI and `gtk/` is the Linux client. gtk keeps its own
`CMakeLists.txt` and its two test binaries; it stops calling
`pkg_check_modules(phig)` and links `phig-core` instead.

CI's gtk job loses its install step — build libphig, install to a prefix, point
`PKG_CONFIG_PATH` at it (`ci.yml:188-198`) — and becomes a configure of the
root build with the gtk option on.

## Rule coverage

Plan 054 records phig's data-integrity rules as TOML, and every rule carries a
`binds_surface`. In `rules/boundary.toml` the only values are `abi` and `cli`.
A `gtk/` that links `core/` is a third surface, and it inherits neither list.
Two rules make that concrete:

- **R66** — "A destructive ABI operation is authorized by a plan the caller
  inspects and then applies with a second call, never by a callback the engine
  invokes mid-run." Its rationale names `cmd_rm`'s `ConfirmFn` as "a `cli/`
  affordance for a command with no plan phase, not a model to copy." Linking
  `core/` puts that `ConfirmFn` within reach of the GTK client, and no rule
  says it may not use it. Either R66's `binds_surface` grows to cover the new
  surface, or this plan states why gtk is exempt — and the second is hard to
  argue for the client that has a Remove dialog in 047.
- **R62** — "No C++ exception crosses an ABI entry point," held by
  `PHIG_TRY`/`PHIG_CATCH` around every entry point where a `phig_ctx*` is in
  scope. Linking `core/` removes that wrapper. GTK enters phig's handlers
  through GLib's C frames, so an exception leaving a signal handler unwinds
  through code with no unwind guarantee — the same discipline R62 imposes at
  the ABI, needed at the GTK callback boundary instead. Nothing states it
  today because no surface needed it.

Both rules are `owner = "053"`, which they keep whether or not 053 still has a
GTK driver. This plan owns describing the obligation; writing or amending a
rule record is 054's machinery and a separate decision.

## Consequences for libphig and 053

libphig stops having a shipping consumer until the macOS client exists. That is
the real cost of this plan and it should be stated rather than absorbed: an ABI
with no caller is infrastructure whose gaps nobody finds. Two things follow.

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
  discipline is added.
- **`libphig/CLAUDE.md`** — "the boundary for foreign callers (`gtk/`, and
  later Elixir)" is no longer accurate about either.
- **047** — its dependency list cites 041, 042 and 043 for the GTK client;
  043 was never a GTK dependency and 053 is not one now.

Per the repo's convention, completed plans are not edited: 035's description of
M0 reading phig only through libphig stays as the record of what shipped.

## Open questions

1. **Does `gtk/` keep its own build root?** `add_subdirectory(gtk)` from the
   root is the cheap route, but `gtk/CLAUDE.md` currently documents a
   standalone `cmake -B build` from inside `gtk/`, and that is a faster
   feedback loop for client work than reconfiguring the engine. A standalone
   build that links `phig-core` needs the target exported, which is the cost
   the previous section describes.
2. **Do gtk's two test binaries link `phig-core`?** They currently link
   libphig via pkg-config and skip (exit 77) when the collection is absent.
   Linking a static engine into two more binaries is cheap in build time and
   makes the tests able to build a real database instead of skipping — which
   is a change in what they cover, not just how they link.
3. **Who maintains libphig in the interim?** It has 69 exported symbols, 22 ABI
   tests and no caller after this lands. Options: keep it green in CI and let
   049 drive it forward, or state that it is frozen at 1.3 until the macOS
   client needs a symbol. The second is honest but makes 053 a plan nobody is
   executing.

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
