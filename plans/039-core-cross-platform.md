---
name: Cross-Platform core/ and libphig/
status: designing
created: 2026-08-06
description: >
  Make `core/` and `libphig/` build and behave correctly on macOS and Windows.
  `libphig/` has no POSIX includes — the work is entirely in `core/` (6 files,
  ~250 lines of POSIX surface) plus the build system. Splits into two phases
  with different risk: macOS already compiles and tests green in CI, so its
  work is behavioral (directory fsync is a documented no-op, case-insensitive
  APFS is unexamined); Windows is a real port where `lockfile.cpp` is a
  rewrite, `paths.cpp` needs a platform split, and the pkg-config/pthread
  dependency chain has no Windows equivalent. Three open decisions — where
  application data lives per platform, whether path comparison becomes
  case-folding, and whether Windows is MSVC or MinGW — come before any code.
  Audit findings inline; no measurement yet on the case-sensitivity exposure.
---

# 039 — Cross-Platform `core/` and `libphig/`

**Status is `designing`, not `planned`.** Three decisions below change what
gets built, and one finding (case sensitivity) is an identified exposure that
has not been reproduced. Writing code before those settle would mean guessing
at the data model.

Scope is `core/` and `libphig/` only. The clients — `cli/` and `gtk/` — have
their own platform assumptions, listed under "Not in scope" so they aren't
lost, but they are not this plan's work.

## What the audit found

`core/` is ~8,300 lines. The POSIX surface is small and concentrated: six
files have POSIX includes, and two of them are the substance. Most filesystem
work already goes through `std::filesystem`, which is why the surface is this
narrow.

**`libphig/` needs no source changes.** All six translation units
(`context.cpp`, `query.cpp`, `records.cpp`, `thumbs.cpp`, `mutate.cpp`,
`settings.cpp`) are pure C++ over `core/`, with zero POSIX includes. Its work
in this plan is the export mechanism and validation, not porting.

The POSIX surface, by file:

| File | Surface | Windows |
|---|---|---|
| `core/lockfile.cpp` | `flock`, `pread`, `pwrite`, `ftruncate`, `open`, `getpid` | Rewrite (75 lines) |
| `core/paths.cpp` | `$HOME`/XDG, `fsync`, `O_DIRECTORY`, `O_NOFOLLOW`, `mkstemp`, `::write`/`::close`, `ssize_t`, `getpid`, `gmtime_r`, `EXDEV`, `link`, `renameat2`/`renamex_np` | Platform split |
| `core/hasher.cpp` | `dup`/`dup2`/`STDERR_FILENO`, `open("/dev/null")` | Mechanical |
| `core/log.cpp` | `dup(STDERR_FILENO)` + `fdopen` | Mechanical |
| `core/metadata.cpp` | **Dead includes** — includes `<fcntl.h>`/`<unistd.h>`, uses neither | Delete the includes |
| `tests/temp_dir.h` | `<unistd.h>` for `getpid()` | Mechanical, but blocks the whole suite |

### The writer lock (R50)

R50, the lock following the database it protects, was closed by plan 067:
`lock_path(db)` takes the database path and the CLI locks its catalog's
`phig.db.lock`. This plan still owns making `lock_path` and `WriterLock`
portable, both in files it rewrites (`core/paths.cpp`, `core/lockfile.cpp`).

Confirmed non-issues, so they don't reappear as questions later: threading is
only `std::thread`/`std::mutex`/`hardware_concurrency()`; `core/dates.cpp:97`
already does a portable `file_clock`→`system_clock` conversion with a comment
saying why; there is no `dlopen`, no `sysconf`, no `PATH_MAX`, no `pthread_*`
call, and no hardcoded `/tmp`, `/usr`, or `/etc`. The single hardcoded POSIX
path in the whole tree is `/dev/null` at `core/hasher.cpp:173`.

## Phase 1 — macOS

**macOS compiles and passes CI today.** `.github/workflows/ci.yml` builds
`macos-latest` with `-DPHIG_WERROR=ON` and runs the full `ctest --parallel`.
`libphig/CMakeLists.txt:38-40` already handles ld64's
`-exported_symbols_list` alongside Linux's `--version-script`, and
`core/paths.cpp:147` already has an `__APPLE__` branch using
`renamex_np(RENAME_EXCL)`.

So Phase 1 is not a port. It is three behavioral gaps that compile cleanly and
do the wrong thing — which is why they've survived a green CI.

### 1a. Directory fsync is a silent no-op

`fsync_dir` (`core/paths.cpp:127-133`) opens with `O_DIRECTORY` and calls
`fsync`. Both exist on macOS; `fsync` on a directory fd is documented as not
flushing to stable storage there. macOS needs `fcntl(fd, F_FULLFSYNC)`.

The function is explicitly best-effort — its comment says "Failure is ignored
— the data itself was already fsynced." So this does not corrupt anything.
What it means is that the durability guarantee `move_file` and
`atomic_copy_file` document does not hold on macOS, and nothing says so. The
fix is small; the reason it matters is that phig's conventions call for
data-safety behavior to be real, not nominal.

Worth checking while here: `fsync_file` (`core/paths.cpp:117-123`) has the
same exposure. On macOS `fsync` on a *file* fd flushes to the drive but does
not force the drive to flush its own write cache — `F_FULLFSYNC` is what does.
For a photo manager placing user files, that is the difference the comment at
`paths.cpp:239-241` is claiming.

### 1b. Case-insensitive filesystem (APFS default)

This is the one finding where the failure mode is **wrong behavior, not
weaker behavior**, and it is the reason this plan is `designing`.

Extension matching is already safe: `core/scanner.cpp:58` and
`core/sidecar.cpp:20` lowercase before comparing, so `.CR2` vs `.cr2` works.
The exposure is in path identity:

- **RAW+JPEG pairing** (`core/scanner.cpp:115-154`) scans a directory for a
  same-basename sibling. On APFS, `IMG_1234.CR2` and `img_1234.jpg` are the
  same basename and a case-sensitive comparison misses the pair — or two
  files Linux treats as distinct collapse into one.
- **The database stores paths as strings.** Every lookup, dedup, and
  `--match` glob compares bytes. `/Photos/img.jpg` and `/photos/IMG.jpg`
  become two rows for one file.
- **`resolve_conflict` (`core/paths.cpp:313-357`) mixes the two.** Its
  `taken()` lambda calls `fs::exists`, which *is* case-insensitive on APFS,
  and ANDs it against the `claimed` set, which is a case-*sensitive*
  `std::set<std::string>`. Those two disagreeing is the shape of bug that
  produces either a rename loop or a clobber.
- **`paths_safe_to_delete` (`core/paths.cpp:282`)** compares exact strings
  for orphan detection — and plan 032 exists because that function
  mis-declining rows is a real, shipped failure mode.

**None of this has been reproduced.** It is an unexamined assumption, not a
demonstrated defect. Per the repo's convention that inferred diagnoses get
measured before they get fixed — the convention plan 037 exists to honor —
Phase 1 opens with an experiment, not a change. See "Before this becomes
implementable."

### 1c. Application data location

XDG paths work on macOS because `$HOME` is set, so `~/.local/share/phig/` is
where the database lands. A Mac user expects `~/Library/Application
Support/phig/`. This is cosmetic, it is a user-visible convention break, and
changing it is a **migration** for anyone already running phig on macOS —
which is why it's decision 1 below rather than a task here.

## Phase 2 — Windows

Ordered by effort. Phase 2 depends on nothing in Phase 1 except decision 1.

### 2a. `core/lockfile.cpp` — rewrite

No `flock`, no `pread`/`pwrite`, no `ftruncate`; `open`/`close` become
`_open`/`_close`. Maps to `LockFileEx`/`UnlockFileEx` with
`LOCKFILE_FAIL_IMMEDIATELY` for the non-blocking probe at `lockfile.cpp:39`
and a blocking call for the wait path at line 60.

The subtlety is not the API swap. **Windows file locking is mandatory, not
advisory.** `flock` lets other processes read the file freely; `LockFileEx`
on a byte range actually blocks readers of that range. The `read_pid`
(`lockfile.cpp:13`) / `write_pid` (line 24) scheme reads the holder's PID out
of the file *while another process holds the lock* — that works under `flock`
and needs the Windows version to lock a byte range that excludes the PID
bytes, or to carry the PID some other way.

Behavior to preserve exactly, since plan 029 established it: reads never take
the lock, mutating commands serialize, `--no-wait` throws `LockBusy` with the
holder's PID in the message.

### 2b. `core/paths.cpp` — platform split

- `xdg_base()` (line 31) and `expand_tilde()` (line 94) both read `$HOME`,
  which Windows does not set. Needs `%LOCALAPPDATA%`/`%APPDATA%` or
  `SHGetKnownFolderPath` — decision 1.
- `fsync_file`/`fsync_dir` → `FlushFileBuffers` on a `HANDLE`. **Windows has
  no directory-flush equivalent at all**, so `fsync_dir` becomes a genuine
  no-op there. It is already documented best-effort, so this is a
  documentation change more than a code one.
- `rename_noreplace` (line 143) → `MoveFileEx` *without*
  `MOVEFILE_REPLACE_EXISTING` is natively no-replace, which is cleaner than
  the POSIX path. The `link()`-then-`unlink()` emulation at lines 151-154 and
  the TOCTOU fallback at 156-159 both drop out on Windows.
- **`EXDEV` does not exist on Windows.** This is the subtle one, not a
  syscall swap. `move_file` (line 176) triggers its entire
  copy→fsync→no-replace-rename fallback on `err != EXDEV`. Windows returns
  `ERROR_NOT_SAME_DEVICE` for a cross-volume move, so the error mapping —
  not just the constant — has to be rethought, and getting it wrong means
  either the fallback never fires (cross-volume moves fail) or it always
  fires (every move becomes a copy+delete).
- `getpid()` (lines 111, 220) → `GetCurrentProcessId()`; `gmtime_r` (line 21)
  → `gmtime_s`.

### 2c. Mechanical substitutions

- **`core/hasher.cpp:169-195`, `StderrSuppressor`.** `dup`/`dup2`/
  `STDERR_FILENO` → `_dup`/`_dup2`/`_fileno(stderr)`; `/dev/null` → `"NUL"`.
  This class is load-bearing, not incidental: it exists to swallow the
  OpenCV/LibRaw decode noise on undecodable CR2 files. Its mutex-serialized
  save/restore must survive the port intact.
- **`core/log.cpp:72-73`.** `dup(STDERR_FILENO)` + `fdopen` → `_dup` +
  `_fdopen`.
- **`core/metadata.cpp:3-4`.** Delete both includes. They are unused today,
  so this is a no-op on Linux and removes a Windows blocker for free.
- **`tests/temp_dir.h:3`.** `<unistd.h>` for `getpid()`. Every fixture uses
  this helper, so the whole suite fails to compile until it's done — do it
  first in Phase 2, not last.

### 2d. Build and dependencies

Sequenced last because it is independent of the source changes, and flagged
as the largest single piece **and the least verifiable from a Linux
machine**:

- `core/CMakeLists.txt:63` links `pthread` unconditionally. No such library
  on Windows; C++ threading is in the runtime.
- The root `CMakeLists.txt` uses `pkg_check_modules` for libexif (line 39),
  LibRaw (line 50), and the four FFmpeg libs (lines 57-60). Windows does not
  ship `.pc` files for these — this becomes vcpkg `find_package` or manual
  discovery, with the version floors preserved.
- `libphig/CMakeLists.txt:41-43`'s `--version-script` is GNU ld. MSVC needs a
  `.def` file. The header side is already done —
  `libphig/include/phig/phig.h:31` has the `_WIN32`/`__declspec(dllexport)`
  guard — so this is the CMake half of a job someone started.
- `SOVERSION`/`VERSION` (`libphig/CMakeLists.txt:14-15`) have no meaning for
  a Windows DLL; the ABI-version contract has to be expressed differently.
- **`tests/test_paths.cpp` hardcodes `/dev/shm`** at lines 231, 257, and 281
  for its three EXDEV tests. They already `GTEST_SKIP` on macOS. On Windows
  they need a second-volume equivalent or the cross-device fallback — the
  branch decision 2b is most likely to get wrong — ships with no test
  coverage at all.

## Before this becomes implementable

Three decisions and one experiment. The experiment first, because its result
may change decision 2.

**Experiment: does case-insensitivity actually break anything?** Create a
case-insensitive volume (`hdiutil` on macOS, or a case-insensitive directory
on Linux via a loopback FAT/exFAT image), then run against it: (a) a scan of
a directory holding `IMG_1234.CR2` + `img_1234.jpg` and check whether pairing
fires, (b) a scan of the same tree twice via `/Photos` and `/photos` and
count rows, (c) an `mv` with `--on-conflict rename` into a directory holding
a case-variant of the destination name. Record what happens. This is cheap
and it determines whether decision 2 is a real design question or a
non-issue.

**Decision 1 — where does application data live?** Options: keep XDG
everywhere (simplest, wrong-looking on both macOS and Windows); native per
platform (`~/Library/Application Support` / `%LOCALAPPDATA%`), which needs a
migration path for existing macOS installs and changes what
`libphig/include/phig/phig.h:61,73,273` document; or native on Windows only,
XDG on macOS, on the grounds that no macOS user base exists yet to migrate.
This blocks both phases.

**Decision 2 — does path comparison become case-folding?** Depends on the
experiment. If the exposure is real, the choice is between normalizing paths
at the DB boundary (one place, changes stored data, needs a migration) versus
case-folding comparisons at each site (`claimed`, `paths_safe_to_delete`,
pairing — more sites, no migration). Note these interact: normalizing at the
boundary on a case-*sensitive* filesystem would merge two genuinely distinct
files, so this cannot be unconditional.

**Decision 3 — MSVC or MinGW for Windows?** MSVC means the `.def` file, the
underscore-prefix and `_`-API substitutions, and vcpkg. MinGW keeps much more
of the POSIX surface working as-is (including `flock` emulation in some
configurations) at the cost of a less native toolchain and a harder story for
anyone consuming `libphig.dll` from MSVC-built code. This decides how much of
2a and 2c is actually needed.

## Not in scope

- **`cli/` and `gtk/`.** For the record, so it isn't rediscovered: `cli/main.cpp`
  uses `isatty(fileno(stdin))` at lines 791 and 2163, `std::signal(SIGINT, ...)`
  at line 72 (works on Windows, but the handler runs on a *separate thread*
  there, which matters for a handler that flips a cancel flag), and `\033[K`
  ANSI escapes at five sites needing virtual-terminal mode on older consoles.
  `gtk/` is a separate question entirely.
- **Packaging, signing, notarization, installers.** Plan 018 deferred
  packaging definitions; this plan does not pick them up.
- **CI matrix expansion to `windows-latest`.** The natural follow-on, but it
  should gate working code rather than drive the port.
- **Native macOS or Windows UI.** Unrelated; this plan is what such a client
  would sit on.

## Tests

- Every existing test keeps passing on Linux and macOS — this plan changes no
  behavior on Linux, so `ctest --parallel` staying at its current count is
  the baseline gate.
- `F_FULLFSYNC`: assert the macOS branch is taken. Durability itself is not
  unit-testable without power loss; the test is that the platform-correct call
  is reached.
- Case-folding (if decision 2 says yes): real-behavior tests on a genuinely
  case-insensitive mount, not a mock. Per repo convention, data-safety
  changes ship with real-behavior tests.
- Cross-device move on Windows: needs a second volume in CI, or the EXDEV
  equivalent path is untested — call it explicitly rather than let it pass by
  omission.
- `libphig`: `tests/test_libphig_abi.cpp` and the C99 compile check
  (`tests/test_libphig_c_compile.c`) must pass on every platform, and the
  exported symbol list must match on all three export mechanisms.

## Verifying against the real collection

The two real-run checks from the root `CLAUDE.md` must still hold on Linux
after Phase 1 and Phase 2:

```bash
./build/phig search --count --catalog ~/.local/share/phig   # 29925
./build/phig --version                                          # phig 0.3.0
```

On macOS and Windows there is no equivalent collection to check against, so
"it builds and the suite is green" is the whole gate there. That is weaker
than what phig usually accepts, and it is worth knowing that going in.
