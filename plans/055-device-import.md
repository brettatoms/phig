---
name: Device Import (PTP/MTP)
status: designing
created: 2026-08-09
description: >
  Import from a camera or phone that exposes no filesystem path. A card reader
  mounts as a directory and plan 021 already handles it; an iPhone or a modern
  camera over USB speaks PTP/MTP, which has no path for phig to walk and no
  path to store in a row. So device import is not a variant of 021 — it is
  049's pipeline with a different source: enumerate, fetch original bytes,
  write into `<dest>/YYYY/MM/`, then index what was written. The open questions
  are where the code lives, given that the device libraries are per-platform
  and `core/` is portable, and whether 049 ships first so there is one working
  implementation to generalize from rather than zero.
---

# 055 — Device Import (PTP/MTP)

## The gap

Plan 021 imports from a directory. That covers more devices than it sounds
like: a card reader, an external disk, and a camera in USB mass-storage mode
all mount as ordinary directories (`/Volumes/…`, `/run/media/…`), so `phig
import /Volumes/EOS_DIGITAL ~/Photos --move` works today with no new mechanism.

It does not cover an iPhone, an iPad, or a camera that speaks PTP/MTP rather
than mass storage. Those expose no filesystem path.

Nothing in the repository addresses this. Searched 2026-08-09 across `plans/`,
`docs/`, `core/` and `cli/` for DCIM, gphoto, MTP, PTP, "card", "removable" and
"device import": no hits.

## Why this cannot be 021 with a different source string

**phig's identity model is the filesystem path.** Every engine assumes one, the
`images` table is keyed on one, and `mv`/`rm`/relink all operate on one. Plan
049 examined the same question for PhotoKit and rejected referencing assets by
`localIdentifier` without copying, for exactly this reason. A PTP object handle
is worse than a `localIdentifier` — it is not stable across a disconnect.

So the bytes must be copied out before phig knows anything about them, which
makes this 049's pipeline, not 021's:

```
enumerate → fetch original bytes → write into <dest>/YYYY/MM/ → index
```

021's pipeline is `scan in place → organize`, and the auto-injected
`--match canonical(<src>)/**` that justifies the command's existence has no
meaning here — there is no source path to scope by. The scope is the list of
files just written.

## Constraints inherited from measured work

These are not new decisions. They follow from findings other plans already
made, and getting them wrong is the predictable failure mode.

- **The destination is dated, not flat.** Plan 044 measured `has_sibling_raw`
  giving every scanned file a full parent-directory listing — 4,000 files cost
  7.33 s in one directory against 2.03 s split across four. Writing a card's
  worth of photos into a single directory walks into that. `<dest>/YYYY/MM/`,
  matching 049.
- **Index after the batch is written, never per file.** RAW+JPEG pairing (026)
  is decided by listing the parent directory, so a `CR2` indexed before its
  `JPG` lands produces two rows instead of one photo, permanently. Plan 052
  states the same constraint as the reason import cannot be a per-file `POST`.
  Fetch first, index second.
- **Original bytes, byte-for-byte.** A re-encoded or resized fetch defeats
  SHA256 and EXIF both. 049 says this about `PHAssetResourceManager`; it
  applies to whatever the device API's equivalent is.
- **Live Photos arrive as two objects.** Plan 038 pairs a still with its MOV on
  Apple's Content Identifier, and 049's probe found 390 of them in a
  1,894-asset library. A phone enumerated over PTP will present both halves; if
  they are fetched into the dated layout and then indexed, 038's pairing does
  the rest — provided 038 has shipped.
- **iPhone stills are HEIC**, which plan 048 made decodable. Without it this
  import produces a collection phig cannot thumbnail, hash, or date.
- **Re-import must not re-fetch.** 049 keeps a state file mapping a foreign
  asset identifier to its exported path. The same need exists here and the
  same reasoning applies — deduplicating by SHA256 works only after paying the
  transfer cost, which is the cost worth avoiding.

## Open questions

None of these are answerable from what is in the repository today. Each names
what would answer it.

### 1. Where does the code live?

This is the question the plan turns on, and it is genuinely open.

Root `CLAUDE.md` says non-GUI functionality belongs in `libphig` rather than
duplicated in clients, and that `core/` must compile and pass tests on Linux,
macOS and Windows. Device access satisfies neither comfortably: the libraries
are per-platform (below), and none of them is portable the way SQLite is.

Three candidate shapes:

- **In `core/` behind a narrow platform split**, like `paths.cpp` and
  `lockfile.cpp`. Consistent with the stated rule, and it puts device import
  behind `libphig` so every client gets it. Costs `core/` a heavy new
  dependency that is unavailable on at least one target platform, which no
  current `core/` dependency is.
- **In each client**, like 049 puts Photos import in the macOS app. Honest
  about the platform coupling. Contradicts the interface-discipline rule unless
  device access is argued to be platform integration rather than phig
  functionality — which is the argument 049 makes for PhotoKit, but 049 has a
  forcing reason this may not: PhotoKit requires a TCC-authorized bundle.
- **A separate optional component** that produces files, with phig indexing
  them afterwards. Weakest coupling, and the closest to what the pipeline
  actually is.

Answering it needs question 2 first: if the macOS device API turns out to need
an app bundle the way PhotoKit does, the decision is made for us on that
platform and the split becomes unavoidable.

### 2. What does each platform actually require?

Candidates, none verified — no code has been run against any of them:

| Platform | Candidate | What must be checked |
|---|---|---|
| macOS | ImageCaptureCore (`ICDeviceBrowser`, `ICCameraDevice`) | Whether a command-line tool can enumerate and read a device, or whether it needs a bundle and a TCC grant as PhotoKit does |
| Linux | libgphoto2 | Whether it reaches iPhones as well as cameras, and how it interacts with a desktop session that has already auto-mounted the device over gvfs |
| Windows | WPD (Windows Portable Devices) | Everything; plan 039 has not finished the `core/` port, so this is furthest out |

The check is small and concrete: connect a phone and a camera, enumerate, fetch
one original, compare its SHA256 against the same file copied off a card. Do
that before designing further.

### 3. Does a device get a row at all?

After a device import, the collection holds rows for files at `<dest>`, which
are ordinary paths on ordinary storage — the device is not represented. That
seems right and matches 049, where an imported Photos library becomes an
ordinary phig collection.

The thing to confirm is that nothing downstream wants provenance: whether a
user asks "which of these came off the Canon", and whether that is a tag
(`phig tag add canon`) rather than a schema concept. Answer it with 047's
screens rather than by adding a column.

### 4. Does this wait for 049?

049 builds this exact pipeline against a real source, including the state file,
resume, and per-asset progress on a slow fetch. Building the second
implementation first, or both at once, means designing the shared shape from
zero working examples.

Recommended: 049 ships, then this plan generalizes from it. The cheap insurance
in the meantime is that **049's state file keys on a source-tagged identifier**
rather than a bare `localIdentifier`, so a second source is an addition rather
than a migration. That is a one-line constraint on 049, not a shared
abstraction, and it does not commit either plan to the other's design.

## Explicitly not in scope

- **Mounted sources.** A card reader is plan 021's, and adding a second path to
  the same outcome would be a worse command, not a better one.
- **Mount and device discovery in the CLI.** Enumerating what is plugged in and
  offering it is a client feature; plan 047 owns it.
- **Tethered capture.** Shooting into phig from a connected camera is a
  different product feature that happens to share a library. Not here.
- **Writing to a device.** Read-only, in the same sense 049 is.

## Relationship to other plans

- **Plan 021** (import command) — the mounted-source half of the same user
  intent, and the owner of `[import] destination`, which this plan reads rather
  than adding its own.
- **Plan 049** (macOS Photos import) — the same pipeline against a different
  unwalkable source, and the first implementation of it. Question 4.
- **Plan 038** (Live Photos) — a phone's stills and their paired MOVs need it.
- **Plan 048** (modern codecs) — iPhone stills are HEIC. Shipped.
- **Plan 044** (large collection scale) — the `has_sibling_raw` measurement
  behind the dated layout.
- **Plan 026** (RAW support) — why indexing must follow the whole batch.
- **Plan 047** (GUI functional design) — owns the device picker and progress UI.
- **Plan 039** (cross-platform `core/`) — gates the Windows row of question 2.
