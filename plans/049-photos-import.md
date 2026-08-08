---
name: macOS Photos Library Import
status: designing
created: 2026-08-08
description: >
  Import an Apple Photos library into a phig collection: PhotoKit enumerates
  assets, exports the original resources into a dated directory tree, and the
  existing libphig index operation indexes them. Photos is never mutated and
  never read through its package — TCC forbids it and the layout is private.
  Because PhotoKit requires a TCC-authorized bundle, this plan necessarily
  creates the macOS app target, the XcodeGen project, and the Swift-to-libphig
  interop layer that 050 then builds the grid on. Depends on 048: iPhone photos
  are HEIC.
---

# 049 — macOS Photos Library Import

## What it does

Copies the originals out of `~/Pictures/Photos Library.photoslibrary` into a
normal directory the user chooses, then indexes that directory with phig's
existing index operation. After import, the collection is an ordinary phig
collection of ordinary files. Photos keeps its library untouched.

## Why export rather than index in place

Indexing files inside the `.photoslibrary` package was considered and rejected
on four independent grounds, any one of which is disqualifying:

- **TCC forbids it.** Measured on this machine: `ls` on the package returns
  `Operation not permitted`, and opening `database/Photos.sqlite` returns
  `authorization denied`. Only PhotoKit, from an authorized bundle, gets in.
- **The layout is private.** `originals/` and its hashed subdirectory structure
  are an implementation detail Apple changes between releases.
- **The files may not be there.** With iCloud Photos and "Optimize Mac Storage",
  originals live in iCloud and the local file is a smaller rendition, or absent.
- **phig would mutate the library.** `phig mv` and `phig rm` operate on indexed
  paths. Pointing them at a Photos library means phig could move or trash files
  out from under Photos, corrupting it. This alone settles it.

A third model — referencing assets by PhotoKit `localIdentifier` without
copying — was also rejected. phig's identity model is the filesystem path:
scanner, hasher, organize and remove all assume one. Introducing a
non-path-backed asset would touch every one of them, for a macOS-only feature.

The cost of exporting is honest and must be stated to the user: **it duplicates
the storage.** A 200 GB Photos library becomes 200 GB more on disk.

## Dependencies: 048 (shipped) and 038 (not yet)

**048 shipped 2026-08-08.** It was a hard prerequisite and the probe shows why:
**92% of the measured library is HEIC** (1,739 of 1,894). Importing before 048
would have produced 1,739 rows with no thumbnail, no phash and no date.

**038 is now also a prerequisite.** Live Photos are 21% of the measured library,
and they are to be treated the way a RAW is treated with its sidecar — always
paired, still indexed, MOV a carried attachment. That is 038's model, so this
plan cannot export them correctly until it lands. 038's own blockers cleared on
the same day: 048 made the still decodable, and this probe supplies real files.

## What the probe measured

A read-only PhotoKit survey (`macos/PhigProbe`) run 2026-08-08 against a
1,894-asset library. **Read the two halves below differently.** The first
generalizes; the second describes one library and must not be used to scope the
feature — a 200k-asset library curated over twenty years will look nothing like
it.

### Capability findings — these generalize

- **`PHAssetResource.fileSize` is readable** (via KVC; not public API, so read
  defensively). An up-front size estimate is therefore possible, which settles
  open question 4.
- **PhotoKit reports Live Photo pairing directly**, so the importer never has to
  infer it from filenames.
- **Apple's Content Identifier survives export in both halves.** Verified on
  four exported pairs: the MOV's `com.apple.quicktime.content.identifier`
  appears in its HEIC every time, and a Live still carries exactly one more
  UUID-shaped string than an ordinary still. This is what makes 038's
  identifier-based pairing work on exported files rather than only on originals.
- **`.fullSizePhoto`/`.fullSizeVideo` marks an edited asset**, so "was this
  adjusted in Photos" is detectable per asset.
- **Paired videos are a large fraction of the disk cost.** Measured: originals
  4.63 GB, paired videos 1.35 GB — pairing costs **29% more** than stills alone.
  A size estimate that counts only `.photo`/`.video` understates the import,
  and an early version of this probe made exactly that mistake.
- **iCloud-only originals are a real state, not a corner case.** Every one of 40
  sampled assets required downloading, consistent with "Optimize Mac Storage".
  Design for download-by-default.

### One library's numbers — do NOT scope from these

1,894 assets: 92% HEIC, 21% Live Photos, 8 albums, 0 photos in more than one
album, 7 edited, 0 shared or iTunes-synced, dated 2025-01 to 2026-08.

Useful for end-to-end validation, useless for deciding whether the
album-to-gallery collision rule matters — it plainly does for someone, and this
library simply does not exercise it.

## This plan creates the app

PhotoKit is only available to a bundled, TCC-authorized application — a command
-line tool cannot reliably obtain Photos authorization. So the import cannot be
a script, and this plan necessarily lands the macOS app foundation that plan
050 was nominally going to introduce:

- `macos/project.yml` — XcodeGen spec, checked in; the `.xcodeproj` is generated
  and gitignored. Chosen over a checked-in `.xcodeproj` because a `pbxproj` is
  an opaque blob that diffs badly and is error-prone to edit.
- `Phig.app` bundle: bundle identifier, `NSPhotoLibraryUsageDescription`,
  hardened runtime, development signing.
- **The Swift-to-libphig interop layer** — a `systemLibrary` module map over
  `<phig/phig.h>`, plus a thin Swift wrapper that owns handle lifetimes
  (`phig_ctx`, `phig_query`, `phig_op`) in `deinit`, converts `phig_status` into
  thrown Swift errors, and reads `phig_last_error` **on the thread that failed**
  — it is thread-local, per `libphig/CLAUDE.md`.
- A minimal window: pick a library, choose a destination, run the import, watch
  progress. No grid, no viewer — those are 050.

The app must include `<phig/phig.h>` and nothing else. The rule in
`gtk/CLAUDE.md` applies unchanged to this client: no `core/` headers, no
linking `phig-core`.

## The import pipeline

```
authorize → enumerate → export originals → index → report
```

**Authorize.** `PHPhotoLibrary.requestAuthorization(for: .readOnly)`. Read-only
is the correct privilege: nothing here writes to Photos. Handle the denied and
limited-access cases with a real explanation, not a dead end.

**Enumerate.** `PHAsset.fetchAssets(with:)` gives the count cheaply and a
cursor to iterate. Report the count before any bytes move.

**Export.** For each asset, `PHAssetResource.assetResources(for:)` and select
the original resource, then `PHAssetResourceManager.requestData` with
`isNetworkAccessAllowed = true`. `originalFilename` supplies the name. Writing
the original resource byte-for-byte is what makes phig's SHA256 and EXIF
meaningful — a re-encoded export would defeat both.

**Index.** Run `phig_op_index_start` over the destination with thumbnails
enabled, then poll. This is plan 031's operation, already shipped; the import
adds no new indexing path.

**Report.** Counts exported, skipped, failed, with reasons retained per
`phig_op_error_at`.

### Destination layout is not flat

Export into `<destination>/YYYY/MM/` keyed on `PHAsset.creationDate`, not into
one directory.

This is a correctness constraint, not taste. Plan 044 measured
`has_sibling_raw` giving every scanned file a full parent-directory listing,
making scan quadratic in directory size: 4,000 files cost 7.33 s in one
directory versus 2.03 s split across four. Exporting 30,000 files into a single
directory would walk straight into that. Dated subdirectories keep each one
small, and match what `phig mv`'s default format would produce anyway.

### Incremental re-import

Re-running the import must not re-export everything. The app keeps a state file
in its container mapping PHAsset `localIdentifier` to exported path, and skips
assets already present. Relying on phig's SHA256 deduplication instead would
work only *after* paying the download and write cost, which is exactly what
needs avoiding for an iCloud library.

The state file is app-owned, not a phig concept: the ABI has no notion of a
foreign asset identifier, and adding one for a macOS-only feature is not
warranted.

## iCloud is the hard part

If the library uses "Optimize Mac Storage", originals are not on disk.
`isNetworkAccessAllowed = true` downloads them one by one — for a large library
that is hours, gigabytes of transfer, and a process that will be interrupted.

Requirements that follow:

- Detect and report the situation **before** starting, not at file 4,000.
- Per-asset progress, since a single download can take seconds.
- Cancellation that stops promptly and leaves a consistent state file.
- Resume: a cancelled or crashed import continues where it left off.
- Never silently substitute a smaller rendition for an unavailable original —
  fail that asset, record the reason, and keep going.

The development machine's library state is unknown (TCC blocks inspection from
a shell), so this is designed defensively rather than from measurement. First
real evidence arrives when the app can ask PhotoKit.

## What comes across besides pixels

Photos knows things EXIF does not, and phig has fields for some of them.
Proposed for this plan, all through existing libphig entry points:

| Photos | phig | Via |
|---|---|---|
| `PHAsset.isFavorite` | `favorite` tag | `phig_favorite_set` |
| Album membership | gallery | `phig_gallery_set` |

Both are cheap, need no ABI change, and carry over organizational work the user
already did. A photo belongs to one gallery in phig but can be in many Photos
albums — the collision needs a rule (see open questions).

Deferred: Photos' People. phig's face model is 128-D embeddings with its own
clustering; Photos exposes named clusters but not embeddings. Mapping one onto
the other is a research problem, not an import detail.

## Testing

PhotoKit cannot be exercised in the existing GoogleTest suite — it needs a
bundle, authorization, and a real library. So the tests split:

- **Pure logic, unit-tested in Swift**: destination path derivation from
  creation date, filename collision handling, state-file read/write/resume,
  the asset-to-import-record mapping. These are the parts with real logic and
  they need no Photos access.
- **A `PhotoLibrary` protocol** over the PhotoKit calls, with a fake
  implementation supplying synthetic assets. Lets the whole pipeline —
  enumerate, export, skip-already-imported, cancel, resume — be tested without
  a real library, which is the difference between this being testable and not.
- **Manual verification** against the real library, recorded in the plan:
  counts match Photos' own, EXIF dates survive, HEICs thumbnail upright.

`core/` and `libphig/` gain no new tests here — this plan adds no C++.

## Open questions

1. **Originals or edits.** If a photo was cropped or adjusted in Photos, the
   original resource does not reflect it. Export the original (phig manages
   originals; adjustments stay in Photos), the current rendered version (what
   the user believes the photo looks like), or both? Leaning original, but this
   is a product decision.
2. **Album-to-gallery collision.** phig allows one gallery per image; Photos
   allows many albums per photo. First album wins, deepest folder path wins, or
   skip multi-album photos and report them?
3. ~~**Live Photos.**~~ **Answered: export both, always paired**, on plan 026's
   master/attachment model — the still is the indexed row, the MOV a carried
   attachment, exactly as a RAW carries its sidecar. This makes 038 a
   prerequisite rather than a follow-up, since exporting MOVs before pairing
   exists would create spurious video rows for a fifth of the library.
4. ~~**Size estimate before import.**~~ **Answered.** `fileSize` is readable
   through KVC, so the total can be summed before a byte moves. It must include
   `.pairedVideo`, which is 29% of the cost in the measured library. Read
   defensively and fall back to summing during export if the key ever
   disappears.
5. **Where the destination defaults to.** `~/Pictures/phig/`, a sibling of the
   Photos library, or force an explicit choice with no default?
6. **Shared and syndication libraries.** `com.apple.photolibraryd` preferences
   reference a separate `Syndication.photoslibrary`. Whether shared-library
   assets are in scope is unexamined.

## What this defers

- **Writing back to Photos.** Read-only, permanently. phig will not modify the
  user's Photos library.
- **Ongoing sync.** This is an import, not a mirror. Re-running picks up new
  assets; deletions in Photos do not propagate.
- **Photos' People, memories, moments, and smart albums.**
- **Aperture and iPhoto libraries.**

## Cross-references

- `plans/048-modern-image-codecs.md` — hard prerequisite; HEIC decode.
- `plans/031-libphig-operations.md` — the index operation this drives.
- `plans/044-large-collection-scale.md` — the measurement behind dated export
  directories.
- `plans/038-live-photos.md` — owns still/MOV pairing.
- `plans/050-macos-app-m0.md` — builds the grid on the app foundation this plan
  creates.
- `plans/047-gui-functional-design.md` — the screen inventory this app grows
  into.
