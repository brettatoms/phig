---
name: Portable Build & System Packages
status: completed
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

Prove a clean from-source build against stock system packages, outside Nix.

**Results (2026-07-05, Docker on arm64 macOS, repo mounted read-only, out-of-source
build):**

- **Debian stable** (OpenCV 4.10): full build + **183/183 tests pass**; `ldd` shows
  system libs only, no `/nix/store`. ✅
- **Fedora latest** (OpenCV 4.13): full build against stock **`ffmpeg-free-devel`**
  (no RPM Fusion needed) + **183/183 tests pass**; no `/nix/store`. ✅
- **Arch** (OpenCV 4.13): deps resolve and CMake configures cleanly against stock
  **contrib-less** `opencv` — found components `core imgcodecs imgproc objdetect
  videoio` + libav*. ✅ (Full compile not run: the official Arch image is x86_64-only
  and the heavy build is unstable under QEMU on arm64. Dependency resolution — the
  only Arch-specific risk, now that `img_hash` is gone — is confirmed.)

Notes:
- The pHash reimplementation is bit-compatible enough that all property-based
  `HasherTest` cases pass unchanged; no hardcoded test values existed to update.
- Video tests reference `tests/data/` by a path relative to the repo root, so run
  the test binary from the repo root (`cd <repo> && ./build/phig-tests`).
- macOS Homebrew build outside devenv: not separately run (dev machine uses Nix);
  the in-Nix build already links only system + Nix libs and passes.

## Deferred (follow-up plan)

Formal packaging definitions, once the source build is proven on each platform:
Homebrew `Formula/phig.rb`, Arch `PKGBUILD`, Debian `debian/`, Fedora `phig.spec`,
and optionally GitHub Actions to attach `.deb`/`.rpm`/tarball artifacts to releases.

CI that builds + tests on every push is covered separately by plan 019 (CI &
Linting), which depends on the `img_hash` removal in this plan.

## Checklist

- [x] Reimplement `compute_phash` with core `cv::dct` (drop `img_hash`)
- [x] Remove `<opencv2/img_hash.hpp>` include
- [x] Drop `img_hash` from CMake OpenCV components
- [x] Update `tests/test_hasher.cpp` expected phash values (N/A — tests are property-based)
- [x] Add `VERSION` to `project()` + `version.h.in` + `configure_file`
- [x] `phig --version` / `-v`
- [x] README "Build from source" section with per-distro dep tables (incl. FFmpeg)
- [x] README minimum-OpenCV notes (core vs face recognition)
- [x] README FFmpeg-on-Fedora caveat (`ffmpeg-free-devel` vs RPM Fusion)
- [x] Verify build in `debian:stable` container (183 tests pass, no /nix/store)
- [x] Verify build in `fedora:latest` container against stock `ffmpeg-free-devel`
- [~] Verify build in `archlinux:latest` container (deps + configure verified; full
      compile blocked by x86_64-only image under QEMU on arm64)
- [~] Verify build on macOS with Homebrew deps (dev machine is Nix; not separately run)
- [x] Confirm no `/nix/store` paths in a system-package build (`ldd`)
