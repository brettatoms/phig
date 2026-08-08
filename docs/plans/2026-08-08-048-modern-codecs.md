# Modern Image Codecs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** phig decodes HEIC, HEIF and AVIF (then JPEG XL) with their EXIF
metadata, producing upright pixels identical to what `cv::imread` already
produces for JPEG.

**Architecture:** A `core/image_codec` module beside `core/raw`, same contract —
empty `Mat` on failure, thread-safe, never throws. Two decode backends compile
on **both** platforms: ImageIO (macOS only) and libheif (everywhere). Production
dispatch picks ImageIO on macOS and libheif elsewhere; both being present on
macOS is what lets the Linux path be tested on a Mac and what the cross-decoder
agreement test compares. EXIF comes from libheif's metadata box on both
platforms, fed to the existing libexif mapping.

**Tech Stack:** C++20, OpenCV, libheif, Apple ImageIO (macOS), libexif, CMake,
GoogleTest.

## Global Constraints

- Owning plan: `plans/048-modern-image-codecs.md`. Update its status to
  `in-progress` at Task 1 and record deviations there, not here.
- `codec_decode` contract, matching `core/raw.h`: returns an empty `cv::Mat` on
  any failure, is safe to call concurrently, and **never throws**.
- Decoded output is 8-bit 3-channel **BGR**, upright, matching `cv::imread`.
- **No colour management.** Both backends ignore the ICC profile identically,
  so macOS and Linux produce the same pixels. Do not add conversion on one
  platform only — that is the divergence this plan exists to avoid. Tracked as
  its own plan.
- Only the primary image of a multi-image container is decoded.
- First-party code builds under `-Wall -Wextra`; CI adds `-DPHIG_WERROR=ON`.
- `clang-format -i` (Google, 4-space, 100-col) before every commit.
- Baseline before starting: **693/693 pass**, 4 skipped (3 `MoveFileTest.CrossDevice*`,
  1 `ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing`). Every task ends at
  or above this.
- JPEG XL is Task 8 and needs `brew install jpeg-xl` / `libjxl-dev`. Do not add
  the dependency before that task.

## File Structure

| File | Responsibility |
|---|---|
| `core/image_codec.h` | Public interface: `is_codec_extension`, `codec_decode`, `codec_exif_blob`. |
| `core/image_codec_internal.h` | Backend entry points exposed for tests: `codec_decode_imageio`, `codec_decode_libheif`. |
| `core/image_codec.cpp` | Extension predicate, dispatch, shared orientation helper. |
| `core/image_codec_apple.cpp` | ImageIO decode. macOS only. |
| `core/image_codec_libheif.cpp` | libheif decode. All platforms. |
| `core/image_codec_exif.cpp` | EXIF box extraction via libheif. All platforms. |
| `core/scanner.cpp` | Prune undecodable extensions. |
| `core/hasher.cpp` | Route codec extensions to `codec_decode`. |
| `core/exif.cpp` | Use `exif_data_load_data` for codec formats. |
| `tests/test_image_codec.cpp` | Decode, orientation, failure, agreement tests. |
| `tests/data/*.heic`, `*.avif` | Checked-in fixtures — CI's Linux runner cannot generate them. |

---

### Task 1: Fixtures, interface, and extension pruning

No decoding yet. This task makes the extension surface honest and gives every
later task its fixtures.

**Files:**
- Create: `tests/data/orient_1.heic`, `orient_6.heic`, `orient_8.heic`,
  `orient_1.jpg`, `orient_6.jpg`, `orient_8.jpg`, `exif_sample.heic`,
  `exif_sample.jpg`, `sample.avif`
- Create: `docs/plans/2026-08-08-048-make-fixtures.swift`
- Create: `core/image_codec.h`, `core/image_codec.cpp`
- Modify: `core/scanner.cpp:20-50` (`known_extensions`)
- Modify: `core/CMakeLists.txt`
- Create: `tests/test_image_codec.cpp`
- Modify: `CMakeLists.txt` (add the test source)

**Interfaces:**
- Consumes: nothing.
- Produces: `bool is_codec_extension(const std::string& ext)` — true for
  `heic`, `heif`, `avif` (and `jxl` from Task 8), case-insensitive, accepts the
  extension with or without a leading dot.

- [ ] **Step 1: Write the fixture generator**

`docs/plans/2026-08-08-048-make-fixtures.swift`. Run on macOS only; its output
is checked in so Linux CI never needs it.

```swift
import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

// 200x100 so a 90-degree rotation is visible in the dimensions, with a marker
// block in one corner so a rotation can be verified by pixel position and not
// just by shape.
func makeImage() -> CGImage {
    let w = 200, h = 100
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8,
                        bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1))
    ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
    // CoreGraphics origin is bottom-left; this lands top-left in the image.
    ctx.setFillColor(CGColor(red: 0, green: 0, blue: 1, alpha: 1))
    ctx.fill(CGRect(x: 0, y: h - 20, width: 20, height: 20))
    return ctx.makeImage()!
}

func write(_ img: CGImage, _ path: String, _ type: UTType, _ props: [CFString: Any]) {
    let url = URL(fileURLWithPath: path) as CFURL
    let dst = CGImageDestinationCreateWithURL(url, type.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dst, img, props as CFDictionary)
    precondition(CGImageDestinationFinalize(dst), "failed to write \(path)")
}

let out = CommandLine.arguments[1]   // tests/data
let img = makeImage()

for o in [1, 6, 8] {
    write(img, "\(out)/orient_\(o).heic", .heic, [kCGImagePropertyOrientation: o])
    write(img, "\(out)/orient_\(o).jpg",  .jpeg, [kCGImagePropertyOrientation: o])
}

// A fixture carrying real EXIF, and a JPEG control with byte-identical tags,
// so the metadata test can compare the two extractions key for key.
let exifProps: [CFString: Any] = [
    kCGImagePropertyOrientation: 1,
    kCGImagePropertyExifDictionary: [
        kCGImagePropertyExifDateTimeOriginal: "2026:08:08 14:32:11",
        kCGImagePropertyExifFNumber: 4.0,
        kCGImagePropertyExifISOSpeedRatings: [400],
        kCGImagePropertyExifExposureTime: 0.002,
    ],
    kCGImagePropertyTIFFDictionary: [
        kCGImagePropertyTIFFMake: "Canon",
        kCGImagePropertyTIFFModel: "Canon EOS R5",
    ],
]
write(img, "\(out)/exif_sample.heic", .heic, exifProps)
write(img, "\(out)/exif_sample.jpg",  .jpeg, exifProps)
write(img, "\(out)/sample.avif", UTType("public.avif")!, [kCGImagePropertyOrientation: 1])
print("fixtures written to \(out)")
```

- [ ] **Step 2: Generate and verify the fixtures**

```bash
swiftc -O docs/plans/2026-08-08-048-make-fixtures.swift -o /tmp/mkfix
/tmp/mkfix tests/data
ls -la tests/data/*.heic tests/data/*.avif
```

Expected: nine files written, each a few hundred bytes to a few KB. Confirm the
AVIF is genuinely AVIF and not a silently-renamed HEIC:

```bash
file tests/data/sample.avif
```

Expected: identifies as AVIF/ISO Media. **If `CGImageDestination` refused to
write AVIF, stop** — drop `sample.avif` and its Task 5 test, and note in
`plans/048` that AVIF has no fixture rather than testing a mislabelled file.

- [ ] **Step 3: Write the failing test**

`tests/test_image_codec.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "image_codec.h"
#include "scanner.h"

TEST(ImageCodecExtension, MatchesModernContainerFormats) {
    EXPECT_TRUE(is_codec_extension("heic"));
    EXPECT_TRUE(is_codec_extension("heif"));
    EXPECT_TRUE(is_codec_extension("avif"));
}

TEST(ImageCodecExtension, IsCaseInsensitiveAndDotTolerant) {
    EXPECT_TRUE(is_codec_extension("HEIC"));
    EXPECT_TRUE(is_codec_extension(".heic"));
    EXPECT_TRUE(is_codec_extension(".HEIC"));
}

TEST(ImageCodecExtension, RejectsFormatsOpenCVAlreadyHandles) {
    EXPECT_FALSE(is_codec_extension("jpg"));
    EXPECT_FALSE(is_codec_extension("png"));
    EXPECT_FALSE(is_codec_extension("cr2"));
    EXPECT_FALSE(is_codec_extension(""));
}

// Formats phig advertised but links no codec for. Scan now skips them the way
// it already skips .tga, instead of reporting one decode error per file.
TEST(KnownExtensions, DropsFormatsNothingCanDecode) {
    EXPECT_FALSE(is_known_image_extension("psd"));
    EXPECT_FALSE(is_known_image_extension("svg"));
    EXPECT_FALSE(is_known_image_extension("xcf"));
    EXPECT_FALSE(is_known_image_extension("ico"));
}

TEST(KnownExtensions, StillAcceptsWhatOpenCVDecodes) {
    EXPECT_TRUE(is_known_image_extension("jpg"));
    EXPECT_TRUE(is_known_image_extension("png"));
    EXPECT_TRUE(is_known_image_extension("tiff"));
    EXPECT_TRUE(is_known_image_extension("jp2"));
    EXPECT_TRUE(is_known_image_extension("heic"));
}
```

- [ ] **Step 4: Run and verify it fails**

Add `tests/test_image_codec.cpp` to the `phig-tests` source list in
`CMakeLists.txt` (alphabetically near `tests/test_hasher.cpp`), then:

```bash
cmake -B build && cmake --build build -j 2>&1 | tail -5
```

Expected: FAIL to compile — `image_codec.h` does not exist.

- [ ] **Step 5: Write the interface and the predicate**

`core/image_codec.h`:

```cpp
#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

// Decoding for modern container formats OpenCV's imgcodecs does not handle:
// HEIC/HEIF, AVIF and JPEG XL. Structurally this is `core/raw.h` for a
// different family of formats, and it keeps the same contract.

// True if `ext` (with or without a leading dot, any case) names a format this
// module decodes rather than cv::imread.
bool is_codec_extension(const std::string& ext);

// Decode to an upright BGR cv::Mat for hashing/thumbnails/faces. Orientation is
// applied so the result matches what cv::imread produces for an equivalent
// JPEG. Returns an empty Mat if decoding fails for any reason. Thread-safe.
// Never throws.
cv::Mat codec_decode(const std::string& path);

// The container's raw EXIF block, ready for exif_data_load_data. Empty if the
// file carries none. Thread-safe. Never throws.
std::vector<unsigned char> codec_exif_blob(const std::string& path);
```

`core/image_codec.cpp`:

```cpp
#include "image_codec.h"

#include <algorithm>
#include <unordered_set>

namespace {

std::string normalize_ext(std::string ext) {
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

}  // namespace

bool is_codec_extension(const std::string& ext) {
    static const std::unordered_set<std::string> exts = {"heic", "heif", "avif"};
    return exts.count(normalize_ext(ext)) > 0;
}
```

In `core/scanner.cpp`, delete the `"ico"`, `"svg"`, `"psd"` and `"xcf"` entries
from the `known_extensions()` initializer list. Leave every other entry alone.

Add `image_codec.cpp` to the `phig-core` source list in `core/CMakeLists.txt`.

- [ ] **Step 6: Run and verify it passes**

```bash
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -8
```

Expected: the five new tests pass; total 698/698 with 4 skipped. **A pre-existing
test asserting `is_known_image_extension("psd")` may now fail** — `tests/test_scanner.cpp:142`
is known to assert `heif`. If a pruned extension is asserted there, update that
assertion to expect `false` and note it in the commit; do not re-add the format.

- [ ] **Step 7: Commit**

```bash
clang-format -i core/image_codec.h core/image_codec.cpp core/scanner.cpp \
    tests/test_image_codec.cpp
git add core/image_codec.h core/image_codec.cpp core/scanner.cpp core/CMakeLists.txt \
    CMakeLists.txt tests/test_image_codec.cpp tests/test_scanner.cpp tests/data \
    docs/plans/2026-08-08-048-make-fixtures.swift
git commit -m "Add image_codec extension surface and prune undecodable formats"
```

---

### Task 2: libheif decode backend

libheif first, not ImageIO, because it compiles on **both** platforms and is
therefore the reference the macOS backend is checked against.

**Files:**
- Create: `core/image_codec_internal.h`, `core/image_codec_libheif.cpp`
- Modify: `core/image_codec.cpp` (dispatch), `core/CMakeLists.txt`, `CMakeLists.txt`
- Test: `tests/test_image_codec.cpp`

**Interfaces:**
- Consumes: `is_codec_extension` from Task 1.
- Produces: `cv::Mat codec_decode_libheif(const std::string& path)` in
  `core/image_codec_internal.h`. Same contract as `codec_decode`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_image_codec.cpp`:

```cpp
#include <filesystem>

#include "image_codec_internal.h"

namespace fs = std::filesystem;

static std::string fixture(const std::string& name) {
    return (fs::path(__FILE__).parent_path() / "data" / name).string();
}

TEST(LibheifDecode, DecodesHeicToThreeChannelBgr) {
    cv::Mat img = codec_decode_libheif(fixture("orient_1.heic"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols, 200);
    EXPECT_EQ(img.rows, 100);
    EXPECT_EQ(img.channels(), 3);
    EXPECT_EQ(img.type(), CV_8UC3);
}

// The fixture is a red field with a blue 20x20 block at top-left. Checking the
// marker's position proves channel order and row order, which dimensions alone
// would not.
TEST(LibheifDecode, ProducesBgrChannelOrderAndTopLeftOrigin) {
    cv::Mat img = codec_decode_libheif(fixture("orient_1.heic"));
    ASSERT_FALSE(img.empty());
    cv::Vec3b marker = img.at<cv::Vec3b>(5, 5);        // inside the blue block
    cv::Vec3b field = img.at<cv::Vec3b>(50, 150);      // red field
    EXPECT_GT(marker[0], 200);  // B high
    EXPECT_LT(marker[2], 60);   // R low
    EXPECT_LT(field[0], 60);    // B low
    EXPECT_GT(field[2], 200);   // R high
}

TEST(LibheifDecode, ReturnsEmptyMatOnGarbageAndDoesNotThrow) {
    std::string path = (fs::path(testing::TempDir()) / "garbage.heic").string();
    { std::ofstream f(path, std::ios::binary); f << "not a heic file at all"; }
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_libheif(path); });
    EXPECT_TRUE(img.empty());
    fs::remove(path);
}

TEST(LibheifDecode, ReturnsEmptyMatForMissingFile) {
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_libheif("/nonexistent/file.heic"); });
    EXPECT_TRUE(img.empty());
}
```

Add `#include <fstream>` to the test's includes.

- [ ] **Step 2: Run and verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```

Expected: FAIL — `image_codec_internal.h` not found.

- [ ] **Step 3: Add libheif to the build**

In the top-level `CMakeLists.txt`, beside the `LIBRAW_R` block:

```cmake
# 1.16: Debian bookworm's libheif-dev. heif_image_handle_get_metadata and the
# heif_decode_image signature used in core/image_codec_libheif.cpp both predate
# it; the floor tracks the distro baseline.
pkg_check_modules(LIBHEIF REQUIRED libheif>=1.16)
```

In `core/CMakeLists.txt`, add `${LIBHEIF_INCLUDE_DIRS}` to
`target_include_directories`, `${LIBHEIF_LIBRARIES}` to `target_link_libraries`,
and `${LIBHEIF_LIBRARY_DIRS}` to `target_link_directories`. Add
`image_codec_libheif.cpp` to the source list.

- [ ] **Step 4: Write the backend**

`core/image_codec_internal.h`:

```cpp
#pragma once

#include <opencv2/core.hpp>
#include <string>

// Backend entry points behind `codec_decode`. Declared separately so tests can
// compare them against each other: on macOS both are compiled, which is what
// makes the Linux decode path testable on a Mac.

// libheif. Built on every platform. Applies the container's rotation and mirror
// transforms itself, which already matches cv::imread's behaviour for JPEG.
cv::Mat codec_decode_libheif(const std::string& path);

#ifdef __APPLE__
// Apple ImageIO. Hardware-accelerated, and does not apply orientation — the
// implementation applies the reported orientation itself.
cv::Mat codec_decode_imageio(const std::string& path);
#endif
```

`core/image_codec_libheif.cpp`:

```cpp
#include <libheif/heif.h>

#include <opencv2/imgproc.hpp>

#include "image_codec_internal.h"

namespace {

// Frees a libheif handle on every exit path, including an exception thrown by
// OpenCV mid-function. libheif's own types are C, so this is the only place
// the ownership rules live.
template <typename T, void (*Free)(T*)>
struct HeifPtr {
    T* p = nullptr;
    ~HeifPtr() {
        if (p) Free(p);
    }
    HeifPtr() = default;
    HeifPtr(const HeifPtr&) = delete;
    HeifPtr& operator=(const HeifPtr&) = delete;
};

}  // namespace

cv::Mat codec_decode_libheif(const std::string& path) try {
    HeifPtr<heif_context, heif_context_free> ctx;
    ctx.p = heif_context_alloc();
    if (!ctx.p) return {};

    if (heif_context_read_from_file(ctx.p, path.c_str(), nullptr).code != heif_error_Ok) return {};

    HeifPtr<heif_image_handle, heif_image_handle_release> handle;
    if (heif_context_get_primary_image_handle(ctx.p, &handle.p).code != heif_error_Ok) return {};

    HeifPtr<heif_image, heif_image_release> img;
    // Interleaved RGB, 8-bit. Container rotation/mirror transforms are applied
    // by default, which is what makes this match cv::imread without further
    // orientation work.
    if (heif_decode_image(handle.p, &img.p, heif_colorspace_RGB, heif_chroma_interleaved_RGB,
                          nullptr)
            .code != heif_error_Ok)
        return {};

    const int w = heif_image_get_width(img.p, heif_channel_interleaved);
    const int h = heif_image_get_height(img.p, heif_channel_interleaved);
    if (w <= 0 || h <= 0) return {};

    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(img.p, heif_channel_interleaved, &stride);
    if (!data || stride <= 0) return {};

    // Wrap without copying, then convert — cvtColor writes into fresh storage,
    // so the result does not alias libheif's buffer once `img` is released.
    const cv::Mat rgb(h, w, CV_8UC3, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return bgr;
} catch (...) {
    return {};
}
```

- [ ] **Step 5: Run and verify it passes**

```bash
cmake -B build && cmake --build build -j && \
  ./build/phig-tests --gtest_filter='LibheifDecode.*' 2>&1 | tail -10
```

Expected: 4 tests pass.

- [ ] **Step 6: Commit**

```bash
clang-format -i core/image_codec_internal.h core/image_codec_libheif.cpp \
    tests/test_image_codec.cpp
git add core/image_codec_internal.h core/image_codec_libheif.cpp core/CMakeLists.txt \
    CMakeLists.txt tests/test_image_codec.cpp
git commit -m "Decode HEIC/AVIF through libheif"
```

---

### Task 3: ImageIO backend and orientation

**Files:**
- Create: `core/image_codec_apple.cpp`
- Modify: `core/image_codec.cpp` (dispatch), `core/CMakeLists.txt`
- Test: `tests/test_image_codec.cpp`

**Interfaces:**
- Consumes: `codec_decode_libheif` (Task 2) as the reference behaviour.
- Produces: `codec_decode_imageio`, and `codec_decode` dispatching per platform.

**Measured behaviour this task exists to correct** (2026-08-08, macOS 26, on the
Task 1 fixtures):

| | orient_1 | orient_6 | orient_8 |
|---|---|---|---|
| `cv::imread` (JPEG control) | 200x100 | 100x200 | 100x200 |
| libheif | 200x100 | 100x200 | 100x200 |
| ImageIO raw | 200x100 | **200x100** | **200x100** |

ImageIO does not apply orientation but reports it correctly via
`kCGImagePropertyOrientation` (1, 6, 8 respectively). Applying that value
reproduces the other two.

- [ ] **Step 1: Write the failing test**

```cpp
#ifdef __APPLE__
// ImageIO is the production decoder on macOS. Its output must be
// indistinguishable from libheif's, because a phash computed on one platform is
// compared against phashes computed on the other.
TEST(ImageIODecode, MatchesLibheifDimensionsForEveryOrientation) {
    for (const char* name : {"orient_1.heic", "orient_6.heic", "orient_8.heic"}) {
        cv::Mat apple = codec_decode_imageio(fixture(name));
        cv::Mat heif = codec_decode_libheif(fixture(name));
        ASSERT_FALSE(apple.empty()) << name;
        ASSERT_FALSE(heif.empty()) << name;
        EXPECT_EQ(apple.cols, heif.cols) << name;
        EXPECT_EQ(apple.rows, heif.rows) << name;
    }
}

// Orientation 6 is a 90-degree rotation, so the 200x100 source must come back
// 100x200. This is the assertion that fails if the reported orientation is
// ignored, and equally if it is applied twice.
TEST(ImageIODecode, AppliesOrientationSoResultIsUpright) {
    cv::Mat upright = codec_decode_imageio(fixture("orient_1.heic"));
    cv::Mat rotated = codec_decode_imageio(fixture("orient_6.heic"));
    ASSERT_FALSE(upright.empty());
    ASSERT_FALSE(rotated.empty());
    EXPECT_EQ(upright.cols, 200);
    EXPECT_EQ(upright.rows, 100);
    EXPECT_EQ(rotated.cols, 100);
    EXPECT_EQ(rotated.rows, 200);
}

TEST(ImageIODecode, ReturnsEmptyMatOnGarbageAndDoesNotThrow) {
    std::string path = (fs::path(testing::TempDir()) / "garbage_io.heic").string();
    { std::ofstream f(path, std::ios::binary); f << "not a heic file at all"; }
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_imageio(path); });
    EXPECT_TRUE(img.empty());
    fs::remove(path);
}
#endif
```

- [ ] **Step 2: Run and verify it fails**

```bash
cmake --build build -j 2>&1 | tail -5
```

Expected: FAIL — `codec_decode_imageio` undefined.

- [ ] **Step 3: Write the backend**

`core/image_codec_apple.cpp`. ImageIO is a C API, so this is plain C++ — no
Objective-C++, no `.mm`.

```cpp
#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>

#include <opencv2/imgproc.hpp>

#include "image_codec_internal.h"

namespace {

struct CFDeleter {
    void operator()(CFTypeRef ref) const {
        if (ref) CFRelease(ref);
    }
};

template <typename T>
using CFPtr = std::unique_ptr<typename std::remove_pointer<T>::type, CFDeleter>;

// EXIF orientation 1-8 -> the rotation and flip that makes the image upright.
// ImageIO hands back unrotated pixels plus the orientation value, so this is
// applied here. libheif applies the equivalent transform itself, which is why
// there is no matching helper on that path.
cv::Mat apply_orientation(const cv::Mat& src, int orientation) {
    cv::Mat out;
    switch (orientation) {
        case 2: cv::flip(src, out, 1); return out;
        case 3: cv::rotate(src, out, cv::ROTATE_180); return out;
        case 4: cv::flip(src, out, 0); return out;
        case 5: cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE); cv::flip(out, out, 1); return out;
        case 6: cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE); return out;
        case 7: cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE); cv::flip(out, out, 1); return out;
        case 8: cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE); return out;
        default: return src.clone();  // 1, and anything malformed
    }
}

}  // namespace

cv::Mat codec_decode_imageio(const std::string& path) try {
    CFPtr<CFStringRef> cfpath(CFStringCreateWithCString(nullptr, path.c_str(),
                                                        kCFStringEncodingUTF8));
    if (!cfpath) return {};
    CFPtr<CFURLRef> url(CFURLCreateWithFileSystemPath(nullptr, cfpath.get(),
                                                      kCFURLPOSIXPathStyle, false));
    if (!url) return {};

    CFPtr<CGImageSourceRef> src(CGImageSourceCreateWithURL(url.get(), nullptr));
    if (!src) return {};

    int orientation = 1;
    CFPtr<CFDictionaryRef> props(CGImageSourceCopyPropertiesAtIndex(src.get(), 0, nullptr));
    if (props) {
        CFTypeRef value = CFDictionaryGetValue(props.get(), kCGImagePropertyOrientation);
        if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &orientation);
        }
    }

    CFPtr<CGImageRef> img(CGImageSourceCreateImageAtIndex(src.get(), 0, nullptr));
    if (!img) return {};

    const size_t w = CGImageGetWidth(img.get());
    const size_t h = CGImageGetHeight(img.get());
    if (w == 0 || h == 0) return {};

    // Draw into a known 8-bit BGRA layout rather than trusting the source's
    // own format: CGImage may be 10-bit, YCbCr, or carry an unusual alpha
    // arrangement, and the bitmap context normalizes all of it in one step.
    cv::Mat bgra(static_cast<int>(h), static_cast<int>(w), CV_8UC4);
    CFPtr<CGColorSpaceRef> cs(CGColorSpaceCreateDeviceRGB());
    CFPtr<CGContextRef> ctx(CGBitmapContextCreate(
        bgra.data, w, h, 8, bgra.step, cs.get(),
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little));
    if (!ctx) return {};
    CGContextDrawImage(ctx.get(), CGRectMake(0, 0, static_cast<CGFloat>(w),
                                             static_cast<CGFloat>(h)), img.get());

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return apply_orientation(bgr, orientation);
} catch (...) {
    return {};
}

#endif  // __APPLE__
```

Add `#include <memory>` and `#include <type_traits>` to that file's includes.

- [ ] **Step 4: Add dispatch**

Append to `core/image_codec.cpp`:

```cpp
#include "image_codec_internal.h"

cv::Mat codec_decode(const std::string& path) {
#ifdef __APPLE__
    // ImageIO uses the hardware HEVC decoder: measured 41.6 ms wall and 0.047 s
    // CPU against libheif's 188.4 ms and 0.64 s for one 3840x2160 HEIC.
    return codec_decode_imageio(path);
#else
    return codec_decode_libheif(path);
#endif
}
```

Add `image_codec_apple.cpp` to `core/CMakeLists.txt` guarded by platform, and
link the frameworks:

```cmake
if(APPLE)
    target_sources(phig-core PRIVATE image_codec_apple.cpp)
    target_link_libraries(phig-core PUBLIC "-framework ImageIO" "-framework CoreGraphics"
                                           "-framework CoreFoundation")
endif()
```

- [ ] **Step 5: Run and verify it passes**

```bash
cmake -B build && cmake --build build -j && \
  ./build/phig-tests --gtest_filter='ImageIODecode.*:LibheifDecode.*' 2>&1 | tail -12
```

Expected: 7 tests pass.

- [ ] **Step 6: Commit**

```bash
clang-format -i core/image_codec_apple.cpp core/image_codec.cpp tests/test_image_codec.cpp
git add core/image_codec_apple.cpp core/image_codec.cpp core/CMakeLists.txt \
    tests/test_image_codec.cpp
git commit -m "Decode HEIC/AVIF through ImageIO on macOS"
```

---

### Task 4: Cross-decoder pixel agreement

The test that bounds the platform-divergence risk with a number.

**Files:**
- Test: `tests/test_image_codec.cpp`

**Interfaces:**
- Consumes: both backends from Tasks 2 and 3.
- Produces: nothing.

- [ ] **Step 1: Write the test**

```cpp
#ifdef __APPLE__
// A phash computed on macOS is compared against one computed on Linux, so the
// two decoders must agree on pixels, not merely on dimensions. Exact equality
// is not required: chroma upsampling differs between implementations. The bound
// is what matters, and it is asserted rather than assumed.
TEST(CrossDecoderAgreement, ImageIOAndLibheifProduceNearIdenticalPixels) {
    for (const char* name : {"orient_1.heic", "orient_6.heic", "exif_sample.heic"}) {
        cv::Mat a = codec_decode_imageio(fixture(name));
        cv::Mat b = codec_decode_libheif(fixture(name));
        ASSERT_FALSE(a.empty()) << name;
        ASSERT_EQ(a.size(), b.size()) << name;
        ASSERT_EQ(a.type(), b.type()) << name;

        cv::Mat diff;
        cv::absdiff(a, b, diff);
        double max_channel_diff = 0;
        cv::minMaxLoc(diff.reshape(1), nullptr, &max_channel_diff);
        EXPECT_LE(max_channel_diff, 8) << name << ": decoders disagree by more than 8/255";
    }
}
#endif
```

- [ ] **Step 2: Run it**

```bash
cmake --build build -j && \
  ./build/phig-tests --gtest_filter='CrossDecoderAgreement.*' 2>&1 | tail -10
```

Expected: PASS. **If it fails, do not raise the threshold to make it green.**
Record the actual maximum difference in `plans/048` and diagnose: a large
difference means a real colour-handling divergence — most likely one backend
applying the ICC profile — which the Global Constraints forbid. A geometric
mismatch means orientation is wrong on one side.

- [ ] **Step 3: Commit**

```bash
clang-format -i tests/test_image_codec.cpp
git add tests/test_image_codec.cpp
git commit -m "Assert ImageIO and libheif decode to near-identical pixels"
```

---

### Task 5: EXIF extraction

**Files:**
- Create: `core/image_codec_exif.cpp`
- Modify: `core/exif.cpp:66`, `core/CMakeLists.txt`
- Test: `tests/test_image_codec.cpp`

**Interfaces:**
- Consumes: `is_codec_extension`.
- Produces: `std::vector<unsigned char> codec_exif_blob(const std::string&)`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "exif.h"

// The HEIC and the JPEG control were written from one source with identical
// tag values, so any difference is an extraction defect. This is the test that
// catches a future platform-specific metadata shortcut.
TEST(CodecExif, HeicMetadataMatchesJpegControl) {
    std::string from_heic = extract_exif_json(fixture("exif_sample.heic"));
    std::string from_jpeg = extract_exif_json(fixture("exif_sample.jpg"));
    EXPECT_NE(from_heic, "{}");
    EXPECT_EQ(from_heic, from_jpeg);
}

TEST(CodecExif, ExtractsBlobFromHeicContainer) {
    std::vector<unsigned char> blob = codec_exif_blob(fixture("exif_sample.heic"));
    EXPECT_FALSE(blob.empty());
}

TEST(CodecExif, ReturnsEmptyBlobForGarbageAndDoesNotThrow) {
    std::vector<unsigned char> blob;
    EXPECT_NO_THROW({ blob = codec_exif_blob("/nonexistent/file.heic"); });
    EXPECT_TRUE(blob.empty());
}
```

- [ ] **Step 2: Run and verify it fails**

Expected: `codec_exif_blob` undefined, and `extract_exif_json` returns `"{}"`
for the HEIC.

- [ ] **Step 3: Write the extractor**

`core/image_codec_exif.cpp`:

```cpp
#include <libheif/heif.h>

#include <vector>

#include "image_codec.h"

std::vector<unsigned char> codec_exif_blob(const std::string& path) try {
    struct heif_context* ctx = heif_context_alloc();
    if (!ctx) return {};
    struct Guard {
        struct heif_context* c;
        ~Guard() { heif_context_free(c); }
    } guard{ctx};

    if (heif_context_read_from_file(ctx, path.c_str(), nullptr).code != heif_error_Ok) return {};

    struct heif_image_handle* handle = nullptr;
    if (heif_context_get_primary_image_handle(ctx, &handle).code != heif_error_Ok) return {};
    struct HGuard {
        struct heif_image_handle* h;
        ~HGuard() { heif_image_handle_release(h); }
    } hguard{handle};

    heif_item_id id = 0;
    if (heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", &id, 1) != 1) return {};

    const size_t size = heif_image_handle_get_metadata_size(handle, id);
    if (size == 0) return {};

    std::vector<unsigned char> raw(size);
    if (heif_image_handle_get_metadata(handle, id, raw.data()).code != heif_error_Ok) return {};

    // A HEIF Exif block is prefixed with a 4-byte big-endian offset to the TIFF
    // header. libexif expects the payload to begin at that header, so the
    // prefix plus the offset it names are skipped.
    if (raw.size() < 4) return {};
    const size_t offset = (static_cast<size_t>(raw[0]) << 24) | (static_cast<size_t>(raw[1]) << 16) |
                          (static_cast<size_t>(raw[2]) << 8) | static_cast<size_t>(raw[3]);
    const size_t start = 4 + offset;
    if (start >= raw.size()) return {};

    return std::vector<unsigned char>(raw.begin() + static_cast<long>(start), raw.end());
} catch (...) {
    return {};
}
```

In `core/exif.cpp`, replace the single `exif_data_new_from_file` call at line 66
so codec formats take the blob path. Everything downstream of `ExifData*` is
unchanged:

```cpp
    ExifData* data = nullptr;
    if (is_codec_extension(std::filesystem::path(path).extension().string())) {
        std::vector<unsigned char> blob = codec_exif_blob(path);
        if (!blob.empty()) {
            data = exif_data_new_from_data(blob.data(), static_cast<unsigned int>(blob.size()));
        }
    } else {
        data = exif_data_new_from_file(path.c_str());
    }
```

Add `#include <filesystem>` and `#include "image_codec.h"` to `core/exif.cpp`,
and `image_codec_exif.cpp` to `core/CMakeLists.txt`.

- [ ] **Step 4: Run and verify it passes**

```bash
cmake --build build -j && ./build/phig-tests --gtest_filter='CodecExif.*' 2>&1 | tail -10
```

Expected: 3 pass. **If `HeicMetadataMatchesJpegControl` fails on the 4-byte
prefix handling**, dump the first 16 bytes of the blob and confirm the offset
convention before adjusting — libheif returns the block verbatim, and some
writers emit a bare TIFF header with no prefix. Handle both: if the bytes at
`start` are not `II*\0` or `MM\0*`, fall back to treating the whole block as the
payload.

- [ ] **Step 5: Commit**

```bash
clang-format -i core/image_codec_exif.cpp core/exif.cpp tests/test_image_codec.cpp
git add core/image_codec_exif.cpp core/exif.cpp core/CMakeLists.txt \
    core/image_codec.h tests/test_image_codec.cpp
git commit -m "Extract EXIF from HEIC containers via libheif"
```

---

### Task 6: Wire into the scan path

The end-to-end task: a HEIC becomes an indexed row with a thumbnail and a date.

**Files:**
- Modify: `core/hasher.cpp:266-290`
- Test: `tests/test_image_codec.cpp`, `tests/test_hasher.cpp`

**Interfaces:**
- Consumes: `codec_decode`, `is_codec_extension`.
- Produces: nothing new; `decode_image_with_warning` now handles codec formats.

- [ ] **Step 1: Write the failing test**

```cpp
#include "hasher.h"

TEST(CodecDecodeIntegration, DecodeImageHandlesHeic) {
    DecodeResult result = decode_image_with_warning(fixture("orient_1.heic"));
    EXPECT_FALSE(result.img.empty());
    EXPECT_TRUE(result.warning.empty());
    EXPECT_EQ(result.img.cols, 200);
    EXPECT_EQ(result.img.rows, 100);
}

TEST(CodecDecodeIntegration, ReportsAReasonWhenAHeicCannotBeDecoded) {
    std::string path = (fs::path(testing::TempDir()) / "bad_integration.heic").string();
    { std::ofstream f(path, std::ios::binary); f << "garbage"; }
    DecodeResult result = decode_image_with_warning(path);
    EXPECT_TRUE(result.img.empty());
    EXPECT_FALSE(result.warning.empty()) << "an undecodable HEIC must say why";
    fs::remove(path);
}
```

- [ ] **Step 2: Run and verify it fails**

Expected: the first test fails with an empty `Mat` — `cv::imread` still handles
the path.

- [ ] **Step 3: Add the branch**

In `core/hasher.cpp`, immediately after the existing RAW branch that ends at
line 273:

```cpp
    if (is_codec_extension(std::filesystem::path(path).extension().string())) {
        result.img = codec_decode(path);
        if (result.img.empty()) result.warning = "Cannot decode image: " + path;
        return result;
    }
```

Add `#include "image_codec.h"` to `core/hasher.cpp`.

- [ ] **Step 4: Run and verify it passes**

```bash
cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -8
```

Expected: full suite green, no regressions.

- [ ] **Step 5: Real-run check**

```bash
rm -f /tmp/heic-e2e.db
mkdir -p /tmp/heic-e2e && cp tests/data/exif_sample.heic tests/data/orient_6.heic /tmp/heic-e2e/
./build/phig scan /tmp/heic-e2e --thumbs --db /tmp/heic-e2e.db
./build/phig search --db /tmp/heic-e2e.db
```

Expected: `Processed: 2`, `Errors: 0`, `Thumbs: 2`. `search` lists both, with
`orient_6.heic` reported as **100x200** (upright, not 200x100) and
`exif_sample.heic` dated **2026-08-08** from EXIF rather than today's mtime.
Record the actual output in `plans/048`.

- [ ] **Step 6: Commit**

```bash
clang-format -i core/hasher.cpp tests/test_image_codec.cpp
git add core/hasher.cpp tests/test_image_codec.cpp
git commit -m "Route HEIC/AVIF through image_codec in the scan path"
```

---

### Task 7: Documentation and CI

**Files:**
- Modify: `README.md` (per-distro package lists), `.github/workflows/ci.yml`,
  `CLAUDE.md` (dependency list), `plans/048-modern-image-codecs.md`

- [ ] **Step 1: Add libheif to every package list**

`README.md`: add `libheif-dev` (Debian/Ubuntu), `libheif-devel` (Fedora),
`libheif` (Arch, Homebrew) beside the existing LibRaw entries. `CLAUDE.md`'s
Dependencies line gains "libheif (HEIC/AVIF decode)".

`.github/workflows/ci.yml`: add `libheif-dev` to the ubuntu `apt-get install`
list and `libheif` to the macOS `brew install` list.

- [ ] **Step 2: Verify CI's Linux path actually builds**

This is the one thing the development Mac cannot check: the `#else` branch of
`codec_decode` and the `if(APPLE)` CMake guard. Push the branch and confirm the
ubuntu job compiles and passes before considering the task done. **Do not mark
this task complete on a green macOS run alone.**

- [ ] **Step 3: Update the owning plan**

In `plans/048-modern-image-codecs.md`: set `status: in-progress`, link this
execution plan, and correct the Orientation section — it claims both backends
need explicit normalization, which measurement disproved. Only ImageIO does.
Update the `plans/000-INDEX.md` row to match.

- [ ] **Step 4: Commit**

```bash
git add README.md CLAUDE.md .github/workflows/ci.yml plans/048-modern-image-codecs.md \
    plans/000-INDEX.md
git commit -m "Document libheif dependency and record 048 progress"
```

---

### Task 8: JPEG XL

Needs a new dependency, so it lands last and separately.

**Files:**
- Modify: `core/image_codec.cpp`, `core/image_codec_libheif.cpp` (or a new
  `core/image_codec_jxl.cpp`), `core/image_codec_exif.cpp`, `CMakeLists.txt`
- Test: `tests/test_image_codec.cpp`
- Create: `tests/data/sample.jxl`

- [ ] **Step 1: Install the dependency and confirm a fixture can be made**

```bash
brew install jpeg-xl
cjxl tests/data/orient_1.jpg tests/data/sample.jxl
file tests/data/sample.jxl
```

Note ImageIO reads `public.jpeg-xl` on macOS 26 (confirmed in
`CGImageSourceCopyTypeIdentifiers`), so the macOS decode path needs no change
beyond adding the extension. Linux needs libjxl.

- [ ] **Step 2: Write the failing test**

```cpp
TEST(ImageCodecExtension, MatchesJpegXl) {
    EXPECT_TRUE(is_codec_extension("jxl"));
}

TEST(JxlDecode, DecodesToThreeChannelBgr) {
    cv::Mat img = codec_decode(fixture("sample.jxl"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.channels(), 3);
    EXPECT_EQ(img.type(), CV_8UC3);
}
```

- [ ] **Step 3: Run and verify it fails**

Expected: `is_codec_extension("jxl")` returns false.

- [ ] **Step 4: Implement**

Add `"jxl"` to the extension set in `core/image_codec.cpp`. On macOS the
existing ImageIO path handles it with no further change. On Linux, add a libjxl
decode branch — `codec_decode_libheif` must not be given a JXL file, since
libheif does not handle the format; dispatch on extension before choosing a
backend. Add `pkg_check_modules(LIBJXL REQUIRED libjxl>=0.7)` and wire the
include/link/library dirs exactly as Task 2 did for libheif.

- [ ] **Step 5: Run and verify it passes**

```bash
cmake -B build && cmake --build build -j && ctest --test-dir build --parallel 2>&1 | tail -8
```

- [ ] **Step 6: Commit**

```bash
clang-format -i core/image_codec.cpp core/image_codec_exif.cpp
git add -A
git commit -m "Decode JPEG XL"
```

---

## Self-Review

**Spec coverage.** Every section of `plans/048` maps to a task: module shape
(1-3), backend split (2-3), integration's three call sites (1 scanner, 5 exif,
6 hasher), orientation (3), testing including cross-decoder agreement (4) and
the metadata-equality test (5), dependencies and docs (7), JXL (8), pruning (1).
The real-run check is Task 6 Step 5.

**Known gaps, deliberate.** 048's open question 2 (HDR/wide gamut) is closed by
the Global Constraint that neither backend colour-manages — tracked as its own
plan rather than resolved here. Open question 4 (`jp2` overlap with libheif's
codec pulls) is not addressed by any task; it surfaces as a build or test
failure if real, and the full suite runs at every task. Open question 3
(multi-image HEIC) is a stated constraint, not a task: both backends take the
primary image by construction.

**Untestable on this machine.** The `#else` branch of `codec_decode` and the
`if(APPLE)` CMake guard. Task 7 Step 2 gates on CI rather than pretending
otherwise. Mitigated by libheif compiling on macOS, so the Linux *decoder* is
exercised here by Tasks 2 and 4 — only the platform selection is unverified.

**Fixture caveat.** The orientation fixtures are written by ImageIO, so the
rotation is stored as a container transform. They do not prove behaviour on a
camera-produced HEIC carrying EXIF orientation with no container transform,
where the HEIF spec makes the container authoritative and double-application is
the risk. A camera-produced fixture arrives with plan 049; add a test then.
