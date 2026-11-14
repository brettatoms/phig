---
name: Escape CSV and JSON Output
status: completed
created: 2026-11-14
completed: 2026-11-14
description: >
  The CSV and JSON written by `duplicates`, `search` and `fav list` do not
  escape their text fields, so a filename with a comma breaks a CSV row, and a
  quote or backslash in a path breaks the JSON. Adds one pair of escaping
  helpers in `core/` and uses them in every writer. Excludes the text and
  porcelain formats.
---

# 078 — Escape CSV and JSON Output

## Problem

`cli/main.cpp` writes CSV and JSON by streaming fields between literal quotes,
without escaping:

- `duplicates --format csv` (`output_csv`) quotes `path` but writes `filename`
  bare. A filename containing a comma shifts every column after it.
- `search --format csv` does the same with `filename`, and quotes `camera` and
  `thumb_path` without doubling any `"` inside them.
- `duplicates --format json` (`output_json`), `search --format json`, and the
  face-search JSON write `path`, `filename` and `camera` unescaped. A `"`, `\`
  or control character produces invalid JSON.

Measured, 2026-11-14: 58 of the 29,925 paths in the real catalog contain a
comma. The first one stopped a script reading `duplicates --format csv`
partway through.

`core/database.cpp` already has a JSON string escaper, used for activity-log
snapshots, but it is not reachable from `cli/`.

## Decisions

1. **One pair of helpers in `core/`**, since formatting is not GUI-specific:
   `csv_field(std::string_view)` and `json_string(std::string_view)`, in
   `core/output_escape.h`. They are for output other programs read. The three
   escapers phig uses for its own stored JSON (`database.cpp`'s activity-log
   snapshots, `exif.cpp` and `metadata.cpp`) stay as they are: a snapshot must
   keep a path's exact bytes for a restore, which decision 4 would break.
2. **CSV follows RFC 4180.** Every text field is quoted, and an embedded `"` is
   doubled. A newline inside a quoted field is legal and is left as it is.
   Numeric fields stay unquoted.
3. **JSON escapes `"`, `\` and every control character below 0x20.**
4. **Invalid UTF-8 in a path is replaced with U+FFFD in JSON.** JSON cannot
   carry arbitrary bytes, and Linux filenames can contain them. The output stays
   valid, and the path in it is then not usable to open the file. `--print0`
   and `--porcelain` remain the exact-bytes formats, and `--help` says so.
   None of the real catalog's paths are affected.
5. **Every writer changes**: duplicates CSV and JSON, search CSV and JSON, the
   face-search JSON, and `fav list`, which shares search's writers.

## Testing

- Unit tests for both helpers: comma, quote, backslash, newline, a control
  character, and invalid UTF-8.
- Subprocess tests that scan files named `a,b.jpg` and `q"uote.jpg`, run each
  of the six writers, and parse the output with a real CSV or JSON parser,
  checking that the field round-trips.

## Relationship to other plans

- **077** uses these helpers for `duplicates resolve --format json`.
- **R69** and **R81** record the truncating writes in `cmd_search`. Their
  output path is unchanged here.

## Outcome

Built 2026-11-14. `core/output_escape.{h,cpp}` holds `csv_field` and
`json_string`; the UTF-8 check rejects stray continuation bytes, truncated
sequences, overlong encodings, surrogates and code points above U+10FFFF, each
byte of which becomes U+FFFD. All six writers use them, and `--help` for
`duplicates` and `search` says which formats keep a path's exact bytes.

Ten tests: four on the helpers, and six that run the built binary on files named
`a,b.jpg`, `q"uote.jpg` and `back\slash.jpg` and parse its output, CSV with an
RFC 4180 reader in the test and JSON with SQLite's `json_valid` and
`json_extract`. All six failed against the old writers. The face-search one runs
only where the face models are installed.
