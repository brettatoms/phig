---
name: Modern Image Codecs (HEIC/HEIF/AVIF/JXL)
status: in-progress
created: 2026-08-08
description: >
  phig advertises heic, heif, avif and jxl in `known_extensions()` and cannot
  decode any of them — measured, each produces one scan error per file. Adds a
  `core/image_codec` module beside `core/raw`: ImageIO for pixels on macOS,
  libheif + libjxl on Linux, and libheif/libjxl EXIF-box extraction fed to
  libexif on both so the stored JSON blob has identical keys everywhere. The
  pixel split is measured, not assumed — ImageIO decodes 4.5x faster in wall
  time and at 1/14th the CPU of libheif on an M3. Orientation is the risk: the
  three decoders disagree about who applies it. Prerequisite for 049 (Photos
  import), since iPhone photos are HEIC.
---

# 048 — Modern Image Codecs (HEIC/HEIF/AVIF/JXL)

## The defect

`known_extensions()` (`core/scanner.cpp:20-50`) lists 24 image formats. phig
cannot decode several of them. Coverage measured 2026-08-08 by scanning a
directory of files generated with `sips`, against the tree at `b29460f`:

| Decodes (measured) | Fails (measured) | Inferred to fail |
|---|---|---|
| jpg, png, gif, bmp, tiff, jp2 | **heic, avif, psd** | heif, jxl, svg, xcf, ico |

webp is linked into OpenCV and was not tested. The inferred column is inference,
not measurement: `sips` would not produce fixtures for those formats, and the
claim rests on no linked codec existing for them — confirm each before relying
on it.

```
[warning] scan: /tmp/fmt/t.avif    -- Cannot decode image
[warning] scan: /tmp/fmt/base.heic -- Cannot decode image
[warning] scan: /tmp/fmt/t.psd     -- Cannot decode image
[warning] scan: /tmp/fmt/t.heic    -- Cannot decode image
  Processed: 7 (7 images, 0 videos)
  Errors:    4
```

The file is found by the walk, routed to `decode_image_with_warning`, rejected
by `cv::imread`, counted as an error, and never indexed. Homebrew's OpenCV
`imgcodecs` links jpeg-turbo, webp, png, tiff and openjpeg — and no HEIF codec,
though libheif 1.23.1 is installed on the machine as another package's
dependency.

This closes plan 038's open question 3 ("whether OpenCV decodes HEIC at all")
with a measurement: it does not.

### The metadata half

`extract_exif_json` calls `exif_data_new_from_file` (`core/exif.cpp:66`),
libexif's loader for JPEG APP1 segments. HEIC is an ISOBMFF container that
stores EXIF in an `Exif` item, so the loader finds nothing. Fixing pixels alone
would index HEICs with **no date, no camera, no GPS** — which silently breaks
organize-by-date, date sort, and every EXIF search predicate for those files.
Both halves have to land together or the format is worse than unsupported.

## Why this is scheduled now

Plan 049 imports a macOS Photos library. Modern iPhone photos are HEIC. Without
this plan, that import produces a collection phig cannot thumbnail, hash, or
date — a grid of blanks. 048 is a hard prerequisite, and it stands on its own
for the CLI and the GTK client regardless.

## Backend choice

Two decoders, split by role. The pixel split is the measured part.

| | Pixels | EXIF blob |
|---|---|---|
| macOS | ImageIO (all four formats) | libheif + libjxl |
| Linux | libheif (heic/heif/avif) + libjxl (jxl) | libheif + libjxl |

### Measurement behind the pixel split

One 3840x2160 HEIC (`DefaultDesktop.heic`, 6.5 MB, 4:4:4 YCbCr, 8x5 tiles),
8 in-process decodes each, decode forced to completion (libheif to interleaved
RGB; ImageIO drawn into a bitmap context, since `CGImage` is otherwise lazy).
MacBook Air M3, 16 GB, macOS 26, 2026-08-08:

| | libheif (libde265) | ImageIO (VideoToolbox) |
|---|---|---|
| Wall, best of 8 | 188.4 ms | **41.6 ms** |
| Wall, steady state | ~190 ms | ~50 ms |
| CPU per decode | 0.64 s | **0.047 s** |

libheif is not losing because it is single-threaded — it used ~3.3 cores
(1.92 s user against 0.59 s wall over 3 decodes) and still took 4.5x the wall
time. ImageIO hands HEVC to the hardware decoder, which is where the 14x CPU
gap comes from. Under `parallel_for_each`, where every worker is already
competing for cores, libheif's CPU appetite costs more than the wall-clock
ratio alone suggests.

Both benchmarks are checked in as
`docs/plans/2026-08-08-048-heic-decode-bench.c` (libheif) and
`.swift` (ImageIO), so the numbers can be re-derived rather than trusted.

Caveat on extrapolation: the test file is 8.3 MP 4:4:4, while iPhone photos are
~12 MP 4:2:0, which decodes differently per pixel. **The ratio is the finding,
not the absolute times** — and a hardware-versus-software gap is not expected to
invert at other sizes. Re-measure on real Photos-library files during 049.

### Why metadata is unified instead

The EXIF JSON blob is stored in the `metadata` column and queried with
`json_extract()`. If macOS produced Apple's key names and Linux produced
libexif's for the same photo, saved queries would silently return different
results per machine, and a database copied between them would be inconsistent.
That divergence is worse than a slow decode, so both platforms take the same
path: libheif (or libjxl for JXL) reads the raw EXIF block out of the container
— a box read, no decode, cheap — and hands it to `exif_data_load_data`, reusing
the existing libexif mapping untouched.

Confirmed the block is reachable: `heif-info` reports `metadata: Exif: 48 bytes`
on the test file (small because it is a wallpaper; real photos carry a few KB).

The consequence is that macOS links libheif and libjxl too, used only for the
cheap metadata path. Accepted deliberately.

### Rejected

- **libheif everywhere.** One code path and bit-identical phashes across
  platforms, at 4.5x wall and 14x CPU on the platform this work exists to serve.
  Rejected on the measurement.
- **ImageIO properties on macOS** (`CGImageSourceCopyPropertiesAtIndex`). No
  libheif dependency on macOS, but a second mapping from Apple's key names into
  phig's JSON shape, to be kept in lockstep with libexif's forever. This is
  precisely the divergence described above.
- **A hand-rolled ISOBMFF `Exif`-box parser in `core/`.** Dependency-free and
  consistent, but binary parsing of untrusted files, with the fuzzing obligation
  that implies.

## Module

```
core/image_codec.h          interface
core/image_codec_apple.cpp  ImageIO            (macOS pixels)
core/image_codec_libs.cpp   libheif + libjxl   (Linux pixels)
core/image_codec_exif.cpp   EXIF box           (both platforms)
```

```cpp
// True for formats routed to this module: heic, heif, avif, jxl.
bool is_codec_extension(const std::string& ext);

// Decode to an upright BGR cv::Mat for hashing/thumbnail/faces.
// Returns an empty Mat if decode fails. Thread-safe. Never throws.
cv::Mat codec_decode(const std::string& path);

// The container's raw EXIF block, or empty if it has none.
// Thread-safe. Never throws.
std::vector<unsigned char> codec_exif_blob(const std::string& path);
```

The contract deliberately matches `core/raw.h`: empty `Mat` on failure, safe to
call concurrently, no exceptions. `raw_decode` is the precedent for "a format
OpenCV cannot handle gets its own decoder behind a narrow function", and this
is the same shape with a platform split behind it.

ImageIO is a C API, so the macOS implementation is a plain `.cpp` including
`<ImageIO/ImageIO.h>` and linking the framework — no Objective-C++, no `.mm`
files in the build. CoreFoundation objects need explicit `CFRelease`; an RAII
holder confines that to one place.

Platform selection happens in CMake by adding one source file or the other, not
with `#ifdef` inside a shared file.

## Integration — three call sites

1. **`core/hasher.cpp:269`** — `decode_image_with_warning` currently branches
   RAW, then falls through to `cv::imread`. Add a codec branch beside the RAW
   one, with the same warning shape (`"Cannot decode HEIC file: " + path`).
2. **`core/exif.cpp:66`** — for codec extensions, call `codec_exif_blob` and
   `exif_data_load_data` instead of `exif_data_new_from_file`. Everything
   downstream of the `ExifData*` is unchanged.
3. **`core/scanner.cpp:20`** — add `is_codec_extension`; prune `psd`, `svg`,
   `xcf` and `ico` from `known_extensions()` so scan skips them silently, the
   way it already skips `.tga`, rather than reporting an error per file for
   formats nothing links a codec for.

## Orientation is the risk

There is no orientation handling anywhere in `core/` — verified by grep, not
assumed. That is not an oversight: `cv::imread` applies EXIF orientation itself
unless passed `IMREAD_IGNORE_ORIENTATION`. Every existing decode path inherits
upright pixels for free.

**Measured 2026-08-08** on fixtures written at three orientations, correcting
this section's original guess that both backends would need normalizing:

| Decoder | orient_1 | orient_6 (90 CW) | orient_8 (90 CCW) | Needs correction |
|---|---|---|---|---|
| `cv::imread` (JPEG) | 200x100 | 100x200 | 100x200 | n/a — the reference |
| libheif | 200x100 | 100x200 | 100x200 | **no** |
| ImageIO | 200x100 | 200x100 | 200x100 | **yes** |

libheif applies the container's transforms itself and so already matches
`cv::imread`. ImageIO alone returns unrotated pixels — but it reports the
orientation correctly through `kCGImagePropertyOrientation`, and that value
already folds in the container transforms, so applying it reproduces the other
two. The correction therefore lives only in the macOS backend.

Get this wrong and HEICs are sideways on one platform and upright on the other,
with every thumbnail and phash reflecting it, so the decode tests compare
ImageIO's output against libheif's rather than against hardcoded numbers.

Note `gtk/CLAUDE.md`'s existing rule still holds and is now load-bearing:
orientation is applied at thumbnail generation time in `core/`, never in a
client. A client that "helpfully" rotates would double-apply.

## Execution

Bite-sized TDD steps: `docs/plans/2026-08-08-048-modern-codecs.md`.

## Testing

Fixtures checked into `tests/`: a small HEIC, AVIF and JXL, plus a JPEG control
carrying identical EXIF, all derived from one source image so their pixels are
comparable.

- **Decode** — non-empty `Mat`, expected dimensions, BGR channel order, for each
  of the four formats.
- **Failure** — a truncated and a garbage-bytes file of each format return an
  empty `Mat` and do not throw, matching `raw_decode`'s contract.
- **Orientation** — all 8 EXIF orientation values decode upright, on both
  platforms, and match `cv::imread`'s result for the equivalent JPEG.
- **Metadata** — the EXIF JSON extracted from the HEIC equals the JSON from the
  JPEG control, key for key. This is the test that would catch a future
  platform-specific metadata shortcut.
- **Cross-decoder phash agreement** — macOS links libheif for metadata anyway,
  so a test can decode the same file through both ImageIO and libheif on one
  machine and assert the phash Hamming distance is within threshold. This turns
  the platform-divergence risk into a measured bound instead of an assumption,
  and it fails loudly if either backend changes its colour conversion.
- **Scanner** — codec extensions route to the new module; pruned extensions are
  skipped silently and do not appear in the error count.
- **Thread safety** — concurrent `codec_decode` across workers under TSan
  (`OMP_NUM_THREADS=1`, per the root `CLAUDE.md`).

### Real-run check

Scan a directory of real HEICs and confirm: rows indexed, thumbnails generated,
dates populated from EXIF rather than mtime, and thumbnails visually upright.
The existing collection has no HEICs, so this check arrives properly with 049.

## Dependencies

libheif and libjxl on both platforms. Installed and verified on the
development Mac: libheif 1.23.1, libjxl 0.12.0. README per-distro package
lists and the CI workflow both need the two added.

JPEG XL support is asymmetric and worth recording: ImageIO on macOS 26 reads
`public.jpeg-xl` natively (confirmed in `CGImageSourceCopyTypeIdentifiers()`,
62 readable types), so JXL costs nothing on macOS pixels, while Linux needs
libjxl. Both platforms need libjxl for JXL's EXIF box regardless, since libheif
does not handle JXL.

## Open questions

1. **Where orientation normalization lives.** Inside each platform backend, or
   once in a shared helper that takes the decoded `Mat` plus an orientation
   value the backend reports? The shared helper is one implementation to test
   but requires both backends to report orientation identically.
2. **HDR and wide gamut.** iPhone HEICs carry Display P3 and often a gain map.
   Decoding the SDR base image in sRGB is what every other tool shows and what
   phash and face detection want. Is a colour-managed conversion to sRGB needed
   for thumbnail fidelity, or is ignoring the ICC profile acceptable? Untested;
   the wallpaper fixture is sRGB so it does not exercise this.
3. **Multi-image HEIC.** Bursts and Live Photo stills are multi-image
   containers. Taking the primary image is assumed correct; auxiliary and depth
   images are out of scope. Plan 038 (Live Photos) owns the MOV side.
4. **`jp2` overlap.** OpenCV already decodes jp2 via openjpeg. No change
   proposed, but if libheif's AVIF path pulls in a conflicting codec the
   interaction needs checking.

## What this defers

- **psd, svg, xcf, ico** — pruned here, tracked in plan 051 for broader format
  support. Each needs a different library and none blocks anything.
- **Encoding.** phig writes JPEG thumbnails only. Nothing here writes HEIC.
- **Live Photos** — plan 038, which pairs a HEIC still with its MOV. This plan
  makes the still decodable, which 038 needs, but does no pairing.

## Cross-references

- `plans/026-raw-support.md` — the precedent: a format OpenCV cannot handle gets
  a dedicated decoder behind a narrow, never-throwing function.
- `plans/038-live-photos.md` — its open question 3 is answered here.
- `plans/039-core-cross-platform.md` — this plan adds the first deliberate
  platform split in the decode path; 039 owns the rest of the macOS/Windows
  divergence.
- `plans/049-photos-import.md` — the consumer, and the reason for the schedule.
- `plans/051-broader-format-support.md` — where the pruned formats go.
