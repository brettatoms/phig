# RAW Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Treat a RAW + same-basename JPEG as one photo — the RAW is the only indexed row, the JPEG is a carried attachment and the RAW row's pixel source — with RAW pixels decoded via a LibRaw-backed 3-tier module instead of OpenCV's flaky `cv::imread`.

**Architecture:** A new `src/raw.{h,cpp}` (phig-lib) resolves RAW pixels (sidecar JPEG → LibRaw embedded preview → LibRaw full decode) and is called from the single decode chokepoint `decode_image_with_warning` (hasher.cpp), so phash/faces/thumbs/blur all get RAW support. Scan skips indexing a JPEG when a same-basename RAW is on disk (disk-based, order-independent). The transition (JPEG already indexed, RAW appears) transfers metadata by basename at scan cleanup, reusing plan 023's `transfer_user_metadata`. mv/cp carry the attached JPEG via an attachments pass alongside 025's `carry_sidecars`. LibRaw joins the system-package dependencies.

**Tech Stack:** C++20, LibRaw (`libraw_r`, thread-safe), OpenCV (cv::Mat), CMake, GoogleTest.

## Global Constraints

- **Language/std:** C++20; `-Wall -Wextra` clean, CI uses `-DPHIG_WERROR=ON` — no warnings.
- **Build/test:** `direnv exec . cmake --build build` then `./build/phig-tests`. A bare shell has a glibc/include mismatch — the `direnv exec .` prefix is required. After editing `CMakeLists.txt`/`devenv.nix`, run `direnv exec . cmake -B build` to reconfigure (the Nix env rebuilds on the next `direnv exec .` after a `devenv.nix` change — slow first time, automatic).
- **Formatting:** clang-format Google/4-space/100-col on new/changed `src/` and `tests/` files (NOT markdown — clang-format corrupts prose).
- **Commits:** plain messages, no AI attribution. Commit directly on `master` (per user).
- **DB/state safety:** any test that invokes the `./build/phig` binary MUST use a temp `--db` under a tempdir and (if it triggers logging) a temp `XDG_STATE_HOME`. Never touch the real `~/.local/share/phig/phig.db`.
- **LibRaw facts (verified):** `pkgs.libraw` is in the pinned nixpkgs with a separate `dev` output; system ships both `libraw.pc` and `libraw_r.pc`; `libraw.h` C++ API present. Link the thread-safe `libraw_r` (scan is parallel).
- **`phig undo` does not exist yet** — sidecar/attachment log entries are write-only; no reverse-replay to implement.
- Every task ends with the FULL suite green.

## File Structure

- `src/raw.h` / `src/raw.cpp` — NEW (phig-lib). `is_raw_extension`, `raw_decode`.
- `src/scanner.h` / `src/scanner.cpp` — add `is_raw_extension` (or expose the RAW set).
- `src/hasher.cpp` — route RAW through `raw_decode` in `decode_image_with_warning` (~line 198).
- `src/faces.cpp` — convert the standalone `cv::imread` (~line 109) to `decode_image` for RAW consistency.
- `src/main.cpp` — scan JPEG-demotion (worker body ~427-435); pass the RAW-pairing info to cleanup.
- `src/relink.{h,cpp}` — basename-triggered RAW↔JPEG metadata transfer at cleanup.
- `src/organize_exec.{h,cpp}` — attachments pass: carry the same-basename JPEG on mv/cp.
- `CMakeLists.txt`, `devenv.nix`, `.github/workflows/ci.yml`, `README.md` — LibRaw dependency.
- `tests/test_raw.cpp` (new), `tests/test_scanner.cpp`, `tests/test_relink.cpp`, `tests/test_organize_exec.cpp`, `tests/test_porcelain.cpp` — tests.
- `AGENTS.md`, `README.md` — document RAW handling.

## Fixture note (read before Tasks 2/8)

`tests/data/truncated_ojpeg.cr2` already exists — a deliberately **truncated,
undecodable** Canon CR2 (used by `test_hasher.cpp`'s `kBadCr2` to assert OpenCV
returns an empty Mat + "Internal imread" warning). It is good for the
"OpenCV-fails, RAW path is attempted, graceful failure" case, but it CANNOT test
the happy tiers (embedded preview / full decode) because it's truncated. For
tier-1 (sidecar JPEG present) tests, generate a `good.jpg` beside a `.cr2` with
the existing `create_image` helper pattern (`cv::Mat img(...); cv::imwrite(...)`)
— tier 1 never touches LibRaw. For tier-2/tier-3 happy paths, Task 2 must either
add a small VALID CR2 fixture to `tests/data/` or, if none can be obtained,
test tiers 2/3 against the truncated fixture asserting graceful empty-Mat return
and cover the LibRaw-success path only if a valid fixture is added — decide in
Task 2 and record it. `simple_dcraw`/`dcraw_emu` (LibRaw CLI) and `identify` are
available on the dev box to inspect/produce a fixture.

---

### Task 1: LibRaw dependency (build env, CMake, CI, docs)

**Files:** `devenv.nix`, `CMakeLists.txt`, `.github/workflows/ci.yml`, `README.md`

**Interfaces produced:** `libraw_r` linked into `phig-lib` — Task 2's `raw.cpp` can `#include <libraw/libraw.h>`.

- [ ] **Step 1: devenv.nix** — add `pkgs.libraw` to the `packages` list (devenv.nix:4-16, after `pkgs.spdlog`) AND `pkgs.libraw.dev` to the `CMAKE_PREFIX_PATH` list (devenv.nix:18-24, after `pkgs.spdlog.dev`). (libraw has a `dev` output like spdlog.)

- [ ] **Step 2: CMakeLists.txt** — after `find_package(spdlog REQUIRED)` (~line 28), add LibRaw discovery. `find_package(PkgConfig REQUIRED)` is already present (line 24); libexif/ffmpeg use the **variable form** `pkg_check_modules(EXIF REQUIRED libexif)` then link `${EXIF_LIBRARIES}` with `${EXIF_INCLUDE_DIRS}`/`${EXIF_LIBRARY_DIRS}` in the include/link-dir blocks. **Match that existing style** for consistency, not `IMPORTED_TARGET`:
```cmake
pkg_check_modules(LIBRAW_R REQUIRED libraw_r)
```
Then wire `${LIBRAW_R_INCLUDE_DIRS}` into the `target_include_directories` block (~66-75), `${LIBRAW_R_LIBRARY_DIRS}` into the link-dirs block (~93-99), and `${LIBRAW_R_LIBRARIES}` into `target_link_libraries(phig-lib PUBLIC ...)` (~77-89, after spdlog) — exactly mirroring how `EXIF`/`AVFORMAT` are threaded through those three blocks.

- [ ] **Step 3: Verify the env builds** — `direnv exec . cmake -B build && direnv exec . cmake --build build && ./build/phig-tests`. Expected: configure finds libraw_r, build clean, full suite green (329 baseline). If find fails, re-enter devenv so the nix env picks up libraw; if still failing, STOP and report what pkg-config/cmake printed. Report whether you used `pkgs.libraw` or `pkgs.libraw.dev` in CMAKE_PREFIX_PATH.

- [ ] **Step 4: CI** — `.github/workflows/ci.yml`: add `libraw-dev` to BOTH Ubuntu apt lists (the `libspdlog-dev` lines) and `libraw` to the macOS brew line (after `spdlog`).

- [ ] **Step 5: README** — add LibRaw to the per-distro deps intro + commands: Debian/Ubuntu `libraw-dev`, Fedora `LibRaw-devel`, Arch `libraw`, macOS `libraw`.

- [ ] **Step 6: Commit** — `git add devenv.nix CMakeLists.txt .github/workflows/ci.yml README.md && git commit -m "Add LibRaw dependency"`

---

### Task 2: `src/raw.{h,cpp}` — `is_raw_extension` + `raw_decode` (3-tier pixel source)

**Files:** Create `src/raw.h`, `src/raw.cpp`; modify `CMakeLists.txt` (add `src/raw.cpp` to phig-lib ~47-64, `tests/test_raw.cpp` to phig-tests ~113-130); create `tests/test_raw.cpp`.

**Interfaces produced (consumed by Tasks 3-6):**
```cpp
// True if `ext` (with or without dot, any case) is a RAW extension
// (cr2, cr3, nef, dng, arw, ...). The set mirrors scanner.cpp's RAW group.
bool is_raw_extension(const std::string& ext);

// Decode a RAW file to a BGR cv::Mat for hashing/thumbnail/faces, resolving:
//   1. same-basename sidecar JPEG on disk (IMG_1.JPG next to IMG_1.CR2);
//   2. the RAW's embedded JPEG preview (LibRaw unpack_thumb, largest JPEG);
//   3. LibRaw full decode.
// Returns an empty Mat if all tiers fail. Thread-safe (uses libraw_r, a fresh
// LibRaw instance per call — no shared state). Never throws.
cv::Mat raw_decode(const std::string& raw_path);
```

- [ ] **Step 1: Decide the RAW set + `is_raw_extension` home.** `is_raw_extension` logically belongs with the extension helpers in `scanner.{h,cpp}` (next to `is_known_image_extension`). Add it there — carve the RAW group (scanner.cpp:20-56) into its own static set and expose `bool is_raw_extension(const std::string&)` in scanner.h. `raw.h` re-declares nothing extension-related; `raw.cpp` includes `scanner.h` if it needs the check. (Keeping the RAW set single-sourced in scanner.cpp avoids drift.) Write a scanner test: `is_raw_extension("cr2")==true`, `("CR2")==true`, `("jpg")==false`, `(".nef")==true`.

- [ ] **Step 2: Fixture decision (record in report).** Per the fixture note: tier-1 tests generate a `good.jpg` beside a `.cr2`. For tier-2/3, inspect `tests/data/truncated_ojpeg.cr2` with `identify`/`dcraw_emu`; if it yields no usable preview/decode (expected — it's truncated), add a small valid CR2 to `tests/data/` if you can produce one with `dcraw_emu`/a real camera file, ELSE test tiers 2/3 as "graceful empty-Mat on the truncated fixture" and note the happy-path gap. Do not fabricate a fake CR2.

- [ ] **Step 3: Write failing tests in `tests/test_raw.cpp`:**
```cpp
// Tier 1: a real sidecar JPEG beside the RAW is used directly (no LibRaw).
TEST(RawDecode, UsesSidecarJpegWhenPresent) {
    // tmpdir: write good.jpg (cv::imwrite of a known Mat) as IMG_1.JPG,
    // and copy tests/data/truncated_ojpeg.cr2 to IMG_1.CR2 (even truncated —
    // tier 1 must NOT touch the CR2). raw_decode(IMG_1.CR2) returns a non-empty
    // Mat whose size matches the good.jpg (proving it used the sidecar).
}
// No sidecar, undecodable RAW -> empty Mat, no crash (graceful).
TEST(RawDecode, TruncatedRawWithNoSidecarReturnsEmpty) {
    // raw_decode(tests/data/truncated_ojpeg.cr2) with no sidecar -> img.empty(),
    // no throw.
}
// (If a valid CR2 fixture was added: UsesEmbeddedPreview / FullDecodeFallback.)
```

- [ ] **Step 4: Implement `raw.cpp`.** Tier 1: derive the same-basename JPEG (`.jpg`/`.jpeg`/`.JPG`, case-insensitive) beside the RAW; if it exists, `cv::imread` it and return if non-empty. Tier 2: `LibRaw ip; ip.open_file(path); ip.unpack_thumb();` → if `ip.imgdata.thumbnail.tformat == LIBRAW_THUMBNAIL_JPEG`, `cv::imdecode` the thumbnail buffer; return if non-empty. Tier 3: `ip.unpack(); ip.dcraw_process(); libraw_processed_image_t* im = ip.dcraw_make_mem_image();` → wrap into a cv::Mat (RGB→BGR), copy, `ip.dcraw_clear_mem(im)`; return. Guard every LibRaw call by return code; on any failure fall through to the next tier; return empty Mat if all fail. Use a fresh `LibRaw` per call (thread-safe with `libraw_r`). Never throw (catch and return empty).

- [ ] **Step 5: Build + test** — `direnv exec . cmake -B build && direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='RawDecode.*:*RawExtension*'`. Expected: PASS.

- [ ] **Step 6: Commit** — `src/raw.h src/raw.cpp src/scanner.h src/scanner.cpp tests/test_raw.cpp tests/test_scanner.cpp CMakeLists.txt`, message "Add LibRaw-backed RAW pixel-source module (sidecar JPEG / preview / decode)".

---

### Task 3: Route RAW through the decode chokepoint

**Files:** `src/hasher.cpp` (`decode_image_with_warning` ~line 198), `src/faces.cpp` (~line 109), `tests/test_hasher.cpp`.

**Interfaces:** consumes `is_raw_extension`, `raw_decode` (Task 2). No new public symbol.

- [ ] **Step 1: Write a failing test** in `tests/test_hasher.cpp`: a `.cr2` with a generated same-basename sidecar JPEG → `decode_image_with_warning(cr2).img` is non-empty (routed through raw_decode → tier 1). (Today it goes to `cv::imread(cr2)` and would be empty for the truncated fixture; a sidecar makes tier 1 succeed.) Keep the existing `DecodeWarningReportsInternalIssueForBadCr2` test meaning in view — a truncated CR2 with NO sidecar should still yield an empty Mat (now via raw_decode's graceful failure); update that test's expectation if the warning text changes (it may no longer say "Internal imread" since OpenCV isn't the decoder for RAW anymore — decide the new warning text and assert it).

- [ ] **Step 2: Implement in `hasher.cpp`** at the `cv::imread` site (~198). Branch:
```cpp
    if (is_raw_extension(fs::path(path).extension().string())) {
        result.img = raw_decode(path);
        if (result.img.empty())
            result.warning = "Cannot decode RAW file: " + path;
    } else {
        // existing cv::imread(...) path, unchanged
    }
```
Add `#include "raw.h"` and `#include "scanner.h"` as needed. Keep the `StderrSuppressor` for the OpenCV branch.

- [ ] **Step 3: faces.cpp consistency** — replace the standalone `cv::imread(image_path)` (~109) with `decode_image(image_path)` so the path-based face API also handles RAW. Add `#include "hasher.h"`.

- [ ] **Step 4: Build + full suite** — `direnv exec . cmake --build build && ./build/phig-tests`. Expected: green (the truncated-CR2 test now asserts the new graceful-failure warning). Confirm the concurrent decode test (`test_hasher.cpp` threads) still passes.

- [ ] **Step 5: Commit** — "Route RAW files through the LibRaw pixel source in decode_image".

---

### Task 4: Scan JPEG-demotion (disk-based, order-independent)

**Files:** `src/main.cpp` (worker body ~427-435), `tests/test_porcelain.cpp` or a new scan-level test.

**Interfaces:** consumes `is_raw_extension`. Produces: a JPEG with a same-basename RAW on disk is not indexed.

- [ ] **Step 1: Add a helper** (file-local in main.cpp or a small exposed function): given a media path, return true if it's a JPEG (`jpg`/`jpeg`, case-insensitive) AND a same-basename RAW file exists on disk in the same directory (scan the dir for a regular file with the same stem whose extension `is_raw_extension`). Case-insensitive stem match; first RAW wins.

- [ ] **Step 2: Insert the skip** in the worker body right after canonicalization (~main.cpp:427-435, before the mtime/skip logic at ~440):
```cpp
        if (jpeg_demoted_by_raw(filepath)) {
            skipped.fetch_add(1);   // or a dedicated demoted counter
            continue;
        }
```
This runs per-worker; the disk check is order-independent and parallel-safe (disk doesn't change mid-scan). Decide whether to count it as `skipped` or add a `demoted` tally for the summary.

- [ ] **Step 3: Test.** The clean way to test this is at the binary level, but no binary-invoking harness exists. Instead: extract `jpeg_demoted_by_raw` (or the sibling-RAW check) into a testable helper — put it in `scanner.{h,cpp}` as `bool has_sibling_raw(const std::filesystem::path& jpeg_path)` (phig-lib, unit-testable), and have main.cpp call it. Test in `tests/test_scanner.cpp`: dir with IMG_1.JPG + IMG_1.CR2 → `has_sibling_raw(IMG_1.JPG)` true; lone IMG_2.JPG → false; IMG_1.CR2 itself → false (not a jpeg); case-insensitive stem.

- [ ] **Step 4: Build + full suite green. Commit** — "Skip indexing a JPEG when a same-basename RAW is on disk".

---

### Task 5: Transition transfer — basename-triggered metadata rescue at cleanup

**Files:** `src/relink.{h,cpp}`, `src/database.{h,cpp}` (maybe a basename-lookup primitive), `src/main.cpp` (cleanup block ~614-658), `tests/test_relink.cpp`.

**Context:** When a JPEG was indexed alone, then a RAW appears and demotes it (Task 4), the JPEG is no longer indexed → at cleanup its file still exists but it's a "removed" candidate ONLY if `paths_safe_to_delete` flags it. But the JPEG file still exists on disk, so it will NOT be in `removed_paths` (that set is files GONE from disk). So the JPEG's stale row would persist wrongly. This task must detect "an indexed JPEG that is now demoted (a same-basename RAW exists on disk, and the RAW is/should be indexed)" and transfer its metadata to the RAW row, then delete the JPEG row.

- [ ] **Step 1: Decide the mechanism (resolve design open-question 6).** Recommended: during cleanup, in addition to `removed_paths`, compute **demoted JPEG rows** — DB rows whose path is a JPEG that now has a same-basename RAW on disk (query `get_paths_with_prefix(scan_prefix)`, filter to JPEGs whose sibling RAW exists on disk). For each, find the RAW's row id (the RAW was indexed this scan), `transfer_user_metadata(jpeg_id, raw_id)`, log a `relink` entry, and add the JPEG path to the delete set. This reuses 023's primitive with a basename trigger. Write this as a new function (e.g. `int demote_jpeg_rows(Database& db, const std::string& scan_prefix, int64_t op_id)` in relink.{h,cpp}) so it's unit-testable, called from the cleanup block alongside `relink_moved_rows`.

- [ ] **Step 2: Failing tests in `tests/test_relink.cpp`:**
```cpp
// A JPEG row with tags/faces, a same-basename RAW row exists, RAW file on disk ->
// demote_jpeg_rows transfers metadata to the RAW row and returns the jpeg path
// for deletion; logs a relink entry.
TEST_F(RelinkTest, DemotesJpegToSameBasenameRaw) { ... }
// Lone JPEG (no RAW on disk) -> not demoted.
TEST_F(RelinkTest, LoneJpegNotDemoted) { ... }
// JPEG + RAW both rows but RAW file gone from disk -> not demoted (nothing to demote to).
```
(Use the RelinkTest fixture's `add_row(rel, sha, on_disk)` helper; create the RAW file on disk for the demote case.)

- [ ] **Step 3: Implement `demote_jpeg_rows`**; wire it into the cleanup block (main.cpp ~637-657) so its returned paths are appended to what `delete_paths_logged`/`delete_paths` remove, within the same scan operation. Order: run demote BEFORE the delete (so the transfer happens before the row is dropped), same as `relink_moved_rows`.

- [ ] **Step 4: Build + full suite green. Commit** — "Transfer metadata from a demoted JPEG row to its RAW at scan cleanup".

---

### Task 6: Carry the attached JPEG on mv/cp (attachments pass)

**Files:** `src/organize_exec.{h,cpp}` (`carry_sidecars` ~109-153), `tests/test_organize_exec.cpp`.

**Context:** `carry_sidecars` iterates `find_sidecars`, which matches only the metadata-sidecar allowlist — NOT `.jpg`. So a RAW's attached JPEG isn't carried. Add an attachments pass.

- [ ] **Step 1: Failing test** in `tests/test_organize_exec.cpp`: moving `IMG_1.CR2` (with a same-basename `IMG_1.JPG` beside it) carries the JPEG to the dest beside the moved CR2, renamed to match a renamed parent, logged as `move_sidecar` with the parent's id. And the copy variant leaves the source JPEG.

- [ ] **Step 2: Implement.** In `carry_sidecars` (or a sibling `carry_attachments` called right after it from `cmd_organize`), when the parent is a RAW (`is_raw_extension(parent ext)`), also find a same-basename JPEG beside the source (`is_known_image_extension` && jpg/jpeg && stem == parent stem) and carry it exactly like a sidecar: derive dest name from the parent's final name (stem style — the JPEG's stem matches the parent stem, so `<dest-stem>.jpg`/`.JPG` preserving the original case/ext), move/copy, log `move_sidecar`/`copy_sidecar` with `{"parent_image_id":N}`. Do NOT add jpg to the sidecar allowlist. Skip if the "JPEG" is itself an indexed lone photo — but in the RAW-master model a same-basename JPEG beside a RAW is by definition the attachment, so no extra guard needed beyond "parent is RAW".

- [ ] **Step 3: Build + full suite green. Commit** — "Carry a RAW's attached JPEG on mv/cp".

---

### Task 7: Amend plan 025's stale RAW stance in code comments (if any) + duplicates sanity

**Files:** grep for any code comment asserting "RAW and JPEG are peers/separate parents"; `tests/test_organize.cpp` or duplicates test if a RAW-pair dedup assertion is warranted.

- [ ] **Step 1:** `rg -n 'two separate parents|peer|RAW.*JPEG.*independent' src/` — update any stale comment to the RAW-master model. (The sidecar.h comment already points RAW↔JPEG to plan 026; confirm no contradicting comment remains.)

- [ ] **Step 2: Duplicates sanity (no code, a verification note in the report).** Confirm by reading that duplicates needs no change: only RAWs + lone JPEGs have rows, so a RAW+JPEG pair can't self-cluster. If a quick test is cheap (two rows can't be created for a pair), skip — this is inspection-only per the design.

- [ ] **Step 3: Commit** if any comment changed — "Align code comments with the RAW-master model".

---

### Task 8: End-to-end verification + docs

**Files:** `AGENTS.md`, `README.md`; a binary-level e2e check (manual, in the report — no new harness).

- [ ] **Step 1: Manual end-to-end with the real binary (temp DB + temp XDG_STATE_HOME).** Build a temp dir with `IMG_1.CR2` (copy the truncated fixture is fine for the scan/organize path — decoding may warn, that's OK) + a generated `IMG_1.JPG` sidecar + an `IMG_1.xmp`. Run: `XDG_STATE_HOME=$T/state ./build/phig scan $T/src --db $T/t.db` → assert exactly ONE row (the CR2), no row for the JPEG (`sqlite3 $T/t.db "SELECT path FROM images"`). Then `./build/phig mv $T/dest --match "$T/src/**" --db $T/t.db` → assert the CR2 row repointed to dest AND `IMG_1.JPG` + `IMG_1.xmp` both landed beside it at the dest. Clean up. Paste output in the report.

- [ ] **Step 2: Docs.** `AGENTS.md` Key Design Decisions — add a RAW bullet (RAW is the indexed shot; same-basename JPEG is an attachment used as pixel source and carried on mv/cp; pixels via LibRaw 3-tier; scan demotes the JPEG, transferring its metadata to the RAW). `README.md` — a short note in scanning/organize + LibRaw already in deps (Task 1). Do NOT run clang-format on the markdown.

- [ ] **Step 3: Full suite + lint** — `direnv exec . cmake --build build && ./build/phig-tests`; `clang-format --dry-run --Werror` on all touched C++ files. Commit — "Document RAW support".

---

## Self-Review Notes

- **Spec coverage:** LibRaw dep → T1; pixel-source 3-tier → T2; decode routing (phash/faces/thumbs/blur) → T3; scan demotion (disk-based) → T4; transition transfer (basename, reuse 023) → T5; carry attached JPEG → T6; stale-comment cleanup + duplicates note → T7; e2e + docs → T8. Open questions 1-8 from the design: RAW set (T2), preview extraction (T2), sibling lookup cost (T4 — per-file disk scan; acceptable, note if a per-dir index is warranted), basename matching (T4/T5 case-insensitive), faces from pixel source (T3 — decode chokepoint feeds faces), transition hook (T5 = mechanism a), carry JPEG (T6), --media/demotion (T4 — demote on disk presence).
- **Type consistency:** `is_raw_extension(const std::string&)->bool`, `raw_decode(const std::string&)->cv::Mat`, `has_sibling_raw(const fs::path&)->bool`, `demote_jpeg_rows(Database&,const std::string&,int64_t)->int`, reusing `transfer_user_metadata(int64_t,int64_t)` and `carry_sidecars(...)`'s logging shape.
- **Risks flagged:** the tier-2/3 happy-path test depends on obtaining a valid CR2 fixture (T2 decision); the transition (T5) is the subtlest — a demoted JPEG's file still exists so it's NOT in `removed_paths`, requiring the separate `demote_jpeg_rows` pass, not a reuse of the vanished-file path.
- **Not in scope:** JPEG as searchable/taggable; storing the pairing in schema; RAW editing; changing lone-JPEG behavior.
