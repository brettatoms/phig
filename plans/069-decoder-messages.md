---
name: Decoder Messages Without Redirecting stderr
status: completed
created: 2026-11-08
completed: 2026-11-09
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
how often. Step 1 finds out. It scans step 0's corpus with the suppressor
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
- **The result reports its frame.** `DecodeResult` gains a `DecodeFrame`: which
  source was decoded (the file itself, a RAW's sidecar JPEG, its embedded
  preview, or a full RAW decode), the decoded width and height, and, for the
  sidecar JPEG and the embedded preview, the sha256 of the bytes decoded. The
  file itself and a full RAW decode read the image file's own bytes, which the
  image row's sha256 already identifies. Plan 060's open question 7 decision
  records this frame with each face; this plan is where the decoder learns to
  report it.
- **Every other format stays on `cv::imread`, unsuppressed.** The inventory
  found libtiff and libpng noisy only on malformed files, which test suites
  build on purpose: 10 libtiff lines and no libpng lines on photo and video
  sources. They get no capture of their own. OpenCV's own log is set to errors
  only (decision 3), which silences what OpenCV routes through it; what libtiff
  or libpng write directly reaches stderr, and says something true about a
  broken file.

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

0. **The corpus.** phig is a general-purpose tool, so steps 1, 3 and 7 run on a
   corpus assembled from public sources, not on one person's collection. It
   covers cameras, phones and editors from several makers, every EXIF
   orientation, progressive and baseline files, grayscale and CMYK, and
   damaged files. Candidate sources: `ianare/exif-samples` and
   `recurser/exif-orientation-examples` on GitHub, libjpeg-turbo's
   `testimages`, raw.pixls.us for RAWs with embedded previews, and Wikimedia
   Commons for volume across cameras and editors. The list of what was
   fetched, with URLs, is recorded here; the files themselves are not
   committed. A copy of a personal collection may be added to it, never used in
   its place.
1. **Inventory (decision 1).**
   - Build with the suppressor removed.
   - Scan the corpus from step 0 into a scratch catalog, with stderr captured.
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
3. **Pixel identity on the corpus.** A test can't carry the corpus, so this is
   a recorded check, not a CI test. Decode every JPEG in step 0's corpus both
   ways, including the RAW previews and sidecars, and count mismatches; the
   expected count is zero. Record the result here. The target was 5,000 JPEGs;
   the corpus holds 687, because Wikimedia Commons throttled the fetch, so the
   result records the shortfall and the check is worth repeating on a larger
   corpus.

   **Result, 2026-11-09, OpenCV 5.0.0, built at `5b9e4c9` plus the test.** The
   test is `JpegCorpus.EveryJpegMatchesOpenCv` in `tests/test_jpeg_decode.cpp`;
   it skips unless `PHIG_JPEG_CORPUS` names a directory. It checked 718 JPEGs
   by signature: 674 from the corpus and 44 RAW previews. `simple_dcraw -e`
   extracted 56 previews from the 154 RAWs, of which 44 were JPEG and 12 were
   PPM bitmaps, which the signature check skips. The other 98 RAWs yielded no
   preview. Mismatches against `cv::imread` through `decode_jpeg_file`: 0.
   Mismatches against `cv::imdecode` through `decode_jpeg_memory` on the same
   bytes: 0. The check ran on 718 files against the 5,000 target, so the
   shortfall stands and the check is worth repeating on a larger corpus. The
   preview JPEGs are in `~/phig-corpus/previews/`, outside the repository.
4. **Switch the call sites.** Move `decode_image_with_warning` and `core/raw.cpp`
   onto the new decoder. The existing scan, thumbnail and faces tests must pass
   unchanged.
5. **Video (decision 3)**, as the inventory shaped it, with a test that a
   truncated video's messages reach its keyframe result and not stderr.
6. **Delete the suppressor (decision 4).** Add a test: while another thread
   decodes a truncated JPEG in a loop, a write to fd 2 from the test thread
   arrives through a pipe. This fails today.
7. **Re-measure.** Repeat the table above on step 0's corpus, both before and
   after this plan, since the table above came from one collection. The
   expectation is that the "without" column's scaling holds. Record it.

   **Result, 2026-11-09.** Two Release builds, one at the branch's base
   `d74485b` (the parent of the branch's first commit, `4e78a52`), which still
   has `StderrSuppressor`, and one at `467c7ee`, the head: the commit measured, not the branch tip,
   which has later commits. Each ran in its own
   git worktree. The two differ by this branch's commits: the plan 069 and 060
   documents, the decoder and its tests, the video log capture, and the
   suppressor's deletion. The machine is an AMD Ryzen 5 5600X (12 threads,
   32 GB), and the files were on tmpfs. The command was
   `phig --catalog <scratch> scan /dev/shm/phig-bench --force hash --parallel N`,
   after one plain scan to create each catalog, with `HOME` and the four XDG
   directories in a scratch directory. Three runs per cell, median shown, base
   and head alternating. Both builds reported 40 errors and 619 rows from the
   plain scan.

   The corpus is 659 JPEGs by signature, 974 MB: the 619 undamaged files and
   the 40 damaged copies, from the sources in step 0, with no RAW previews.
   The 2026-11-08 table above is 600 JPEGs, 2.1 GB, from one personal
   collection, so the two tables differ in source and in file size (about
   1.5 MB per file here, 3.5 MB there) and are not like for like.

   | `--parallel` | base (suppressor) | head | base / head |
   |---|---|---|---|
   | 1 | 21.13 s | 21.12 s | 1.00 |
   | 4 | 12.98 s | 6.38 s | 2.03 |
   | 12 | 13.35 s | 3.71 s | 3.60 |

   Raw times, base then head, in seconds. `--parallel 1`: 21.30, 21.13, 20.96
   and 21.12, 21.20, 20.80. `--parallel 4`: 12.98, 12.91, 13.09 and 6.34, 6.45,
   6.38. `--parallel 12`: 13.15, 13.68, 13.35 and 3.29, 3.71, 3.75.

   The head goes from 21.12 s at one worker to 3.71 s at twelve, 5.7 times; the
   base goes from 21.13 s to 13.35 s, 1.6 times, and does not improve between
   4 and 12 workers. The 2026-11-08 "without" column scaled 7.0 times over the
   same range. The machine was not idle: another process, not phig, held the
   load average at 9.1 before the runs and 6.6 after, and between 4.6 and 10.8
   during them. The head's times may be slightly pessimistic. Base and head
   alternated, so both saw the same load.

   The expectation that the "without" column's scaling holds was met in kind
   and not in size: it was 7.0 times on the one collection and is 5.7 times on
   this corpus, and the difference is the corpus, as above.

   At `--parallel 1` the head matches the base, so the decoder is not slower
   per file.
8. **Docs.**
   - Root `CLAUDE.md`: say which formats decode through phig's own code.
   - `README.md` and CI: package lists. Done in Task 2 of the execution
     plan, commit `840e72b`.
   - `core/log.cpp`'s comment. Done in Task 6, commit `8805780`.

## Outcome

Facts that came out of the implementation and that the decisions above do not
state.

- **The EXIF rule follows the linked OpenCV.** The decoder reads
  `cv::getVersionMajor()` at runtime. On 4.x it takes the orientation from the
  first APP1 marker only. On 5.x it takes it from every `Exif\0\0` APP1 marker.
  Pixels therefore match `cv::imread` on each machine. Only the 5.x branch has
  run here (OpenCV 5.0.0); the first run of the 4.x branch is CI's ubuntu legs,
  at 4.6 and 4.10.
- **Truncated data decodes to an empty image from memory.**
  `decode_jpeg_memory` returns an empty image when the data is truncated, at
  every OpenCV version: 4.x `imdecode` returns uninitialised rows there, and
  5.0.0 returns empty. A RAW's sidecar JPEG is decoded with file semantics
  (`decode_jpeg_file`, `decode_jpeg_file_bytes`) and its embedded preview with
  memory semantics, matching the `imread` and `imdecode` calls they replaced.
- **What `warning` holds.** The first warning libjpeg would have printed
  through its default handler, plus any fatal error. Later warnings are not
  kept, as with the default handler. This narrows decision 2's "every message".
- **Video keyframes open with `cv::CAP_FFMPEG`** when the OpenCV build has it,
  and with `CAP_ANY` otherwise. A video FFmpeg cannot open gets no keyframe,
  even if another backend could open it. The reason is that one backend makes a
  video's keyframe independent of the OpenCV build, and keeps GStreamer's GLib
  output off stderr: disposing the pipeline of a truncated file prints a
  GStreamer-CRITICAL there.
- **OpenCV's log level** is set to errors once, before any OpenCV decode, by
  `ensure_opencv_log_level` in `core/hasher.h`.
- **Pixel identity** was checked on 718 JPEGs against OpenCV 5.0.0 only (step
  3). It assumes phig and OpenCV link the same libjpeg-turbo. That is unproven
  where OpenCV bundles its own copy, such as Windows or vcpkg builds, and a
  different libjpeg-turbo build could differ in IDCT output. Homebrew's
  jpeg-turbo link on macOS is unverified until a manual macOS CI run.
- **Decoder warnings now reach phig's own log.** A decoder message is reported
  as `[warning] <path> -- <message>` on stderr and in the log, where the
  suppressor hid it. This follows decision 2: messages are reported against
  their file rather than hidden. The plain scan of the 659-file measurement
  corpus wrote 283 stderr lines at the head and 40 at the base. The base's 40
  contain no blank lines. At the head, 124 are blank lines and 159 are
  `[warning]` lines. `decode_image` writes one `\n` to `std::cerr` before each
  decoder message, to break out of the scan's progress line, which accounts
  for 119 of the blank lines: re-running the head's scan at `--parallel 1`
  and 2 gave exactly 119 blank lines. At the default worker count the blank
  count varied from run to run (119 to 124 over eight runs at 12 workers), the extra ones
  falling next to other blank lines or before a `Cannot decode image` line.
  The source of those extra newlines was not found. Of those 159, 40
  are `Cannot decode image` for the same 40 undamaged files that fail at the
  base (the base's 40 lines are exactly these). The other 119 are decoder
  messages, each on a different file: no file produced more than one. 38 come
  from the 40 damaged copies (the other 2 printed none) and 81
  from undamaged files; none of the 40 undecodable files also produced one.
  By message: 24 `Premature end of JPEG file`, 30 `Corrupt JPEG data` with
  extraneous bytes before a marker, 10 `Corrupt JPEG data: premature end of
  data segment`, and 55 others, either one other message or several joined by
  `; ` (for example `Invalid component ID N in SOS`, `Bogus DAC index N`). These counts
  come from one corpus, which is full of deliberately malformed files, and say
  little about how often a photo collection prints them.

## Inventory

Run 2026-11-09 at `4e78a52`. The build differs from the tree in two ways, neither
committed: `StderrSuppressor`'s constructor and destructor are empty, and both
`av_log_set_level(AV_LOG_QUIET)` calls (`core/metadata.cpp`, `core/live_photo.cpp`)
are `AV_LOG_WARNING`, because with FFmpeg's log silenced process-wide the
inventory could not see its messages. The command was
`phig --catalog <scratch> scan /home/brett/phig-corpus --recursive --media all --hidden`
with `HOME` and the four XDG directories in a scratch directory and stderr
redirected to a file. No `--faces`.

**The corpus** is 6.2 GB: 5,671 files that are not text, of which 2,618 are
files the scanner takes as images or video, and 687 carry a `.jpg`/`.jpeg`
extension (40 of those are damaged copies made locally). It fell short of the
5,000 JPEGs step 0 asks for. The reachable public sources held fewer, and
Wikimedia Commons answered 429 with `Retry-After: 600` after about 30 MB and kept
doing so for the rest of a 30-minute attempt, so it contributed 25 files, of
which 8 are JPEG. It contributed no video and no WebP, although step 0 asks for
both; the FFmpeg FATE suite and exiftool's `t/images/` supplied the videos
instead. Most of the corpus comes from `drewnoakes/metadata-extractor-images`
(2.9 GB, 430 JPEGs, and many PNG, TIFF, GIF, BMP and fuzzing files) and
raw.pixls.us (154 RAWs, 77 makes). The scan processed 2,088 files (2,018 images, 70
videos), skipped 5 and reported 525 errors, mostly formats OpenCV cannot decode.

**Lines on stderr:** 8,104 in total, classified by the library that wrote them.
"Files" is the number of files that produced at least one line of that kind,
measured by scanning each file alone into its own directory.

| Library | Lines | Files | Example |
|---|---|---|---|
| libtiff (through OpenCV's TIFF reader) | 5,930 | 238 | `TIFF_Warning TIFFFetchNormalTag: ASCII value for tag "Make" does not end in null byte` |
| FFmpeg | 561 | 23 | `[mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] st: 0 edit list: 1 Missing key frame while searching for timestamp: 0` |
| libpng | 296 | 190 | `libpng warning: Invalid bit depth in IHDR` |
| OpenCV's own log (`[ERROR:`, `[ WARN:`, `> ` continuations) | 217 | 105 | `imread_('...gif'): can't open/read file` |
| libjpeg | 108 | 108 | `Premature end of JPEG file`, `Corrupt JPEG data: N extraneous bytes before marker 0xd9` |
| GStreamer (through OpenCV) | 13 | 4 | `GStreamer warning: Embedded video playback halted` |
| libwebp | 0 | 0 | |
| phig's own `[warning]` | 670 | 525 | `[warning] ... Cannot decode image` |
| unclassified | 6 | | OpenJPEG and RAW reader lines |
| blank (spacing inside multi-line messages) | 303 | | |

The FFmpeg line count is not stable: the same corpus gave 561 lines in the full
scan and 678 summed over per-directory scans. The table above uses the full scan,
561, as the canonical figure. The split below comes from the per-directory scans,
so its FFmpeg cells sum to 678, not 561. The libjpeg, libtiff, libpng, OpenCV and
GStreamer counts were identical across the two runs.

**By source.** libjpeg's 108 lines: 38 from the damaged copies, 61 from
`metadata-extractor-images/jpg`, 7 from `Pillow`, 1 from `exif-samples`, 1 from a
RAW preview. So about 70 lines in roughly 650 real JPEGs, one line per affected
file. libtiff's 5,930 lines come from the TIFF files in two test suites:
5,375 from `metadata-extractor-images/tif` and 545 from `Pillow`. libpng's 296 come
almost entirely from `metadata-extractor-images/png` and `Pillow`, which hold
deliberately malformed files. In the per-directory scans (678 FFmpeg lines),
517 come from `ffmpeg-fate`, which is also a conformance suite, and 161 from the
other video sources.

Splitting the corpus by what its sources are for:

| | libjpeg | FFmpeg | libtiff | libpng | OpenCV | GStreamer |
|---|---|---|---|---|---|---|
| Photo and video sources (`exif-samples`, `exif-orientation-examples`, `libjpeg-turbo`, `exiftool`, `raw-pixls-us`, `metadata-extractor-images` jpg, raw and video directories) | 63 | 161 | 10 | 0 | 0 | 6 |
| Damaged JPEG copies | 38 | 0 | 0 | 0 | 0 | 0 |
| Format and codec test suites (`Pillow`, `ffmpeg-fate`, `metadata-extractor-images` tif, png, gif, bmp, jp2) | 7 | 517 | 5,920 | 296 | 217 | 7 |
| Sum of per-directory scans | 108 | 678 | 5,930 | 296 | 217 | 13 |

**Result against decisions 2 and 3.** The decisions assume libjpeg is the
largest group and video is second. Neither holds on this corpus. By lines in the
full scan, libtiff is first (5,930), FFmpeg second (561) and libpng third (296);
libjpeg is fifth, ahead of only GStreamer and libwebp, not counting phig's own
`[warning]` row, which is not a library. By files, libtiff (238) and libpng (190)
also lead libjpeg (108). The libtiff, libpng and most of the FFmpeg lines come
from test suites built to be malformed or to exercise codecs. On the photo and
video sources alone, FFmpeg has 161 lines (per-directory scans) against libjpeg's
63, so video is still ahead of libjpeg there, by a smaller margin; libjpeg's 108
lines are one line per file. libtiff prints 10 lines on those sources and libpng
none. This plan's decisions are unchanged here; whether they stand, or whether decision 2's
"every other format stays on `cv::imread`, unsuppressed" needs a TIFF or PNG capture
before the suppressor is deleted, is open.

**Corpus contents.** `/home/brett/phig-corpus/SOURCES.md` lists every file
fetched with its URL and a count per source. It is not committed. The sources
are:

- https://github.com/ianare/exif-samples (91 JPEG)
- https://github.com/recurser/exif-orientation-examples (18 JPEG, orientations 1 to 8)
- https://github.com/libjpeg-turbo/libjpeg-turbo, `testimages/` only
- https://github.com/drewnoakes/metadata-extractor-images (430 JPEG, 26 video)
- https://github.com/exiftool/exiftool, `t/images/` only (41 JPEG, 8 video)
- https://github.com/python-pillow/Pillow, `Tests/images/` only (55 JPEG)
- https://github.com/mozilla/mozjpeg, `test/` only (no images)
- https://raw.pixls.us/data/, 154 RAWs from 77 makes (listed in `SOURCES.md`)
- https://fate-suite.ffmpeg.org/, 54 videos (16 mp4, 15 webm, 7 mkv, 7 mov, 3 flv, 3 ts, 2 mpg, 1 avi)
- https://commons.wikimedia.org/, 25 files through the API (8 JPEG, 16 PNG, 1 TIFF), originals
- Of the 88 video-extension files on disk (26 + 8 + 54), the scan processed 70 as video; the rest were skipped or counted among the errors.
- 40 damaged JPEGs derived locally: 20 truncated, 20 with 20 bytes flipped

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
- **060:** built together with 060's open question 7 decision, on one branch.
  This plan adds the decode frame to `DecodeResult`; 060 records it with each
  face. The embedded preview stays unrotated here, because pixels match
  `cv::imdecode`; turning it upright moves stored hashes and is its own
  finding.
- **039:** the Windows port gains libjpeg-turbo as a dependency.
- **067:** step 1's inventory scan uses a scratch catalog. If 067 has landed,
  that is `--catalog`; otherwise it is `--db` with the XDG directories
  redirected.
