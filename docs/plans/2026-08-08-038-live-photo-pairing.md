# Live Photo Pairing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Live Photo becomes one row, not two — the still is indexed, its MOV
is a carried attachment that travels with it on `mv`/`cp` and never appears as
an independent video.

**Architecture:** A `core/live_photo` module reads the Content Identifier from a
MOV via the libavformat path `core/metadata.cpp` already opens, and pairs a
still whose Apple MakerNote **contains** that exact UUID. Pairing then plugs
into the two places plan 026 already established for RAW+JPEG: scan skips the
attachment, and organize carries it.

**Tech Stack:** C++20, libavformat, libexif, libheif (for HEIC EXIF via 048's
`codec_exif_blob`), CMake, GoogleTest.

## Global Constraints

- Owning plan: `plans/038-live-photos.md`. Record deviations there, not here.
- **Identifier only — never guess.** No basename fallback, no heuristics. An
  unpaired still and MOV stay independent rows, which is today's behaviour, so
  the failure mode is "no improvement" and never "corrupted".
- **Refuse ambiguity.** If two stills contain one MOV's identifier, pair
  neither and leave the MOV independent.
- **The still is master.** It holds the row, EXIF, phash, thumbnail and faces.
- **No new dependency.** exiv2 would read tag 0x0011 properly but is GPL while
  phig links only LGPL — see plan 054. Do not add it here.
- **No parsing of untrusted binary structure.** The still side is a bounded
  containment scan, not an IFD walk. Same objection that ruled out a hand-rolled
  ISOBMFF parser in 048.
- Personal photos must never become committed fixtures. Real-file tests read
  `~/phig-live-samples` and **skip** when it is absent, following the face-model
  skip precedent.
- Baseline: **729/729 pass**, 4 skipped. Every task ends at or above this.
- `mise exec -- clang-format -i` before every commit; CI builds with
  `-DPHIG_WERROR=ON`.

## Measured facts this plan is built on

From four real exported Live Photo pairs plus two ordinary stills, 2026-08-08:

- The MOV's `com.apple.quicktime.content.identifier` survives export and differs
  per shot.
- That UUID appears in its still's EXIF every time — 4 of 4.
- `exif_data_get_mnote_data()` returns **NULL** on these HEICs; libexif 0.6.26
  exports zero Apple symbols. The still's tag is not readable through libexif.
- A Live still carries **3** UUID-shaped strings in its MakerNote; an ordinary
  still carries **2**. Position is therefore not a safe discriminator.
- `ffmpeg -movflags use_metadata_tags -metadata com.apple.quicktime.content.identifier=…`
  produces a 3 KB MOV that round-trips the key — a committable fixture with no
  personal content.

## File Structure

| File | Responsibility |
|---|---|
| `core/live_photo.h` | Interface: identifier read, containment test, pairing decision. |
| `core/live_photo.cpp` | Implementation. |
| `core/scanner.cpp` | `is_live_photo_attachment` alongside `has_sibling_raw`. |
| `core/scan_engine.cpp` | Skip paired MOVs during the walk. |
| `core/organize_exec.cpp` | Carry the MOV with its still. |
| `tests/test_live_photo.cpp` | Pure-function tests + skipped real-file tests. |
| `tests/data/live_photo.mov` | Synthetic MOV fixture carrying a known identifier. |
| `docs/plans/2026-08-08-038-make-mov-fixture.sh` | Regenerates that fixture. |

---

### Task 1: Read the Content Identifier from a MOV

**Files:**
- Create: `core/live_photo.h`, `core/live_photo.cpp`, `tests/test_live_photo.cpp`,
  `tests/data/live_photo.mov`, `docs/plans/2026-08-08-038-make-mov-fixture.sh`
- Modify: `core/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `std::string live_photo_content_identifier(const std::string& video_path)` —
  the UUID, or empty if the file is not a MOV, has no such key, or cannot be
  opened. Never throws.

- [ ] **Step 1: Generate the fixture**

`docs/plans/2026-08-08-038-make-mov-fixture.sh`:

```bash
#!/bin/sh
# Regenerates tests/data/live_photo.mov — a 1-second clip carrying the same
# QuickTime key a real Live Photo's video does. Synthetic on purpose: real Live
# Photos are personal media and must not be committed.
#
# -movflags use_metadata_tags is required; without it ffmpeg silently drops
# arbitrary QuickTime keys and the fixture tests nothing.
set -e
ffmpeg -f lavfi -i testsrc=size=64x48:rate=10 -t 1 -c:v libx264 -pix_fmt yuv420p \
  -movflags use_metadata_tags \
  -metadata com.apple.quicktime.content.identifier=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE \
  -y "$(dirname "$0")/../../tests/data/live_photo.mov"
```

Run it, then confirm the key survived:

```bash
sh docs/plans/2026-08-08-038-make-mov-fixture.sh
ffprobe -v error -show_entries format_tags -of default=noprint_wrappers=1 \
  tests/data/live_photo.mov | grep content.identifier
```

Expected: `TAG:com.apple.quicktime.content.identifier=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE`.
**If the tag is absent, stop** — the fixture is worthless and the flag or the
ffmpeg build is wrong.

- [ ] **Step 2: Write the failing test**

`tests/test_live_photo.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "live_photo.h"

namespace fs = std::filesystem;

namespace {
std::string fixture(const std::string& name) {
    return (fs::path(__FILE__).parent_path() / "data" / name).string();
}
}  // namespace

TEST(LivePhotoIdentifier, ReadsTheContentIdentifierFromAMov) {
    EXPECT_EQ(live_photo_content_identifier(fixture("live_photo.mov")),
              "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
}

// An ordinary camera clip has no such key. This is the assertion that keeps
// MVI_*.MOV files from ever being treated as Live Photo motion.
TEST(LivePhotoIdentifier, ReturnsEmptyForAnOrdinaryVideo) {
    EXPECT_EQ(live_photo_content_identifier(fixture("test_red.mov")), "");
}

TEST(LivePhotoIdentifier, ReturnsEmptyForAMissingFileAndDoesNotThrow) {
    std::string id;
    EXPECT_NO_THROW({ id = live_photo_content_identifier("/nonexistent/x.mov"); });
    EXPECT_EQ(id, "");
}

TEST(LivePhotoIdentifier, ReturnsEmptyForANonVideoAndDoesNotThrow) {
    std::string id;
    EXPECT_NO_THROW({ id = live_photo_content_identifier(fixture("orient_1.heic")); });
    EXPECT_EQ(id, "");
}
```

- [ ] **Step 3: Run it and confirm it fails**

Add `tests/test_live_photo.cpp` to `phig-tests` in `CMakeLists.txt`, then:

```bash
cmake -B build && cmake --build build -j 2>&1 | tail -5
```

Expected: FAIL — `live_photo.h` not found.

- [ ] **Step 4: Implement**

`core/live_photo.h`:

```cpp
#pragma once

#include <string>

// Apple Live Photo pairing. A Live Photo is a still plus a short MOV written
// together; Apple links them with a Content Identifier UUID carried in both.
//
// The MOV's copy is authoritative and cleanly readable. The still's copy sits
// in an Apple MakerNote that libexif cannot parse (measured: zero Apple symbols
// in 0.6.26), so the still is tested for containment of the MOV's UUID rather
// than having its own identifier extracted. See plans/038-live-photos.md.

// The Content Identifier from a video container, or empty if absent or
// unreadable. Never throws.
std::string live_photo_content_identifier(const std::string& video_path);
```

`core/live_photo.cpp`:

```cpp
#include "live_photo.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

// Apple's key on the MOV half of a Live Photo. Read from the container's
// metadata dictionary, which core/metadata.cpp already opens for every video.
constexpr const char* kContentIdentifierKey = "com.apple.quicktime.content.identifier";

}  // namespace

std::string live_photo_content_identifier(const std::string& video_path) try {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, video_path.c_str(), nullptr, nullptr) != 0) return {};
    struct Closer {
        AVFormatContext** p;
        ~Closer() { avformat_close_input(p); }
    } closer{&fmt};

    // The key lives in the container dictionary and needs no stream info, so
    // avformat_find_stream_info is deliberately not called — it decodes packets
    // and this runs once per video during a scan.
    AVDictionaryEntry* tag = av_dict_get(fmt->metadata, kContentIdentifierKey, nullptr, 0);
    if (!tag || !tag->value) return {};
    return std::string(tag->value);
} catch (...) {
    return {};
}
```

Add `live_photo.cpp` to the `phig-core` sources in `core/CMakeLists.txt`.

- [ ] **Step 5: Run and confirm it passes**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='LivePhotoIdentifier.*'
```

Expected: 4 pass.

- [ ] **Step 6: Commit**

```bash
mise exec -- clang-format -i core/live_photo.h core/live_photo.cpp tests/test_live_photo.cpp
git add core/live_photo.h core/live_photo.cpp core/CMakeLists.txt CMakeLists.txt \
    tests/test_live_photo.cpp tests/data/live_photo.mov \
    docs/plans/2026-08-08-038-make-mov-fixture.sh
git commit -m "Read a Live Photo's Content Identifier from its MOV"
```

---

### Task 2: Test a still for containment of an identifier

**Files:**
- Modify: `core/live_photo.h`, `core/live_photo.cpp`, `tests/test_live_photo.cpp`

**Interfaces:**
- Consumes: `codec_exif_blob` (plan 048) for HEIC; libexif for JPEG.
- Produces:
  - `bool exif_blob_contains_identifier(const std::vector<unsigned char>& blob, const std::string& id)`
    — pure, exposed for testing.
  - `bool still_has_identifier(const std::string& still_path, const std::string& id)`

- [ ] **Step 1: Write the failing test**

```cpp
#include <vector>
#include "live_photo_internal.h"

TEST(LivePhotoContainment, FindsAnIdentifierPresentInTheBlob) {
    const std::string id = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    std::vector<unsigned char> blob = {0x00, 0x11, 0x22};
    blob.insert(blob.end(), id.begin(), id.end());
    blob.push_back(0x00);
    EXPECT_TRUE(exif_blob_contains_identifier(blob, id));
}

TEST(LivePhotoContainment, RejectsAnIdentifierThatIsAbsent) {
    std::vector<unsigned char> blob = {'n', 'o', 't', ' ', 'h', 'e', 'r', 'e'};
    EXPECT_FALSE(exif_blob_contains_identifier(blob, "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
}

// A truncated identifier must not match a longer one, or a MOV could pair with
// a still carrying a different UUID that happens to share a prefix.
TEST(LivePhotoContainment, DoesNotMatchOnAPrefix) {
    const std::string full = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    std::vector<unsigned char> blob(full.begin(), full.end() - 4);
    EXPECT_FALSE(exif_blob_contains_identifier(blob, full));
}

TEST(LivePhotoContainment, EmptyInputsNeverMatch) {
    EXPECT_FALSE(exif_blob_contains_identifier({}, "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
    std::vector<unsigned char> blob = {'x'};
    EXPECT_FALSE(exif_blob_contains_identifier(blob, ""));
}

// Real files, skipped when absent: these are personal photos exported by the
// Photos probe and deliberately not committed.
TEST(LivePhotoRealFiles, PairsAnExportedLivePhotoWithItsVideo) {
    const std::string dir = std::string(getenv("HOME")) + "/phig-live-samples";
    if (!fs::exists(dir)) GTEST_SKIP() << "no exported samples at " << dir;

    const std::string mov = dir + "/IMG_1112.MOV";
    const std::string heic = dir + "/IMG_1112.HEIC";
    const std::string other = dir + "/IMG_2147.HEIC";
    if (!fs::exists(mov) || !fs::exists(heic) || !fs::exists(other)) {
        GTEST_SKIP() << "expected sample files are missing";
    }

    const std::string id = live_photo_content_identifier(mov);
    ASSERT_FALSE(id.empty());
    EXPECT_TRUE(still_has_identifier(heic, id)) << "the still did not claim its own video";
    EXPECT_FALSE(still_has_identifier(other, id)) << "an unrelated still claimed this video";
}
```

- [ ] **Step 2: Run and confirm it fails**

Expected: `live_photo_internal.h` not found.

- [ ] **Step 3: Implement**

`core/live_photo_internal.h`:

```cpp
#pragma once

#include <string>
#include <vector>

// Exposed for tests. The containment check is deliberately a pure function over
// a buffer so it can be exercised without a real Live Photo, which cannot be
// committed as a fixture.
bool exif_blob_contains_identifier(const std::vector<unsigned char>& blob, const std::string& id);
```

Append to `core/live_photo.cpp`:

```cpp
bool exif_blob_contains_identifier(const std::vector<unsigned char>& blob,
                                   const std::string& id) {
    // An identifier is a 36-character UUID; anything shorter is not one, and an
    // empty needle would match everything.
    if (id.size() != 36 || blob.size() < id.size()) return false;
    return std::search(blob.begin(), blob.end(), id.begin(), id.end()) != blob.end();
}

bool still_has_identifier(const std::string& still_path, const std::string& id) try {
    if (id.empty()) return false;
    const std::vector<unsigned char> blob = still_exif_blob(still_path);
    return exif_blob_contains_identifier(blob, id);
} catch (...) {
    return false;
}
```

`still_exif_blob` reads the raw EXIF for either container: for a codec
extension it is 048's `codec_exif_blob`; for a JPEG, read the file's APP1
payload. Declare `still_has_identifier` in `live_photo.h`.

- [ ] **Step 4: Run and confirm it passes**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='LivePhoto*'
```

Expected: containment tests pass; the real-file test passes if
`~/phig-live-samples` exists and skips otherwise. Run it both ways — temporarily
rename the directory — to prove the skip works and is not hiding a failure.

- [ ] **Step 5: Commit**

```bash
mise exec -- clang-format -i core/live_photo.h core/live_photo_internal.h \
    core/live_photo.cpp tests/test_live_photo.cpp
git add core/live_photo.h core/live_photo_internal.h core/live_photo.cpp \
    core/CMakeLists.txt tests/test_live_photo.cpp
git commit -m "Pair a still to its Live Photo video by identifier containment"
```

---

### Task 3: The pairing decision over a directory

**Files:**
- Modify: `core/live_photo.h`, `core/live_photo.cpp`, `tests/test_live_photo.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct LivePhotoPair {
      std::string still;   // the master
      std::string video;   // the attachment
  };
  // Pairs found among `paths`. A video with no identifier, no matching still,
  // or more than one matching still is absent from the result.
  std::vector<LivePhotoPair> find_live_photo_pairs(const std::vector<std::string>& paths);
  ```

- [ ] **Step 1: Write the failing test**

```cpp
#include "live_photo.h"

// Ambiguity is refused, not guessed: a wrong pairing silently removes a video
// row and makes `mv` drag the file somewhere it does not belong.
TEST(LivePhotoPairing, RefusesToPairWhenTwoStillsClaimOneVideo) {
    // Built from the real samples: copy one still so two files carry the same
    // identifier, then confirm the video pairs with neither.
    const std::string dir = std::string(getenv("HOME")) + "/phig-live-samples";
    if (!fs::exists(dir + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples";

    const std::string tmp = make_temp_dir("livepair");
    fs::copy_file(dir + "/IMG_1112.MOV", tmp + "/a.mov");
    fs::copy_file(dir + "/IMG_1112.HEIC", tmp + "/a.heic");
    fs::copy_file(dir + "/IMG_1112.HEIC", tmp + "/duplicate.heic");

    auto pairs = find_live_photo_pairs({tmp + "/a.mov", tmp + "/a.heic",
                                        tmp + "/duplicate.heic"});
    EXPECT_TRUE(pairs.empty()) << "ambiguous match must pair nothing";
    fs::remove_all(tmp);
}

TEST(LivePhotoPairing, LeavesAnOrdinaryVideoAlone) {
    auto pairs = find_live_photo_pairs({fixture("test_red.mov"), fixture("orient_1.heic")});
    EXPECT_TRUE(pairs.empty());
}

TEST(LivePhotoPairing, PairsARealExportedLivePhoto) {
    const std::string dir = std::string(getenv("HOME")) + "/phig-live-samples";
    if (!fs::exists(dir + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples";
    auto pairs = find_live_photo_pairs({dir + "/IMG_1112.MOV", dir + "/IMG_1112.HEIC",
                                        dir + "/IMG_2147.HEIC"});
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(fs::path(pairs[0].still).filename(), "IMG_1112.HEIC");
    EXPECT_EQ(fs::path(pairs[0].video).filename(), "IMG_1112.MOV");
}
```

Add `#include "temp_dir.h"` to the test file.

- [ ] **Step 2: Run, confirm failure, implement, confirm pass**

The implementation reads each video's identifier once, then tests candidate
stills for containment. **Read each still's EXIF blob at most once** — plan 044
measured `has_sibling_raw` making scan quadratic by re-listing a directory per
file, and re-reading EXIF per (video, still) pair would repeat that mistake.
Skip all work when no video in the set has an identifier.

- [ ] **Step 3: Commit**

```bash
git commit -m "Decide Live Photo pairs over a set of paths"
```

---

### Task 4: Scan skips the attachment

**Files:**
- Modify: `core/scanner.h`, `core/scanner.cpp`, `core/scan_engine.cpp`
- Test: `tests/test_scan_engine.cpp`

This is the data-safety task: a wrong decision here removes a video row.
Per the repo convention, it ships with real-behaviour tests, not mocks.

- [ ] **Step 1: Write the failing test** — scan a directory containing a real
  exported pair (skipped when absent) and assert exactly one row exists, that it
  is the still, and that the MOV is on disk but unindexed.
- [ ] **Step 2: Confirm it fails** (today both are indexed — two rows).
- [ ] **Step 3: Implement**, mirroring `has_sibling_raw`'s placement, and cache
  the per-directory pairing so the walk stays linear.
- [ ] **Step 4: Confirm the full suite passes**, including the existing
  `MVI_*`-style ordinary-video tests, which must be unaffected.
- [ ] **Step 5: Commit.**

---

### Task 5: Organize carries the MOV with its still

**Files:**
- Modify: `core/organize_exec.cpp`, `core/sidecar.cpp` (or its carry list)
- Test: `tests/test_organize_exec.cpp`

- [ ] **Step 1: Write the failing test** — `mv` a paired still and assert the
  MOV moved with it, into the still's dated directory, renamed to match if the
  still was renamed. Then the same for `cp`. Assert the activity log records the
  carry, as `move_sidecar` does for plan 025.
- [ ] **Step 2: Confirm failure, implement, confirm pass.**
- [ ] **Step 3: Commit.**

---

### Task 6: Migrate already-indexed MOV rows

**Files:**
- Modify: `core/scan_engine.cpp` (reconcile phase)
- Test: `tests/test_relink.cpp` or `tests/test_scan_engine.cpp`

A collection scanned before this plan has independent MOV rows. Adopting the
model demotes any that turn out to be paired, transferring user metadata to the
still first via plan 023's relink primitive — exactly as 026 demotes a JPEG.

- [ ] **Step 1: Write the failing test** — index a pair as two rows (simulating
  the old behaviour), re-scan, and assert the MOV row is gone, the still row
  survives, and tags/favourites that were on the MOV row are now on the still.
- [ ] **Step 2: Confirm failure, implement, confirm pass.**
- [ ] **Step 3: Real-run check** against `~/phig-live-samples` in a scratch
  database: scan, confirm one row per shot, `mv` the collection, confirm pairs
  stay together. Record the output in `plans/038`.
- [ ] **Step 4: Commit.**

---

## Deferred from this plan

- **Goal 3, libphig visibility.** Exposing the pairing so a client can show a
  Live badge and play the motion is ABI surface, not pairing, and belongs with
  the client work in 050. Note it in `plans/038` as outstanding.
- **Goal 4, round-trip fidelity.** Verifying an Apple client re-accepts the pair
  needs a device, and `plans/038` already flags that writing Apple metadata
  would be a new capability requiring its own plan.
- **Identifier-stripped pairs.** No fallback, by decision. Revisit only with real
  stripped files to measure against.

## Self-Review

**Spec coverage.** 038's goal 1 is Tasks 3–4; goal 2 is Task 5; the migration in
its open decision 2 is Task 6. Goals 3 and 4 are explicitly deferred above.

**Type consistency.** `live_photo_content_identifier`, `still_has_identifier`,
`exif_blob_contains_identifier` and `find_live_photo_pairs` are used with the
same signatures in every task that references them.

**Known gap.** Tasks 4–6 are written as step outlines rather than full code,
because their shape depends on what Tasks 1–3 reveal about where pairing must be
cached to keep the walk linear. They must be expanded before being executed, and
that expansion is itself a checkpoint.

**Fixture honesty.** The only committed fixture is a synthetic MOV. Every
still-side real-file assertion skips without `~/phig-live-samples`, so CI proves
the pure logic and the MOV path, and the still path is proven only on a machine
with exported samples. That is a real coverage limit and is stated rather than
papered over.
