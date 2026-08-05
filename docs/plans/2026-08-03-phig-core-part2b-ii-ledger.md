# SDD ledger — plan: /home/brett/devel/phig/docs/plans/2026-08-03-phig-core-part2b-ii.md
Branch: phig-core-part2b-ii (from master b2ce936)

## Pre-flight rulings (human partner, before Task 1)
- Task 4 progress counter: FIX IT. Atomic completion counter replaces `idx + 1`,
  which jumps backwards at --parallel>1 (parallel_for_each shares a cursor).
  `face rebuild` per-file lines WILL differ from baseline. Intentional, not a regression.
- Task 4 transaction block moved verbatim from main.cpp: controller ruling, not a
  defect. It is a move, and the CLI copy is deleted in the same task.
- Task 6 testing: measurement is sufficient. Observability fix, not data-safety —
  no user data at risk, only whether a warning reaches the terminal. Record
  before/after --parallel 8 counts in the commit message. Do NOT accept a
  shape-only test that cannot fail.

Task 1: complete (no commits — baseline artifacts in /tmp/phig-2bii-baseline, verified by controller)
  Artifacts: phig-baseline (1115504 B), thumbs-rebuild.txt (264 B),
  thumbs-rebuild-cached.txt (215 B), faces-rebuild.txt (175 B). Face models ARE
  installed; faces baseline captured (3 faces detected).
Task 1: controller found 2 gaps in the plan's own verification strategy, both fixed:
  (a) video keyframe branch unexercised — .mov thumbs already existed from scan, so
      Generated: 2 / Skipped: 12. Added --force baselines (Generated: 14) to cover it.
  (b) --parallel 4 progress bytes are shredded by interleaved worker writes
      (`^M[^M[78//14]`), so they are NOT a byte-for-byte contract. Added a
      --parallel 1 force baseline as the real contract.
  Plan Task 3 Step 6 amended with both. New artifacts: thumbs-rebuild-force.txt,
  thumbs-rebuild-force-p1.txt.

Task 2: complete (commits b2ce936..e039f96, review clean — spec OK, quality approved)
  Implementer deviated from the brief's test code and was RIGHT to: the brief's
  ForceRegeneratesEvenWhenTheThumbExists used a sha with no cached thumb, so
  `force || !exists` and `!exists` both return true and mutation #1 would have
  SURVIVED. Added an XDG_CACHE_HOME fixture creating a real cache file.
  Controller verified the path layout independently: cache_dir()=$XDG_CACHE_HOME/phig,
  get_thumb_path=thumbs_dir()+"/"+sha[:2]+"/"+sha+".jpg" — fixture matches exactly.
  Both mutations confirmed caught.
Task 2: minor (deferred): 3rd test's assertion is a strict subset of the 1st test's
  first line — brief-inherited redundancy, not introduced.
Task 2: minor (deferred): ThumbsEngineTest fixture duplicates ThumbsTest in
  tests/test_thumbs.cpp verbatim (~15 lines). Candidate for a shared fixture header.
Task 2: minor (deferred): setenv XDG_CACHE_HOME is process-global; safe under serial
  gtest, would break under a parallel runner. Identical to pre-existing ThumbsTest.
Task 2: NOTE — security warning on the commit is the known false positive (subagent
  cannot see the human partner's standing per-task commit authorization).

Task 3: implemented (commit 213666d) — faithful port, drain loop mirrors cmd_scan.
  Implementer fixed 2 real bugs in the brief's OWN test code: db->insert_image()
  does not exist (real method is Database::insert()), and the fixture's all-zero
  shas collided across instances AND polluted the developer's real
  ~/.cache/phig/thumbs. Both fixes verified correct.
Task 3: reviewer said "no duplicated work across the CLI/engine boundary" — WRONG.
  Controller verified directly: db.get_all_images() + passes_selection run TWICE
  (main.cpp:1268-1275 and again in ThumbsEngine::run), and ThumbsStage::Selected is
  published by the engine but discarded at main.cpp:1308. Escalated to Important
  and sent to fix round 1.
Task 3: fix round 1/5 dispatched — remove the CLI's duplicate query+filter, render
  the banner from the Selected stage instead.

Task 6 pre-research (controller, while Task 3 fix ran) — CORRECTS the plan's premise:
  StderrSuppressor ALREADY serializes itself with a static mutex held for the whole
  suppressed region (src/hasher.cpp:169-194), and its comment says why. So it is NOT
  the unserialized save/restore race the plan describes.
  The real mechanism: the suppressor's mutex serializes suppressors against EACH OTHER,
  but log_warn (src/log.cpp:97) -> spdlog -> stderr sink writes fd 2 without taking
  that mutex. A warning emitted by worker B while worker A holds the suppressor across
  a full cv::imread goes to /dev/null. Wide window = frequent loss.
  Consequence: plan option 1 (silence OpenCV/LibRaw at source, delete the fd
  redirection) is now clearly preferred — it also removes a mutex held across every
  decode, i.e. a serialization point on the hot path. Option 2 (share one mutex with
  log_warn) would make that contention worse.
  Call sites: src/hasher.cpp:284 and :387 only.
Task 3: fix round 1/5 (1 addressed, 0 open — duplicate get_all_images+passes_selection
  removed, banner now rendered from ThumbsStage::Selected via a switch mirroring
  cmd_scan; commits 213666d..392f5ec)
Task 3: complete (commits e039f96..392f5ec, review clean)
  Controller independently ran the full suite: 600/600 pass, 1 pre-existing unrelated
  skip (ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing). All four baseline
  comparisons identical, including force runs showing Generated: 14 (proves the video
  keyframe branch survived the move).

Task 4: implemented (commit bda8c93) — full-depth review: spec OK, quality approved,
  NO Critical/Important findings. Verified clean on all 5 priority axes:
  - Transaction: exactly one BEGIN per critical section. delete_faces_for_image,
    insert_face, auto_label_face all use bare statements with no internal
    transaction, so the insert_batch-style double-BEGIN defect did NOT recur.
  - Exception safety: progress.close() runs OUTSIDE the catch (main.cpp:1578), so the
    drain loop cannot hang on a worker throw. DB left consistent; per-image atomicity.
  - vec_faces: delete_faces_for_image deletes vec_faces FIRST then faces (order
    required — the subquery needs the faces rows). No orphaned shadow rows.
    insert_face captures face_id BEFORE the vec_faces insert, so the old
    last_insert_rowid() defect is absent.
  - Cancellation: in-flight items finish, no half-written transaction, no image left
    face-less.
  - Mutations both believable: (a) stored=12 vs 6 arithmetic is exactly right;
    (b) mixed-dimension fixture forces setInputSize per item, reallocating the DNN
    blob pool — concurrent access hitting legacy_backend refcount assertions is the
    signature failure for that race.
  Implementer caught a real omission in the brief: the brief's engine snippet would
  have silently dropped "No matching images found."
Task 4: fix round 1/5 dispatched — 2 Minors worth fixing: (1) an already-cancelled run
  still constructs every FaceDetector (~150MB each, up to ~600MB) before
  parallel_for_each checks the token — real production path, not test-only;
  (2) DetectsFacesInEveryImageAcrossWorkers used EXPECT_GT(>0) so it passes with only
  1 of 4 images processed, not discriminating what its name promises.

Task 6 MEASUREMENT (controller, before dispatch) — reproduces, and settles the fix choice:
  Repro (10 undecodable files): --parallel 1 -> 10/10 warnings on stderr.
                                --parallel 8 -> 8/10 (bad2, bad10 swallowed).
                                second run at --parallel 8 -> 9/10. Nondeterministic.
  OPTION 1 IS DEAD. Removed the imread suppressor and measured what leaks: libjpeg's
  "Premature end of JPEG file" x3, written DIRECTLY to fd 2 (needs a genuinely
  truncated JPEG to reproduce — a 19-byte "not an image" file is rejected before
  libjpeg engages). The second call site (:387) suppresses GStreamer for video.
  Neither goes through OpenCV's logging API, so cv::redirectError / setLogLevel
  cannot suppress them. install_cv_error_handler already handles OpenCV's OWN
  messages via a separate thread-local path (t_cv_warning).
  MECHANISM CONFIRMED: log.cpp:64 uses spdlog::sinks::stderr_sink_mt, which writes via
  FILE* stderr. dup2 swaps what fd 2 POINTS AT while stderr keeps writing to fd 2, so
  phig's own warnings are collateral damage of the libjpeg suppression.
  NOTHING IS ACTUALLY LOST: same run, --parallel 8 -> stderr 9/10 but the rotating
  file sink captured 10/10. The defect is confined to the TERMINAL copy.
  => Supports the human partner's ruling that a measurement suffices (terminal
     fidelity, not data safety). Option 2 (share a mutex with log_warn) or option 3
     remain; option 2 adds contention to a mutex already held across every decode.
  src/hasher.cpp restored to pristine; build verified.
Task 4: fix round 1/5 (2 minors + 1 comment addressed, 0 open; commits bda8c93..56423a4)
  Early cancel check placed after images.empty() and before the LoadingModels publish;
  runtime for a pre-cancelled run dropped ~128ms -> ~1ms, direct evidence the detectors
  are no longer constructed. Test now EXPECT_EQ(faces_detected, 6u) + FileDone count==4
  through a real queue (6 established over 7 instrumented runs, deterministic).
Task 4: complete (commits 392f5ec..56423a4, review clean)
  Controller independently verified: 606/606 pass, 1 pre-existing unrelated skip, and
  all 6 FacesEngine tests RAN (none skipped — models installed).
  Also removed a stray untracked test_switch.o (reviewer scratch from the -Wswitch
  check); .gitignore does not cover *.o, so it would otherwise risk being swept into a
  later commit.
Task 4: minor (deferred): brief listed face_models_exist()/yunet_model_path() as
  consumed but the engine uses neither — FaceDetector's ctor throws the actionable
  "run phig models download" message instead. No regression (pre-change CLI had no
  explicit guard either), but the message now surfaces AFTER "Loading face recognition
  models (N instances)..." has printed, which is a slightly odd sequence. Unchanged
  from pre-change behavior.

Task 5: implemented (commit a882ec7) — RelinkTest.RelinkRunsBeforeDemoteOnSharedTarget.
  Real divergence found, but NOT via the mechanism the brief hypothesized. Actual
  mechanism: transfer_user_metadata fills only a NULL target (first-write-wins), so
  relink-first leaves gallery='trip1' (from the moved RAW row) while demote-first
  leaves 'trip2' (from the JPEG). relink/demote COUNTS are identical in both orders —
  which is exactly why counting rows never caught this.
Task 5: the gap is NOT closed. Controller independently verified: with
  src/scan_engine.cpp:525-527 swapped, the full suite passes 607/607. The new test
  fails when swapped in its OWN body but is blind to the production site, because the
  brief scoped the task to tests/test_relink.cpp, which cannot reach run_reconcile.
  This is a plan defect (my scoping), not an implementer failure — it reported the
  negative result accurately rather than claiming success.
  Production ordering restored and rebuild verified after the experiment.
Task 5: fix round 1/5 dispatched — lift the file scope; add a run_reconcile integration
  test in tests/test_scan_engine.cpp (which already drives run_reconcile and has a
  ScratchCollection fixture) so a swap at :525-527 actually fails a test. Note
  ScanOptions.log_enabled selects the branch: true -> :525-527, false -> :547-549.
Task 5: minor (deferred): DatabaseConcurrency.ConcurrentMigrationIsSafe failed once
  then passed on repeat. Pre-existing flake, out of scope, not touched.
Task 6 option-3 viability (controller): CONFIRMED VIABLE and likely the smallest fix.
  spdlog's stdout_sink_base has `explicit stdout_sink_base(FILE *file)` and a `FILE*
  file_` member (spdlog 1.17.0, stdout_sinks.h:23,40), so a sink can be built on a
  private FILE*. Duplicating fd 2 once at log_init (fdopen(dup(STDERR_FILENO), "w"))
  makes phig's own warnings immune to the dup2 suppression, because the private
  descriptor still points at the ORIGINAL stderr target while fd 2 is redirected.
  libjpeg/GStreamer keep writing to fd 2 and stay suppressed. Contrast: the current
  stderr_sink_mt writes via FILE* stderr, which resolves fd 2 at write time — which is
  exactly why suppression catches phig's warnings as collateral.
  Caveat for the implementer to check: ordering/interleaving with std::cout progress
  lines (a different FILE* buffer) and whether the dup'd descriptor needs explicit
  flushing. Also confirm behavior when stderr is a pipe vs a tty.
Task 5: fix round 1/5 (1 addressed, 0 open; commits a882ec7..49ecf04) — added
  ScanEngine.ReconcileRunsRelinkBeforeDemoteOnSharedTarget driving the real
  run_reconcile. CONTROLLER VERIFIED THE GATE DIRECTLY: with production swapped the
  test fails with gallery "trip2" vs expected "trip1"; with production restored the
  suite is 608/608. The gap IS closed for the logged branch.
Task 5: re-reviewer returned a self-contradictory verdict (said NOT ADDRESSED while
  confirming the test catches the production swap, and contradicted itself twice about
  which paths leak). Controller adjudicated by reading the code: its ASSERT-early-return
  claim was BACKWARDS — restoration sits BEFORE the assertions, so ASSERT paths leak
  only the temp dir, not the env var. But its underlying point is real and worth fixing:
  run_reconcile throwing skips BOTH the XDG_STATE_HOME restoration and fs::remove_all,
  leaving that env var pointing at a deleted directory for every later test in the
  binary.
Task 5: fix round 2/5 dispatched — RAII cleanup for both escape paths, drop stale
  line-number citations from comments, and cover the UNLOGGED branch (log_enabled=false,
  the pair inside the else) which still has zero coverage.
Task 5: fix round 2/5 (3 addressed, 0 open; commits 49ecf04..6f5998e) — ReconcileOrderScratch
  RAII guard covers all exit paths; stale line-number comments dropped; unlogged branch
  now covered by a second test sharing a parameterized probe.
Task 5: complete (commits 56423a4..6f5998e, review clean)
  CONTROLLER VERIFIED BOTH GATES BY DIRECT EXPERIMENT:
  - logged pair swapped   -> ReconcileRunsRelinkBeforeDemoteOnSharedTarget FAILS
                             (gallery "trip2" vs expected "trip1")
  - unlogged pair swapped -> ...OnSharedTargetUnlogged FAILS, and the LOGGED test stays
                             green, proving the two tests discriminate their own
                             branches independently.
  Production restored both times; full suite 609/609; pgrep clean.
  Re-review skipped for round 2 per calibration: test-only diff whose central claim the
  controller verified by direct experiment.

Task 6: complete (commits 6f5998e..7d3c27c, verified by controller — no re-review needed)
  Option 3 implemented in src/log.cpp: log_init opens a private FILE* on dup(fd 2) and
  builds the warn/error sink on spdlog::sinks::stdout_sink_base against it, instead of
  the parameterless stderr_sink_mt which resolves fd 2 at write time. Falls back to
  plain stderr if dup fails. log_close closes it. hasher.cpp UNCHANGED.
  CONTROLLER-VERIFIED INDEPENDENTLY:
  - warnings at --parallel 8: 10/10 across 5 runs (was 8/10 and 9/10 before).
  - libjpeg "Premature end of JPEG file" still suppressed (truncated fixtures).
  - video/GStreamer path clean (only normal progress output).
  - stderr as a redirected FILE: 10/10. stderr as a PTY: 10/10. Both open questions
    from the plan resolved.
  - full suite 609/609.
  MUTATION-TESTED THE REWORKED TESTS: the fix required changing 4 pre-existing
  test_log.cpp tests off gtest's CaptureStderr (which dup2's fd 2 — the very thing the
  sink now deliberately survives) onto a real fd-2 redirect taken before log_init.
  Controller broke the sink pattern to "[BROKEN] %v" and confirmed all 4 FAIL, so they
  still genuinely read the sink. InfoReachesFileOnly correctly stayed green (it asserts
  stderr is EMPTY). Assertions and expected strings are unchanged from before.
  Per the human partner's pre-flight ruling, verified by measurement rather than a new
  race test — a cross-thread fd race needs test-only synchronization hooks in
  StderrSuppressor to be deterministic.

FINAL REVIEW (opus, whole branch b2ce936..7d3c27c): no Critical. 2 Important, both fixed.
  Deferred minors 1-8 triaged: ALL cleared to ship. Drain-loop triplication ruled
  CORRECT (cmd_scan uses std::optional<ScopedInterruptHandler> reset mid-run, calls
  run_workers not run, formats its own exception, handles p.error with scan-relative
  paths; a helper parameterized over engine/progress/render/guard would exceed the
  duplication). main.cpp judged genuinely simpler: ~12 lines of drain ceremony bought
  removal of ~50 lines of concurrency logic per command, now under test.
FINAL FIX WAVE (commit 1be2d86, one dispatch as the skill requires):
  1. IMPORTANT: FacesEngine checked images.empty() BEFORE cancel.stop(), so a cancelled
     run over an empty selection returned completed=true. Controller confirmed at
     faces_engine.cpp:35/37 (reviewer's line numbers 120/122 were wrong; the ordering
     claim was right). ThumbsEngine had the same defect via parallel_for_each(0,...)
     returning true unconditionally.
     Implementer's fix is subtler than what I asked for and better: it used
     `images.empty() ? !cancel.stop() : parallel_for_each(...)` rather than an
     unconditional `&& !cancel.stop()`, because the latter reintroduces the race
     parallel_for_each's own doc warns about — cancel flipping true right after the
     last item legitimately finishes. Verified both tests FAIL before / PASS after.
  2. IMPORTANT: removed orphaned #include <mutex> and "parallel.h" from main.cpp
     (controller verified both symbol counts are 0). Left scanner.h,
     <condition_variable>, <queue> alone — pre-existing dead code, project convention
     is to mention not delete.
  3. Minor: FacesStage::LoadingModels -> LoadingFaceModels, matching
     ScanStage::LoadingFaceModels for the identical event. Walked vs Selected left
     alone (genuinely different: walking a tree vs querying a DB).
  4. Minor: moved test_thumbs_engine.cpp's mid-file includes (line 80, stranded when
     Task 3 appended to Task 2's file) into the top block.
  Suite: 611/611 (609 + 2 new cancelled-empty tests).
FLAKE NOTE — recorded honestly: one full-suite run showed 1 failure out of 611. I could
  NOT identify which test: LastTest.log was overwritten by subsequent runs before I
  checked. Four later full runs were 611/611 clean, and
  DatabaseConcurrency.ConcurrentMigrationIsSafe passed 8/8 under --gtest_repeat.
  Consistent with the pre-existing flake two implementers independently reported, but
  NOT proven to be that test. Worth watching in CI.
FINAL FIX WAVE re-review: all 4 findings ADDRESSED, new breakage none. Independent
  reviewer built the empty/cancelled truth table and confirmed ThumbsEngine and
  FacesEngine are semantically consistent across all 6 combinations, including the
  "cancel races the last item" cell where both correctly report completed=true. It also
  independently confirmed the implementer's rejection of `parallel_for_each(...) &&
  !cancel.stop()` was correct, citing parallel_for_each's own doc comment.
  main.cpp's Selected/empty-check cooperation comment verified still accurate.
BRANCH COMPLETE — 10 commits b2ce936..1be2d86, 611/611 tests. Not pushed, not merged.

MERGED to master as 002b7ac (--no-ff, matching part 2b-i's convention). master is now
75 commits ahead of origin/master. NOT PUSHED.

FLAKE IDENTIFIED AND CLEARED — the earlier unattributed failure is
DatabaseConcurrency.ConcurrentMigrationIsSafe. Caught it by name after 5 full-suite
runs post-merge (~1 in 5 full runs fail). Proven pre-existing, not caused by this
branch: built a worktree at pre-merge b2ce936 and stress-ran the test there —
6 failures out of 60 repeats (10% rate) on code that predates every commit in this
branch. Unrelated to the extraction. Worth fixing separately; it will flake CI.

FLAKE FIXED 2026-08-04 (commit 73d1e1a) — supersedes the note above calling it merely
"pre-existing, worth fixing separately". It was a real bug, not test noise:
sqlite3_busy_timeout was called AFTER `PRAGMA journal_mode=WAL` in Database's ctor, so
those pragmas ran with SQLite's default ZERO timeout. journal_mode=WAL takes an
exclusive lock to rewrite the db header, so a connection opening while another held
that lock failed instantly with "database is locked" instead of waiting.
Diagnosis method: instrumented the test's `catch (...)` to print e.what() (it was
discarding the message) -> "SQL error: database is locked", then found busy_timeout at
database.cpp:195 sitting after the pragmas at 190-191.
Fix: move the busy_timeout call before the first statement. One line.
Measured: 4-6 failures/60 repeats before -> 0/100 after; full suite 611/611 x3.
Applies outside tests too: two phig processes opening one collection at once could fail
rather than wait out the lock.

CLEANUP: branch phig-core-part2b-ii deleted (with -d, which only succeeds if fully
merged). libphig-core and libphig-settings remain and are also fully merged into
master — left in place, not mine to delete without asking. No stale worktrees.
