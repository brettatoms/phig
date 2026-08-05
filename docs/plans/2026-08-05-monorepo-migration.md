# phig Monorepo Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Combine `phig` and `phig-gtk` into one repository with per-product directories, splitting the shared C++ layer out of the CLI's `src/`, while preserving both repos' git history.

**Architecture:** `phig` becomes the monorepo root. `phig-gtk` is merged in at `gtk/` with `git subtree`, keeping all 14 of its commits. Today's flat `src/` — which holds phig-core, the C ABI, and the CLI together — splits into `core/`, `libphig/`, and `cli/`, siblings of the product directories. The build stays CMake and keeps working at every task boundary.

**Tech Stack:** C++20, CMake, GCC 15, pkg-config. devenv/Nix for now; the environment swap to mise is Task 8 and starts with a spike.

## Global Constraints

- **Every task must end with a working build and a green test suite.** Baselines: `phig` 640 tests (one pre-existing unrelated skip, `ScanEngine.FacesModeFailsClearlyWhenModelsAreMissing`), `phig-gtk` 13 tests across two binaries. A task that leaves either broken is not done.
- **Build commands keep the `direnv exec .` prefix** until Task 8 replaces the environment. Bare `cmake` fails on missing opencv4.
- **Preserve git history.** Verified working: `git subtree add --prefix=gtk <path> master` yields 239 commits = 224 (phig master) + 14 (phig-gtk) + 1 merge, and all ledger-referenced SHAs still resolve. Never squash, never start fresh — roughly 40 SHAs are cited across `docs/plans/`.
- **Use `git mv`, never `mv` plus `git add`.** Rename detection is what keeps `git log --follow` and `git blame` working through the restructure.
- **The `core/` ↔ `libphig/` split is the point.** They have different stability contracts: phig-core is internal with no promise, libphig is a versioned ABI (1.2, 42 symbols, soname 1, hidden visibility, version script). Do not merge them into one `lib/`.
- **phig-gtk may only include `<phig/phig.h>`.** Never a `core/` header, never link phig-core, never a `cv::Mat`. Non-GUI functionality belongs in libphig.
- **Do not change behaviour.** This is a move. No refactoring, no new features, no fixing unrelated things noticed along the way — note them instead.
- Comments describe the present, not the move. No "moved from src/" annotations.
- **Backup exists at `~/phig-backup-20260805-101910`** (962 MB, both histories verified). If a task goes irrecoverably wrong, restore from there rather than improvising.

---

## Target layout

```
phig/                       (the monorepo root — today's phig repo)
├── CLAUDE.md               monorepo-wide conventions
├── CMakeLists.txt          root: options, dependency discovery, add_subdirectory
├── core/                   phig-core, 27 sources + headers. Internal, no stability promise.
│   ├── CLAUDE.md
│   └── CMakeLists.txt
├── libphig/                the C ABI: 6 sources + include/phig/phig.h. ABI 1.2.
│   ├── CLAUDE.md
│   └── CMakeLists.txt
├── cli/                    main.cpp (2,363 lines)
│   ├── CLAUDE.md
│   └── CMakeLists.txt
├── gtk/                    the GTK4 client (merged via subtree)
│   ├── CLAUDE.md
│   └── CMakeLists.txt
├── tests/                  33 test sources — stays at the root, one suite
├── vendor/                 sqlite-vec, inih — unchanged
├── scripts/                unchanged
└── docs/                   specs + plans, moved in from the workspace root
```

Task order is deliberate: Task 1 merges gtk in while the tree is otherwise untouched (easiest to abort). Tasks 2-4 split `src/` one component at a time, each ending green. Task 5 wires the root CMake. Tasks 6-7 are docs and CLAUDE.md. Task 8 is the environment, last, because it is the only task that could block on an unknown.

---

### Task 1: Subtree-merge phig-gtk at `gtk/`

Do this first, while the tree is otherwise unchanged — if it goes wrong, `git reset --hard` restores everything.

**Files:**
- Create: `gtk/` (the entire phig-gtk tree, via subtree)

**Interfaces:**
- Consumes: nothing.
- Produces: `gtk/CMakeLists.txt`, `gtk/src/`, `gtk/tests/`, `gtk/devenv.nix`, `gtk/README.md` at their new paths. Later tasks reference `gtk/`.

- [ ] **Step 1: Confirm both repos are clean and record the starting state**

```bash
cd ~/devel/phig/phig && git status --short && git rev-parse --short HEAD
cd ~/devel/phig/phig-gtk && git status --short && git rev-parse --short HEAD
```

Expected: no output from either `git status`. phig should be at `94fc2f9` on branch
`phig-pc-for-clients`; phig-gtk at `fc64943` on `master`. If either is dirty, stop and report
— the subtree add will fail or produce a confusing result.

- [ ] **Step 2: Merge phig's working branch into master first**

The monorepo root should be `master`, and phig's 6 ABI commits are on
`phig-pc-for-clients`. Merge them before restructuring so there is one line of history:

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | grep -E "tests passed"   # expect 640
git checkout master
git merge --no-ff phig-pc-for-clients -m "Merge phig-pc-for-clients: phig.pc, thread-local last_error, phig_thumb_generate

Adds a pkg-config file so foreign clients can discover libphig, makes
phig_last_error thread-local so one phig_ctx can be shared across threads,
and exports phig_thumb_generate for lazy thumbnail generation. ABI 1.2,
42 exported symbols, soname unchanged at 1."
```

`--no-ff` matches the convention of the three previous part-merges. Then delete the branch
with `git branch -d phig-pc-for-clients` — `-d` refuses unless fully merged, which is the
check you want.

- [ ] **Step 3: Add phig-gtk as a subtree**

```bash
cd ~/devel/phig/phig
git subtree add --prefix=gtk ~/devel/phig/phig-gtk master
```

Expected output ends with `Added dir 'gtk'`. This creates a merge commit plus the imported
history.

- [ ] **Step 4: Verify history survived — this is the gate**

```bash
cd ~/devel/phig/phig
git rev-list --count HEAD          # expect 239 + however many the Step 2 merge added
git log --oneline -- gtk/ | wc -l  # expect >= 14
for s in fc64943 1d8a855 673e306 8163a0d 2647a63 3c65309; do
  printf "%s -> " "$s"; git log -1 --format="%s" "$s" 2>/dev/null || echo MISSING
done
```

Every one of those six SHAs is cited in `docs/plans/2026-08-04-phig-gtk-m0-ledger.md` and
must still resolve. If any prints MISSING, stop and report — the merge did not preserve
history and the plan's premise is broken.

- [ ] **Step 5: Confirm both builds still work from their new locations**

phig builds from the root as before. gtk now lives at `gtk/` and has its own devenv, so it
needs its own direnv context:

```bash
cd ~/devel/phig/phig && direnv exec . cmake --build build -j && direnv exec . ctest --test-dir build 2>&1 | grep -E "tests passed"
cd ~/devel/phig/phig/gtk && direnv allow 2>/dev/null; direnv exec . cmake -B build && direnv exec . cmake --build build -j
```

The gtk build may need `direnv allow` at its new path. If gtk's build cannot find libphig,
that is expected at this step — it reads `~/.local/lib/pkgconfig`, which is unaffected by the
move. Report what you observe rather than fixing it here.

- [ ] **Step 6: Report, do not commit**

The subtree add already committed. Report the commit count, the six SHA lookups, and both
build results. Do not delete `~/devel/phig/phig-gtk` — leave it until the whole migration is
verified.

---

### Task 2: Move phig-core to `core/`

**Files:**
- Move: 27 `src/*.cpp` + their headers → `core/`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's merged tree.
- Produces: `core/` holding phig-core's sources; the `phig-core` CMake target reads from
  `core/`. Tasks 3-5 include headers from `core/`.

**The exact 27 sources** (from `add_library(phig-core STATIC ...)`), so you do not have to
derive the list: `scanner hasher exif database selection faces models thumbs metadata paths
config settings organize_exec organize_plan remove_exec relink log sidecar dates duplicates
raw lockfile parallel scan_engine thumbs_engine faces_engine search_engine` — each as
`.cpp`. Verified: those 27 plus `main.cpp` account for all 28 `src/*.cpp`.

- [ ] **Step 1: Create the directory and move the sources with `git mv`**

```bash
cd ~/devel/phig/phig
mkdir -p core
for f in scanner hasher exif database selection faces models thumbs metadata paths config \
         settings organize_exec organize_plan remove_exec relink log sidecar dates \
         duplicates raw lockfile parallel scan_engine thumbs_engine faces_engine search_engine; do
  git mv "src/$f.cpp" "core/$f.cpp"
done
```

`git mv` (not `mv`) so rename detection keeps `git log --follow` working.

- [ ] **Step 2: Move the headers those sources need**

`src/` has 31 headers. Move every header EXCEPT ones only `main.cpp` uses. Determine
membership rather than guessing:

```bash
cd ~/devel/phig/phig
for h in src/*.h; do
  b=$(basename "$h")
  if grep -ql "$b" core/*.cpp src/libphig/*.cpp 2>/dev/null; then
    git mv "$h" "core/$b"
  else
    echo "KEEP in src/ (only main.cpp uses it?): $b"
  fi
done
```

Report anything the loop left behind, with which file references it. `cancel.h` and
`key_normalize.h` are header-only and belong in `core/` if anything in `core/` includes them.

- [ ] **Step 3: Update the CMake source paths**

In `CMakeLists.txt`, change `add_library(phig-core STATIC ...)` so every entry reads
`core/<name>.cpp` instead of `src/<name>.cpp`. Also update `target_include_directories` for
`phig-core` to expose `core/` (it currently exposes `src/`), and any
`${CMAKE_SOURCE_DIR}/src` reference used by other targets.

- [ ] **Step 4: Fix include paths in the moved sources**

Sources that did `#include "foo.h"` still work if the header moved alongside them. Sources
that reach into `src/libphig/` or are reached *from* there will break. Build and let the
compiler tell you:

```bash
cd ~/devel/phig/phig
direnv exec . cmake -B build && direnv exec . cmake --build build -j 2>&1 | grep -E "fatal error|No such file" | head -20
```

Fix each reported include. Do not add `-I` flags to paper over it — correct the include paths.

- [ ] **Step 5: Verify the suite**

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"
```

Expected: 640 tests passed. If the count differs from 640, say so — a changed count means a
test file stopped being compiled, which a green run would otherwise hide.

- [ ] **Step 6: Confirm rename detection worked**

```bash
cd ~/devel/phig/phig
git status --short | head -5
git log --follow --oneline core/database.cpp | wc -l
```

`git status` should show `R` (renamed) entries, not `D`+`A` pairs. The `--follow` count should
be well above 1 — `database.cpp` has a long history. If it shows 1, rename detection failed
and history is effectively lost for that file.

- [ ] **Step 7: Commit**

```bash
cd ~/devel/phig/phig
git add -A
git commit -m "Move phig-core sources to core/

Separates the internal C++ engine from the CLI and the C ABI, which all
shared src/ until now. core/ carries no stability promise; libphig/ is the
versioned boundary."
```

---

### Task 3: Move the C ABI to `libphig/`

**Files:**
- Move: `src/libphig/*.cpp` and `src/libphig/internal.h` → `libphig/`
- Move: `include/phig/phig.h` → `libphig/include/phig/phig.h`
- Modify: `CMakeLists.txt`, `phig.pc.in`

**Interfaces:**
- Consumes: `core/` from Task 2.
- Produces: `libphig/` holding the ABI; the installed header path is unchanged
  (`<prefix>/include/phig/phig.h`), so **consumers must not need any change**.

**The 6 sources:** `context.cpp query.cpp records.cpp thumbs.cpp mutate.cpp settings.cpp`,
plus `internal.h`.

- [ ] **Step 1: Move the sources and the public header**

```bash
cd ~/devel/phig/phig
mkdir -p libphig/include/phig
for f in context query records thumbs mutate settings; do
  git mv "src/libphig/$f.cpp" "libphig/$f.cpp"
done
git mv src/libphig/internal.h libphig/internal.h
git mv src/libphig/phig.version libphig/phig.version 2>/dev/null || echo "no phig.version at that path — find it and move it"
git mv include/phig/phig.h libphig/include/phig/phig.h
```

`phig.version` is the linker version script referenced by `target_link_options`; it must move
with the target. If the path differs, `grep -rn "phig.version" CMakeLists.txt` to find it.

- [ ] **Step 2: Update CMake**

In `CMakeLists.txt`:
- `add_library(phig_abi SHARED ...)` entries become `libphig/<name>.cpp`
- `target_include_directories(phig_abi ...)` — the `BUILD_INTERFACE` path becomes
  `${CMAKE_CURRENT_SOURCE_DIR}/libphig/include`; leave `INSTALL_INTERFACE` as `include`
- the version-script path in `target_link_options` becomes `libphig/phig.version`
- `install(DIRECTORY include/phig ...)` becomes `install(DIRECTORY libphig/include/phig ...)`

- [ ] **Step 3: Build and fix includes**

```bash
cd ~/devel/phig/phig
direnv exec . cmake -B build && direnv exec . cmake --build build -j 2>&1 | grep -E "fatal error|No such file" | head -20
```

`libphig/*.cpp` include both `internal.h` (now beside them) and `core/` headers. Fix each
reported path.

- [ ] **Step 4: Verify the ABI is unchanged — this is the gate**

The whole point of libphig is that its surface is stable. A move must not alter it:

```bash
cd ~/devel/phig/phig
nm -D --defined-only build/libphig.so | grep -c " T phig_"       # expect 42
grep -n "PHIG_ABI_VERSION_MINOR" libphig/include/phig/phig.h     # expect 2u
readelf -d build/libphig.so | grep SONAME                         # expect libphig.so.1
```

Any deviation means the move changed the ABI and must be corrected before committing.

- [ ] **Step 5: Verify a foreign consumer still builds against the installed header**

This is what actually proves the move is invisible to clients:

```bash
cd ~/devel/phig/phig
direnv exec . cmake --install build --prefix /tmp/phig-mig-test
PKG_CONFIG_PATH=/tmp/phig-mig-test/lib/pkgconfig pkg-config --cflags --libs phig
cat > /tmp/mig.c <<'EOF'
#include <phig/phig.h>
#include <stdio.h>
int main(void) {
    phig_config cfg = {0};
    cfg.db_path = "/home/brett/.local/share/phig/phig.db";
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = NULL;
    if (phig_init(&cfg, &ctx) != PHIG_OK) { printf("init failed\n"); return 1; }
    phig_query* q = NULL; phig_query_new(ctx, &q);
    int64_t n = 0; phig_query_count(ctx, q, &n);
    printf("%lld\n", (long long)n);
    phig_query_free(q); phig_shutdown(ctx);
    return 0;
}
EOF
direnv exec . sh -c 'gcc -std=gnu11 /tmp/mig.c -o /tmp/mig $(PKG_CONFIG_PATH=/tmp/phig-mig-test/lib/pkgconfig pkg-config --cflags --libs phig)'
LD_LIBRARY_PATH=/tmp/phig-mig-test/lib /tmp/mig
rm -rf /tmp/phig-mig-test /tmp/mig /tmp/mig.c
```

Expected: prints **29925** (the real row count). Report the number you got.

- [ ] **Step 6: Verify the suite and commit**

```bash
cd ~/devel/phig/phig
direnv exec . ctest --test-dir build 2>&1 | grep -E "tests passed"   # expect 640
git add -A
git commit -m "Move the C ABI to libphig/

The versioned boundary now has its own directory, separate from the internal
engine in core/. The installed header path is unchanged, so consumers need
no change: 42 exported symbols, ABI 1.2, soname 1."
```

---

### Task 4: Move the CLI to `cli/`

**Files:**
- Move: `src/main.cpp` → `cli/main.cpp`, plus any headers only it uses
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `core/` and `libphig/` from Tasks 2-3.
- Produces: `cli/` holding the CLI. `src/` should be empty or gone afterwards.

- [ ] **Step 1: Move main.cpp and whatever is left in src/**

```bash
cd ~/devel/phig/phig
mkdir -p cli
git mv src/main.cpp cli/main.cpp
ls src/ 2>/dev/null
```

Report anything still in `src/`. If Task 2 Step 2 left headers behind, move them to `cli/`
now if only `main.cpp` uses them, or to `core/` if anything in `core/` does.

- [ ] **Step 2: Update CMake and remove the empty src/**

`add_executable(phig src/main.cpp)` becomes `add_executable(phig cli/main.cpp)`. Then:

```bash
cd ~/devel/phig/phig
rmdir src 2>/dev/null && echo "src/ removed" || ls src/
```

- [ ] **Step 3: Build, fix includes, verify**

```bash
cd ~/devel/phig/phig
direnv exec . cmake -B build && direnv exec . cmake --build build -j 2>&1 | grep -E "fatal error|No such file" | head -20
direnv exec . ctest --test-dir build 2>&1 | grep -E "tests passed"   # expect 640
```

- [ ] **Step 4: Verify the CLI actually still works**

A green test suite does not exercise argv parsing. Run the real binary:

```bash
cd ~/devel/phig/phig
./build/phig search --count --db ~/.local/share/phig/phig.db
./build/phig --version
```

Expected: the first prints **29925**; the second prints a version. Report both.

- [ ] **Step 5: Commit**

```bash
cd ~/devel/phig/phig
git add -A
git commit -m "Move the CLI to cli/

src/ is now gone: its three tenants live in core/, libphig/, and cli/."
```

---

### Task 5: Split CMakeLists into root plus per-directory files

Until now everything stayed in one root `CMakeLists.txt` with updated paths. This task gives
each component its own, which is what makes the directories independently comprehensible.

**Files:**
- Modify: `CMakeLists.txt` (becomes the root: options, dependency discovery, `add_subdirectory`)
- Create: `core/CMakeLists.txt`, `libphig/CMakeLists.txt`, `cli/CMakeLists.txt`

**Interfaces:**
- Consumes: the moved directories from Tasks 2-4.
- Produces: the same four build targets (`phig-core`, `phig_abi`, `phig`, `phig-tests`,
  `phig-perf-tests`) with identical names and properties, reachable from the root build.

- [ ] **Step 1: Move the per-target blocks into their own files**

The root keeps: `cmake_minimum_required`, `project`, `configure_file` for `version.h`,
`CMAKE_CXX_STANDARD`, the warning options, `PHIG_WERROR`, all 12 `find_package`/
`pkg_check_modules` calls, `enable_testing()`, and the `phig.pc` generation. Then:

```cmake
add_subdirectory(core)
add_subdirectory(libphig)
add_subdirectory(cli)
```

`core/CMakeLists.txt` gets the `add_library(phig-core STATIC ...)` block with paths now
relative to `core/` (so bare `scanner.cpp`, not `core/scanner.cpp`), plus its
`target_include_directories`, `target_compile_options`, and vendored-source entries.
`libphig/CMakeLists.txt` gets `add_library(phig_abi SHARED ...)` plus its
`set_target_properties`, visibility settings, version script, and `install` rules.
`cli/CMakeLists.txt` gets `add_executable(phig main.cpp)` and its `install`.

Leave `tests/` in the root `CMakeLists.txt` — it is one suite spanning all components, and
splitting it buys nothing.

- [ ] **Step 2: Build from scratch to catch stale cache assumptions**

An incremental build can hide a broken `add_subdirectory` wiring:

```bash
cd ~/devel/phig/phig
rm -rf build
direnv exec . cmake -B build && direnv exec . cmake --build build -j 2>&1 | tail -3
```

- [ ] **Step 3: Verify every target and property survived**

```bash
cd ~/devel/phig/phig
ls build/libphig.so* build/phig build/phig-tests build/phig-perf-tests
nm -D --defined-only build/libphig.so | grep -c " T phig_"   # expect 42
readelf -d build/libphig.so | grep SONAME                     # expect libphig.so.1
direnv exec . ctest --test-dir build 2>&1 | grep -E "tests passed"   # expect 640
./build/phig search --count --db ~/.local/share/phig/phig.db   # expect 29925
```

All five must pass. The symbol count and SONAME matter most: a per-directory CMakeLists that
drops `C_VISIBILITY_PRESET hidden` or the version script would silently export far more than
42 symbols.

- [ ] **Step 4: Commit**

```bash
cd ~/devel/phig/phig
git add -A
git commit -m "Give core/, libphig/, and cli/ their own CMakeLists

The root now holds only project setup and dependency discovery; each
component declares its own target. Same five targets, same ABI properties."
```

---

### Task 6: Move `docs/` into the repository

**Files:**
- Move: `~/devel/phig/docs/` → `phig/docs/`
- Modify: `.gitignore` if it excludes anything under `docs/`

**Interfaces:**
- Consumes: nothing.
- Produces: `docs/specs/` and `docs/plans/` under version control.

**Why this is a change in kind, not just location.** The workspace `CLAUDE.md` says the docs
live outside any git repo *because they span repos*. A monorepo removes that reason, so they
become versioned — 23 files including every spec, plan, and ledger.

- [ ] **Step 1: Copy them in and verify nothing is excluded**

```bash
cd ~/devel/phig/phig
cp -a ~/devel/phig/docs .
cat .gitignore | grep -iE "docs|plans|specs" || echo "nothing in .gitignore excludes docs"
git status --short docs/ | wc -l
```

Note there is an existing `plans/` directory in the phig repo — check whether it duplicates
`docs/plans/` and report before merging them. Do not silently combine two plan directories.

- [ ] **Step 2: Commit**

```bash
cd ~/devel/phig/phig
git add docs
git commit -m "Bring docs/ into the repository

Specs, plans, and ledgers were kept outside git because they spanned two
repos. One repo now holds everything they describe, so they are versioned
with the code they document."
```

- [ ] **Step 3: Report the count and any duplication**

Report how many files were added and whether `plans/` and `docs/plans/` overlap.

---

### Task 7: The CLAUDE.md hierarchy

**Files:**
- Modify: `CLAUDE.md` (currently phig-specific `AGENTS.md` exists too — see Step 1)
- Create: `core/CLAUDE.md`, `libphig/CLAUDE.md`, `cli/CLAUDE.md`, `gtk/CLAUDE.md`

**Interfaces:**
- Consumes: the final directory layout from Tasks 1-6.
- Produces: per-directory guidance. Claude Code reads nested `CLAUDE.md` by directory.

- [ ] **Step 1: Reconcile the existing files first**

There are three sources today: `~/devel/phig/CLAUDE.md` (workspace-level),
`phig/AGENTS.md` (phig's build/stack/layout), and `phig-gtk`'s README. Read all three, then
report what you propose to put where **before writing**. Do not discard content — the
workspace CLAUDE.md carries hard-won conventions (measured-not-reasoned performance claims,
comments-describe-the-present, data-safety-ships-with-real-tests) that must survive.

Decide and state whether `AGENTS.md` stays as a separate file or folds into the root
`CLAUDE.md`.

- [ ] **Step 2: Write the root `CLAUDE.md`**

Must contain: the directory layout table, the `direnv exec .` requirement (until Task 8),
build and test commands with expected counts, the interface-discipline rule (clients go
through libphig only), the **"non-GUI functionality belongs in libphig, not the clients"**
rule, and the conventions listed in Step 1.

- [ ] **Step 3: Write the four per-directory files**

Each should be short and specific to its directory — what it is, what its stability contract
is, and the rules a change there must respect:

- `core/CLAUDE.md` — internal, no stability promise; the five engines' shared shape (options
  in, result out, options by value, no user-facing prose); `CancelToken`/`ProgressQueue`
  conventions.
- `libphig/CLAUDE.md` — the versioned boundary. ABI 1.2, 42 symbols, soname 1. Every entry
  point wraps its body in `PHIG_TRY`/`PHIG_CATCH`. Adding a symbol bumps
  `PHIG_ABI_VERSION_MINOR` and CMake `VERSION`, never `SOVERSION`. `phig_last_error` is
  thread-local: read it on the thread whose call failed.
- `cli/CLAUDE.md` — argv parsing, usage text, all five output formats, the SIGINT handler,
  the progress printer. Flag order is `phig <subcommand> <args> --db <path>`.
- `gtk/CLAUDE.md` — GTK calls on the main thread only; the `ThumbLoader` seam and why
  `max_dim` exists; cancel-on-unbind; **do not add orientation handling** (applied at
  generation time); the real-run check that GTK warnings must be 0.

- [ ] **Step 4: Verify and commit**

```bash
cd ~/devel/phig/phig
ls CLAUDE.md core/CLAUDE.md libphig/CLAUDE.md cli/CLAUDE.md gtk/CLAUDE.md
git add -A && git commit -m "Add a CLAUDE.md hierarchy for the monorepo"
```

---

### Task 8: Environment — spike first, then decide

**This task starts with an investigation, not an implementation.** The decision was mise +
vcpkg, but three facts are unverified and one could invalidate the approach. Do not write a
`vcpkg.json` until Step 1 answers them.

**Reframing that changes what this task is for.** Distro packages will be built against the
*system* libraries of each distro, not against anything the dev environment provides. That
removes the portable-binary argument for vcpkg entirely — the artifact users install is
compiled by the distro's builder against its own OpenCV, so whether *your* dev binary embeds
nix or vcpkg paths is irrelevant to shipping.

What actually matters once packaging is system-based:

1. **The build must accept whatever version a distro ships.** Verified spread: Arch 5.0.0,
   Fedora ~4.10, Debian/Ubuntu ~4.6, homebrew 4.x–5.x. So phig's *code* has to compile against
   both 4.x and 5.x — a requirement on the source, not the environment. `find_package(OpenCV
   REQUIRED COMPONENTS ...)` is already version-agnostic, so the shape is right; what is
   missing is proof it compiles across the range.
2. **Version floors must be declared** (Step 2), so a too-old distro fails at configure time
   rather than misbehaving at runtime.
3. **Vendored copies of packaged libraries become a packaging objection.** `libinih` is
   packaged (Arch `core/libinih`, already installed here); `sqlite-vec` is not. A packager will
   likely want phig to use the system inih. Worth an option (`PHIG_USE_SYSTEM_INIH`) rather
   than a hard switch.

**So the dev environment's only remaining jobs are reproducibility and convenience** — which
makes the vcpkg-vs-pixi-vs-system question much lower stakes than it looked, and argues for
whichever is *simplest* rather than whichever produces a portable binary. Step 1 should be
judged against that, not against portability.

**Files:**
- Create (only after the spike): `mise.toml`, `vcpkg.json`
- Delete (only after the new path works): `devenv.nix`, `devenv.lock`, `devenv.yaml`,
  `gtk/devenv.nix`, `gtk/devenv.lock`
- Modify: `CMakeLists.txt` (version floors), `README.md` (setup instructions)

**Why the environment needs changing at all.** Measured: the nix-built `phig` binary has
**230 dependencies resolving to `/nix/store`, zero to system libraries, and 13 store paths
baked into its RUNPATH**. It cannot run on a machine without an identical nix store.

**What is already verified:**
- 9 of 11 native libs are present on this machine (EndeavourOS/Arch): libexif 0.6.26,
  sqlite3 3.53.4, libraw_r 0.22.2, libavformat 62.12.102, libavcodec 62.28.102,
  libavutil 60.26.102, libswscale 9.5.102, spdlog 1.17.0, gtest 1.17.0, gtk4 4.22.4,
  libadwaita-1 1.9.3.
- **OpenCV is the one real gap.** nix pins 4.13.0; the system ships **5.0.0** with
  `opencv5.pc` and `/usr/include/opencv5`, so `find_package(OpenCV)` asking for opencv4 will
  not find it. A probe using every OpenCV API phig touches (`cv::Mat`, `imread`,
  `VideoCapture`, `FaceDetectorYN`, `FaceRecognizerSF`, `redirectError`) **compiled, linked,
  and ran against 5.0.0**, so a port looks feasible.
- **Caveat:** `pkg-config --libs opencv5` links every module and fails on this system because
  `libopencv_viz` has unresolved VTK symbols. Link only the 5 components phig needs:
  `core imgcodecs imgproc objdetect videoio`.
- `sqlite-vec` and `inih` are **vendored**, so they are immune to any of this.

- [ ] **Step 1: The spike — answer these four questions and report before proceeding**

1. **Does vcpkg have all seven ports?** `opencv4` (or `opencv`), `libraw`, `ffmpeg`,
   `libexif`, `spdlog`, `gtest`, `sqlite3`. Install vcpkg (`pacman -S vcpkg`, or its official
   bootstrap) and check. **vcpkg is not currently installed** — earlier port queries returned
   nothing for that reason, not because the ports are absent.
2. **Can vcpkg supply gtk4 and libadwaita on Linux?** Historically weak. If not, `gtk/` keeps
   system-provided GTK while the C++ libs come from vcpkg — a split mechanism that partly
   defeats the single-file goal. This is the finding most likely to change the approach, so
   answer it early.
3. **How long is a cold vcpkg build?** OpenCV and FFmpeg from source are not quick. Report
   wall-clock time; if it is hours, that is a real contributor-experience cost worth knowing.
4. **Does phig compile against both OpenCV 4.x and 5.x?** This is now the most important
   question, because distro packaging means the code must accept whatever each distro ships
   (Arch 5.0.0, Fedora ~4.10, Debian/Ubuntu ~4.6). Build against the system OpenCV 5.0.0 —
   linking only `core imgcodecs imgproc objdetect videoio`, since `pkg-config --libs opencv5`
   pulls in a broken `libopencv_viz` on this machine — and report every compile error. A probe
   of phig's API surface already compiled and ran against 5.0.0, so expect few, but the full
   codebase is a bigger surface than the probe.

Note what is **no longer** a spike question: whether static linking yields a portable binary.
Distro packages are built against system libraries, so dev-environment portability does not
matter. Judge the three options on simplicity and reproducibility instead.

**Stop after Step 1 and report.** If gtk4 is unavailable from vcpkg, or the build takes hours,
or OpenCV 5 needs source changes, the controller may redirect to pixi or plain system
packages. Do not proceed on assumptions.

- [ ] **Step 2: Add version floors to CMakeLists (do this regardless of the outcome)**

**No dependency declares a minimum version today** — every one is a bare `REQUIRED`. Safe
under nix pins, dangerous with any system-provided path. Add floors for what phig actually
relies on, at minimum:

- OpenCV: **≥ 3.4** — `cv::imread` applies EXIF orientation by default from that version, and
  phig depends on it (verified: a `Right-top` image's thumbnail matches the auto-oriented
  source at RMSE 0.0175 vs 0.378 for the raw one). If the OpenCV 5 port happens, the floor
  becomes whatever that requires.
- The others: use the versions verified present above as the floor, since those are known to
  work.

This is a small, clearly-correct change and is worth committing on its own even if the rest
of Task 8 is deferred.

- [ ] **Step 2b: Make the vendored `inih` optional (also worth doing regardless)**

`libinih` is packaged on Arch (`core/libinih`, already installed) and on most distros. A
packager will object to phig bundling a copy of a library the distro already ships. Add a
CMake option defaulting to the vendored copy so nothing changes for developers:

```cmake
option(PHIG_USE_SYSTEM_INIH "Link the system inih instead of the vendored copy" OFF)
```

When `ON`, discover it with `pkg_check_modules(INIH REQUIRED inih)` (confirm the real `.pc`
name first — it may be `inih` or `libinih`) and drop `vendor/inih/*` from the source list.
Verify **both** paths build and the suite stays at 640 either way, then report both counts.

Leave `sqlite-vec` vendored: it is not packaged by any major distro, so bundling it is the
correct choice and a packager will accept it.

- [ ] **Step 3 onwards: write only after the spike reports**

The remaining steps depend on Step 1's answers, so they are deliberately not pre-written.
Once the spike lands, the controller will either extend this task or file a follow-up plan
with concrete steps. **Do not invent them.**

---

## Review Calibration

**Full-depth review:** Tasks 3 and 5 — both can silently change the ABI surface (symbol
count, visibility, soname, version script) while leaving a green build.

**Single-pass review:** Tasks 1, 2, 4, 6, 7.

**Task 8 Step 1 is a report, not a code review.** Check that all four questions have concrete
answers with numbers, and that nothing was implemented on an assumption.

**Keep regardless:** every task ends with the full suite at **640** and, where a binary is
involved, a real-run check. A changed test count means a test file stopped compiling, which a
green run hides.

**Known false positive:** subagents that commit may trip a "no explicit user approval"
warning. Per-task commits are authorized for this plan. **Pushing is not** — the human
partner explicitly deferred all pushing until after the migration.

---

## Self-Review

**Spec coverage.** Every element the user asked for maps to a task: the monorepo with product
directories is Tasks 1-5; "does libphig/core go in src/ or lib/" is answered by the target
layout (`core/` and `libphig/` as siblings of the products, neither `src/` nor `lib/`); mise
replacing devenv is Task 8; the single root `CLAUDE.md` with per-product files is Task 7;
moving the superpowers docs to the root is Task 6. History preservation via subtree is Task 1,
per the user's explicit choice.

**Placeholder scan.** One deliberate exception: Task 8 Step 3 says the remaining steps are not
pre-written. That is not a TBD hiding work — it is because three unverified facts (vcpkg's
ports, GTK on Linux, build time) could each change the approach, and writing steps for an
approach that may be discarded would be worse than saying so. Step 1 names exactly what to
find out and requires a stop-and-report. Task 7 Step 1 similarly requires a proposal before
writing, because reconciling three existing docs is a judgement call, not a mechanical merge.

**Type consistency.** Target names (`phig-core`, `phig_abi`, `phig`, `phig-tests`,
`phig-perf-tests`) are used identically in Tasks 2-5. The 27 core sources are listed once,
explicitly, in Task 2 and verified to account for all `src/*.cpp` except `main.cpp`. The 6 ABI
sources are listed in Task 3. Expected values are consistent throughout: 640 tests, 42
symbols, ABI minor `2u`, soname `1`, row count 29925.

**Known risks, stated rather than hidden.**
- **The `plans/` vs `docs/plans/` overlap** — phig already has a `plans/` directory and the
  workspace has `docs/plans/`. Task 6 Step 1 requires reporting before merging them rather
  than assuming.
- **gtk/'s devenv comes along with the subtree** and is only removed in Task 8. Between Tasks
  1 and 8 the repo has two devenv files, which is untidy but harmless.
- **Task 2 Step 2's header-membership loop is heuristic** (grep for the basename). It reports
  anything it cannot classify rather than guessing, and Task 4 Step 1 sweeps whatever is left.
- **`~/devel/phig/phig-gtk` is deliberately not deleted** by any task. Removing it is a
  judgement call for after the whole migration verifies.
