---
name: Decoder Messages Without Redirecting stderr
status: planned
created: 2026-11-08
description: >
  Remove `StderrSuppressor`, which redirects the whole process's stderr and
  serializes every OpenCV decode behind one mutex. Capture decoder messages
  where they are produced instead: decode JPEGs through libjpeg-turbo with a
  per-call message handler, and route the video decoders' logs through their
  callbacks. Decoded pixels must match `cv::imread` exactly, so stored
  perceptual hashes do not move. 068 depends on this.
---

# 069 — Decoder Messages Without Redirecting stderr

## Problem

`decode_image_with_warning` (`core/hasher.cpp`) wraps `cv::imread` in a
`StderrSuppressor`, and `extract_video_keyframe` wraps the whole video decode
in another. The suppressor has two parts:

- **A redirect.** It points file descriptor 2 at `/dev/null`, to hide messages
  libjpeg writes straight to stderr, such as "Premature end of JPEG file".
- **A mutex.** It holds one process-wide mutex for its lifetime, because
  overlapping save/restore pairs could leave stderr pointing at `/dev/null`.

**The mutex serializes every decode in the process.** Measured 2026-11-08 at
`40d9361`. The setup was two Release builds differing only in whether
`decode_image_with_warning` constructs the suppressor, run against 600 JPEGs
(2.1 GB) sampled from a real collection and copied to tmpfs. The command was
`phig scan --force hash --parallel N`, three runs per cell, on a 12-thread
machine:

| `--parallel` | suppressor | without | on / off |
|---|---|---|---|
| 1 | 50.97 s | 50.91 s | 1.00 |
| 4 | 27.01 s | 14.48 s | 1.86 |
| 12 | 27.10 s | 7.24 s | 3.75 |

- **The redirect itself costs nothing.** The mutex costs everything: scans
  stop getting faster past about two workers.
- **Every decode path pays it,** including `thumbs rebuild`, `face rebuild` and
  gtk's thumbnail loader.

**The redirect hides other people's output.** While any decode runs, nothing
any thread writes to fd 2 appears. In gtk that means GTK's own warnings,
including the ones its real-run check counts. In a libphig host it means the
host's output. Under a sanitizer it means AddressSanitizer's reports. phig's
own log survives only because `core/log.cpp` writes through a duplicate of fd
2 made before any redirect.

**It is also incomplete.** The RAW pixel source decodes JPEGs too, with no
suppressor: `try_sidecar_jpeg` calls `cv::imread` and `try_embedded_preview`
calls `cv::imdecode` (`core/raw.cpp`). Their libjpeg messages already reach
stderr unfiltered.

**JPEG goes through OpenCV on every platform.** Root `CLAUDE.md`'s "on macOS
decode goes through Apple's ImageIO instead" covers only the formats
`is_codec_extension` claims, which are HEIC, HEIF, AVIF and JXL
(`core/image_codec.cpp:19`). This plan therefore applies to macOS as much as to
Linux.

## Decisions

### 1. Measure what prints before deciding what to capture

Nothing records which libraries actually write to stderr during a decode, or
how often. Step 1 finds out. It scans a real corpus with the suppressor
removed, using a mix of JPEG, PNG, TIFF, WebP and video, and classifies every
stderr line by the library that produced it. The decisions below assume
libjpeg dominates and video is second. If the inventory shows otherwise, the
plan is revised before step 2.

### 2. phig decodes JPEG itself

A new `core/jpeg_decode.{h,cpp}`:

```cpp
// Decodes a JPEG to 8-bit BGR, applying EXIF orientation as cv::imread does.
// Never throws, never writes to stderr. On failure img is empty; warning holds
// every message libjpeg emitted, in order, whether or not decoding succeeded.
DecodeResult decode_jpeg_file(const std::string& path);
DecodeResult decode_jpeg_memory(std::span<const unsigned char> bytes);
```

- **The libjpeg API, not TurboJPEG.** OpenCV's JPEG decoder is written against
  the libjpeg API. Using the same API means its decode settings map one to one
  onto ours: DCT method, upsampling, and CMYK/YCCK and grayscale handling.
- **Each call has its own error manager.** `output_message` appends to the
  call's warning, and `error_exit` longjmps out to return an empty image. No
  global state is touched, so concurrent decodes need no lock, and a message
  belongs to the decode that produced it.
- **EXIF orientation follows `cv::imread` with `IMREAD_COLOR`,** which applies
  orientation tags 2 to 8.
- **All three JPEG paths use it:** `decode_image_with_warning` for `.jpg` and
  `.jpeg`, and both RAW tiers in `core/raw.cpp`.
- **Selection is by content, not extension.** A file is treated as JPEG when it
  starts with the JPEG SOI marker, `FF D8 FF`, as `imread` itself chooses. A
  `.jpg` that is really a PNG then still decodes.
- **Every other format stays on `cv::imread`, unsuppressed.** If the inventory
  shows one of their libraries is noisy, it gets its own capture by the same
  pattern. Until then, anything they print reaches stderr, which is the honest
  default.

**Pixels must match `cv::imread` exactly.** Scans store a perceptual hash
computed from these pixels, and duplicate detection compares new hashes
against stored ones. If the decoder changes even one pixel value, a
`--force hash` rescan can move a stored hash. The tests below require
byte-identical output against `cv::imread` on the same machine.

### 3. Video logs go through their callbacks

`extract_video_keyframe` decodes through `cv::VideoCapture`. Here OpenCV's
videoio links both FFmpeg and GStreamer, and which backend opens a file depends
on the build. phig already discards FFmpeg's log process-wide with
`av_log_set_level(AV_LOG_QUIET)` (`core/metadata.cpp:62`,
`core/live_photo.cpp:49`).

The inventory decides what remains. The expected shape is:

- **FFmpeg:** one `av_log_set_callback`, installed once, that appends to a
  thread-local buffer while a decode on that thread is in progress and drops
  everything else. It replaces both `AV_LOG_QUIET` calls.
- **OpenCV's own log**, which reports backend warnings such as GStreamer's:
  `cv::utils::logging::setLogLevel`, set once to errors only.

Both are process-wide settings, but they are callbacks and levels, not a
redirect of fd 2. They suppress or capture only the messages of the library
that owns them.

### 4. The suppressor is deleted

`StderrSuppressor` and its mutex go, with nothing left that touches fd 2.
`core/log.cpp`'s comment about writing through a duplicate of fd 2 "even while
StderrSuppressor redirects" no longer has a reason. The duplicate stays only if
something else needs it. 068's planned `set_suppress_decoder_stderr` switch is
not needed, because there is nothing left to switch.

### 5. Build

libjpeg-turbo becomes a direct dependency, found with CMake's `FindJPEG`. It
is already present wherever OpenCV is, since OpenCV links it, but phig now
links it itself:

- **ubuntu:** add `libjpeg-dev` to both CI install lists and `README.md`'s apt
  line.
- **macOS:** add `jpeg-turbo` to the brew lines.
- **Fedora and Arch:** add the package to `README.md`'s lines.
- **Windows (039):** add libjpeg-turbo to its dependency list.

## Steps

1. **Inventory (decision 1).**
   - Build with the suppressor removed.
   - Scan a mixed corpus copied from a real collection, read-only, into a
     scratch catalog, with stderr captured.
   - Classify the lines by library and record the counts in this plan. Revise
     decisions 2 and 3 if the counts disagree with them.
2. **The JPEG decoder, test first (decision 2).** Add fixtures under
   `tests/data/jpeg/`:
   - baseline, progressive, grayscale, CMYK with an Adobe marker, 4:2:0 and
     4:4:4 sampling;
   - EXIF orientations 1 to 8;
   - a truncated file;
   - a non-JPEG with a `.jpg` name.

   Tests, each comparing against `cv::imread` or `cv::imdecode` on the same
   input:
   - identical bytes, dimensions and type for every decodable fixture, from
     both a file and memory;
   - for the truncated file, the same partial image `imread` returns, with the
     libjpeg message in `warning`;
   - two threads decoding different truncated files each get only their own
     message.
3. **Pixel identity on a real corpus.** A test can't carry a real collection, so
   this is a recorded check, not a CI test. Decode every JPEG in a copied
   sample of at least 5,000 files both ways and count mismatches; the expected
   count is zero. Record the result here.
4. **Switch the call sites.** Move `decode_image_with_warning` and `core/raw.cpp`
   onto the new decoder. The existing scan, thumbnail and faces tests must pass
   unchanged.
5. **Video (decision 3)**, as the inventory shaped it, with a test that a
   truncated video's messages reach its keyframe result and not stderr.
6. **Delete the suppressor (decision 4).** Add a test: while another thread
   decodes a truncated JPEG in a loop, a write to fd 2 from the test thread
   arrives through a pipe. This fails today.
7. **Re-measure.** Repeat the table above on the same kind of corpus. The
   expectation is the "without" column. Record it.
8. **Docs.**
   - Root `CLAUDE.md`: say which formats decode through phig's own code.
   - `README.md` and CI: package lists.
   - `core/log.cpp`'s comment.

## Risks

- **OpenCV versions differ across CI.** ubuntu-latest ships OpenCV 4.6.0,
  ubuntu-26.04 ships 4.10.0, and macOS has Homebrew's current version. If
  OpenCV changed its JPEG settings between them, `imread` already gives
  different pixels on different machines, and "match `imread`" means a
  different thing on each leg. The identity test runs on every leg, so a
  difference shows up as a failure there. If it happens, the decision is
  which version's output phig's decoder pins, and that is recorded here.
- **Orientation edge cases.** Files with several or malformed EXIF orientation
  tags are where a reimplementation is most likely to differ from OpenCV. The
  fixtures cover the eight valid values. Step 3's corpus check is what covers
  the rest.

## Later, not this plan

Owning the JPEG decode makes libjpeg's scaled decode available, which reads at
1/2, 1/4 or 1/8 size. That could cut thumbnail and perceptual-hash time
sharply. It changes pixel values, and so stored hashes, which makes it a
decision of its own.

## Relationship to other plans

- **068:** depends on this. It no longer needs its stderr switch.
- **039:** the Windows port gains libjpeg-turbo as a dependency.
- **067:** step 1's inventory scan uses a scratch catalog. If 067 has landed,
  that is `--catalog`; otherwise it is `--db` with the XDG directories
  redirected.
