# Sidecar Files Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make phig carry a media file's sidecars on mv/cp and read a sidecar date when organizing, without indexing sidecars as media.

**Architecture:** A new pure `sidecar` module pairs a media file with its sidecars (both `IMG_1234.xmp` and `IMG_1234.CR2.xmp` naming, known-extension allowlist) and extracts an organizing date (XMP + Google Takeout JSON). The organize executor gains a sidecar-carry function that moves/copies each parent's sidecars alongside it and logs `move_sidecar`/`copy_sidecar` entries linked to the parent's image id. `get_best_date` consults sidecar dates after EXIF, before mtime.

**Tech Stack:** C++20, CMake, GoogleTest, std::filesystem. No new dependencies — XMP and JSON parsed with existing string scanning (the codebase already hand-parses JSON in `json_get`).

## Global Constraints

- **Language:** C++20; build with `-Wall -Wextra`, CI uses `-DPHIG_WERROR=ON` — no warnings.
- **Formatting:** clang-format LLVM-derived style, 4-space, 100-col; run `clang-format -i` on new/changed `src/` and `tests/` files before each commit.
- **Plans layout:** design is `plans/025-sidecar-files.md`; this is its implementation plan. Do NOT create `docs/` — `plans/` is the single source of truth.
- **No EXIF writing** anywhere in this work.
- **No schema migration:** `operation_entries.kind` is free text; `restore_json` is free-form JSON. New kinds (`move_sidecar`, `copy_sidecar`) need no `meta` version bump.
- **`phig undo` does not exist yet** (plan 020 deferred it). This plan only *writes* correct `*_sidecar` log entries. The reverse-replay branch lands with whatever plan implements `undo`.
- **Build:** `cmake --build build`. **Test:** `./build/phig-tests`. Run a single suite with `./build/phig-tests --gtest_filter='Sidecar*'`.
- Real-behavior tests only (tmpdir fixtures, real files) — this repo's standing rule for data-safety changes.

---

### Task 1: `sidecar` module — `find_sidecars`

**Files:**
- Create: `src/sidecar.h`
- Create: `src/sidecar.cpp`
- Create: `tests/test_sidecar.cpp`
- Modify: `CMakeLists.txt` (add `src/sidecar.cpp` to `phig-lib` sources after line 58; add `tests/test_sidecar.cpp` to `phig-tests` after line 119)

**Interfaces:**
- Consumes: nothing (leaf module).
- Produces:
  - `std::vector<std::filesystem::path> find_sidecars(const std::filesystem::path& media_file);`
  - `bool is_known_sidecar_extension(const std::string& ext);` (helper, exposed for testing)

- [ ] **Step 1: Write `src/sidecar.h`**

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <vector>

// True if `ext` (with or without a leading dot, any case) is a known metadata
// sidecar extension: xmp, pp3, dop, on1, aae, json, thm, lrv. Media/RAW
// extensions (jpg, cr2, ...) are NOT sidecars here — the RAW↔JPEG same-shot
// relationship is plan 026's concern, not this module's.
bool is_known_sidecar_extension(const std::string& ext);

// Existing sidecar files belonging to `media_file`, matched against the
// known-sidecar allowlist by BOTH naming conventions:
//   stem style:   IMG_1234.xmp       (media stem + sidecar ext)
//   append style: IMG_1234.CR2.xmp   (full media filename + sidecar ext)
// Returns absolute/normalized paths that exist on disk. Empty if none.
std::vector<std::filesystem::path> find_sidecars(const std::filesystem::path& media_file);
```

- [ ] **Step 2: Write the failing test `tests/test_sidecar.cpp`**

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "sidecar.h"

namespace fs = std::filesystem;

class SidecarTest : public ::testing::Test {
   protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-sidecar-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    void touch(const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }
};

TEST_F(SidecarTest, StemStyleAttachesToImage) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.xmp");
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].filename(), "IMG_1234.xmp");
}

TEST_F(SidecarTest, AppendStyleAttachesToRaw) {
    touch(dir / "IMG_1234.CR2");
    touch(dir / "IMG_1234.CR2.xmp");
    auto s = find_sidecars(dir / "IMG_1234.CR2");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].filename(), "IMG_1234.CR2.xmp");
}

TEST_F(SidecarTest, RawAndJpegKeepDistinctSidecars) {
    touch(dir / "IMG_1234.CR2");
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.CR2.xmp");  // belongs to the CR2
    touch(dir / "IMG_1234.xmp");      // belongs to the JPG (stem style)

    auto raw = find_sidecars(dir / "IMG_1234.CR2");
    ASSERT_EQ(raw.size(), 1u);
    EXPECT_EQ(raw[0].filename(), "IMG_1234.CR2.xmp");

    auto jpg = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(jpg.size(), 1u);
    EXPECT_EQ(jpg[0].filename(), "IMG_1234.xmp");
}

TEST_F(SidecarTest, NonAllowlistSameStemIgnored) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.txt");  // not a sidecar ext
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    EXPECT_TRUE(s.empty());
}

TEST_F(SidecarTest, MultipleSidecarsReturned) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.xmp");
    touch(dir / "IMG_1234.aae");
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    EXPECT_EQ(s.size(), 2u);
}

TEST_F(SidecarTest, NoSidecarIsEmpty) {
    touch(dir / "IMG_1234.JPG");
    EXPECT_TRUE(find_sidecars(dir / "IMG_1234.JPG").empty());
}

TEST_F(SidecarTest, ExtensionMatchIsCaseInsensitive) {
    touch(dir / "IMG_1234.JPG");
    touch(dir / "IMG_1234.XMP");
    auto s = find_sidecars(dir / "IMG_1234.JPG");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].filename(), "IMG_1234.XMP");
}
```

- [ ] **Step 3: Add `src/sidecar.cpp` to `phig-lib` and `tests/test_sidecar.cpp` to `phig-tests` in `CMakeLists.txt`**

In the `phig-lib` source list (after `src/organize_exec.cpp`, line 58):
```cmake
    src/sidecar.cpp
```
In the `phig-tests` source list (after `tests/test_organize_exec.cpp`, line 119):
```cmake
    tests/test_sidecar.cpp
```

- [ ] **Step 4: Run the build to verify it fails (no `sidecar.cpp` yet)**

Run: `cmake -B build && cmake --build build 2>&1 | tail -20`
Expected: link/compile error — `find_sidecars` undefined (or missing `src/sidecar.cpp`).

- [ ] **Step 5: Write `src/sidecar.cpp` (`find_sidecars` + helper)**

```cpp
#include "sidecar.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace fs = std::filesystem;

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

bool is_known_sidecar_extension(const std::string& ext) {
    static const std::array<const char*, 8> kExts = {"xmp", "pp3", "dop", "on1",
                                                     "aae", "json", "thm", "lrv"};
    std::string lower = to_lower(ext);
    if (!lower.empty() && lower[0] == '.') lower = lower.substr(1);
    for (const char* e : kExts)
        if (lower == e) return true;
    return false;
}

std::vector<fs::path> find_sidecars(const fs::path& media_file) {
    std::vector<fs::path> out;
    fs::path parent = media_file.parent_path();
    if (parent.empty()) parent = ".";
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) return out;

    const std::string media_name = media_file.filename().string();          // IMG_1234.CR2
    const std::string media_stem = media_file.stem().string();              // IMG_1234

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (p.filename() == media_file.filename()) continue;  // never itself

        const std::string ext = p.extension().string();  // ".xmp"
        if (!is_known_sidecar_extension(ext)) continue;

        const std::string base = p.stem().string();  // filename minus final ext
        // stem style:   base == media_stem       (IMG_1234.xmp   -> IMG_1234)
        // append style: base == media_name       (IMG_1234.CR2.xmp -> IMG_1234.CR2)
        if (base == media_stem || base == media_name) out.push_back(p);
    }
    return out;
}
```

- [ ] **Step 6: Build and run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='Sidecar*'`
Expected: all 7 SidecarTest cases PASS.

- [ ] **Step 7: Format and commit**

```bash
clang-format -i src/sidecar.h src/sidecar.cpp tests/test_sidecar.cpp
git add src/sidecar.h src/sidecar.cpp tests/test_sidecar.cpp CMakeLists.txt
git commit -m "feat(sidecar): find_sidecars pairs media with its sidecars"
```

---

### Task 2: `sidecar` module — `sidecar_date`

**Files:**
- Modify: `src/sidecar.h` (add declaration)
- Modify: `src/sidecar.cpp` (add implementation)
- Modify: `tests/test_sidecar.cpp` (add tests + a fixture writer)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `std::string sidecar_date(const std::filesystem::path& sidecar_file);`
    Returns a date string in EXIF shape `YYYY:MM:DD HH:MM:SS` (so the existing
    `parse_date` in `main.cpp` accepts it), or `""` if none found.

- [ ] **Step 1: Add the declaration to `src/sidecar.h`** (below `find_sidecars`)

```cpp
// Organizing date read from a sidecar, in EXIF shape "YYYY:MM:DD HH:MM:SS",
// or "" if none. Parses XMP (xmp:CreateDate / exif:DateTimeOriginal, whose
// values may be "M/D/YYYY h:mm:ss AM" or "YYYY-MM-DDThh:mm:ss") and Google
// Takeout JSON (photoTakenTime.formatted / .timestamp). Returns "" on any
// unparseable or dateless file.
std::string sidecar_date(const std::filesystem::path& sidecar_file);
```

- [ ] **Step 2: Write failing tests in `tests/test_sidecar.cpp`**

Add a fixture writer to the `SidecarTest` class body (next to `touch`):
```cpp
    void write(const fs::path& p, const std::string& contents) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << contents;
    }
```

Add tests (a real f-spot XMP shape, an ISO XMP shape, Google JSON, and negatives):
```cpp
TEST_F(SidecarTest, FspotXmpCreateDate) {
    // Shape observed in the real 2010 library.
    write(dir / "a.xmp",
          R"(<x:xmpmeta><rdf:Description xmp:CreateDate="11/25/2010 8:34:57 AM">)"
          R"(</rdf:Description></x:xmpmeta>)");
    EXPECT_EQ(sidecar_date(dir / "a.xmp"), "2010:11:25 08:34:57");
}

TEST_F(SidecarTest, IsoXmpDateTimeOriginal) {
    write(dir / "b.xmp",
          R"(<rdf:Description exif:DateTimeOriginal="2012-06-03T14:07:00"/>)");
    EXPECT_EQ(sidecar_date(dir / "b.xmp"), "2012:06:03 14:07:00");
}

TEST_F(SidecarTest, GoogleJsonPhotoTakenTime) {
    write(dir / "c.json",
          R"({"photoTakenTime":{"timestamp":"1288000497","formatted":"Nov 25, 2010"}})");
    // From the epoch timestamp (UTC): 2010-10-25 ... just assert year/month/day prefix.
    std::string d = sidecar_date(dir / "c.json");
    ASSERT_GE(d.size(), 10u);
    EXPECT_EQ(d.substr(0, 4), "2010");
}

TEST_F(SidecarTest, DatelessSidecarReturnsEmpty) {
    write(dir / "d.xmp", R"(<x:xmpmeta><rdf:Description xmp:Rating="0"/></x:xmpmeta>)");
    EXPECT_EQ(sidecar_date(dir / "d.xmp"), "");
}

TEST_F(SidecarTest, MissingFileReturnsEmpty) {
    EXPECT_EQ(sidecar_date(dir / "nope.xmp"), "");
}
```

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='SidecarTest.*Xmp*:SidecarTest.*Json*:SidecarTest.*Dateless*:SidecarTest.*Missing*'`
Expected: FAIL — `sidecar_date` undefined.

- [ ] **Step 4: Implement `sidecar_date` in `src/sidecar.cpp`**

Add includes at top: `#include <ctime>`, `#include <fstream>`, `#include <sstream>`.

```cpp
namespace {

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Value of attribute `attr="..."` in `xml`, or "".
std::string attr_value(const std::string& xml, const std::string& attr) {
    std::string key = attr + "=\"";
    auto pos = xml.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    auto end = xml.find('"', pos);
    if (end == std::string::npos) return "";
    return xml.substr(pos, end - pos);
}

// Format y/m/d/H/M/S into "YYYY:MM:DD HH:MM:SS".
std::string exif_shape(int y, int mo, int d, int h, int mi, int s) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    return buf;
}

// Parse the two XMP value shapes into EXIF shape, or "".
std::string normalize_xmp_date(const std::string& v) {
    if (v.empty()) return "";
    // ISO: "YYYY-MM-DDThh:mm:ss"
    if (v.size() >= 19 && v[4] == '-' && v[7] == '-' && (v[10] == 'T' || v[10] == ' ')) {
        int y, mo, d, h, mi, s;
        if (std::sscanf(v.c_str(), "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
            return exif_shape(y, mo, d, h, mi, s);
        return "";
    }
    // f-spot/US: "M/D/YYYY h:mm:ss AM|PM"
    {
        int mo, d, y, h, mi, s;
        char ampm[3] = {0, 0, 0};
        int n = std::sscanf(v.c_str(), "%d/%d/%d %d:%d:%d %2s", &mo, &d, &y, &h, &mi, &s, ampm);
        if (n >= 6) {
            if (n == 7) {
                if ((ampm[0] == 'P' || ampm[0] == 'p') && h < 12) h += 12;
                if ((ampm[0] == 'A' || ampm[0] == 'a') && h == 12) h = 0;
            }
            return exif_shape(y, mo, d, h, mi, s);
        }
    }
    return "";
}

std::string json_string_value(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

}  // namespace

std::string sidecar_date(const fs::path& sidecar_file) {
    std::string ext = sidecar_file.extension().string();
    std::string content = read_all(sidecar_file);
    if (content.empty()) return "";

    std::string lower_ext = ext;
    std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower_ext == ".json") {
        // Google Takeout: photoTakenTime.timestamp is epoch seconds (UTC).
        std::string ts = json_string_value(content, "timestamp");
        if (!ts.empty()) {
            std::time_t t = static_cast<std::time_t>(std::strtoll(ts.c_str(), nullptr, 10));
            std::tm tm{};
            gmtime_r(&t, &tm);
            return exif_shape(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                              tm.tm_sec);
        }
        return "";
    }

    // XMP: prefer CreateDate, then DateTimeOriginal.
    std::string v = attr_value(content, "xmp:CreateDate");
    std::string d = normalize_xmp_date(v);
    if (!d.empty()) return d;
    v = attr_value(content, "exif:DateTimeOriginal");
    return normalize_xmp_date(v);
}
```

- [ ] **Step 5: Build and run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='Sidecar*'`
Expected: all SidecarTest cases PASS (Task 1 + Task 2).

- [ ] **Step 6: Format and commit**

```bash
clang-format -i src/sidecar.h src/sidecar.cpp tests/test_sidecar.cpp
git add src/sidecar.h src/sidecar.cpp tests/test_sidecar.cpp
git commit -m "feat(sidecar): sidecar_date reads XMP and Google JSON dates"
```

---

### Task 3: Date fallback — consult sidecar date after EXIF, before mtime

**Files:**
- Modify: `src/main.cpp` (`get_best_date`, ~line 992-1008; add `#include "sidecar.h"` near the other includes ~line 28)
- Modify: `tests/test_organize.cpp` (date-fallback tests already live here per AGENTS.md coverage note)

**Interfaces:**
- Consumes: `find_sidecars`, `sidecar_date` from `src/sidecar.h`; `ImageInfo` (has `.path`, `.metadata_json`, `.modified_at`).
- Produces: no new public symbol — `get_best_date` behavior changes.

- [ ] **Step 1: Write a failing test in `tests/test_organize.cpp`**

Follow the file's existing style. If `get_best_date` is file-local (`static`/anonymous namespace), the test must exercise it through the smallest reachable seam. Check first:

Run: `grep -n 'get_best_date' src/main.cpp tests/test_organize.cpp`

- If `get_best_date` is already tested directly in `test_organize.cpp`, it is reachable — add the test below in the same manner as existing date tests.
- If it is NOT reachable from tests (file-local to `main.cpp`), instead assert the behavior at the `sidecar_date` + fallback-order boundary: add a test named `SidecarDatePreferredOverMtime` to `tests/test_sidecar.cpp` that builds a temp media file + `.xmp`, calls `find_sidecars` then `sidecar_date`, and asserts the resulting date — and record in the task notes that the ordering wiring is verified by the porcelain test in Task 5. Do not expose `get_best_date` solely for testing.

Test (add wherever `get_best_date` is reachable):
```cpp
// A file with no EXIF date but a dated sidecar must date from the sidecar,
// not fall through to mtime.
TEST(GetBestDate, SidecarUsedWhenNoExif) {
    // ... construct an ImageInfo with metadata_json="{}" and a real .xmp beside
    //     img.path carrying xmp:CreateDate="11/25/2010 8:34:57 AM";
    //     EXPECT year==2010, month==11.
}
// EXIF wins over a differently-dated sidecar.
TEST(GetBestDate, ExifWinsOverSidecar) {
    // metadata_json has DateTimeOriginal 2015:01:02...; sidecar says 2010 ->
    // EXPECT year==2015.
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='GetBestDate.*'`
Expected: FAIL (sidecar not yet consulted; `SidecarUsedWhenNoExif` yields the mtime year).

- [ ] **Step 3: Add `#include "sidecar.h"` and the sidecar step in `get_best_date`**

Near the top includes of `src/main.cpp` (with the other `#include "..."`):
```cpp
#include "sidecar.h"
```

In `get_best_date` (`src/main.cpp`), insert between the EXIF `DateTimeDigitized` check and the mtime check:
```cpp
    // 3. Sidecar date (e.g. f-spot .xmp, Google Takeout .json). Only consulted
    //    when the embedded EXIF has no date; a sidecar can be edited, so EXIF
    //    wins. Sits before mtime so EXIF-stripped exports still date correctly.
    for (const auto& side : find_sidecars(img.path)) {
        d = parse_date(sidecar_date(side));
        if (d.valid) return d;
    }

    // 4. File modified time
    d = parse_date(img.modified_at);
    if (d.valid) return d;
```
(Renumber the trailing mtime comment from `3.` to `4.` accordingly.)

- [ ] **Step 4: Build and run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='GetBestDate.*:Sidecar*'`
Expected: PASS.

- [ ] **Step 5: Format and commit**

```bash
clang-format -i src/main.cpp tests/test_organize.cpp tests/test_sidecar.cpp
git add src/main.cpp tests/test_organize.cpp tests/test_sidecar.cpp
git commit -m "feat(organize): use sidecar date after EXIF, before mtime"
```

---

### Task 4: Carry sidecars on mv/cp with logging

**Files:**
- Modify: `src/organize_exec.h` (declare the sidecar-carry function)
- Modify: `src/organize_exec.cpp` (implement it; `#include "sidecar.h"`)
- Modify: `src/database.h` (extend the `OperationEntry::kind` comment, line 31, to note the new kinds)
- Modify: `src/main.cpp` (`cmd_organize`: call the carry function per non-skipped action)
- Modify: `tests/test_organize_exec.cpp` (unit tests for carry + logging)

**Interfaces:**
- Consumes: `find_sidecars` (Task 1); `execute_organize_action` and `Database` (existing); `Database::get_image_id(const std::string&)`; `Database::log_add_entry(int64_t, const OperationEntry&)`.
- Produces:
  ```cpp
  // Move/copy every sidecar of `parent_source` to sit beside `parent_dest`,
  // deriving each sidecar's destination name from parent_dest's final name
  // (stem style: IMG_1234.xmp -> <dest-stem>.xmp; append style:
  // IMG_1234.CR2.xmp -> <dest-stem>.CR2.xmp). Each real move/copy is logged as
  // kind "move_sidecar"/"copy_sidecar" with restore_json {"parent_image_id":N}
  // (N from get_image_id(parent_dest); omitted if the parent row is absent).
  // op_id <= 0 skips logging. Never touches the images table.
  void carry_sidecars(Database& db, const std::string& parent_source,
                      const std::string& parent_dest, bool is_move, int64_t op_id);
  ```

- [ ] **Step 1: Declare `carry_sidecars` in `src/organize_exec.h`** (below `execute_organize_action`, copy the doc comment from the Interfaces block above).

- [ ] **Step 2: Extend the `OperationEntry::kind` comment in `src/database.h` line 31**

```cpp
        std::string kind;  // "move" | "copy" | "purge_row" | "move_sidecar" | "copy_sidecar"
```

- [ ] **Step 3: Write failing tests in `tests/test_organize_exec.cpp`**

```cpp
TEST_F(OrganizeExecTest, CarryMovesStemSidecarBesideParent) {
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1.JPG";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    // Parent first (repoints the row so get_image_id(dst) works), then sidecars.
    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), /*is_move=*/true, /*op_id=*/0);

    EXPECT_FALSE(fs::exists(dir / "src" / "IMG_1.xmp"));
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1.xmp"));
}

TEST_F(OrganizeExecTest, CarryRenamesSidecarToMatchRenamedParent) {
    // Parent was renamed to avoid a conflict: IMG_1.JPG -> IMG_1-1.JPG.
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1-1.JPG";  // final (renamed) name
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), true, 0);

    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1-1.xmp"));
}

TEST_F(OrganizeExecTest, CarryHandlesAppendStyleRawSidecar) {
    fs::path src = dir / "src" / "IMG_1.CR2";
    fs::path dst = dir / "dest" / "IMG_1-1.CR2";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.CR2.xmp", "<xmp/>");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), true, 0);
    carry_sidecars(*db, src.string(), dst.string(), true, 0);

    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1-1.CR2.xmp"));
}

TEST_F(OrganizeExecTest, CarryCopyLeavesSourceSidecar) {
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1.JPG";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    execute_organize_action(*db, src.string(), dst.string(), false, 0);
    carry_sidecars(*db, src.string(), dst.string(), /*is_move=*/false, 0);

    EXPECT_TRUE(fs::exists(dir / "src" / "IMG_1.xmp"));   // copy leaves source
    EXPECT_TRUE(fs::exists(dir / "dest" / "IMG_1.xmp"));
}

TEST_F(OrganizeExecTest, CarryLogsSidecarEntryWithParentId) {
    fs::path src = dir / "src" / "IMG_1.JPG";
    fs::path dst = dir / "dest" / "IMG_1.JPG";
    write_file(src, "AAA");
    write_file(dir / "src" / "IMG_1.xmp", "<xmp/>");
    insert_row(src.string());

    int64_t op = db->log_begin_operation("mv", "argv", "2026-07-11T00:00:00Z");
    execute_organize_action(*db, src.string(), dst.string(), true, op);
    carry_sidecars(*db, src.string(), dst.string(), true, op);
    db->log_finish_operation(op, "completed");

    // Exactly one move_sidecar entry, dst is the moved sidecar, restore_json
    // names the parent image id.
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db->db_ptr(),
                       "SELECT kind, dst, restore_json FROM operation_entries "
                       "WHERE kind='move_sidecar'",
                       -1, &st, nullptr);
    ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    std::string kind = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    std::string edst = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
    std::string rj = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
    sqlite3_finalize(st);
    EXPECT_EQ(kind, "move_sidecar");
    EXPECT_EQ(edst, (dir / "dest" / "IMG_1.xmp").string());
    EXPECT_NE(rj.find("parent_image_id"), std::string::npos);
}
```

(If `db_ptr()` is not public, reuse the accessor `test_organize_exec.cpp` already uses in `set_readonly` — it calls `db->db_ptr()`, so it is available.)

- [ ] **Step 4: Run to verify failure**

Run: `cmake --build build 2>&1 | tail -20`
Expected: FAIL — `carry_sidecars` undefined.

- [ ] **Step 5: Implement `carry_sidecars` in `src/organize_exec.cpp`**

Add `#include "sidecar.h"` and `#include <string>` if absent. Append:

```cpp
void carry_sidecars(Database& db, const std::string& parent_source,
                    const std::string& parent_dest, bool is_move, int64_t op_id) {
    const bool logging = op_id > 0;
    fs::path src_media(parent_source);
    fs::path dst_media(parent_dest);
    std::string src_stem = src_media.stem().string();      // IMG_1
    std::string src_name = src_media.filename().string();  // IMG_1.CR2
    std::string dst_stem = dst_media.stem().string();      // IMG_1-1
    fs::path dst_dir = dst_media.parent_path();

    // Parent id for linking (parent row already repointed to dst by the caller).
    int64_t parent_id = logging ? db.get_image_id(parent_dest) : 0;

    for (const auto& side : find_sidecars(src_media)) {
        std::string side_name = side.filename().string();  // IMG_1.xmp or IMG_1.CR2.xmp
        // Rewrite the leading parent stem to the destination stem; keep the rest
        // (".xmp" for stem style, ".CR2.xmp" for append style).
        std::string tail;
        if (side_name.rfind(src_name + ".", 0) == 0) {
            tail = side_name.substr(src_name.size());       // ".xmp" after "IMG_1.CR2"
            // append style: dest keeps full media name -> dst_stem + parent ext + tail
            // dst media filename is dst_stem + parent-ext; reconstruct from dst_media.
            std::string dst_name = dst_media.filename().string();  // IMG_1-1.CR2
            tail = dst_name + tail;                                 // IMG_1-1.CR2.xmp
        } else {
            // stem style: IMG_1.xmp -> IMG_1-1.xmp
            tail = dst_stem + side_name.substr(src_stem.size());
        }
        fs::path side_dst = dst_dir / tail;

        if (src_media.parent_path() == dst_dir && side == side_dst) continue;  // no-op

        if (is_move) {
            move_file(side.string(), side_dst.string());
        } else {
            atomic_copy_file(side.string(), side_dst.string());
        }
        if (logging) {
            std::string rj =
                parent_id > 0 ? ("{\"parent_image_id\":" + std::to_string(parent_id) + "}") : "";
            db.log_add_entry(op_id, {is_move ? "move_sidecar" : "copy_sidecar", side.string(),
                                     side_dst.string(), rj});
        }
    }
}
```

Note: `move_file` and `atomic_copy_file` are the same `paths.h` helpers `execute_organize_action` uses (already included via `paths.h`).

- [ ] **Step 6: Call `carry_sidecars` from `cmd_organize` in `src/main.cpp`**

In the execute loop (after the successful `execute_organize_action` call, inside the `try`, ~line 1177, right after `done++;`):
```cpp
            carry_sidecars(db, a.source, a.dest.string(), opts.is_move,
                           logging ? op_id : 0);
```
Placed inside the same `try` so a sidecar failure is caught and reported per-file like any other error, without aborting the run.

- [ ] **Step 7: Build and run the tests**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='OrganizeExecTest.*'`
Expected: all OrganizeExecTest cases PASS (existing + 5 new).

- [ ] **Step 8: Format and commit**

```bash
clang-format -i src/organize_exec.h src/organize_exec.cpp src/database.h src/main.cpp tests/test_organize_exec.cpp
git add src/organize_exec.h src/organize_exec.cpp src/database.h src/main.cpp tests/test_organize_exec.cpp
git commit -m "feat(organize): carry sidecars on mv/cp, logged and parent-linked"
```

---

### Task 5: End-to-end porcelain test + dry-run + docs

**Files:**
- Modify: `tests/test_porcelain.cpp` (end-to-end mv-with-sidecar + dry-run)
- Modify: `AGENTS.md` ("Key Design Decisions" — add a sidecar bullet)
- Modify: `README.md` (note mv/cp carry sidecars and organize reads sidecar dates)

**Interfaces:**
- Consumes: the full binary (`./build/phig`) via the porcelain harness in `tests/test_porcelain.cpp`.
- Produces: no new symbol.

- [ ] **Step 1: Inspect the porcelain harness to match its exact run pattern**

Run: `sed -n '1,64p' tests/test_porcelain.cpp`
Note how it invokes the binary, sets `--db`, and asserts on filesystem/DB state; mirror it.

- [ ] **Step 2: Write failing end-to-end tests in `tests/test_porcelain.cpp`**

Two cases, using the harness's existing scaffolding (scan a temp source, run `mv`, assert):
```cpp
// mv carries the sidecar to the destination beside its image.
TEST_F(PorcelainTest, MvCarriesSidecar) {
    // 1. Create <src>/IMG_1.JPG (a tiny valid image the scanner accepts) and
    //    <src>/IMG_1.xmp with xmp:CreateDate="11/25/2010 8:34:57 AM".
    // 2. phig scan <src>; phig mv <dest> --match '<src>/**' --format '%Y/%m/%original'.
    // 3. EXPECT <dest>/2010/11/IMG_1.JPG exists (dated from the sidecar) AND
    //    <dest>/2010/11/IMG_1.xmp exists beside it; source .xmp is gone.
}

// --dry-run moves nothing, including the sidecar.
TEST_F(PorcelainTest, MvDryRunLeavesSidecar) {
    // scan, then phig mv <dest> --match '<src>/**' --dry-run.
    // EXPECT source IMG_1.JPG and IMG_1.xmp both still present; <dest> empty.
}
```
Use whatever minimal valid image the other porcelain/scanner tests use as a fixture (check `tests/data/`).

Run: `ls tests/data` to find an existing tiny image to copy as the fixture.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build && ./build/phig-tests --gtest_filter='PorcelainTest.Mv*'`
Expected: `MvCarriesSidecar` FAILS (sidecar not at dest) before the feature is wired end-to-end; it PASSES once Tasks 3-4 are in. If Tasks 3-4 already passed, this test confirms the full path — run it to prove the date+carry work together through the real binary.

- [ ] **Step 4: Confirm both pass**

Run: `./build/phig-tests --gtest_filter='PorcelainTest.Mv*'`
Expected: PASS. If `MvCarriesSidecar` fails on the date (image lands in `unsorted` not `2010/11`), verify Task 3's ordering; if it fails on the sidecar location, verify Task 4's `cmd_organize` call site.

- [ ] **Step 5: Update `AGENTS.md` and `README.md`**

`AGENTS.md`, under "Key Design Decisions", add:
```markdown
- **Sidecar files** — `mv`/`cp` carry a media file's sidecars (xmp, pp3, dop,
  on1, aae, json, thm, lrv; both `IMG_1234.xmp` and `IMG_1234.CR2.xmp` naming)
  to the destination beside their image, renaming them to match a renamed
  parent, logged as `move_sidecar`/`copy_sidecar` entries linked to the parent
  image id. Sidecars are never indexed as media. Organize also reads a sidecar
  date (XMP / Google Takeout JSON) as a date source after EXIF and before mtime.
```

`README.md`: add a short note in the organize/mv section mirroring the above (one or two sentences, plain).

- [ ] **Step 6: Full test run, format, commit**

```bash
cmake --build build && ./build/phig-tests
clang-format -i tests/test_porcelain.cpp
git add tests/test_porcelain.cpp AGENTS.md README.md
git commit -m "test(sidecar): end-to-end mv carry + dry-run; document sidecar handling"
```

---

## Self-Review Notes

- **Spec coverage:** module (`find_sidecars`/`sidecar_date`) → Tasks 1-2; move/copy carry + fate → Task 4 + Task 5 dry-run; log `*_sidecar` + parent link → Task 4; date fallback after EXIF before mtime → Task 3; undo → out of scope this plan (write-only entries; noted in Global Constraints per design Open Question #1); tests → every task. No mtime change, no EXIF writing, no indexing — honored.
- **Type consistency:** `find_sidecars(const fs::path&) -> vector<fs::path>`, `sidecar_date(const fs::path&) -> string`, `carry_sidecars(Database&, const string&, const string&, bool, int64_t)`, `OperationEntry{kind, src, dst, restore_json}`, `get_image_id(const string&) -> int64_t` — consistent across tasks.
- **Open items deferred to implementation** (from design): XMP attribute precedence settled as CreateDate → DateTimeOriginal in Task 2; single `find_sidecars` call per parent in both date and carry paths is acceptable (two small `directory_iterator` passes) — micro-optimization not pursued (YAGNI).
```
