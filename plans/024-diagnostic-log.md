---
name: Diagnostic Log File
status: completed
created: 2026-07-11
description: >
  Route phig's diagnostic messages (command invocations, warnings, errors)
  through one spdlog logger with two sinks: stderr (warnings and errors, plain
  format) and a rotating file at ~/.local/state/phig/phig.log (timestamped,
  level from config). A single log call per site replaces today's bare
  std::cerr writes, so messages like scan's "Cannot decode image" survive the
  terminal. Adds spdlog (+fmt) as a system dependency. Complements the activity
  log (plan 020), which records file operations in the DB — this records
  diagnostics as text.
---

# Diagnostic Log File

## Goal

Scan warnings like `[warning] 2014/IMG_0721.JPG -- Cannot decode image: ...`
go only to stderr today and are gone when the terminal scrolls. Persist them —
plus command invocations at INFO and failures at ERROR — to a plain-text log
file so "which files failed and when" can be answered after the fact.

Diagnostics go through **one log call per site** that feeds both the terminal
and the file (decided over a file-only logger with duplicated `std::cerr`
lines): sinks with per-sink levels and formats do the routing, which is
exactly what spdlog provides — hand-rolling that would mean reimplementing it.

This is distinct from the activity log (plan 020): that records *file
operations* (mv/cp/purge) in `phig.db` for audit/undo. This records
*diagnostics* as human-readable text. They share the `[log]` config section
but nothing else.

## Design

**spdlog as a system dependency** (decided over hand-rolled and vendored
plog): packaged on every platform phig builds on (Debian `libspdlog-dev`,
Fedora `spdlog-devel`, Arch `spdlog`, Homebrew `spdlog`, nixpkgs `spdlog`),
matching the plan-018 stock-packages approach. Distro builds use external
`fmt`, which spdlog's CMake config pulls in automatically; `fmt` joins the
documented dependency lists.

One logger, two sinks:

- **stderr sink** — level `warn`, pattern `[%l] %v`: warnings/errors reach the
  terminal as `[warning] <msg>` / `[error] <msg>`; INFO entries (invocation,
  scan summary) do not. Real newlines print as-is, so multi-line advice
  messages stay readable.
- **rotating file sink** — `~/.local/state/phig/phig.log` via a new
  `state_dir()` (`$XDG_STATE_HOME` or `~/.local/state`, + `/phig`, XDG's home
  for logs) and `default_log_path()` in `paths.h`. Level from `[log]
  file_level` (default `info`). Rotates at 5 MB keeping one backup —
  spdlog names it `phig.1.log`. Pattern
  `%Y-%m-%dT%H:%M:%SZ [%l] %*` with UTC time, where `%*` is a custom flag
  that escapes embedded newlines to literal `\n`/`\r` — **one line is always
  exactly one entry**, so `grep`/`tail -f` never see partial entries and
  parsing needs no timestamp-regex entry detection. Example line:
  `2026-07-11T18:22:03Z [warning] scan: 2014/IMG_0721.JPG -- Cannot decode image: ...`

Wrapper API (`src/log.h`) keeps spdlog headers out of every other TU:

- `log_init(path, file_enabled, file_level, max_bytes)` once at startup —
  builds the logger; if the file sink can't be created (read-only $HOME,
  etc.), warns once on stderr and continues **stderr-only**, so terminal
  diagnostics never disappear. Never throws.
- `log_info/log_warn/log_error(msg)` anywhere, including library code —
  **no-op before `log_init`** (tests and library consumers never write files
  by accident). Messages are logged verbatim, not as fmt format strings, so
  braces in filenames are safe. Thread-safe (`_mt` sinks; scan workers log
  concurrently); every entry flushed (`flush_on(info)` — diagnostics are
  low-volume).
- `log_close()` for tests.

**Terminal output changes at the instrumented sites** (accepted): the bare
`std::cerr` writes are *replaced* by the log call, so `[warning] <rel> --
<what>` keeps its shape while top-level failures become `[error] scan: <what>`
instead of `Error: <what>`. Argument-validation messages (`Error: --media must
be ...`) are not instrumented and keep their current form. stdout is out of
scope entirely — progress lines, scan summary blocks, and command results are
program output, not diagnostics, and stay `std::cout`.

## Instrumented call sites (this slice)

| Site | Level | Change |
|------|-------|--------|
| `main()` once dispatch begins | INFO | new: full invocation (`join_argv`) — file-only via sink levels |
| scan worker per-file warn (`main.cpp:565`) | WARN | cerr → `log_warn("scan: <rel> -- <what>")`; stdout progress-line clear stays |
| scan worker `--on-error fail` (`main.cpp:562`) | ERROR | cerr → `log_error("scan: <rel> -- <what>")`, then rethrow (top-level also logs, matching today's two stderr lines) |
| scan mount-warning (`main.cpp:610`) | WARN | cerr → one `log_warn` with the multi-line advice text |
| scan end summary (`main.cpp:624`) | INFO | new: compact counts line — file-only; the stdout `Done in ...` block is unchanged |
| hasher OpenCV decode warning (`hasher.cpp:176`) | WARN | cerr → `log_warn`; keep a bare `std::cerr << '\n'` first (breaks out of the scan progress line, as today) |
| faces reference-photo decode failures (`main.cpp:1511`, `main.cpp:1989`) | ERROR | cerr → `log_error` |
| every top-level `catch` in `main()` printing `Error: <what>` | ERROR | cerr → `log_error(command + ": " + e.what())` |

Not in this slice (adopt incrementally): `models.cpp` download warnings,
argument-validation `Error:` messages, a `--log-level`/`--quiet` CLI flag,
structured (JSON) log lines.

## Config

Two new keys in the existing `[log]` section, following the `Config` pattern
(optional fields; unset = built-in default; validated at parse):

```ini
[log]
file       = true    # default; false disables the file sink (stderr sink stays)
file_level = info    # default; info | warn | error — file sink threshold
```

The stderr sink is always on at `warn` — `file = false` must not silence
terminal warnings, since the log call is now their only path to stderr.
`enabled`/`max_entries` (activity log, plan 020) are untouched and orthogonal.

## Testing

- **paths:** `state_dir()` honors `XDG_STATE_HOME` and falls back to
  `~/.local/state/phig`; `default_log_path()` is `state_dir() + "/phig.log"`.
- **logger** (spdlog behavior we configure, via the wrapper): uninitialized
  `log_*` writes nothing and doesn't crash; a warn reaches stderr and the file
  (file line = UTC timestamp + `[warning]` + message); info reaches the file
  only; a message containing newlines is escaped in the file but raw on
  stderr; `file_level = error` keeps warns on stderr but out of the file;
  `file_enabled = false` still warns on stderr; braces in messages are logged
  verbatim; a small `max_bytes` rotates to `phig.1.log`.
- **config:** `[log] file` and `file_level` parse; invalid `file_level`
  throws; unset stays unset.
- **wiring:** manual end-to-end — a scan over a corrupt image prints
  `[warning]` to the terminal and appends the same entry to the file;
  `file = false` writes no file; `file_level = error` drops the WARN from the
  file only.

---

# Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route phig's diagnostics through one spdlog logger — stderr sink (warn+, plain) + rotating file sink at `~/.local/state/phig/phig.log` (timestamped UTC, level from `[log] file_level`, newline-escaped entries) — replacing the bare `std::cerr` writes at the instrumented sites with a single log call.

**Architecture:** spdlog joins the system-package dependencies (CMake `find_package`, CI, devenv, README). A thin wrapper `src/log.{h,cpp}` in `phig-lib` owns a process-global `spdlog::logger` with the two sinks and exposes `log_init/log_info/log_warn/log_error/log_close`, keeping spdlog headers out of other TUs. `paths.cpp` gains `state_dir()`/`default_log_path()`. `main()` builds the logger from config right after config load; instrumented stderr sites swap `std::cerr` for one log call.

**Tech Stack:** C++20, spdlog (+fmt) as system packages, std::filesystem, CMake, GoogleTest.

## Global Constraints

- **Language/standard:** C++20. Match existing `src/` style (clang-format, 4-space, 100-col).
- **Build/test:** `direnv exec . cmake --build build` then `./build/phig-tests`. A bare shell has a glibc/include mismatch; wipe `build/` if the CMake cache goes stale. Adding spdlog to `devenv.nix` requires re-entering the direnv env and reconfiguring (`direnv exec . cmake -B build`).
- **Lint gate:** `-Wall -Wextra` clean (CI uses `-DPHIG_WERROR=ON`); `clang-format --dry-run --Werror` clean on touched files.
- **spdlog stays behind `src/log.h`:** no spdlog include outside `src/log.cpp` — call sites use the wrapper only.
- **Logging must never break a command:** `log_init` never throws (file-sink failure → stderr-only + one warning); `log_*` never throws (messages logged verbatim, never treated as fmt format strings).
- **stdout output unchanged everywhere.** stderr changes only as specified in the call-site table.

## File Structure

- `src/log.h` / `src/log.cpp` — NEW. `LogLevel`, `log_level_from_string()`, `log_init/log_info/log_warn/log_error/log_close`; all spdlog usage lives in `log.cpp`.
- `src/paths.h` / `src/paths.cpp` — `state_dir()`, `default_log_path()`.
- `src/config.h` / `src/config.cpp` — `log_file`, `log_file_level` fields + `[log]` parsing.
- `src/main.cpp` — logger init from config; INFO invocation line; cerr→log swaps per the table.
- `src/hasher.cpp` — cerr→log swap at the OpenCV decode-warning site.
- `CMakeLists.txt` — `find_package(spdlog REQUIRED)`, link `spdlog::spdlog`, add `src/log.cpp` + `tests/test_log.cpp`.
- `devenv.nix`, `.github/workflows/ci.yml`, `README.md` — spdlog (+fmt) in the dependency lists.
- `tests/test_log.cpp` — NEW. Wrapper/sink behavior tests.
- `tests/test_paths.cpp`, `tests/test_config.cpp` — new cases.
- `AGENTS.md`, `README.md` — document the log file + config keys.

---

### Task 1: `state_dir()` + `default_log_path()`

**Files:**
- Modify: `src/paths.h` (declarations near `cache_dir()`/`config_path()`), `src/paths.cpp`
- Test: `tests/test_paths.cpp`

**Interfaces:**
- Produces (consumed by Task 5):

```cpp
std::string state_dir();         // $XDG_STATE_HOME or ~/.local/state, + /phig
std::string default_log_path();  // state_dir() + /phig.log
```

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_paths.cpp` (match the file's existing env-var save/restore pattern if it has one):

```cpp
TEST(PathsTest, StateDirHonorsXdgStateHome) {
    setenv("XDG_STATE_HOME", "/tmp/xdg-state-test", 1);
    EXPECT_EQ(state_dir(), "/tmp/xdg-state-test/phig");
    unsetenv("XDG_STATE_HOME");
}

TEST(PathsTest, StateDirFallsBackToHome) {
    unsetenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(state_dir(), std::string(home) + "/.local/state/phig");
}

TEST(PathsTest, DefaultLogPathUnderStateDir) {
    EXPECT_EQ(default_log_path(), state_dir() + "/phig.log");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='PathsTest.StateDir*:PathsTest.DefaultLogPath*'`
Expected: FAIL to compile — `state_dir` not declared.

- [ ] **Step 3: Implement**

`src/paths.h`, after `cache_dir()` (line 15):

```cpp
std::string state_dir();   // $XDG_STATE_HOME  or ~/.local/state, + /phig
```

and after `config_path()` (line 21):

```cpp
std::string default_log_path();  // state_dir() + /phig.log
```

`src/paths.cpp`, after `cache_dir()` (line 36) and after `config_path()` (line 45) respectively:

```cpp
std::string state_dir() { return xdg_base("XDG_STATE_HOME", "/.local/state") + "/phig"; }
std::string default_log_path() { return state_dir() + "/phig.log"; }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='PathsTest.StateDir*:PathsTest.DefaultLogPath*'`
Expected: PASS (all three).

- [ ] **Step 5: Commit**

```bash
git add src/paths.h src/paths.cpp tests/test_paths.cpp
git commit -m "Add XDG state dir + default log path"
```

---

### Task 2: spdlog as a dependency (build env, CMake, CI, docs)

**Files:**
- Modify: `devenv.nix` (packages list line 4-15, `CMAKE_PREFIX_PATH` list line 17-22), `CMakeLists.txt` (find_package block lines 23-31, `phig-lib` link libraries line 72-83), `.github/workflows/ci.yml` (Ubuntu apt lists lines 33-37 and 61-65, macOS brew line 41), `README.md` (per-distro dependency commands, lines 42-54)

**Interfaces:**
- Produces: `spdlog::spdlog` linked into `phig-lib` — Task 3's `log.cpp` can `#include <spdlog/...>`.

- [ ] **Step 1: devenv**

In `devenv.nix`, add `pkgs.spdlog` to `packages` (after `pkgs.ffmpeg-headless`, line 14) and to the `CMAKE_PREFIX_PATH` list (after `pkgs.ffmpeg-headless.dev`, line 21):

```nix
    pkgs.spdlog
```

- [ ] **Step 2: CMake**

In `CMakeLists.txt`, after `find_package(CURL REQUIRED)` (line 27):

```cmake
find_package(spdlog REQUIRED)
```

In `target_link_libraries(phig-lib PUBLIC ...)` (line 72), after `CURL::libcurl`:

```cmake
    spdlog::spdlog
```

- [ ] **Step 3: Verify the environment builds**

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && ./build/phig-tests`
Expected: configure finds spdlog, build clean, all tests PASS. (If configure can't find spdlog, re-enter the devenv shell so the nix env picks up the new package.)

- [ ] **Step 4: CI**

In `.github/workflows/ci.yml`, add `libspdlog-dev` to **both** Ubuntu `apt-get install` lists (build job line 37, lint job line 65 — append to the `libgtest-dev` line):

```yaml
            libgtest-dev libspdlog-dev
```

Add `spdlog` to the macOS brew line (41):

```yaml
        run: brew install cmake pkg-config opencv sqlite libexif ffmpeg googletest spdlog
```

- [ ] **Step 5: README build deps**

In `README.md`: extend the intro sentence (line 37) to mention spdlog, and add the package to each install command — Debian/Ubuntu (line 42): `libspdlog-dev`; Fedora (line 47): `spdlog-devel`; Arch (line 51): `spdlog`; macOS (line 54): `spdlog`. (fmt is pulled in automatically as each distro's spdlog dependency; no separate entry needed.)

- [ ] **Step 6: Commit**

```bash
git add devenv.nix CMakeLists.txt .github/workflows/ci.yml README.md
git commit -m "Add spdlog dependency"
```

---

### Task 3: Logger wrapper (`src/log.{h,cpp}`)

**Files:**
- Create: `src/log.h`, `src/log.cpp`
- Modify: `CMakeLists.txt` (add `src/log.cpp` to `phig-lib` at line 58, `tests/test_log.cpp` to `phig-tests` at line 119)
- Test: `tests/test_log.cpp` (new)

**Interfaces:**
- Consumes: `spdlog::spdlog` (Task 2).
- Produces (consumed by Tasks 4–6):

```cpp
enum class LogLevel { Info = 0, Warn = 1, Error = 2 };

LogLevel log_level_from_string(const std::string& s);   // "info"|"warn"|"error"; throws otherwise
void log_init(const std::string& path, bool file_enabled, LogLevel file_level,
              std::size_t max_bytes = 5 * 1024 * 1024);
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);
void log_close();
```

- [ ] **Step 1: Write the failing tests**

Create `tests/test_log.cpp`:

```cpp
#include "log.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

class LogTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("phig-log-test-" + std::to_string(::getpid()));
        fs::create_directories(dir_);
        path_ = (dir_ / "phig.log").string();
    }
    void TearDown() override {
        log_close();
        fs::remove_all(dir_);
    }
    std::string read_log() {
        std::ifstream f(path_);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    fs::path dir_;
    std::string path_;
};

TEST_F(LogTest, UninitializedIsNoOp) {
    log_warn("nothing should happen");
    EXPECT_FALSE(fs::exists(path_));
}

TEST_F(LogTest, WarnReachesStderrAndFile) {
    log_init(path_, true, LogLevel::Info);
    testing::internal::CaptureStderr();
    log_warn("scan: a.jpg -- Cannot decode image");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err, "[warning] scan: a.jpg -- Cannot decode image\n");
    // file line: UTC ISO-8601 timestamp + level + message
    std::regex line(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z \[warning\] )"
                    R"(scan: a\.jpg -- Cannot decode image\n$)");
    EXPECT_TRUE(std::regex_search(read_log(), line));
}

TEST_F(LogTest, InfoReachesFileOnly) {
    log_init(path_, true, LogLevel::Info);
    testing::internal::CaptureStderr();
    log_info("phig scan /pics");
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
    EXPECT_NE(read_log().find("[info] phig scan /pics"), std::string::npos);
}

TEST_F(LogTest, MultilineEscapedInFileRawOnStderr) {
    log_init(path_, true, LogLevel::Info);
    testing::internal::CaptureStderr();
    log_warn("first\nsecond");
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "[warning] first\nsecond\n");
    std::string body = read_log();
    EXPECT_EQ(std::count(body.begin(), body.end(), '\n'), 1);  // one entry, one line
    EXPECT_NE(body.find("[warning] first\\nsecond\n"), std::string::npos);
}

TEST_F(LogTest, FileLevelErrorKeepsStderrWarnings) {
    log_init(path_, true, LogLevel::Error);
    testing::internal::CaptureStderr();
    log_warn("dropped from file");
    log_error("kept everywhere");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("[warning] dropped from file"), std::string::npos);
    EXPECT_NE(err.find("[error] kept everywhere"), std::string::npos);
    std::string body = read_log();
    EXPECT_EQ(body.find("dropped from file"), std::string::npos);
    EXPECT_NE(body.find("[error] kept everywhere"), std::string::npos);
}

TEST_F(LogTest, FileDisabledStillWarnsOnStderr) {
    log_init(path_, false, LogLevel::Info);
    testing::internal::CaptureStderr();
    log_warn("terminal only");
    EXPECT_NE(testing::internal::GetCapturedStderr().find("[warning] terminal only"),
              std::string::npos);
    EXPECT_FALSE(fs::exists(path_));
}

TEST_F(LogTest, BracesAreLoggedVerbatim) {
    log_init(path_, true, LogLevel::Info);
    log_warn("weird {file} {} name.jpg");  // must not be treated as a fmt string
    EXPECT_NE(read_log().find("weird {file} {} name.jpg"), std::string::npos);
}

TEST_F(LogTest, RotatesAtMaxBytes) {
    log_init(path_, true, LogLevel::Info, 512);
    for (int i = 0; i < 20; i++) {
        log_info("padding entry to force rotation " + std::to_string(i));
    }
    EXPECT_TRUE(fs::exists((dir_ / "phig.1.log").string()));  // spdlog's rotated name
}

TEST_F(LogTest, LevelFromStringParsesAndThrows) {
    EXPECT_EQ(log_level_from_string("info"), LogLevel::Info);
    EXPECT_EQ(log_level_from_string("warn"), LogLevel::Warn);
    EXPECT_EQ(log_level_from_string("error"), LogLevel::Error);
    EXPECT_THROW(log_level_from_string("debug"), std::runtime_error);
}
```

(`std::count` needs `<algorithm>` — include it if the compiler complains.)

- [ ] **Step 2: Register the files and run to verify failure**

Add `src/log.cpp` to the `phig-lib` sources (`CMakeLists.txt:58`, after `src/organize_exec.cpp`) and `tests/test_log.cpp` to `phig-tests` (`CMakeLists.txt:119`, after `tests/test_organize_exec.cpp`).

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build`
Expected: FAIL to compile — `log.h` not found.

- [ ] **Step 3: Implement**

Create `src/log.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>

enum class LogLevel { Info = 0, Warn = 1, Error = 2 };

// Parse "info" | "warn" | "error"; throws std::runtime_error on anything else.
LogLevel log_level_from_string(const std::string& s);

// Build the process-wide diagnostic logger: a stderr sink (warn and above,
// "[<level>] <msg>") and, when file_enabled, a rotating file sink at `path`
// (UTC ISO-8601 timestamps, embedded newlines escaped so one line is one
// entry, file_level and above, rotating to phig.1.log at max_bytes). If the
// file sink can't be created, warns once on stderr and continues stderr-only.
// Never throws. Call once at startup (tests may call again after log_close()).
void log_init(const std::string& path, bool file_enabled, LogLevel file_level,
              std::size_t max_bytes = 5 * 1024 * 1024);

// Log one message to every sink whose level admits it. The message is logged
// verbatim (never treated as a fmt format string — braces in filenames are
// safe). Thread-safe; every entry is flushed. No-op before log_init (or after
// log_close), so library code may call unconditionally.
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// Flush and drop the logger; log_* become no-ops again. For tests.
void log_close();
```

Create `src/log.cpp` (the only TU that includes spdlog):

```cpp
#include "log.h"

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stderr_sinks.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

std::shared_ptr<spdlog::logger> g_logger;

spdlog::level::level_enum to_spdlog(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        default:
            return spdlog::level::err;
    }
}

// Custom pattern flag %*: the message with embedded newlines escaped to
// literal \n / \r, so one line in the log file is always exactly one entry.
class EscapedMessageFlag : public spdlog::custom_flag_formatter {
   public:
    void format(const spdlog::details::log_msg& msg, const std::tm&,
                spdlog::memory_buf_t& dest) override {
        for (const char c : std::string_view(msg.payload.data(), msg.payload.size())) {
            if (c == '\n') {
                dest.append(std::string_view("\\n"));
            } else if (c == '\r') {
                dest.append(std::string_view("\\r"));
            } else {
                dest.push_back(c);
            }
        }
    }
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<EscapedMessageFlag>();
    }
};

}  // namespace

LogLevel log_level_from_string(const std::string& s) {
    if (s == "info") return LogLevel::Info;
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    throw std::runtime_error("invalid log level '" + s + "' (expected info, warn, or error)");
}

void log_init(const std::string& path, bool file_enabled, LogLevel file_level,
              std::size_t max_bytes) {
    std::vector<spdlog::sink_ptr> sinks;

    auto err_sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
    err_sink->set_pattern("[%l] %v");
    err_sink->set_level(spdlog::level::warn);
    sinks.push_back(err_sink);

    if (file_enabled) {
        try {
            namespace fs = std::filesystem;
            fs::path p(path);
            if (p.has_parent_path()) {
                fs::create_directories(p.parent_path());
            }
            auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, max_bytes, 1);
            auto formatter =
                std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
            formatter->add_flag<EscapedMessageFlag>('*').set_pattern(
                "%Y-%m-%dT%H:%M:%SZ [%l] %*");
            file_sink->set_formatter(std::move(formatter));
            file_sink->set_level(to_spdlog(file_level));
            sinks.push_back(file_sink);
        } catch (const std::exception& e) {
            std::cerr << "[warning] cannot open log file: " << e.what() << "\n";
        }
    }

    g_logger = std::make_shared<spdlog::logger>("phig", sinks.begin(), sinks.end());
    g_logger->set_level(spdlog::level::info);  // per-sink levels do the filtering
    g_logger->flush_on(spdlog::level::info);   // diagnostics are low-volume; flush every entry
}

void log_info(const std::string& msg) {
    if (g_logger) g_logger->info(msg);
}
void log_warn(const std::string& msg) {
    if (g_logger) g_logger->warn(msg);
}
void log_error(const std::string& msg) {
    if (g_logger) g_logger->error(msg);
}

void log_close() {
    if (g_logger) g_logger->flush();
    g_logger.reset();
}
```

(Single-argument `logger->warn(msg)` logs the string as-is — fmt formatting only runs when extra arguments are passed, which the wrapper never does.)

- [ ] **Step 4: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='LogTest.*'`
Expected: PASS (all nine).

- [ ] **Step 5: Commit**

```bash
git add src/log.h src/log.cpp tests/test_log.cpp CMakeLists.txt
git commit -m "Add spdlog-backed diagnostic logger (stderr + rotating file sinks)"
```

---

### Task 4: Config — `[log] file` + `file_level`

**Files:**
- Modify: `src/config.h` (fields after `log_max_entries`), `src/config.cpp` (dispatch chain after the `max_entries` branch at line 133)
- Test: `tests/test_config.cpp`

**Interfaces:**
- Produces (consumed by Task 5): `Config::log_file` (`std::optional<bool>`), `Config::log_file_level` (`std::optional<std::string>`, validated `"info"|"warn"|"error"`).

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_config.cpp` (uses the existing `write_ini` fixture helper):

```cpp
TEST_F(ConfigTest, ParsesLogFileKeys) {
    std::string p = write_ini("[log]\nfile = false\nfile_level = warn\n");
    Config cfg = load_config(p);
    ASSERT_TRUE(cfg.log_file.has_value());
    EXPECT_FALSE(*cfg.log_file);
    ASSERT_TRUE(cfg.log_file_level.has_value());
    EXPECT_EQ(*cfg.log_file_level, "warn");
}

TEST_F(ConfigTest, LogFileKeysDefaultUnset) {
    std::string p = write_ini("[log]\nenabled = true\n");
    Config cfg = load_config(p);
    EXPECT_FALSE(cfg.log_file.has_value());
    EXPECT_FALSE(cfg.log_file_level.has_value());
}

TEST_F(ConfigTest, RejectsInvalidFileLevel) {
    std::string p = write_ini("[log]\nfile_level = debug\n");
    EXPECT_THROW(load_config(p), std::runtime_error);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='ConfigTest.*LogFile*:ConfigTest.RejectsInvalidFileLevel'`
Expected: FAIL to compile — `log_file` not a member.

- [ ] **Step 3: Add fields + parsing**

`src/config.h`, after `log_max_entries` (line 25):

```cpp
    std::optional<bool> log_file;  // [log] file — default true (unset)
    std::optional<std::string>
        log_file_level;  // [log] file_level — "info" | "warn" | "error", default info (unset)
```

`src/config.cpp`, add branches after the `max_entries` branch (line 136), matching the file's validation style:

```cpp
            } else if (sec == "log" && k == "file") {
                cfg.log_file = parse_bool(path, section, key, val);
            } else if (sec == "log" && k == "file_level") {
                if (val != "info" && val != "warn" && val != "error")
                    fail(path, section, key, "must be 'info', 'warn', or 'error', got " + val);
                cfg.log_file_level = val;
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `direnv exec . cmake --build build && ./build/phig-tests --gtest_filter='ConfigTest.*LogFile*:ConfigTest.RejectsInvalidFileLevel'`
Expected: PASS (all three).

- [ ] **Step 5: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "Add [log] file + file_level config keys"
```

---

### Task 5: Wire logger into `main()` — init, invocation INFO, top-level ERROR

**Files:**
- Modify: `src/main.cpp` — add `#include "log.h"`; logger init after `set_path_overrides` (line 2541); INFO invocation line after the help block (~line 2555); swap cerr→`log_error` in every top-level `catch` that prints `Error: <what>`.

**Interfaces:**
- Consumes: `log_init`/`log_info`/`log_error`/`log_level_from_string` (Task 3), `default_log_path()` (Task 1), `Config::log_file`/`log_file_level` (Task 4), existing `join_argv` (`main.cpp:58`).
- Produces: an initialized process-wide logger every later site (Task 6) writes through. `iso_utc_now` stays in `main.cpp` untouched (the activity log's timestamps are unrelated to spdlog's).

- [ ] **Step 1: Initialize the logger**

Add `#include "log.h"` to `main.cpp`'s includes. After `set_path_overrides(...)` (line 2541), add:

```cpp
    // Diagnostic logger (plan 024): stderr sink for warnings/errors plus a
    // rotating file sink at ~/.local/state/phig/phig.log unless disabled.
    // Init before any dispatch so every instrumented site has a live logger.
    log_init(default_log_path(), !cfg.log_file || *cfg.log_file,
             cfg.log_file_level ? log_level_from_string(*cfg.log_file_level) : LogLevel::Info);
```

(`log_file_level` was validated at config parse, so `log_level_from_string` cannot throw here.)

- [ ] **Step 2: Log the invocation at INFO**

Right after the help block ends (after line 2555, before the per-command `--help` scan), add:

```cpp
    log_info(join_argv(argc, argv));  // file-only: the stderr sink is warn+
```

(`--help`/`--version`/usage paths above this line intentionally log nothing.)

- [ ] **Step 3: Swap the top-level failure catches to the logger**

In every `catch (const std::exception& e)` block inside `main()` that prints `std::cerr << "Error: " << e.what() << "\n";` — currently at lines 2637, 2690, 2722, 2771, 2785, 2801, 2816, 2833, 2935 (verify the full set with `rg -n 'std::cerr << "Error: " << e.what' src/main.cpp`) — **replace** the cerr statement with:

```cpp
            log_error(command + ": " + e.what());
```

Terminal output for these becomes `[error] <command>: <what>` (via the stderr sink) instead of `Error: <what>`. The two argument-validation catches at 2911/2918 (`invalid --min-duration` / `--max-duration`) become `log_error(command + ": invalid --min-duration: " + e.what());` and the `--max-duration` equivalent. Leave untouched: the config-load catch at 2537 (runs before `log_init`) and the non-exception `Error:` usage messages.

- [ ] **Step 4: Build, test, manual check**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: all PASS.

Manual:

```bash
T=$(mktemp -d)
XDG_STATE_HOME="$T/state" ./build/phig duplicates --db "$T/nope/missing.db"; echo "exit=$?"
cat "$T/state/phig/phig.log"
```

Expected: `[error] duplicates: ...` on stderr, `exit=1`, and the file holds an `[info]` invocation line plus the same `[error]` entry, each timestamped. Then:

```bash
printf "[log]\nfile = false\n" > "$T/cfg.ini"
XDG_STATE_HOME="$T/state2" ./build/phig --config "$T/cfg.ini" duplicates --db "$T/nope/missing.db" 2>&1 | grep '\[error\]' && \
    test ! -e "$T/state2/phig/phig.log" && echo "stderr kept, no file"
```

Expected: `stderr kept, no file`.

- [ ] **Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "Route invocation info and command failures through the diagnostic logger"
```

---

### Task 6: Swap scan + hasher + faces sites to the logger

**Files:**
- Modify: `src/main.cpp` (scan worker ~562-567, mount warning ~610, scan summary ~624, faces decode ~1511 and ~1989), `src/hasher.cpp` (decode warning at 176, add `#include "log.h"`)

**Interfaces:**
- Consumes: `log_info`/`log_warn`/`log_error` (Task 3), initialized by Task 5. stdout output at every site is unchanged.

- [ ] **Step 1: Scan worker per-file error/warning**

Replace the two cerr statements in the worker's catch (`main.cpp:561-567`) — the stdout progress-line clear above them stays:

```cpp
                    if (opts.fail_on_error) {
                        log_error("scan: " + rel_path(filepath) + " -- " + e.what());
                        throw;
                    } else {
                        log_warn("scan: " + rel_path(filepath) + " -- " + e.what());
                    }
```

(The rethrow still reaches the top-level catch, which logs `scan: <what>` too — same two stderr lines as today, modulo the `[error]` prefix.)

- [ ] **Step 2: Scan mount warning**

Replace the three-sentence cerr block (`main.cpp:610-613`) with one call — the stderr sink prints the newlines as-is, the file stores one escaped line:

```cpp
            log_warn("scan: all " + std::to_string(removed_paths.size()) +
                     " previously scanned files in this directory are missing.\n"
                     "This may indicate an unmounted drive or incorrect path.\n"
                     "Use --ignore-mount-warning to remove these entries from the database.");
```

- [ ] **Step 3: Scan summary at INFO**

Next to the `Done in ...` stdout block (`main.cpp:624`), add (file-only via sink levels):

```cpp
    log_info("scan " + opts.directory + " done: processed=" + std::to_string(processed.load()) +
             " skipped=" + std::to_string(skipped.load()) +
             " errors=" + std::to_string(errors.load()) +
             " removed=" + std::to_string(removed_count));
```

- [ ] **Step 4: Hasher decode warning**

In `src/hasher.cpp`, add `#include "log.h"` and replace the cerr statement at line 176:

```cpp
        std::cerr << '\n';  // break out of the scan progress line, as before
        log_warn(path + " -- " + cv_warning);
```

- [ ] **Step 5: Faces reference-photo decode failures**

Replace the cerr statements at `main.cpp:1511` and `main.cpp:1989` (using each site's path variable):

```cpp
            log_error("Cannot decode image: " + face_path);   // 1511
            log_error("Cannot decode image: " + image_path);  // 1989
```

- [ ] **Step 6: Build, test, manual end-to-end**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: all PASS.

Manual — a corrupt image warns on the terminal and in the file; `file_level = error` keeps the terminal warning but drops it from the file:

```bash
T=$(mktemp -d); mkdir "$T/pics"
printf 'not an image' > "$T/pics/bad.jpg"
XDG_STATE_HOME="$T/state" ./build/phig scan "$T/pics" --db "$T/t.db" 2>&1 | grep '\[warning\]' \
    && echo "terminal warn ok"
grep -E '^\S+Z \[warning\] scan: .*bad\.jpg' "$T/state/phig/phig.log" && echo "file warn ok"
grep -E '\[info\] scan .* errors=1' "$T/state/phig/phig.log" && echo "summary ok"

printf "[log]\nfile_level = error\n" > "$T/cfg.ini"
XDG_STATE_HOME="$T/state2" ./build/phig --config "$T/cfg.ini" scan "$T/pics" --db "$T/t2.db" \
    2>&1 | grep '\[warning\]' && ! grep -q 'warning' "$T/state2/phig/phig.log" \
    && echo "stderr warn kept, file filtered"
```

Expected: `terminal warn ok`, `file warn ok`, `summary ok`, `stderr warn kept, file filtered`.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp src/hasher.cpp
git commit -m "Route scan/hasher/faces diagnostics through the logger"
```

---

### Task 7: Docs + lint

**Files:**
- Modify: `AGENTS.md` (tech stack, paths paragraph, Key Design Decisions), `README.md` (config keys / feature docs)

**Interfaces:** none new.

- [ ] **Step 1: Document**

In `AGENTS.md`:
- Add spdlog to the Tech Stack dependency line (line 10).
- Extend the XDG paths paragraph (~line 70): `...thumbnails at ~/.cache/phig/thumbs, diagnostic log at ~/.local/state/phig/phig.log, config at ~/.config/phig/config.ini`.
- Add a Key Design Decisions bullet after the Activity log one:

```
- **Diagnostic log** — one spdlog logger, two sinks: stderr (warnings/errors,
  `[warning]`/`[error]` prefix) and a rotating file at
  `~/.local/state/phig/phig.log` (UTC-timestamped; one line per entry with
  newlines escaped; 5 MB rotation to `phig.1.log`). Command invocations and
  scan summaries log at INFO (file only). `[log] file = false` drops the file
  sink; `[log] file_level = info|warn|error` sets its threshold. All logging
  goes through `src/log.h` — no spdlog includes elsewhere.
```

In `README.md`, document the log file location and the two `[log]` keys wherever config/XDG paths are described.

- [ ] **Step 2: Full suite + lint**

Run: `direnv exec . cmake --build build && ./build/phig-tests`
Expected: all PASS.
Run: `clang-format --dry-run --Werror src/log.h src/log.cpp src/paths.h src/paths.cpp src/config.h src/config.cpp src/main.cpp src/hasher.cpp tests/test_log.cpp tests/test_paths.cpp tests/test_config.cpp`
Expected: no output (clean).

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md README.md
git commit -m "Document diagnostic log"
```

---

## Outcome (2026-07-11)

Implemented via subagent-driven development (7 tasks, each with real-behavior
tests + spec/quality review); final whole-branch review returned READY TO MERGE.
Full suite: 305 tests passing; clang-format clean; spdlog confined to `log.cpp`.

Commits (on `master`):

| Task | Commit |
|------|--------|
| 1 — `state_dir()` + `default_log_path()` (XDG state dir) | `23d65f4` |
| 2 — spdlog dependency (devenv.nix, CMake, CI, README) | `ae0b0ef` |
| 3 — `src/log.{h,cpp}` logger wrapper (stderr + rotating file sinks) + tests | `f1acd90` |
| 4 — `[log] file` + `file_level` config keys | `9636687` |
| 5 — wire logger into `main()`: init, invocation INFO, top-level ERROR | `91ab3f3` |
| 6 — swap scan/hasher/faces diagnostics to the logger | `e4611fc` |
| 7 — document the diagnostic log (AGENTS/README) | `589e15e` |

Notes:
- spdlog is added via `pkgs.spdlog.dev` in the devenv `CMAKE_PREFIX_PATH`
  (spdlog has a separate `dev` output); resolves to spdlog 1.17.0. `find_package`
  works, no `devenv.lock` change.
- `stderr_sink_mt` is included from `spdlog/sinks/stdout_sinks.h` (v1.17.0 header
  layout), not the plan's `stderr_sinks.h` — header path only, class/behavior
  unchanged.
- Task 4 used the file's existing `parse_enum` helper for `file_level` validation
  rather than an inline check — same behavior, matches the file's other enum keys.
- Whole-branch review confirmed: thread-safe (`g_logger` set once before scan
  workers spawn; `_mt` sinks lock internally); `log_*` is a safe no-op before
  `log_init` (so phig-lib's `hasher.cpp` logging doesn't affect the test binary,
  which never inits); `log_level_from_string` in `main()` can't throw (config
  validates `file_level` at parse); multi-process append is atomic per line
  (worst case a benign rotation race, never corruption).
- Terminal format change (accepted, documented): top-level failures now print
  `[error] <command>: <what>`; scan per-file diagnostics `[warning] scan: <rel>
  -- <what>`. Argument-validation `Error:` messages and all stdout are unchanged.
- Distinct from the activity log (plan 020): this records diagnostics as text in
  `~/.local/state/phig/phig.log`; 020 records file operations in the DB.
