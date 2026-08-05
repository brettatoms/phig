# SDD ledger — plan: /home/brett/devel/phig/docs/plans/2026-08-04-search-engine.md
Branch: search-engine (from master 73d1e1a)

Pre-flight decisions (human partner, before Task 1):
- Fix the face-match dedup bug DURING the extraction. --face ordering changes
  (correctly); every other mode stays byte-identical.
- Engine NEVER decodes pixels: caller passes face_embeddings / phash in.
  SearchEngine holds no OpenCV dependency.
Controllerverified before dispatch: hamming_distance in duplicates.h:12; FaceInfo in
faces.h:10 (embedding + x,y,width,height); flag is --count (main.cpp:1843); face mode
does NOT apply --match (branch returns at main.cpp:1070 before the selection block).

Task 1: complete (no commits — 19 baseline artifacts in /tmp/phig-search-baseline,
  verified by controller). 7 images, 10 faces. Person mode captured; the real argument
  form is `phig face name <image-path> <name>`.
Task 1: controller found a gap in the plan's verification strategy and fixed it:
  the plan's face baseline queried face_a.jpg, which has ONE face — so the dedup bug
  (which needs MULTIPLE query faces) could not show in that diff either way.
  Added face-2q-text.txt: `search --face coll/face_b.jpg`, which has 2 faces at
  184,76 24x32 and 165,7 16x20 (verified by SQL).
  This exposes the bug VISIBLY and in a way I had not predicted — not as wrong ordering
  but as a wrong FACE BOX. Old output reports all 4 matches with the SAME box
  (184,76 24x32) even though each matched image has two faces and both query faces
  match at distance 0. First-seen dedup keeps query face 1 and discards query face 2,
  so half the reported boxes belong to the wrong face. A caller drawing that rectangle
  draws it around the wrong face. Distances tie at 0, so ordering looks unaffected.
  Already covered by Task 2's PreservesTheFaceBoxOfTheClosestMatch test.
  Plan Task 5 Step 5 amended to diff BOTH face baselines: face-text.txt must stay
  byte-identical (1 query face = nothing to dedupe), face-2q-text.txt is where the fix
  shows.

Task 2: implemented (commit 4292211) — review clean: spec OK, quality approved, no
  Critical/Important. Controller independently applied the first-seen-wins mutation and
  confirmed it fails TWO tests: KeepsTheClosestMatchPerPathNotTheFirstSeen (out[0]
  becomes /p/other.jpg@0.5 instead of /p/both.jpg@0.3) AND
  PreservesTheFaceBoxOfTheClosestMatch (face_x 11 instead of 77 — exactly the
  real-world symptom found in the Task 1 baseline). Suite 615/615.
  Reviewer confirmed the path->index map is never invalidated (out only grows via
  push_back in the same loop; the sort runs after the loop) and that the header serves
  all four modes without a signature change.
Task 2: fix round 1/5 dispatched — reviewer rated the undefined tie order Minor "since
  exact float ties are rare". In THIS project they are not: the Task 1 baseline has a
  2-face query returning FOUR matches all at distance 0 (duplicate images), and Task 5
  diffs that output byte-for-byte. Controller verified the pre-change binary is stable
  across 4 runs (dup3 face_b dup2 dup1) but only INCIDENTALLY — sorting an
  already-tied sequence at this size leaves it alone, and the new dedup pass changes
  the sequence feeding the sort. Switching to stable_sort makes the guarantee explicit.
  Implementer told to report honestly if the new tie test cannot fail under plain sort.
Task 2: minor (deferred): EmptyInputYieldsEmptyOutput does not discriminate — kept as a
  cheap edge guard.
Task 2: fix round 1/5 (1 addressed; commit df65d82) — std::sort -> std::stable_sort with
  a comment on why. Implementer added PreservesInsertionOrderForTiedDistances and
  REPORTED HONESTLY that it passes under plain sort too, so it does not guard the
  behavior. Controller verified: 5/5 passes under std::sort. Honest negative report,
  exactly as asked.
Task 2: controller measured WHY, rather than accepting a non-failing test. libstdc++'s
  std::sort falls back to insertion sort below a threshold, which incidentally preserves
  order. Standalone experiment (/tmp/tie.cpp) found the cutoff:
    3 tied -> preserved; 8 tied -> preserved; 16 tied -> REORDERS; 20/32/64/100 -> REORDERS.
  The test used 2 tied entries, far below the cutoff. At >=16 it becomes a real guard.
Task 2: fix round 2/5 dispatched — rewrite the tie test with 20 tied entries (not
  exactly 16, to stay off the implementation-defined boundary) and require it to
  genuinely FAIL under plain sort. Comment must note the count is chosen to exceed the
  insertion-sort threshold, so a future reader shrinking the fixture cannot silently
  disarm it.
Task 2: fix round 2/5 (1 addressed, 0 open; commit 1a97c4e) — tie test rewritten with 20
  tied entries. CONTROLLER VERIFIED THE GATE: under plain std::sort the test now
  genuinely FAILS, with t09 where t00 is expected and the introsort partition pattern
  visible (t09, t19, t18, t17...). Restored stable_sort -> 5/5 dedup tests pass.
Task 2: complete (commits 73d1e1a..1a97c4e, review clean; suite 616/616)
  Re-review skipped for round 2 per calibration: test-only diff plus a one-word
  implementation change whose gate the controller verified by direct experiment.

Task 3: implemented (commit 68c8f47) — Filter and Similar faithful, limit interlock exact.
  Implementer caught a REAL flaw in the brief's own mutation gate: mutation 2 (delete the
  final resize) SURVIVED the brief's LimitTruncatesWhenNoSelectionIsPending, because
  Filter mode truncates via SQL LIMIT before the resize runs. It added
  SearchEngineSimilar.LimitTruncatesAfterDistanceSort (no SQL limit, so only the resize
  can satisfy it). Controller verified: with the resize deleted ONLY the new test fails;
  the brief's test passes. Warranted addition, good reasoning.
Task 3: CRITICAL finding, and it is MY PLAN'S DEFECT, not the implementer's — it built
  exactly what my spec said. My search_engine.h comment claimed "path selection and the
  final limit apply to every mode". FALSE. Verified by reading main.cpp: the shared
  selection/limit block is at :1130-1143, but Person RETURNS AT :984 and Face RETURNS AT
  :1070, so neither reaches it. `phig search --person alice --match X --limit 5` ignores
  both flags today; the implementation applied them = behavior change. Task 5 diffs
  person output against a baseline, so it would have surfaced there.
  Corrected in BOTH source documents (plan verified-facts + Task 3 code block, and the
  design spec's SearchQuery comment) so Tasks 4-5 do not inherit it.
Task 3: fix round 1/5 dispatched — drop selection+limit from Person, correct the header
  comment, and add a test with a KNOWN person that would fail if selection were applied
  (the existing UnknownPersonYieldsNoPaths cannot catch it — empty before the code runs).
Task 3: fix round 1/5 (1 Critical addressed, 0 open; commits 68c8f47..ff389fe) —
  Person mode now a plain get_images_for_person call; header comment corrected;
  SearchEnginePerson.IgnoresPathSelectionAndLimit added (3 labeled images, a match
  excluding one, limit below the count, so it fails if EITHER selection or limit runs).
  CONTROLLER VERIFIED THE GATE: reintroducing selection+limit makes it fail (1 path vs
  expected 3, both assertions firing).
Task 3: complete (commits 1a97c4e..ff389fe, review clean; suite 626/626)
  Re-reviewer's only open question was whether the new test's zero-initialized FaceInfo
  (all-zero embedding, zero box) could interfere with Task 4's face-search tests in the
  same binary — it based its "no" on those tests not existing yet, which Task 4 changes.
  Controller checked the structural answer instead: ScratchSearch creates a per-instance
  database keyed on `this`, so those rows cannot leak into any other test's DB. Safe by
  construction, not by absence.

Task 4: implementer ESCALATED rather than fudging — the Step 5 gate did not fire. Correct
  call, and its diagnosis was right: MY TEST DESIGN was the defect (third plan defect
  this branch). With a single-image fixture, query face 1's batch already contains BOTH
  of that image's faces sorted ascending, so it holds the true minimum and there is no
  cross-batch information for first-seen-wins to lose. The test could not fail either way.
  Reported actual distances: exact flat embeddings -> 0.0; flat(0.90) vs flat(0.10) -> 9.05.
  per_face_limit confirmed identical to the original.
Task 4: CONTROLLER BUILT AND MEASURED A REPRODUCING FIXTURE before instructing anything.
  The missing ingredient is a DECOY image plus a small per_face_limit, so query 1's batch
  shows the target only via its WORSE face while the exact match arrives in query 2's.
  Shape: target.jpg with faces flat(0.50)+flat(0.10); decoy.jpg with flat(0.90);
  queries [flat(0.90), flat(0.10)]; limit=2 (LOAD-BEARING — a larger k pulls the target's
  exact face into batch 1 and the bug hides again).
  Measured, same code, only the dedup rule changed:
    closest-wins (correct): target.jpg dist=0.000000
    first-seen-wins (bug):  target.jpg dist=4.525482
  Handed the verified fixture to the implementer for round 1.
Task 4: implemented (commit de6a54e). Implementer's measured 4.5254817 matched the
  controller's independently-derived 4.525482 to 6 decimals.
Task 4: full-depth review: spec OK, quality approved, no Critical. Merge confirmed a
  faithful move of main.cpp:1002-1024; per_face_limit identical to the original; the
  make_move_iterator concatenation clean (batch is a fresh local, all is distinct, dedup
  stores indices not pointers, no use-after-move); face mode correctly applies NO path
  selection, so Task 3's Critical was not repeated; no stray fprintf probes left.
  CONTROLLER VERIFIED THE GATE: reintroducing first-seen-wins fails
  CrossBatchConflictKeepsTheClosestMatch (target.jpg 4.525482 vs expected ~0.0).
Task 4: 2 Important findings, both fixable and neither a shipped-behavior defect:
  (1) Comments on dedupe_face_matches and the Face case promise "keeps the SMALLEST
      distance", but each search_faces call is a KNN truncated to k=per_face_limit BEFORE
      the merge, so dedup only picks the minimum among SURVIVING rows. Concrete: limit=2,
      A.jpg's better 0.05 face sits at rank 3 in batch 2 and is truncated, so A is
      reported at 0.1. Matches the ORIGINAL's behavior (it truncates identically), so not
      a regression — but the comment must not overpromise.
  (2) AppliesTheLimitAfterDeduplication does NOT test its name. Controller confirmed:
      4 images with ONE face each and a SINGLE query embedding means no path repeats, so
      dedup is a no-op and limit-before-dedup would pass identically. Another
      non-discriminating test inherited from MY brief (the third such).
Task 4: fix round 2/5 dispatched — qualify both comments; rewrite the limit test with one
  image having TWO matching faces plus one other image so limit-first yields 1 result and
  dedup-first yields 2; verify by moving the resize before dedup.
Task 4: fix round 2/5 (2 Important addressed, 0 open; commit e11cd56) — both comments now
  say "the SMALLEST distance AMONG THE ROWS EACH PER-FACE QUERY RETURNED", and the limit
  test was rewritten.
  The implementer went FURTHER than the controller's suggested fixture and was right to:
  my proposal (single query embedding) ALSO could not discriminate, because with one query
  per_face_limit == query_.limit, so the DB-level k cap already bounds the batch to the
  final result size and pre/post-dedup resize are identical. It used TWO query embeddings
  so the independent per-batch caps can sum above the limit. That is the fourth
  non-discriminating test shape traced to my specs, and the second the implementer caught
  independently.
  CONTROLLER VERIFIED THE GATE: moving the resize before dedup makes the test FAIL
  (1 result vs expected 2).
Task 4: complete (commits ff389fe..e11cd56, review clean; suite 631/631)
  Re-review skipped for round 2 per calibration: comment-and-test-only diff whose gate the
  controller verified by direct experiment.

Task 5: complete (commits e11cd56..9d45b6d, review clean; suite 631/631)
  cmd_search 298 -> 250 lines; main.cpp 2410 -> 2363.
  Implementer reported 4 baseline comparisons as DIFFERS, attributing all to pre-existing
  scan-order nondeterminism. CONTROLLER TESTED THAT CLAIM DIRECTLY rather than trusting it:
  ran the OLD binary against two independently-scanned copies of identical inputs and got
  two different orderings (dup3 face_b dup2 dup1 vs face_b dup3 dup1 dup2). Claim holds —
  the nondeterminism predates this change.
  CONTROLLER THEN RAN A STRONGER CHECK the brief did not specify: old vs new binary against
  ONE FIXED DATABASE, removing scan nondeterminism entirely. ALL 14 mode/format
  combinations BYTE-IDENTICAL: text, csv, json, porcelain, print0 (cmp), media, limit,
  limit+match, count, similar-hash, similar-file, person, face 1-query-face, face
  2-query-faces. Path sets matched on both DBs too.
  Full-depth review then covered the half measurement cannot: enumerated all 29
  SearchOptions fields and confirmed NONE dropped or mistranslated (the named risk was a
  silently dropped min_sharpness, which byte-comparison could never catch because the
  fixture never exercised that flag). Mode precedence person->face->similar->filter
  preserved; --count reads the right vector per mode; no orphaned includes; scope clean.
Task 5: minor (deferred): rendering re-derives the mode from is_person_search/
  is_face_search rather than switching on query.mode — two sources of truth, cannot
  diverge today, but a future mode addition must touch both.
Task 5: minor (deferred): the CLI->SearchQuery field copy (~20 lines) is the one untested
  surface; closed by enumeration, not by a test. SearchOptions is file-local to main.cpp,
  so testing it needs a larger change than this task should make.
Task 5: minor (deferred, pre-existing): the Person block's output-file error message omits
  << opts.output while Face and Filter include it. Present in the original; left alone.

FINAL REVIEW (opus, whole branch 73d1e1a..9d45b6d): no Critical. 1 Important, fixed.
  Cross-engine consistency confirmed clean across all four engine headers: same ctor
  shape, same options-by-value rationale comment, same "free function not a private
  method" testability idiom. Each deliberate difference is documented where a reader
  would ask (no-cancel/no-progress and why; the no-OpenCV boundary). SearchQuery vs
  *Options naming break judged defensible (it IS a query), and run() taking no arguments
  is the visible signal that no cancel/progress exists. SearchResult's three-vector shape
  judged the right call — a variant would force a rendering-side rewrite on a branch whose
  premise is byte-identical output.
  Bug fix judged COMPLETE and its qualification ACCURATE, not overstated: search_faces
  binds k=limit and caps each batch BEFORE the dist>threshold filter, and the comments say
  exactly that, explicitly disclaiming the recovery they cannot make.
  main.cpp judged genuinely simpler: four query mechanisms plus the SQL-limit interlock all
  left the file; what replaced them is a flat field copy.
  All 6 deferred minors triaged as SHIP.
FINAL FIX WAVE (commit 8918308, test-file only):
  IMPORTANT — a FIFTH non-discriminating test, same failure mode as the four earlier ones.
  The reviewer deleted the Similar-mode std::sort entirely and ALL THREE SearchEngineSimilar
  tests still passed, including one whose own assertion message says "closest must sort
  first". Cause: get_all_images has no ORDER BY, so rows return in insertion order, and the
  fixtures inserted in already-ascending distance order — insertion order coincidentally
  equalled sorted order.
  CONTROLLER CONFIRMED BOTH DIRECTIONS: deleted the sort and watched all 3 pass (before);
  after the fix, deleted it again and watched the 2 ordering tests FAIL while
  ThresholdIsInclusive correctly still passed (single row, no ordering claim).
  Fix: reversed fixture insertion order (worst first, exact last) plus "do not tidy into
  ascending order" comments, since this failure mode hit five times on this branch.
BRANCH COMPLETE — 9 commits 73d1e1a..8918308, 631/631 tests. Not pushed, not merged.

MERGED to master as 20c7ad4 (--no-ff, matching the convention of the two prior merges).
master is now 86 commits ahead of origin/master. NOT PUSHED. Branch search-engine deleted
with -d (succeeds only if fully merged). Suite verified 631/631 on master post-merge.
Workspace CLAUDE.md "Current state" updated — it still described Part 1 as unmerged and
Part 2 as not started, which was three merges out of date.
