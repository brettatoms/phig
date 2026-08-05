# Does phig's decode path actually crash? — investigation

**Date:** 2026-08-04
**Question:** the workspace architecture states "OpenCV, LibRaw, and FFmpeg segfault on
malformed input... A crash must lose one file, not the app," and that premise is the
primary justification for building `phig-decode` as a separate process. Is it true of
this codebase today?

**Method:** every probe runs one of phig's four real decode entry points —
`decode_image`, `raw_decode`, `extract_video_keyframe`, `compute_phash` — inside a
`fork()`ed child with `alarm()` set, so a segfault is observable as a signal and a hang
is observable as a timeout rather than either killing the harness.

## Result: no crash found

| corpus | probes | crashes | wall time |
|---|---|---|---|
| Synthetic, single-threaded, fork-per-probe | 644 | **0** | 3.3s |
| Synthetic, 16 threads in one process | 644 | **0** | 0.7s |
| Real collection (`/mnt/data/tmp/Photos`), 12 threads in-process | 11,396 | **0** | 311s |

The in-process runs are the harsher ones: a segfault takes the test binary down, so
survival is the signal. Neither died.

Real-collection breakdown, each file routed only to the decoder its extension implies:

| path | files | wall time | clean failures |
|---|---|---|---|
| `decode_image` | 11,206 | 304.2s | 72 |
| `raw_decode` | 177 | 6.0s | **0** |
| `extract_video_keyframe` | 13 | 0.5s | 6 |

**All 177 CR2 files decoded successfully.** The earlier investigation found 62 of these
177 undecodable *through OpenCV's libtiff path*; `raw_decode`'s three-tier fallback
resolves every one via a sidecar JPEG or LibRaw embedded preview. That is the fallback
earning its keep on real data.

### A note on measurement cost

The first version of this harness `fork()`ed once per probe and ran every file through
all four decode paths — 45,584 process spawns, each paying OpenCV/LibRaw first-touch
init, and `cv::VideoCapture` opened against 11k JPEGs for nothing. It was heading for
20+ minutes. One process, a 12-thread pool, and extension-appropriate routing brought
the same coverage to 311s.

The remaining 27ms/image is genuine full-resolution JPEG decode to `cv::Mat`, roughly 37
files/sec at 12 threads. Decode is not free; the harness was simply measuring itself.

### What the synthetic corpus covered (161 files)

- **Truncations** at 1/5/10/25/50/75/90/99% of file length, for JPEG, CR2, and MOV.
- **Bit-flip fuzzing** at four mutation rates (0.1%, 1%, 5%, 20% of bytes), 6 samples
  each, for JPEG and CR2, with headers left intact so decoders engage rather than
  rejecting on magic.
- **Allocation bombs** — PNG `IHDR` declaring `0x7FFFFFFF × 0x7FFFFFFF`, 65535², and
  1M², with a stub `IDAT`.
- **Extension lying** — valid JPEG and MOV bytes placed under 17 exotic extensions
  (`cr3 nef arw dng orf rw2 raf x3f heic avif jxl exr hdr j2k tiff webp pnm`) plus 6
  video ones, to route data into LibRaw, HEIF, and EXR decoders that never expect it.
- **Hostile-but-structurally-valid TIFF/CR2** — minimally valid IFDs with strip offsets
  past EOF (`0xFFFFFF00`), `0xFFFFFFF0` strip byte counts, zero and `0x80000000`
  dimensions, plus big-endian (`MM`) variants to exercise byte-swap paths, and 20
  structurally-fuzzed derivatives of a valid TIFF.
- **Degenerate headers** — empty files, one byte, JPEG SOI only, TIFF header only,
  EXR/HDR magic followed by garbage, HEIF `ftyp` boxes declaring `0xFFFFFFF0` and `0`
  sizes.

### Why the failures are clean

Three things in the existing code appear to be doing the work:

1. **`raw_decode` (`src/raw.h`) is documented "Never throws"** and implements three
   tiers: same-basename sidecar JPEG, LibRaw embedded preview (`unpack_thumb`), then
   full LibRaw decode. Each tier failing falls through to the next; all three failing
   returns an empty `cv::Mat`.
2. **`decode_image` catches OpenCV's internal errors** via a `cv::redirectError`
   handler installed once with `std::call_once`, capturing into `thread_local` storage.
   Undecodable input surfaces as an empty `Mat` plus a warning string.
3. **libraw_r** is used with a fresh instance per call, so there is no shared mutable
   decoder state across threads.

This is consistent with the earlier CR2 investigation (2026-07-11, recorded in memory as
`phig-imread-issue-cr2`): 62 of 177 CR2 files in this same collection are undecodable
OJPEG-compressed TIFFs, they fail **deterministically** under serial, concurrent, and
mutex-guarded execution alike, and the conclusion then was "correct behavior for an
unsupported format — no data loss, no crash."

## Did the code used to crash and get fixed?

No. Checked directly, because "the fallback fixed it" would be a reasonable guess.

`raw_decode` landed 2026-07-12 (`f07218b`), one day after the CR2 investigation — before
that, CR2 files went straight to `cv::imread` with no fallback and no error handler. That
older path, run against all 177 real CR2 files:

```
cv::imread on CR2: ok=115  empty=62  threw=0   exit=0
```

115 + 62 = 177, matching the 2026-07-11 finding exactly. **The unprotected path also
fails cleanly.** The 62 undecodable files were graceful failures then and are decodable
now only because the fallback finds a sidecar or preview — a coverage improvement, not a
crash fix.

The three commits that touched this area did not fix decoder crashes:

| commit | date | what it actually fixed |
|---|---|---|
| `cba3276` | Jul 5 | stderr save/restore race, plus a `std::terminate` escaping `parse_duration` — an **argv** bug |
| `e488e20` | Jul 11 | misattributed warning *text* from a `cv::redirectError` data race — cosmetic |
| `f07218b` | Jul 12 | added `raw_decode`'s three-tier fallback — coverage |

The only genuine crash in that history was `parse_duration` reaching `std::terminate`
from the argument loop. A decode worker pool would not have helped with it.

So the architecture's premise appears to have been written as a defensive assumption
rather than from an observed crash.

## What this does and does not establish

**Does not** prove decode cannot crash. Absence of a crash across 46,872 probes is
evidence, not proof. Untested surfaces remain: genuinely valid exotic RAW formats this
collection lacks (Fuji `.raf`, Sigma `.x3f`, Phase One `.iiq` — only synthetic
extension-lying versions were tried), real HEIC/AVIF/JXL files (the collection has
none), FFmpeg on structurally valid but corrupt containers beyond truncation, and
adversarial input crafted against a specific decoder CVE.

**Does** establish that crash isolation is not a demonstrable requirement for this
codebase against its own real collection and a deliberately hostile synthetic corpus.
The premise should stop driving architecture decisions unexamined.

## Consequence for `phig-decode`

The GTK design (`2026-07-31-phig-gtk-design.md`) justifies `phig-decode` as a separate
process on two grounds. They now have different standing:

- **Crash isolation** — not demonstrable. Defer.
- **`max_dim`-aware decoding** — real and independent of crashes. Decoding a 24MP RAW
  at full resolution to display in a 2560px window is wasted work, and `raw_decode`'s
  tier-2 embedded-preview path already exists to avoid it. But `max_dim` is a
  *parameter on a request*, not a reason for a process boundary. An in-process pool can
  honour it on day one.

**Recommended sequencing:** build the GTK client with decode behind an **async
request/response queue** backed by a thread pool. The queue is the seam. The design
already forbids the GUI from assuming synchronous decode (`get_item` must never block,
cells cancel on unbind) and already requires it to handle failure statuses with a
placeholder and a failure count — so swapping the queue's backend for a subprocess
later replaces one component rather than touching call sites.

Two rules make that swap cheap, and both are decisions to take now:

- Never call `raw_decode`/`decode_image` directly from a widget callback.
- Never let a `cv::Mat` cross into GUI code — it cannot cross a process boundary.

## Reproducing

The harness was temporary and is not committed; `tests/test_hasher.cpp` is unchanged.
To rebuild it, add a test that forks per probe and reads a corpus directory from an
environment variable, then generate the corpus described above. The real-corpus run is
`REAL_CORPUS=/mnt/data/tmp/Photos` and takes roughly 20 minutes for 45,584 probes.
