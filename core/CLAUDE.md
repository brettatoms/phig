# core/

The C++ engine: scanning, hashing, EXIF/metadata extraction, the SQLite
database, glob selection, RAW decoding, and the five engines. Internal —
no stability promise, no ABI. Change freely; the API surface other code
depends on is `libphig/`, not this.

## The five engines

`ScanEngine`, `ThumbsEngine`, `FacesEngine`, `SearchEngine`, plus the
`remove`/`organize` executors. They share one shape:

- Options in (a struct, stored **by value** in the engine — a caller builds
  a temporary and passes it, so a reference would dangle).
- Result out (a struct the caller renders; the engine carries no
  user-facing prose — wording is the caller's job).
- `ScanEngine`, `ThumbsEngine`, `FacesEngine` take a `CancelToken` and
  publish progress to an optional `ProgressQueue<T>*` (may be null).
- `SearchEngine` takes neither: search is single-threaded and single-shot,
  and it is also the only engine with no OpenCV dependency — the caller
  decodes the query image and passes embeddings or a phash in. Follow this
  pattern when a client needs a query path that shouldn't drag in pixels.

Follow this shape for any new engine. Don't add prose strings to a result
struct, and don't make an engine reach for global state — everything it
needs comes through its options struct and the `Database&` passed in.

## Cancellation boundaries

Where an engine's `run()` is split into phases (e.g. `ScanEngine::
run_workers` / `run_reconcile`), the split exists because only some phases
are safely cancellable — a phase that mutates user-authored metadata inside
one logged operation may not poll the token. Check the class comment before
assuming cancellation covers the whole `run()`.

## Rules of record

A data-integrity guard in this directory is recorded in `rules/`, and most of
phig's rules anchor here — root `CLAUDE.md` puts enforcement in `core/` or
`libphig/` and nowhere else. Changing a guard means updating its record's
`enforced_at` and re-running `python3 scripts/rules.py --check --emit` in the
same commit. CI resolves every anchor against the source, so renaming or
deleting an enforcing symbol without touching its record fails the build.
