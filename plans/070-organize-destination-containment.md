---
name: Organize Destination Containment
status: completed
created: 2026-11-10
completed: 2026-11-13
closes_rules: [R87]
description: >
  An organize plan can place a file outside the destination the caller named,
  because `apply_format` substitutes a file's EXIF Make and Model and its
  gallery name verbatim. Clean those values, refuse a format that could leave
  the destination or uses an unknown token, and check every planned
  destination lies inside it. Excludes conflict handling, which R01 and R02
  cover.
---

# 070 — Organize Destination Containment

## Problem

`OrganizeEngine::plan` builds each destination as
`fs::path(opts_.destination) / apply_format(opts_.format, img, date)`
(`core/organize_engine.cpp`). `apply_format` (`core/organize_plan.cpp`) copies
`%camera` from EXIF `Model`, `%make` from EXIF `Make` and `%gallery` from the
row's gallery into the path with no cleaning. `fs::path`'s `operator/` then
treats a `/` in a value as a directory separator, keeps a `..` component, and
lets an absolute right-hand side replace the destination.

Measured on 2026-11-10 with the built binary, in a scratch catalog:

- A JPEG with EXIF `Model` set to `../../escaped` was copied by
  `phig cp <dest> --format '%camera/%original'` to a directory two levels above
  `<dest>`.
- A JPEG with EXIF `Make` set to `/tmp/abs` was planned by
  `phig cp <dest> --format '%make/%original' --dry-run` to `/tmp/abs/a.jpg`.

`mv` takes the same path with the original. R01 and R02 still refuse to
overwrite an occupied destination, so nothing is clobbered, but the file leaves
the tree the user chose. A camera writes Make and Model, and any tool can edit
them, so the file's own metadata decides where phig puts it. R87 records it as
`violated`.

Found while surveying unrecognized ABI input for plan 060's R73.

## Decisions

Decided 2026-11-13.

1. **A substituted value is cleaned, not refused.** In `apply_format`, a value
   taken from the file or its row (`%camera` from EXIF `Model`, `%make` from
   EXIF `Make`, `%gallery`) has `/` and `\` replaced with `_` and control
   characters dropped. A value that is then empty, `.` or `..` takes the token's
   existing placeholder (`unknown-camera`, `unknown-make`, `no-gallery`). Every
   file stays organizable, and the cleaned name is visible in the plan and in a
   dry run. A `Model` that genuinely contains `/` becomes one folder named with
   `_` rather than nested folders. `%original` is a filename and cannot hold a
   separator; the date tokens are digits or `unsorted`.
2. **The format is refused before planning when it could leave the
   destination or is not understood.** `validate(const OrganizeOptions&)` throws
   `InvalidOption`, R73's shape, for an absolute format, a format with a `..`
   component, and an unknown `%` token. The last one also catches a typo such as
   `%y`, which today creates a folder literally named `%y`. A format that wants a
   literal `%` before something other than a known token stops working; phig
   has no escape for one, and none is added.
3. **A containment check backs both up.** `OrganizeEngine::plan` normalizes
   each built destination lexically and requires it to lie under the normalized
   destination. It does not resolve symlinks: a symlinked directory inside the
   destination is the user's own layout, such as a link to another drive, and
   following it is what they asked for. After 1 and 2 the check cannot fail, so
   a failure is a bug: it throws and the plan is not produced, rather than one
   item being skipped.
4. **All of it is in `core/`.** The CLI and libphig's organize plan both get
   it. Because 1 and 2 prevent the escape and 3 stops the run, no per-item refusal
   reason is needed and the ABI does not change; `phig.h` documents the format
   refusals.

## Testing

Real-behavior tests against the engine, and the CLI for the measured case:

- An image whose EXIF `Make` or `Model` is `../../x`, and one whose `Make` is
  absolute, plan inside the destination with the cleaned name.
- A gallery name containing `/` plans one folder, not two.
- A value that cleans to empty, `.` or `..` takes its placeholder.
- An absolute format, a format with `..`, and an unknown token each throw
  `InvalidOption` before planning, and libphig's plan start returns
  `PHIG_ERR_INVALID_ARG`.
- `phig cp <dest> --format '%camera/%original'` on a file whose `Model` is
  `../../escaped` copies it under `<dest>`, repeating the 2026-11-10
  measurement.

R87 moves to `holds` with these as its tests.

## Outcome

Built as decided, 2026-11-13. `apply_format` and the new `first_unknown_token`
read one token table in `core/organize_plan.cpp`, so the expander and the
validator cannot disagree about which tokens exist. `validate(const
OrganizeOptions&)` refuses an absolute format, a `..` component and an unknown
token; `OrganizeEngine::plan` checks every destination lexically lies under the
destination, treating a trailing separator on the destination as no separator.
Six tests, each failing before the change; the CLI one reproduced the escape
before the fix. R87 holds.

## Relationship to other plans

- **060** found it, through R73's survey.
- **042** (organize engine, completed) built the plan/apply this sits in.
- **059** (`--on-conflict overwrite`) is about what happens at an occupied
  destination, not where the destination is.
