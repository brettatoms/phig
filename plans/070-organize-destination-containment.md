---
name: Organize Destination Containment
status: designing
created: 2026-11-10
closes_rules: [R87]
description: >
  An organize plan can place a file outside the destination the caller named.
  `apply_format` substitutes a file's EXIF Make and Model and its gallery name
  verbatim, so a value containing `..` or starting with `/` moves the file
  elsewhere, and a format string can do the same. Decide how values are
  cleaned and how the plan enforces that every destination stays inside the
  destination directory. Excludes conflict handling, which R01 and R02 cover.
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

## Open questions

1. **Clean or refuse a substituted value.** Replacing `/`, `\` and NUL in a
   substituted value, and refusing a value that is `.` or `..`, keeps every
   file organizable. Refusing the file instead makes the problem visible but
   leaves it unorganized. A value cleaned silently should probably still be
   reported per item.
2. **What the format itself may say.** The format is the caller's own input.
   An absolute format or a literal `..` could be refused outright (R73's shape,
   in `validate(const OrganizeOptions&)`), or allowed and caught by the
   containment check below. Unknown `%` tokens are kept literally today; whether
   that stays is the same question.
3. **The containment check.** After building a destination, check that its
   lexically normalized form is under the normalized destination, and refuse
   the item otherwise. A symlinked directory inside the destination tree that
   points elsewhere passes a lexical check; whether the check resolves symlinks,
   and at plan or apply time, needs deciding.
4. **Surfaces.** The fix belongs in `core/`, so the CLI and libphig's organize
   plan both get it. libphig's plan already reports per-item destinations, so a
   refused item needs a way to say why.

## Relationship to other plans

- **060** found it, through R73's survey.
- **042** (organize engine, completed) built the plan/apply this sits in.
- **059** (`--on-conflict overwrite`) is about what happens at an occupied
  destination, not where the destination is.
