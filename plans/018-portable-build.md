---
name: Portable Build & System Packages
status: planned
created: 2026-07-05
description: >
  Make phig buildable and installable from system packages on any Linux distro
  (Debian, Fedora, Arch, ...) and macOS via Homebrew. Remove the sole OpenCV
  contrib dependency (img_hash) by reimplementing pHash against core OpenCV, add
  a project version, and document exact per-distro build/runtime dependencies with
  a verified build-from-source path. Formal packaging definitions are a follow-up.
---

# Portable Build & System Packages

## Goal

Today phig is developed inside a Nix `devenv` shell, and the resulting binary bakes
absolute `/nix/store/...` paths into its Mach-O/ELF load commands — so it only runs
on the dev machine. The build itself is *not* Nix-specific (CMake uses
`find_package`/`pkg_check_modules`), but one dependency and a couple of gaps stop it
from building cleanly against ordinary system packages.

This plan makes phig **build from source against stock system packages on any
Linux distro and on macOS/Homebrew**. It does not set up CI or ship prebuilt
binaries — the goal is to guarantee (and document) that packaging is *possible*
everywhere. Formal packaging definitions (Homebrew formula, PKGBUILD, `debian/`,
`.spec`) are deferred to a follow-up plan.

`devenv`/`direnv` stays exactly as-is for development.

## Design Decisions

### Remove the OpenCV `contrib` dependency (the real blocker)

`compute_phash()` in `hasher.cpp:174` is the only code that touches OpenCV
`contrib`: it calls `cv::img_hash::PHash::create()` from `<opencv2/img_hash.hpp>`
(still present after the video-support merge). The `img_hash` module is part of
`opencv_contrib`, which is **not** in every distro's
stock OpenCV:

| Platform | Stock OpenCV | `img_hash` present |
|----------|--------------|--------------------|
| Homebrew `opencv` | built with contrib | yes |
| Fedora `opencv-devel` | built with contrib | yes (verify) |
| Debian `libopencv-dev` | contrib split out | no — needs `libopencv-contrib-dev` |
| Arch `opencv` | no contrib | no — needs a contrib rebuild |

Every other OpenCV module phig uses is **core** and universally present:
`core`, `imgcodecs`, `imgproc`, `videoio` (video keyframe extraction, added with
video support), and the face modules `FaceDetectorYN` / `FaceRecognizerSF` (in
`objdetect` + `dnn`, core since OpenCV 4.5.4). The current
`find_package(OpenCV ... COMPONENTS core imgcodecs imgproc img_hash objdetect videoio)`
line drops only `img_hash`.

**Decision:** reimplement pHash in-tree using only core OpenCV, mirroring how the
project already hand-rolls SHA-256 rather than pulling a dependency. This removes
`opencv_contrib` from the dependency set entirely, so phig builds against stock
**core** OpenCV on every platform.

Algorithm (core `cv::dct`, no contrib):

```
decode -> cvtColor BGR2GRAY -> resize 32x32 -> convert CV_32F
       -> cv::dct -> take top-left 8x8 low-frequency block
       -> median of the 64 coefficients
       -> bit i set when coeff_i > median  => 64-bit hash
```

- Remove `img_hash` from the `find_package(OpenCV ... COMPONENTS ...)` list in
  `CMakeLists.txt` and drop `#include <opencv2/img_hash.hpp>`.
- The `compute_phash(const cv::Mat&)` / `compute_phash(const std::string&)`
  signatures and the `PhashResult` struct are unchanged, so callers and the DB
  schema are untouched. Video keyframe hashing also goes through `compute_phash`,
  so it inherits the core-only implementation automatically.

**Compatibility note:** a fresh implementation will not be bit-identical to
OpenCV's PHash, so previously stored `phash` values would need a re-scan
(`phig scan --force hash`) to remain comparable. phig has no releases and no version
string yet, so there are no external databases to preserve — this is a non-issue in
practice, but is called out for the record.

### Minimum OpenCV version, and graceful behavior on older ones

Core features (scan, phash/duplicates, organize, purge, search) work with any
reasonably recent OpenCV (>= 4.5). **Face recognition** uses the `FaceDetectorYN`
API and the `2023mar` YuNet model, which effectively require **OpenCV >= 4.7**.
Some current stable distros ship older OpenCV (e.g. Debian stable ~4.6,
Ubuntu 22.04 ~4.5.4), where face recognition may not work even though everything
else does.

**Decision:** document a minimum of **OpenCV 4.5** for core functionality and
**OpenCV 4.7** for face recognition. This is a documentation/expectations matter,
not a code gate; no hard version check is added unless verification shows a build
failure on a target distro.

### Project version

Packaging and `--version` both need a real version string, which the project
currently lacks.

**Decision:**
- Add `VERSION` to the `project()` call in `CMakeLists.txt` (start at `0.1.0`).
- Generate `src/version.h` from a `version.h.in` template via
  `configure_file()`, exposing `PHIG_VERSION`.
- Add a top-level `phig --version` (and `-v`) that prints it.

### Build stays package-manager-agnostic

No change to the build model is needed: `CMakeLists.txt` already resolves deps via
`find_package`/`pkg_check_modules`, which work identically against Homebrew, apt,
dnf, and pacman. The `CMAKE_PREFIX_PATH` set in `devenv.nix` only matters inside the
dev shell. This plan just verifies and documents building outside Nix.

## Dependencies (build from source)

Runtime libraries: OpenCV (core), SQLite3, libexif, libcurl, and **FFmpeg**
(`libavformat`, `libavcodec`, `libavutil` — required for video metadata extraction,
added with video support). Build also needs a C++20 compiler, CMake >= 3.20,
pkg-config, and (for tests) GoogleTest. `sqlite-vec` is vendored; no system package
needed.

| Distro | Install command |
|--------|-----------------|
| Debian/Ubuntu | `apt install build-essential cmake pkg-config libopencv-dev libsqlite3-dev libexif-dev libcurl4-openssl-dev libavformat-dev libavcodec-dev libavutil-dev libgtest-dev` |
| Fedora | `dnf install gcc-c++ cmake make pkgconf-pkg-config opencv-devel sqlite-devel libexif-devel libcurl-devel ffmpeg-free-devel gtest-devel` |
| Arch | `pacman -S base-devel cmake opencv sqlite libexif curl ffmpeg gtest` |
| macOS (Homebrew) | `brew install cmake pkg-config opencv sqlite libexif ffmpeg googletest` |

Notes to confirm during verification:
- Debian: with `img_hash` removed, **`libopencv-contrib-dev` is no longer needed**.
- Arch: stock `opencv` provides the needed core modules once `img_hash` is gone.
- macOS: system `sqlite`/`curl` are used via pkg-config; Homebrew `opencv`/`libexif`/
  `ffmpeg` supply the rest.

### FFmpeg on Fedora (portability caveat)

FFmpeg is the one dependency that isn't cleanly "in every distro's main repos."
Fedora historically excluded full FFmpeg for patent reasons; today its official
repos ship **`ffmpeg-free`** (and `ffmpeg-free-devel`, providing
`libavformat`/`libavcodec`/`libavutil`), which is sufficient to build and run phig's
container-metadata extraction. The fully-featured `ffmpeg-devel` lives in the
third-party **RPM Fusion** repo. Debian/Ubuntu, Arch, and Homebrew all ship FFmpeg
dev packages directly. Verification must confirm phig builds against Fedora's
stock `ffmpeg-free-devel`; if a needed symbol is missing there, document RPM Fusion
`ffmpeg-devel` as the Fedora requirement.

## Implementation

### Modified Files

- `src/hasher.cpp`:
  - Reimplement `compute_phash(const cv::Mat&)` with core `cv::dct` (see algorithm)
  - Remove `#include <opencv2/img_hash.hpp>`
- `CMakeLists.txt`:
  - Drop `img_hash` from `find_package(OpenCV ... COMPONENTS ...)`
  - Add `VERSION 0.1.0` to `project()`
  - `configure_file(src/version.h.in src/version.h)` and add build dir to includes
- `src/main.cpp`:
  - Add `phig --version` / `-v`
- `README.md`:
  - "Build from source" section with the per-distro dependency table above,
    minimum-OpenCV notes, and `cmake -B build && cmake --build build && cmake --install`

### New Files

- `src/version.h.in` — `#define PHIG_VERSION "@PROJECT_VERSION@"`

## Verification

Prove a clean from-source build against stock system packages, outside Nix:

- Docker: `debian:stable`, `fedora:latest`, `archlinux:latest` — install the deps
  above, `cmake -B build && cmake --build build`, run `./build/phig-tests`, and
  smoke-test `phig scan` + `phig duplicates` on a few sample images (and a sample
  video, to exercise the FFmpeg path).
- Fedora specifically: confirm the build succeeds against stock **`ffmpeg-free-devel`**
  (no RPM Fusion). If it fails, document `ffmpeg-devel` from RPM Fusion instead.
- macOS: same build with Homebrew deps, outside the devenv shell.
- Confirm `otool -L` / `ldd` shows the binary linking against system library paths
  (e.g. `/opt/homebrew/...`, `/usr/lib/...`) rather than `/nix/store/...`.
- Confirm the full test suite still passes after the pHash reimplementation
  (phash test values will change — update expected values in `tests/test_hasher.cpp`).

## Deferred (follow-up plan)

Formal packaging definitions, once the source build is proven on each platform:
Homebrew `Formula/phig.rb`, Arch `PKGBUILD`, Debian `debian/`, Fedora `phig.spec`,
and optionally GitHub Actions to attach `.deb`/`.rpm`/tarball artifacts to releases.

CI that builds + tests on every push is covered separately by plan 019 (CI &
Linting), which depends on the `img_hash` removal in this plan.

## Checklist

- [ ] Reimplement `compute_phash` with core `cv::dct` (drop `img_hash`)
- [ ] Remove `<opencv2/img_hash.hpp>` include
- [ ] Drop `img_hash` from CMake OpenCV components
- [ ] Update `tests/test_hasher.cpp` expected phash values
- [ ] Add `VERSION` to `project()` + `version.h.in` + `configure_file`
- [ ] `phig --version` / `-v`
- [ ] README "Build from source" section with per-distro dep tables (incl. FFmpeg)
- [ ] README minimum-OpenCV notes (core vs face recognition)
- [ ] README FFmpeg-on-Fedora caveat (`ffmpeg-free-devel` vs RPM Fusion)
- [ ] Verify build in `debian:stable` container (tests + smoke test)
- [ ] Verify build in `fedora:latest` container against stock `ffmpeg-free-devel`
- [ ] Verify build in `archlinux:latest` container
- [ ] Verify build on macOS with Homebrew deps (outside devenv)
- [ ] Confirm no `/nix/store` paths in a system-package build (`ldd`/`otool -L`)
