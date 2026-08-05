# SDD ledger — plan: /home/brett/devel/phig/docs/plans/2026-08-04-phig-gtk-m0.md
SPANS TWO REPOS:
  ~/devel/phig/phig      — branch phig-pc-for-clients (from master 20c7ad4). Only Task 1 touches it (phig.pc).
  ~/devel/phig/phig-gtk  — NEW repo, created in Task 1. All other tasks live here.
Workspace lives in the phig repo because phig-gtk does not exist yet at setup time.

## Pre-flight rulings (human partner, before Task 1)
- .envrc: GITIGNORE it in phig-gtk, matching phig's convention (phig ignores .envrc as
  local setup). devenv.nix IS committed — that's the environment definition. README
  documents `printf 'use devenv' > .envrc && direnv allow`.
  This OVERRIDES the plan's Task 1 Step 3, which had .envrc committed.
- libphig install location: `~/.local` (OUTSIDE both repos), not phig/install/.
  Consequence: NO change to phig's .gitignore is needed, so the phig-side diff for this
  whole plan is exactly one new file (phig.pc.in) plus 3 CMakeLists lines.
  PKG_CONFIG_PATH points at ~/.local/lib/pkgconfig; README documents the install step.
  This OVERRIDES the plan's Task 1 Step 4.

Task 1: implemented (phig 665baae, phig-gtk 2b365a7 root commit).
  gtk4 4.22.4, libadwaita 1.9.2 (both >= the 4.10 Task 5 needs). phig tests 631/631,
  same pre-existing skip — no regression.
  Window: NOT visually confirmed. Compositor lacks the screen-capture protocol and the
  portal Screenshot call needs an interactive grant. Indirect evidence the implementer
  gave: the process ran 42s with zero output and org.phig.Gtk was registered live on the
  session D-Bus under its PID, which requires a working display connection. Honest
  reporting rather than a faked gate.
Task 1: CONTROLLER FOUND A REAL DEFECT IN MY OWN phig.pc.in — the implementer flagged it
  as a "CMake gotcha"; it is worse than that. `prefix=@CMAKE_INSTALL_PREFIX@` bakes the
  CONFIGURE-time prefix, so `cmake --install --prefix X` emits a .pc claiming a different
  prefix. Reproduced: installing to /tmp/pctest produced a .pc saying
  prefix=/home/brett/.local. Silently wrong for any consumer.
  The ~/.local install is correct only because the implementer passed
  -DCMAKE_INSTALL_PREFIX at configure time. Fix: derive prefix from ${pcfiledir}
  (pkg-config 3.0.5 supports it), so the file is correct wherever installed.
Task 1: fix round 1/5 dispatched — pcfiledir-relative prefix, plus the .envrc README line
  which the implementer found does not work as written.
Task 1: fix round 1/5 (1 defect addressed; phig 00157f1, phig-gtk 760634f) — phig.pc now
  derives prefix from ${pcfiledir}. CONTROLLER VERIFIED INDEPENDENTLY: installed the same
  build tree to two prefixes, each resolved to its own tree; then compiled+linked a C
  client using ONLY $(pkg-config --cflags --libs phig) and it printed 29953. README's
  .envrc block also fixed (the plan's `printf 'use devenv'` form fails with
  "use_devenv: command not found"; phig's fuller form with eval "$(devenv direnvrc)" is
  required).
Task 1: complete (phig 20c7ad4..00157f1, phig-gtk root..760634f, review clean)
  Reviewer independently verified all 6 libadwaita signatures against the installed 1.9.2
  headers and confirmed via objdump that phig-gtk links ONLY libphig.so.1 — the OpenCV
  .so's in ldd are transitive through libphig, not linked by phig-gtk.
  gtk4 4.22.4, libadwaita 1.9.2, phig.pc 0.3.0 all resolve in the phig-gtk shell.
Task 1: PARKED — "repo boundary enforced by absence, not an active guard" (Important).
  Ruling: the code stands. Controller verified phig-core is a STATIC lib with NO install
  rule, so there is no installed .pc or header for a contributor to discover — a violation
  would require hand-writing a path into phig's source tree, which no cheap guard
  prevents. A CI job in phig-gtk is the right answer and phig-gtk has no CI yet; that is
  its own task, not a Task 1 fix. Final review should triage whether to add one.
Task 1: minor (deferred): builtins.getEnv "HOME" returns "" if HOME is unset, making
  PKG_CONFIG_PATH=/.local/... and pkg-config fail with a generic "not found". Virtually
  all shells set HOME; documented in devenv.nix.
Task 1: minor (deferred): the window was NOT visually confirmed (no compositor capture
  protocol; portal Screenshot needs interactive grant). D-Bus name registration happens at
  g_application_register, BEFORE activate, so it proves the process started, not that a
  window mapped. The 42s zero-stderr run plus verified signatures plus clean -Wall build
  are the real evidence. Tasks 4-7 need a display; if they cannot get one, that changes
  how they can be verified.

NOTE: workspace moved to ~/devel/phig/phig-gtk/.superpowers/sdd/2026-08-04-phig-gtk-m0/
  (the sdd-workspace script resolves to the current repo root; Task 1 ran from the phig
  repo, Tasks 2-7 run from phig-gtk). Task 1 artifacts copied here. The phig-side copy is
  stale from this point on.

Task 2: implemented (commit 673e306). 5/5 tests pass, 29,953 rows, ctest skips 77 without
  PHIG_TEST_DB. Both brief mutations caught — controller independently confirmed the
  ownership mutation dies with "double free or corruption" at row-strings-survive-eviction.
  Ownership rule verified structurally: g_strdup at gallery-model.c:72-74, list freed
  immediately after at :129.
  Implementer found and fixed a REAL SEGV my brief's design sketch would have shipped
  (GTask holding a strong model ref released only via main-loop dispatch).
Task 2: FULL-DEPTH REVIEW: spec FAILED, quality not approved. 1 Critical + 2 Important,
  all controller-verified by reading the code:
  - CRITICAL: install_page emits g_list_model_items_changed(position, 0, 0) — removed==0
    && added==0 is a NO-OP, so GtkGridView never re-queries and the grid stays PERMANENTLY
    GREY. Exists only on the async path that NO test covers, which is why 5/5 was green.
    Correct emission is (position, n_rows, n_rows), valid because the total never changes.
  - IMPORTANT: fetch_task_data_free never calls phig_image_list_free(data->fetched), so
    every discarded async result leaks ~200 phig_image structs plus strings.
  - IMPORTANT: the GWeakRef redesign pins the MODEL but ctx/query are NOT owned by it, so
    the use-after-free moved rather than closed — between weak_ref_get (:194) and
    phig_query_page (:201) the main thread can phig_shutdown(ctx). Rarer race, not fixed.
    The removed in_flight/GMutex/GCond that finalize waited on was closer to correct.
  - MINOR: fresh placeholder object per get_item call breaks pointer identity for
    GtkSelectionModel; get_item can return NULL for position < total if the DB shrinks;
    phig_ctx is documented not-thread-safe but two workers can call phig_query_page on it.
  - MY DEFECTS, not the implementer's: get-item-no-block does not discriminate (a fully
    blocking get_item passes it), and the 64-byte LSan leak is in the brief's own verbatim
    test code (unref'd first row in row-strings-survive-eviction).
Task 2: fix round 1/5 dispatched.
Task 2: fix round 1/5 (2 Critical + 2 Important addressed, 0 open; commit fcf7f3c)
  C1 items_changed(position, n_rows, n_rows); C2 one shared placeholder ref'd per call;
  I3 fetch_task_data_free frees data->fetched with the field NULLed after the page ctor
  consumes it; I4 option (a) — in_flight counter + GMutex/GCond with dispose waiting.
  Plus a new async test, and both of MY test defects fixed.
  CONTROLLER VERIFIED THE GATE: reverting the emission to (position, 0, 0) fails the new
  test on the ARGUMENT assertion (capture.removed == 200 got 0), not a timeout, while the
  other 5 still pass — proving they never covered that path. 6/6 green after restore.
  Re-reviewer's sharper analysis: the in_flight machinery is REDUNDANT, not the fix. The
  temporary strong ref from g_weak_ref_get already prevents dispose from running until
  leave_in_flight unrefs, so dispose can only ever observe in_flight==0 and the g_cond_wait
  is dead code in practice. Harmless belt-and-braces; controller ruling = keep it rather
  than churn working concurrency code. Logged as a possible simplification.
Task 2: complete (commits 760634f..fcf7f3c, review clean; 6/6 tests)
Task 2: minor (deferred): the in_flight/GMutex/GCond/dispose override (~25 lines) could
  likely be deleted in favour of relying on the temporary strong ref's lifetime.
Task 2: DEFERRED BUT LOAD-BEARING FOR TASK 4 — phig_ctx is documented NOT thread-safe
  (~/.local/include/phig/phig.h:9: "Use one ctx per thread, or serialize access
  externally"), but g_task_run_in_thread uses a shared pool and PHIG_PAGE_RADIUS 2 means
  scrolling across a page boundary can put TWO workers inside phig_query_page on the SAME
  ctx concurrently. Flagged independently by both reviewers. Controller confirmed the
  header text and the concurrency. NOT in Task 2's four findings, but Task 4 scrolls for
  real, so it must be resolved there: either serialize page fetches (single-thread the
  fetch queue) or give each worker its own ctx.

TSan "data races" DIAGNOSED — THEY ARE FALSE POSITIVES, NOTHING TO FIX (2026-08-04).
  Root cause: OpenCV's parallel framework is OpenMP (cv::getNumThreads()==12). A phig
  worker calling detect()/decode spawns an OpenMP team INSIDE the call — visible in the
  stack as gomp_team_start creating a thread from within process_file. libgomp is NOT
  TSan-instrumented (verified: 0 __tsan symbols in libgomp.so.1 AND libopencv_dnn.so), so
  TSan cannot see OpenMP's barriers and every parallel region looks unsynchronised.
  PROOF, phig code unchanged:
    *Face*       default 77-86 -> OMP_NUM_THREADS=1 -> 0
    *Scan*       default 42    -> OMP_NUM_THREADS=1 -> 0
    *Duplicates* default 0     -> 0
  Dispositive: serialising OpenCV's INTERNAL pool cannot fix races in phig's own data
  structures. Neither hypothesis held — not a FaceDetector lifetime bug, not OpenCV DNN
  global state. detector_for_worker's per-worker isolation works as designed.
  TWO CORRECTIONS TO MY EARLIER REPORTS: (1) these are not races at all; (2) my
  attribution to LibRaw::get_timestamp/tzset was wrong — it appeared in the first log
  entry I read, but by group *Raw* and *Hasher* produce ZERO. I generalised from one
  report. Setting TZ also did NOT help (40->40 suite-wide) despite fixing one isolated
  test 6->0, which nearly became a second wrong conclusion.
  ACTIONABLE RESIDUE: to make TSan meaningful on phig, run it with OMP_NUM_THREADS=1.
  Otherwise ~120 false positives drown any real signal. Worth putting in AGENTS.md.
  Tree untouched; 634/634.

Task 3: implemented (commit 39654e0). 5/5 tests, valgrind 0 lost / 0 errors (controller
  re-verified both). Seam intact: max_dim in the signature, THUMB_MISS on miss, ZERO decode
  references in thumb-loader.c.
  Controller mutation check: FIRST ATTEMPT WAS MY ERROR — there are 4
  g_cancellable_is_cancelled sites and I mutated line 155 (the post-work deliver check),
  which all 5 tests survived. Mutating the correct pre-dispatch site (~300, in
  thumb_loader_request) failed with (1 <= 0). The implementer's strengthening (zero-worker
  loader + thumb_loader_queue_length) works as described.
Task 3: FULL-DEPTH REVIEW: spec OK, quality NOT approved. 1 Critical + 2 Important. The
  reviewer REPRODUCED the Critical under ASan rather than reasoning about it, and extended
  my cancellation finding by instrumenting all 4 sites across 10 runs.
  - CRITICAL: dispose() calls g_main_context_iteration(NULL, TRUE) to drain pending_idle,
    which DISPATCHES USER CALLBACKS DURING TEARDOWN. ASan stack captured:
    cb <- thumb_loader_deliver:164 <- g_idle_dispatch <- g_main_context_iteration <-
    thumb_loader_dispose:250 <- g_object_unref. The breaking teardown order is the NATURAL
    one (free my per-cell state, then unref the loader last), because nothing warns that
    g_object_unref is itself a callback dispatch point. Also dispatches UNRELATED sources
    (reviewer queued an independent g_idle_add and it ran inside dispose) and is
    re-entrancy-unguarded (a dispatched callback can call thumb_loader_request on a
    half-disposed loader whose workers are already freed -> silent leak).
    Recommended fix: keep pending_idle, but have the IDLE SOURCE hold the loader alive via
    g_idle_add_full + g_object_ref + destroy-notify unref. No pumping, no cycle, because
    the ref lives on the source rather than on ThumbRequest.
  - IMPORTANT: queued requests are never drained at shutdown — valgrind on a probe shows
    120 bytes definitely lost + 72 indirect (the GCancellable) per request. Invisible to
    the suite for TWO compounding reasons: the only zero-worker test cancels pre-dispatch
    so nothing is pushed, AND g_new0(GThread*, 0) returns NULL (controller verified), so a
    zero-worker loader skips the whole dispose block including the pending_idle wait.
  - IMPORTANT: cancel-after-dispatch-discards-result asserts nothing — `called` may be
    either value and `result` is only checked when called is true. In 20/20 runs it was
    caught at pre-lookup, never reaching delivery, so its name is wrong. The deliver check
    IS load-bearing (reviewer reached it with a 300ms sleep) — a real coverage gap.
  - MINORS: header says "LRU tail is evicted" but code pops head (code is right, comment
    inverted); byte-budget test can only ever hold ONE entry (242x512x4=495,616 vs 716,800
    budget) so LRU ordering is never exercised; a texture bigger than the budget evicts
    itself and re-decodes forever; cache is consulted only AFTER lookup+decode, so a repeat
    request costs ~0.5ms it need not (relevant to Task 7); phig_last_error never read, so
    THUMB_FAILED discards phig's diagnostic; workers unref GdkTexture off the main thread
    (fine for a plain GObject but an undocumented assumption).
Task 3: fix round 1/5 dispatched.
Task 3: fix round 1/5 (1 Critical + 2 Important + 2 minors addressed, 0 open; commit b33bb33)
  Pump removed entirely (g_main_context_iteration count now 0); delivery posts via
  g_idle_add_full with the IDLE SOURCE holding the loader ref, released in the
  GDestroyNotify whether or not the source runs. dispose now gates only the join loop on
  n_threads>0 and drains the queue unconditionally. cancel-after-dispatch rewritten to
  spin-wait on a new test-only thumb_loader_pending_idle_count() until nonzero, forcing
  delivery instead of racing for it.
  CONTROLLER VERIFIED THE PREVIOUSLY-UNCOVERED GATE: removing the deliver-side cancel check
  now fails the test ('called' should be FALSE). Before this fix that check was dead to the
  suite. First attempt at this mutation failed because the fix restructured deliver() — my
  script's pattern no longer matched, not a test problem.
  Implementer introduced and SELF-CAUGHT a second UAF while fixing: GLib passes the same
  user_data to both the callback and its GDestroyNotify, so req must be freed in exactly
  one. Re-reviewer verified all 3 paths clean (delivered / cancelled / never-dispatched),
  and that the idle source's ref cannot stall dispose forever.
Task 3: complete (commits fcf7f3c..b33bb33, review clean)
  Both suites green; valgrind 0 definitely lost / 0 errors (controller re-verified).
Task 3: minor (deferred): cache is consulted only AFTER lookup+decode, so a repeat request
  costs ~0.5ms it need not — a pre-dispatch cache probe would remove it. Relevant to Task 7.
Task 3: minor (deferred): phig_last_error is never read, so THUMB_FAILED discards phig's
  diagnostic. If added, must be read on the worker thread that made the failing call.

Task 4: implemented (commit 6ff00c9). App RUNS — window maps ~2s, RSS ~104MB flat from 20s
  to 75s idle. Startup order per spec: window presents empty, then phig_init/query/count on
  a worker, model attached via the GTask completion callback on the main thread.
  ctx sharing confirmed against the Task 2b contract: one ctx shared between the model's
  page fetches and the loader's pool, and NEITHER path calls phig_last_error, so the
  per-calling-thread carve-out is never at risk.
  Implementer could NOT verify scrolling or a before/after-scroll RSS: no input injection
  works in this sandbox (ydotool needs /dev/uinput, wtype has no virtual-keyboard protocol,
  no AT-SPI, no xdotool) and grim cannot capture (same as Task 1).

Task 2 REOPENED — fix round 2/5. THE USER LOOKED AT THE RUNNING APP AND SAID ALL GRID
  SQUARES ARE EMPTY. That is the bug the Task 4 implementer had just reported and I
  reproduced: 223 "Gtk-WARNING: Duplicate item detected in list. Picking one randomly."
  ROOT CAUSE, AND IT IS MY REVIEW FINDING THAT CAUSED IT: gallery-model.c:332 returns
  g_object_ref(self->placeholder) — ONE shared object — for every non-resident position.
  GTK's list-item manager keys recycled widgets by item POINTER IDENTITY, so one object
  claiming 223 positions makes GTK pick widgets randomly. Documented unsupported pattern
  (glib#3030).
  In Task 2 fix round 1 I promoted "fresh placeholder per call" to a CRITICAL and had it
  changed to a shared singleton, reasoning about GtkSelectionModel identity. That fixed
  identity WITHIN a position and broke it ACROSS positions — the worse failure, because it
  corrupts what the user sees. Neither of my two decisions was right; the answer is one
  placeholder PER POSITION, cached and bounded.
  Lesson: no headless test caught this. It only appears in a real GtkGridView. The
  automated suite was 6/6 green while the app displayed nothing.
Task 2: fix round 2/5 (1 addressed; commit 1d8a855) — per-position cached placeholders in a
  GHashTable keyed by position, capped at PHIG_MAX_PLACEHOLDERS (1000) with its own LRU
  clock INDEPENDENT of the page LRU (a fast scroll touches far more distinct pages than the
  page budget bounds before their fetches land, so tying them together would be effectively
  unbounded). Entries dropped for a range as soon as its real rows land.
  CONTROLLER VERIFIED: duplicate-item warnings 223 -> 0 on a real run. 7 gallery-model tests
  pass. Ownership mutation still fails as expected.
Task 2: minor (deferred): no dedicated test for placeholder-cap eviction itself (mirrors the
  tested page eviction structurally but is not independently exercised).

THUMBS REBUILD (user-requested, 2026-08-04): 20,529 generated in 986s, 9,348 skipped, 76
  errors (mostly the 541 videos). Cache 24,484 files / 945 MB. 44/60 of the first visible
  rows now have thumbnails.
  PARALLELISM ANSWERED WITH MEASUREMENT: it IS parallel — 146 threads (12 workers +
  OpenCV OpenMP teams) — but only 151% CPU on 12 cores, with 19/20 sampled threads sleeping.
  Source photos are on /dev/sda1, rotational=1, a 3.6TB HDD; cache target is NVMe. HDD seek
  time dominates and decode is nearly free, so parallelism cannot help. Rate: 13 files/sec.
  On an SSD library this would be CPU-bound and much faster.
  phig-gtk restarted afterwards: RSS 173MB, 19 threads, 0 GTK warnings, 2.2% CPU idle.

ORIENTATION INVESTIGATED (user raised it) — ALREADY CORRECT, no work needed.
  26,493/29,953 rows carry an Orientation tag; 9,778 (~1 in 3) are non-normal: Right-top
  7,730, Left-bottom 1,857, Bottom-right 191.
  phig has ZERO orientation code, but cv::imread applies EXIF orientation BY DEFAULT
  (OpenCV >= 3.4) and phig never passes IMREAD_IGNORE_ORIENTATION. So thumbnails are written
  already-rotated. PROVEN by pixel comparison on a Right-top image: RMSE 0.0175 against the
  auto-oriented source vs 0.378 against the raw one. DB dimensions are also stored oriented
  (3024x4032 for a 4032x3024 file).
  Consequence recorded in the spec AND plan: the GUI must NOT add orientation handling —
  rotating at display time would double-apply it.

SPEC + PLAN UPDATED with the user's rule: non-GUI functionality belongs in libphig, not
  phig-gtk. The old spec had only the negative rule (don't read phig's schema/cache layout);
  the positive rule is stronger. phig-gtk = presentation and interaction only; image and
  database manipulation stay in phig and are exposed through the ABI. When a task needs a
  missing capability it must STOP and say so rather than reimplement GUI-side.
  Supporting fact for lazy thumbnail generation: generate_thumb is ALREADY compiled into
  libphig.so (local symbol 't', via phig-core) and libphig.so already links 9 OpenCV libs —
  so phig_thumb_generate is a one-function EXPORT, not a new dependency.
Task 4: complete (commits b33bb33..1d8a855, review clean — spec OK, quality approved)
  Reviewer traced the cancel-on-unbind rebind race concretely and confirmed it is NOT
  exploitable: GtkSignalListItemFactory guarantees unbind runs before any bind/rebind on the
  same GtkListItem, so unbind NULLs the data-key before a new cancellable is installed and a
  stale callback bails. A real verified guard, not a coincidence. Startup order confirmed:
  window presents before any query; phig_query_count runs inside phig_gallery_model_new from
  startup_done on the main thread. items-changed math verified for full AND partial last
  pages.
Task 4: DEFERRED TO TASK 7 (Important, with the reviewer's own suggested disposition):
  placeholder eviction uses a GLOBAL LRU clock bumped by every get_item, so a full-range
  scrollbar drag across 29,953 rows can touch >1000 distinct non-resident positions in a
  burst and evict a placeholder still bound to a live realized GtkListItem. If GTK then
  re-queries that position for identity re-validation it gets a different pointer — the SAME
  GtkListItemManager identity collision that caused the original 223 warnings, now triggered
  by eviction pressure instead of a shared singleton. No test forces the cap
  (test_placeholders_are_unique_per_position exercises only 2 positions), so it is unverified
  in either direction. Task 7 already owns tuning PHIG_MAX_PLACEHOLDERS against real
  scrolling, which is exactly where a cap value should be chosen by measurement rather than
  guessed now. Task 7 MUST watch for duplicate-item warnings during its scrollbar-drag test.
Task 4: minor (deferred): startup failure (phig_init/phig_query_new) only g_warnings and
  leaves an empty grid forever with no user-visible indication — Task 6 owns this.
Task 4: minor (deferred): main.c's default_db_path() reimplements phig's data_dir()/
  default_db_path() byte-for-byte. Not a schema/cache violation, but exactly the client-side
  reimplementation the libphig rule warns about. Candidate for a phig_default_db_path()
  accessor.

Task 4b: complete (phig 14e8299, phig-gtk 8163a0d) — lazy per-page thumbnail generation.
  phig: exported phig_thumb_generate(ctx, image_id), wrapping ThumbsEngine's REAL per-file
  logic behind PHIG_TRY/PHIG_CATCH, keyed by a new Database::get_image_for_thumb. ABI minor
  1u->2u, CMake VERSION 1.1.0->1.2.0, SOVERSION unchanged at 1 (matching the settings-API
  precedent). Symbols 41->42. 639/639 tests (6 new AbiThumbGenerate).
  phig-gtk: thumb_loader_request now takes image_id alongside sha256; the worker checks
  cancellation immediately before phig_thumb_generate then re-looks-up. THUMB_MISS retained
  for no-sha256/failed rows.
  CONTROLLER VERIFIED ALL OF IT: 42 symbols, MINOR 2u, VERSION 1.2.0, SOVERSION 1, and the
  video branch really does use extract_video_keyframe + the cv::Mat overload.
  THE TRAP WAS AVOIDED AND IS NOW GUARDED: bypassed the video branch to simulate a naive
  generate_thumb(path, sha) wrap and VideoRowProducesThumbnailViaKeyframe FAILED
  (test_libphig_abi.cpp:379). That is the 541-video silent-failure the libphig rule exists to
  prevent, now covered by a discriminating test.
  END-TO-END PROOF: deleted the cached thumbnail for the first visible row, ran the app 20s,
  and it regenerated at 512x341 / 25,230 bytes purely from scrolling that page into view.
  Both suites green after reinstall: phig 639/639, phig-gtk 2/2.
Task 4b: minor (deferred): phig-gtk has no unit test for miss->generate against a
  never-cached row (needs a scratch DB + cache copy). Covered by phig's 6 ABI tests plus the
  real-app run.
Task 4b: minor (deferred): no visual screenshot — the compositor lacks screen-capture
  protocol and the portal call needs interactive approval. Verified via cache mtime/content
  and warning counts instead. Same limitation as Tasks 1 and 4.

Task 5: implemented (commit d04f754) — on_cell_activate + on_launched + an AdwToastOverlay.
  Launcher lifetime is CORRECT (unreffed in on_launched, which must outlive the async call).
  Signatures verified by the controller against the installed GTK 4.22.4 header before
  dispatch: gtk_file_launcher_new(GFile*), gtk_file_launcher_launch(self, parent, cancellable,
  cb, user_data), gtk_file_launcher_launch_finish(self, result, error).
Task 5: CONTROLLER FOUND 2 DEFECTS THE REPORT MISSED, plus a contradiction:
  - IMPORTANT: no NULL check on g_list_model_get_item. It returns NULL for an out-of-range
    position — PhigGalleryModel's own out-of-range-returns-null test asserts exactly that —
    so PHIG_IMAGE_ROW(NULL) then feeds NULL to phig_image_row_is_placeholder.
  - IMPORTANT: g_object_get(selection, "model", &list_model, NULL) returns a NEW reference
    that is never unreffed, leaking one model ref per activation.
  - The report claimed "no unverifiable runtime behavior" while ALSO saying it could not
    verify the launch fires. Both cannot be true, and the second is false: the brief asked
    for g_signal_emit_by_name and that was not attempted. Neither defect would show up in
    the GTK warning count, which is what the report leaned on.
Task 5: fix round 1/5 dispatched — NULL check, fix the leak on every exit path, and actually
  emit "activate" behind an env-var guard to prove the launch path runs.
Task 5: fix round 1/5 (2 Important addressed; commit 3078840) — NULL check on
  g_list_model_get_item, and g_object_unref(list_model) on all four exit paths.
Task 5: complete (commits 8163a0d..3078840, verified by controller experiment)
  The implementer could not confirm the launch fires and attributed on_launched never running
  to "no dbus session / portal unavailability". THAT DIAGNOSIS WAS WRONG — the controller
  verified 12 portal services live on the session bus, image/jpeg registered to
  org.gnome.Loupe.desktop, and `gio open` on the very same file launching Loupe successfully.
  CONTROLLER FOUND THE REAL CAUSE by installing a temporary probe that emits
  g_signal_emit_by_name(grid, "activate", 0): the app was being killed before the async
  callback could land. With timeout 40 instead of 18, on_launched ran — 35 SECONDS after the
  activate, because Loupe was cold-starting via D-Bus activation
  (--gapplication-service). On the next run, warm, launch_finish returned OK in 4ms.
  So the launch path is CORRECT and now demonstrated: "PROBE: launch_finish -> OK".
  Probes removed; 0 GTK warnings; both suites pass.
  Lesson for Task 7: a first-launch portal round-trip can take tens of seconds cold. Any
  timing measurement that involves launching an external app must account for that, and a
  short `timeout` will silently truncate async callbacks.

Task 6: complete (commit 2647a63, verified by controller)
  Startup failure now shows an in-window AdwStatusPage; phig's own text is captured via
  phig_last_error(ctx) ON THE WORKER THREAD (per the ABI's per-calling-thread rule) and
  carried to the main thread in a GError. A failed phig_init has no ctx to query, so that
  case gets a fixed message rather than a decoded status enum — correct, and it avoids
  building a status->text lookup table in the GUI (which would belong in phig).
  Added a page-fetch-failed signal on PhigGalleryModel, one AdwToast per session rather than
  per row. Added a header-bar failure counter, hidden until the first THUMB_FAILED.
  IMPLEMENTER FOUND A REAL BUG: a failed phig_thumb_generate was falling through to
  THUMB_MISS instead of THUMB_FAILED, which would have made every generation failure
  invisible AND uncounted — defeating the counter's entire purpose. Now an empty sha256
  short-circuits to MISS and a lookup/generate error is FAILED. The stale test that encoded
  the wrong behaviour was corrected and a new one added.
  CONTROLLER VERIFIED: nonexistent DB -> app stays ALIVE with NO console warning (the error
  moved on-screen). Real DB -> 0 GTK warnings. Both suites pass (now 6 thumb-loader tests).
  No perm-000 files left in the cache after the chmod test.
  CONTROLLER VERIFIED THE NEW GATE: regressing THUMB_FAILED back to THUMB_MISS fails
  generation-failure-yields-thumb-failed with (1 == 2). Genuinely discriminating.
Task 6: minor (deferred): the phig-thumb-failed / phig-thumb-placeholder CSS classes have no
  stylesheet backing them, so the broken-image state is not visually distinct. Pre-existing
  from earlier tasks; cosmetic.
Task 6: minor (deferred): no screenshot verification possible (compositor lacks the capture
  protocol) — all confirmation is behavioural and log-based. Same limitation as Tasks 1, 4, 4b.

Task 7: complete (commit 3c65309) — measured, values set, criterion reported honestly.
  Harness drives the scrolled window's GtkAdjustment behind PHIG_MEASURE=1 (no input
  injection exists here). CONTROLLER REPRODUCED THE HEADLINE NUMBERS INDEPENDENTLY:
  startup_ms=72.084 (reported median 73.8, budget 500 — PASSES), and
  worst_frame_ms=83.335 with over_16ms=199 of 210 frames (95%).
  M0 CRITERION: NOT MET, and stated plainly rather than adjusted. Startup passes; the grid
  does NOT hold 60fps warm. Miss-heavy scroll far worse (worst ~1.3s), which is the HDD.
  Four values swept — PAGE_RADIUS 1/2/4, THREADS 2/4/8, CACHE 64/256/512MB. NONE changed
  warm-cache frame timing; only RSS moved. All kept at their starting values (2, 4, 256MB)
  with RSS as the tiebreaker, and MAX_PLACEHOLDERS left derived. That is a real result: the
  bottleneck is not any of the things the spec guessed might need tuning.
  TASK 4's DEFERRED RISK CLEARED: fast full-range drags produced ZERO duplicate-item
  warnings at the derived placeholder cap AND at a smaller one, warm and miss-heavy.
  IMPLEMENTER FOUND AND FIXED A REAL SIGSEGV: main() freed the ThumbLoader before draining
  the main context, so a posted-but-undelivered result could hold the last ref, preventing
  dispose (and its g_thread_join) from ever running — leaving a worker inside libphig.so
  while exit() unloaded shared libraries. Controller verified 5 clean start/stop cycles
  (all exit 124 from timeout, never 139).
  CONTROLLER CHECKED whether this drain reintroduces Task 3's Critical: it does NOT. That one
  pumped inside dispose, DURING teardown, so callbacks hit a half-destroyed object. This
  pumps in main() BEFORE any teardown with the loader fully alive, so callbacks are ordinary
  deliveries. Materially different, and the comment says so.
Task 7: OPEN FINDING, not fixed: at THREADS=8 only, concurrent phig_thumb_generate against a
  DB row whose source file is MISSING kills the process silently (exit 0, no signal, no
  diagnostic). Specific to one dangling row and to 8-thread concurrency; does not reproduce
  at the kept value of 4. Root cause not traced past phig-core's video/GStreamer path.
Task 7: minor (deferred): OpenCV/GStreamer cannot open mvi_2541-2.avi and phig-gtk retries it
  every ~5s, so a permanently-failing row produces a retry loop. Worth a negative cache.

FINAL WHOLE-BRANCH REVIEW (opus, both repos, 12 + 5 commits): 2 Important, merge-blocking.
  Corroborated: libphig boundary held across all 12 commits with exactly ONE exception
  (default_db_path); phig_thumb_generate is a faithful wrap of ThumbsEngine's real logic
  (identical video/keyframe branch); the phig-side thread-local last_error is correct and its
  test genuinely discriminating; phig.pc's ${pcfiledir} fix is right; no cv::Mat, no
  phig-core header, no orientation code anywhere in the GUI. It also CONFIRMED my
  Task-3-vs-Task-7 drain-loop judgement.
  - IMPORTANT 1: main()'s drain loop guards on thumb_loader_pending_idle_count, which counts
    only results ALREADY POSTED to the main thread. A request a worker has popped but not yet
    posted is counted nowhere. Race: worker calls post_result (bumping the count and taking a
    loader ref) JUST AFTER the main thread reads 0 -> loop exits -> g_clear_object drops the
    app's ref but the idle source's ref keeps the loader alive, so dispose and its
    g_thread_join NEVER run -> phig_shutdown frees ctx while workers are still about to call
    phig_thumb_lookup on it -> exit() unloads libphig.so with workers inside. Exactly the
    stack Task 7 diagnosed. The drain loop NARROWED the race, it did not close it — which is
    why 5 clean cycles passed.
  - IMPORTANT 2: PhigGalleryModel is NEVER DISPOSED. It is owned by GtkNoSelection <- GridView
    <- window, and main() never unrefs the window; it goes straight to g_object_unref(gtk_app)
    then phig_app_free. Controller verified phig_app_free does exactly: clear loader,
    phig_shutdown(ctx), free. So the in_flight/GMutex/GCond wait added in Task 2 fix round 1 —
    correctly implemented — is UNREACHABLE, and a worker inside phig_query_page can be running
    when the ctx is freed.
  - THIS REVISES MY OWN EARLIER RULING: I accepted the Task 2 re-reviewer's "in_flight is
    redundant" as a deferred minor. It is not redundant; it is unreachable. Do NOT delete
    those 25 lines — make dispose run.
  Both findings share one fix: make shutdown an explicit ordered sequence (stop and join
  workers, tear down the model, THEN free ctx) instead of three components each assuming
  someone else's refcount falls to zero at the right time.
  60 FPS DIAGNOSIS from the code: nothing scales a thumbnail to the cell. Cache size is
  512x341, PHIG_CELL_SIZE is 200, and max_dim is explicitly discarded ((void)max_dim), so
  every cell uploads ~6.5x the pixels needed and GTK_CONTENT_FIT_COVER downscales on the GPU
  at draw time. Corroborated by the data: the recurring 66.668/83.335 ms are exact 4x/5x
  multiples of a 16.667 ms refresh (fixed per-frame overrun, not stochastic jank), and timing
  was invariant across all three swept values — so it is not fetch latency, worker starvation,
  or cache thrash. Next measurements named, in order.
FINAL FIX WAVE (phig-gtk fc64943, phig 94fc2f9) — all 4 findings ADDRESSED, no new breakage.
  New teardown is an explicit ordered sequence, not refcount guessing:
    thumb_loader_shutdown (stopped=TRUE, sentinels, join all workers, drain queue)
    -> g_clear_object(&loader) -> gtk_grid_view_set_model(grid, NULL) -> phig_shutdown(ctx)
  The pending_idle drain loop is GONE (0 refs in main.c). An idle source posted before
  shutdown still holds a loader ref, but deliver() now checks `stopped` and returns without
  touching ctx or invoking the callback. stopped is written and read on the MAIN THREAD ONLY,
  so no new race. thumb_loader_shutdown is idempotent (guarded on workers != NULL).
  CONTROLLER VERIFIED: phig_gallery_model_dispose now runs — 5 independent cycles, dispose=1
  each, 0 GTK warnings. Previously it NEVER ran. phig-gtk 13 tests pass; phig 640/640.
  Re-reviewer established WHY set_model(NULL) drops the last ref rather than assuming it:
  phig_gallery_model_new's ref goes straight into gtk_no_selection_new and is never separately
  unreffed, and `selection` is unreffed right after set_model — a single-owner chain by
  construction, durable unless a future query-replace feature stashes a second ref.
  Startup-failure path checked: app->ctx is only assigned in the success branch, so
  phig_shutdown is skipped there and the failure branch frees startup->ctx itself.
  FINDING 4 NEGATIVE RESULT, worth recording: 15 runs of the new 8-thread dangling-path test
  did NOT reproduce the hypothesised silent exit(0) — all returned PHIG_ERR_INTERNAL cleanly.
  MY GStreamer-exit() hypothesis was WRONG, and this is consistent with the earlier
  "decode does not crash" measurement (12,684 probes, 0 crashes). The existence check was kept
  anyway because it is semantically right, not because it fixed the hypothesised bug.
  Re-reviewer confirmed the test WOULD have caught a silent death (EXPECT_EXIT distinguishes
  exited-0 from died-by-signal), so the negative result is trustworthy.
M0 COMPLETE — 13 phig-gtk commits, 6 phig commits. Neither repo pushed.

## 2026-08-05 — empty grid cells traced to stale DB rows, and cleaned up

The empty cells were NOT a GUI bug. 76 rows pointed at files under
/mnt/data/Pictures/Brett/2026/07/, a directory that had been deleted. 70 of those rows fell
in the first 400 of a newest-first sort, so they filled the first screen.

CONTROLLER ERROR WORTH RECORDING: I first reported these 76 as "genuinely lost", based on a
DB-only search — for each row I found other ROWS sharing its sha and tested those paths.
That is blind to a file that exists on disk with no row, which was true for all 48 in
unsorted/. The user caught it by handing me a copy of image54.jpg; that file turned out not
to be one of the 76 at all (it came from a 5-row audit-log SAMPLE I printed, and had already
been relinked to 2010/10). I illustrated a claim with an example that disproved it.

Corrected accounting, all verified by sha256:
  48 files sat unindexed in /mnt/data/Pictures/Brett/unsorted/ (48 files, 0 DB rows)
  28 were in /mnt/data/.Trash-1000/files
   0 genuinely missing
No tags, faces, favorites, or galleries existed on any of the 76, so nothing user-authored
was at risk.

The audit log gave full provenance: all 76 arrived in 2026/07 via phig's own mv on
2026-07-11/12 from /mnt/data/tmp/Photos staging folders. But NONE of the 76 appears as a
move SOURCE, and NO log entry has a destination under unsorted/ — so phig did not move them
there. Confirmed by reading the code: organize_exec.cpp:24-50 moves then updates the row and
logs in ONE transaction, moving the file back on failure, and "unsorted" appears zero times
in organize_exec.cpp or remove_exec.cpp. The user's hypothesis that a move-to-unsorted skips
the DB update is not what the code does.

FIX APPLIED (DB backed up first to phig.db.bak-20260805-091416):
  1. scan /mnt/data/Pictures/Brett --recursive: 371 processed, 0 errors
     NOTE: this did NOT relink as I predicted — relink only considers rows that
     paths_safe_to_delete classifies as missing, and that guard skipped all 74. So it
     created 48 NEW rows alongside the stale ones, briefly leaving duplicate pairs.
  2. rm --match '/mnt/data/Pictures/Brett/2026/07/*' --db-only --yes: removed 74
     (rm --orphans found ZERO — see the follow-up below)
  3. thumbs rebuild: 47 generated in 2.0s, 0 errors
Result: 29,925 rows, ZERO dangling (verified every row's path), 0/400 visible rows missing a
thumbnail, phig-gtk running with 0 GTK warnings and 0 thumb failures.

FOLLOW-UP FILED: docs/plans/2026-08-05-orphans-mount-guard.md
  rm --orphans cannot remove orphans when a whole directory is deleted, because
  paths_safe_to_delete's mount heuristic skips any row with a missing parent — and it
  reports "No matching entries found.", success-shaped output for a case where it declined
  to act on 74 rows. Wants an --ignore-mount-warning flag on rm (matching scan's existing
  flag name) AND, more importantly, a report of what was skipped and why.

NOT A BUG (user was right): the mv/organize date fallback is already fixed. dates.cpp:30-69
resolves EXIF DateTimeOriginal -> EXIF CreateDate/DateTimeDigitized -> sidecar -> file mtime
-> invalid (unsorted). No fallback to "now". The July 2026/07 misfiling was tier-4 mtime
resolving to the import date on files that had been copied around — documented behaviour.

FOLLOW-UP OUTLINED (not planned yet): docs/plans/2026-08-05-thumbnail-sizing-zoom-hidpi-outline.md
  Texture downscaling AND HiDPI are ONE problem, because they pull in opposite directions.
  Measured for a 200px logical cell: 1x needs 200 physical px (so uploading 512 wastes 6.6x
  the pixels — the perf bug), 2x Retina needs 400 (512 is adequate), 3x needs 600 (512 is
  INSUFFICIENT). So a naive "downscale to cell size" fix would look right at 1x and BLURRY on
  every Retina display. One number cannot satisfy both.
  Also measured: THUMB_SIZE is a hard-coded 512 (thumbs.cpp:13), PHIG_CELL_SIZE is 200
  (main.c:10), max_dim is explicitly discarded, and the GUI reads the display scale factor
  NOWHERE (0 refs across all 3 source files). Raising the cache costs ~2.1GB at 768px or
  ~3.7GB at 1024px for this collection, plus a full regeneration (16 min here, HDD-bound).
  All four GTK scale APIs confirmed present in 4.22.4, including the FRACTIONAL
  gdk_surface_get_scale (4.12+) — needed because Wayland does 1.25/1.5 scaling that the
  integer API rounds.
  The outline names three decisions (where to downscale, what size the cache holds, how the
  GUI learns the scale factor), a 3-step measurement sequence that must run FIRST because the
  diagnosis is inferred rather than isolated, and it folds in two deferred ledger items that
  live in the same code (the post-lookup cache probe, and the permanently-failing-row retry
  loop). Step 1 — set PHIG_CELL_SIZE to 512 so no scaling occurs — discriminates the two
  hypotheses with no new code.
  ZOOM ADDED to that outline (user raised it): a file-browser-style zoomable grid makes
  max_dim a RUNTIME RANGE, not a constant, and it reshapes all three decisions rather than
  being an extra feature. Three consequences that are easy to miss, all measured:
   - The texture cache is keyed by sha256 ALONE (thumb-loader.c:112). Correct for one fixed
     size; with zoom it serves a stale wrong-size texture or thrashes on every zoom step. Key
     must become (sha256, physical_size).
   - A FIXED BYTE BUDGET gives wildly different cache depth per zoom. At today's 256MB, RGBA:
     200px holds 1677 textures, 600px holds 186, 1536px holds 28 — a 60x swing, because
     zooming in shrinks the resident window by the SQUARE of the zoom. 28 textures at max zoom
     on a 3x display is ~one screenful with no prefetch margin. The budget likely needs
     expressing as a target texture COUNT (or N screenfuls) that resolves to bytes at the
     current size.
   - EVERY M0 MEASUREMENT WAS TAKEN AT ONE ZOOM LEVEL (200px cell) — the 73.8ms startup, the
     83.3ms worst frame, and all four tuned values including the 256MB budget. None
     generalises to a zoomable grid. Added as measurement step 4.
  Also flagged: PHIG_CELL_SIZE is compile-time in 4 places and becomes runtime state; a zoom
  step rebinds every visible cell, itself an unmeasured cost; and the outline asks whether zoom
  should snap to DISCRETE levels (bounding cache generations) versus continuous.
  New framing the zoom analysis produced: the cache-size question is not "what is the cell
  size" but "what is the largest zoom x scale we intend to support" — and one cheap answer is
  to CAP max zoom at what the existing 512px cache can serve, treating bigger cells as a
  viewer concern, which needs no phig-side regeneration at all.
