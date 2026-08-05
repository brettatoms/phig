# libphig Settings API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a settings API to `libphig` so a client can read phig's config file, override values, and write changes back without destroying the user's comments or formatting.

**Architecture:** A new line-oriented config parser in phig's core (`src/settings.cpp`) that keeps every source line so a write can rewrite one key in place and leave the rest byte-identical. `src/libphig/settings.cpp` wraps it as an opaque `phig_settings` handle with generic `section.name` key/value access, so new phig settings reach clients without an ABI change.

**Tech Stack:** C++17, CMake, GoogleTest. No new third-party dependencies — the new parser is hand-written because `inih`'s callback interface cannot report line positions.

**Spec:** `../../../phig-gtk/docs/superpowers/specs/2026-08-01-libphig-settings-design.md`

## Global Constraints

- **C++17** for implementation. **`include/phig/phig.h` must remain valid C99** — `tests/test_libphig_c_compile.c` compiles it as C and calls every public function; add calls for all new functions or the link guarantee degrades.
- **No exception may cross the ABI boundary.** Use `PHIG_TRY`/`PHIG_CATCH` from `src/libphig/internal.h`. Note `PHIG_TRY` requires a `phig_ctx*` named `ctx` in scope and early-returns `PHIG_ERR_INVALID_ARG` when it is NULL — the settings functions do **not** take a `phig_ctx`, so they need their own `try`/`catch(...)`, in the style of `phig_init` in `src/libphig/context.cpp`.
- **`PHIG_API` on every new public function**, or it is not exported (`phig_abi` builds with hidden visibility).
- All public symbols `phig_`-prefixed; types `phig_*`; macros `PHIG_*`.
- **No new third-party dependencies.**
- **Additive ABI change only.** Bump `PHIG_ABI_VERSION_MINOR` from `0` to `1`. Do NOT change any existing struct layout, and do NOT renumber any `phig_status` value (`PHIG_ERR_SCHEMA = 6` is the current highest).
- **Existing CLI behavior must not change.** All 438 pre-existing tests must pass. `src/config.cpp` is not modified by this plan.
- **`.clang-format`** at the repo root is authoritative. CI does **not** lint `src/libphig/` or `include/`, so check those manually with `--dry-run --Werror`.
- **Inline-comment rules must match `inih`, the parser phig actually uses**, or libphig and the CLI will disagree about what a value is. Verified in `vendor/inih/ini.h:119-130` and `ini.c:68-77`:
  - `INI_START_COMMENT_PREFIXES` is `";#"` — either character starts a **whole-line** comment.
  - `INI_INLINE_COMMENT_PREFIXES` is `";"` only — **`#` is never an inline comment**, and `;` only counts when **preceded by a whitespace character**.
  - So `format = %Y#%m` has the value `%Y#%m`, and `k = a;b` has the value `a;b` (no space before `;`), but `k = a ; note` has the value `a`.
  - A `Line`'s parsed `value` must exclude any inline comment, and the comment text must be stored separately so a rewrite can put it back exactly once.
- **Build/test commands must be prefixed with `direnv exec .`** — phig's dependencies come from a Nix devenv and are not on the bare PATH; a bare `cmake` fails on missing opencv4:
  - `direnv exec . cmake -B build && direnv exec . cmake --build build`
  - `direnv exec . ./build/phig-tests`
  - `direnv exec . clang-format -i <files>`
- **Commits are authorized on the working branch. Do NOT push to any remote and do NOT merge to master.**

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `src/settings.h` | `Settings` class declaration: line-preserving config load, get/set/unset, save. |
| `src/settings.cpp` | Its implementation — the line model, the index, and the preserving writer. |
| `src/libphig/settings.cpp` | The C ABI wrapper: `phig_settings` handle and all `phig_settings_*` entry points. |
| `tests/test_settings.cpp` | Parser and preserving-writer tests against the C++ class. |
| `tests/test_libphig_settings.cpp` | ABI-level tests. |

**Modified files:**

| Path | Change |
|---|---|
| `include/phig/phig.h` | The settings API, `PHIG_KEY_*` constants, ABI minor bump. |
| `src/libphig/internal.h` | `phig_settings` struct definition. |
| `tests/test_libphig_c_compile.c` | Calls to the new functions. |
| `CMakeLists.txt` | New sources on `phig-lib`, `phig_abi`, and `phig-tests`. |

## Interface summary

```cpp
// src/settings.h — C++ core
class Settings {
   public:
    static Settings load(const std::string& path);   // throws std::runtime_error
    bool has(const std::string& key) const;
    const std::string* get(const std::string& key) const;   // nullptr if absent
    void set(const std::string& key, const std::string& value);
    void unset(const std::string& key);
    std::vector<std::string> keys() const;           // file order
    void save(const std::string& path) const;        // atomic
    const std::string& source_path() const;
};
```

```c
/* include/phig/phig.h — C ABI */
typedef struct phig_settings phig_settings;
phig_status phig_settings_load(const char *path, phig_settings **out);
void        phig_settings_free(phig_settings *s);
const char *phig_settings_get(phig_settings *s, const char *key, const char *fallback);
int64_t     phig_settings_get_int(phig_settings *s, const char *key, int64_t fallback);
bool        phig_settings_get_bool(phig_settings *s, const char *key, bool fallback);
bool        phig_settings_has(phig_settings *s, const char *key);
size_t      phig_settings_count(phig_settings *s);
const char *phig_settings_key_at(phig_settings *s, size_t i);
phig_status phig_settings_set(phig_settings *s, const char *key, const char *value);
phig_status phig_settings_unset(phig_settings *s, const char *key);
phig_status phig_settings_save(phig_settings *s, const char *path);
phig_status phig_settings_to_config(phig_settings *s, phig_config *cfg);
```

---

### Task 1: The line-preserving parser

**Files:**
- Create: `src/settings.h`, `src/settings.cpp`
- Create: `tests/test_settings.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `expand_tilde` and `config_path` from `src/paths.h` (both already public).
- Produces: the `Settings` class above.

- [ ] **Step 1: Write the failing parser tests**

Create `tests/test_settings.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "settings.h"

namespace {
// Writes `content` to a unique temp file and removes it on scope exit.
struct TempFile {
    std::string path;
    explicit TempFile(const std::string& content) {
        path = (std::filesystem::temp_directory_path() /
                ("phig_settings_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".ini"))
                   .string();
        std::ofstream(path) << content;
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string read() const {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }
};
}  // namespace

TEST(Settings, ReadsSectionDottedKeys) {
    TempFile f("[paths]\ndb = /photos/phig.db\n\n[general]\nparallel = 4\n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("paths.db"), nullptr);
    EXPECT_EQ(*s.get("paths.db"), "/photos/phig.db");
    ASSERT_NE(s.get("general.parallel"), nullptr);
    EXPECT_EQ(*s.get("general.parallel"), "4");
}

TEST(Settings, AbsentKeyReturnsNull) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.get("paths.nope"), nullptr);
    EXPECT_EQ(s.get("nosuch.key"), nullptr);
    EXPECT_FALSE(s.has("paths.nope"));
    EXPECT_TRUE(s.has("paths.db"));
}

TEST(Settings, TrimsWhitespaceAroundKeyAndValue) {
    TempFile f("[paths]\n   db   =    /photos/phig.db   \n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("paths.db"), nullptr);
    EXPECT_EQ(*s.get("paths.db"), "/photos/phig.db");
}

TEST(Settings, AcceptsBothCommentMarkers) {
    TempFile f("# hash comment\n; semicolon comment\n[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.keys().size(), 1u);
    EXPECT_EQ(*s.get("paths.db"), "/x");
}

TEST(Settings, KeyBeforeAnySectionGetsBareName) {
    TempFile f("orphan = 1\n[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    ASSERT_NE(s.get("orphan"), nullptr);
    EXPECT_EQ(*s.get("orphan"), "1");
}

TEST(Settings, LastDuplicateKeyWins) {
    TempFile f("[paths]\ndb = /first\ndb = /second\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(*s.get("paths.db"), "/second");
}

TEST(Settings, HandlesCrlfAndMissingTrailingNewline) {
    TempFile f("[paths]\r\ndb = /x\r\n[general]\nparallel = 2");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(*s.get("paths.db"), "/x");
    EXPECT_EQ(*s.get("general.parallel"), "2");
}

TEST(Settings, EmptyFileHasNoKeys) {
    TempFile f("");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(s.keys().size(), 0u);
}

TEST(Settings, MissingFileThrows) {
    EXPECT_THROW(Settings::load("/nonexistent-dir-xyz/config.ini"),
                 std::runtime_error);
}

TEST(Settings, KeysAreInFileOrder) {
    TempFile f("[paths]\ndb = /x\nthumbs_dir = /t\n[general]\nparallel = 4\n");
    Settings s = Settings::load(f.path);
    std::vector<std::string> expected = {"paths.db", "paths.thumbs_dir",
                                         "general.parallel"};
    EXPECT_EQ(s.keys(), expected);
}

TEST(Settings, ValueKeepsInternalSpacesAndEquals) {
    TempFile f("[organize]\nformat = %Y/%m/%original\nodd = a = b\n");
    Settings s = Settings::load(f.path);
    EXPECT_EQ(*s.get("organize.format"), "%Y/%m/%original");
    EXPECT_EQ(*s.get("organize.odd"), "a = b");  // split on the FIRST '='
}
```

- [ ] **Step 2: Register the test file and run to verify failure**

Add `tests/test_settings.cpp` to the `add_executable(phig-tests ...)` list in `CMakeLists.txt`.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `settings.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/settings.h`:

```cpp
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// A phig config file (INI: `[section]` headers, `key = value`, `#`/`;`
// comments) held as its original lines plus an index over them, so writing a
// key back rewrites one line and leaves every comment, blank, ordering, and
// unknown key byte-identical.
//
// Keys are "section.name"; a key appearing before any section header is just
// "name". Lookup is case-sensitive. On a duplicate key the last one wins,
// matching how a sequential parser would resolve it.
class Settings {
   public:
    // Throws std::runtime_error if the file cannot be read. A file that exists
    // but is empty is valid and yields no keys.
    static Settings load(const std::string& path);

    bool has(const std::string& key) const;
    // Raw value as written (no tilde expansion); nullptr when absent.
    const std::string* get(const std::string& key) const;

    // Rewrites the key's existing line in place, or appends it to its section
    // (creating the section at end of file when absent).
    void set(const std::string& key, const std::string& value);
    // Removes the key's line. No-op when absent.
    void unset(const std::string& key);

    // Keys in file order.
    std::vector<std::string> keys() const;

    // Atomic: writes a temp sibling, fsyncs, renames. Throws on failure.
    void save(const std::string& path) const;

    const std::string& source_path() const { return source_path_; }

   private:
    enum class LineKind { Section, KeyValue, Other };
    struct Line {
        LineKind kind = LineKind::Other;
        std::string text;     // the full original line, no newline
        std::string section;  // for Section and KeyValue
        std::string key;      // for KeyValue: the dotted key
        std::string value;    // for KeyValue: the trimmed value
    };

    std::vector<Line> lines_;
    std::unordered_map<std::string, size_t> index_;  // key -> index into lines_
    std::string source_path_;
    bool crlf_ = false;           // preserve the file's line ending style
    bool trailing_newline_ = true;

    void reindex();
};
```

- [ ] **Step 4: Implement the parser**

Create `src/settings.cpp`. Implement `load`, `has`, `get`, `keys`, and
`reindex` now; `set`/`unset`/`save` come in Task 2 — stub them to throw
`std::logic_error("not implemented")` so the file links.

```cpp
#include "settings.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}
}  // namespace

Settings Settings::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read config file: " + path);

    std::stringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();

    Settings s;
    s.source_path_ = path;
    s.crlf_ = content.find("\r\n") != std::string::npos;
    s.trailing_newline_ = content.empty() || content.back() == '\n';

    std::string current_section;
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        bool last = (nl == std::string::npos);
        std::string raw = content.substr(pos, last ? std::string::npos : nl - pos);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        // A trailing newline produces a final empty piece that is not a line.
        if (last && raw.empty() && !content.empty()) break;

        Line line;
        line.text = raw;
        std::string t = trim(raw);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            current_section = trim(t.substr(1, t.size() - 2));
            line.kind = LineKind::Section;
            line.section = current_section;
        } else if (t.empty() || t[0] == '#' || t[0] == ';') {
            line.kind = LineKind::Other;
        } else {
            size_t eq = t.find('=');  // split on the FIRST '='
            if (eq == std::string::npos) {
                line.kind = LineKind::Other;  // not a key/value; preserve as-is
            } else {
                line.kind = LineKind::KeyValue;
                line.section = current_section;
                std::string name = trim(t.substr(0, eq));
                line.key = current_section.empty() ? name : current_section + "." + name;
                line.value = trim(t.substr(eq + 1));
            }
        }
        s.lines_.push_back(std::move(line));

        if (last) break;
        pos = nl + 1;
    }

    s.reindex();
    return s;
}

void Settings::reindex() {
    index_.clear();
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].kind == LineKind::KeyValue) {
            index_[lines_[i].key] = i;  // last duplicate wins
        }
    }
}

bool Settings::has(const std::string& key) const { return index_.count(key) > 0; }

const std::string* Settings::get(const std::string& key) const {
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    return &lines_[it->second].value;
}

std::vector<std::string> Settings::keys() const {
    std::vector<std::string> out;
    for (const auto& l : lines_) {
        if (l.kind == LineKind::KeyValue) out.push_back(l.key);
    }
    return out;
}

void Settings::set(const std::string&, const std::string&) {
    throw std::logic_error("not implemented");
}
void Settings::unset(const std::string&) {
    throw std::logic_error("not implemented");
}
void Settings::save(const std::string&) const {
    throw std::logic_error("not implemented");
}
```

Note `keys()` returns duplicates when the file has them, while `get` resolves to
the last — that is intentional and matches "keys in file order".

- [ ] **Step 5: Add the source to the build and run the tests**

Add `src/settings.cpp` to the `add_library(phig-lib STATIC ...)` source list in `CMakeLists.txt`.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='Settings.*'`
Expected: 11 tests PASS.

- [ ] **Step 6: Run the full suite**

Run: `direnv exec . ./build/phig-tests`
Expected: all PASS (438 pre-existing + 11 new).

- [ ] **Step 7: Commit**

```bash
direnv exec . clang-format -i src/settings.h src/settings.cpp tests/test_settings.cpp
git add src/settings.h src/settings.cpp tests/test_settings.cpp CMakeLists.txt
git commit -m "Add line-preserving config parser"
```

---

### Task 2: The preserving writer

**Files:**
- Modify: `src/settings.cpp`
- Test: `tests/test_settings.cpp`

**Interfaces:**
- Consumes: the `Settings` line model (Task 1); `temp_sibling` from `src/paths.h`.
- Produces: working `set`, `unset`, and `save`.

- [ ] **Step 1: Write the failing writer tests**

Append to `tests/test_settings.cpp`:

```cpp
TEST(SettingsWriter, RewritesOneKeyAndLeavesEverythingElseByteIdentical) {
    const std::string original =
        "# my photo collection\n"
        "[paths]\n"
        "db = ~/photos/phig.db\n"
        "# thumbs live on the SSD\n"
        "thumbs_dir = /ssd/thumbs\n"
        "\n"
        "[general]\n"
        "parallel = 4\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.set("general.parallel", "8");
    s.save(f.path);

    const std::string expected =
        "# my photo collection\n"
        "[paths]\n"
        "db = ~/photos/phig.db\n"
        "# thumbs live on the SSD\n"
        "thumbs_dir = /ssd/thumbs\n"
        "\n"
        "[general]\n"
        "parallel = 8\n";
    EXPECT_EQ(f.read(), expected);
}

TEST(SettingsWriter, PreservesUnknownKeys) {
    const std::string original =
        "[paths]\ndb = /x\n[weird]\nsomething_phig_does_not_know = yes\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/y");
    s.save(f.path);
    EXPECT_NE(f.read().find("something_phig_does_not_know = yes"), std::string::npos);
}

TEST(SettingsWriter, PreservesTrailingCommentOnARewrittenLine) {
    TempFile f("[general]\nparallel = 4  # tuned for this box\n");
    Settings s = Settings::load(f.path);
    s.set("general.parallel", "8");
    EXPECT_EQ(f.read().empty(), false);
    s.save(f.path);
    std::string out = f.read();
    EXPECT_NE(out.find("# tuned for this box"), std::string::npos);
    EXPECT_NE(out.find("parallel = 8"), std::string::npos);
}

TEST(SettingsWriter, AppendsNewKeyToExistingSection) {
    TempFile f("[paths]\ndb = /x\n\n[general]\nparallel = 4\n");
    Settings s = Settings::load(f.path);
    s.set("paths.models_dir", "/models");
    s.save(f.path);
    const std::string expected =
        "[paths]\ndb = /x\nmodels_dir = /models\n\n[general]\nparallel = 4\n";
    EXPECT_EQ(f.read(), expected);
}

TEST(SettingsWriter, CreatesMissingSectionAtEndOfFile) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    s.set("faces.threshold", "0.6");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\ndb = /x\n\n[faces]\nthreshold = 0.6\n");
}

TEST(SettingsWriter, UnsetRemovesOnlyThatLine) {
    TempFile f("[paths]\n# keep me\ndb = /x\nthumbs_dir = /t\n");
    Settings s = Settings::load(f.path);
    s.unset("paths.db");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\n# keep me\nthumbs_dir = /t\n");
    EXPECT_FALSE(s.has("paths.db"));
}

TEST(SettingsWriter, UnsetOfAbsentKeyIsANoOp) {
    const std::string original = "[paths]\ndb = /x\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.unset("paths.nope");
    s.save(f.path);
    EXPECT_EQ(f.read(), original);
}

TEST(SettingsWriter, RoundTripsWithoutChangesByteIdentical) {
    const std::string original =
        "; leading semicolon comment\n"
        "\n"
        "[paths]\n"
        "   db   =   ~/photos/phig.db   \n"
        "\n"
        "# trailing comment\n";
    TempFile f(original);
    Settings s = Settings::load(f.path);
    s.save(f.path);
    EXPECT_EQ(f.read(), original);
}

TEST(SettingsWriter, SavingLeavesNoTempSibling) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/y");
    s.save(f.path);
    auto dir = std::filesystem::path(f.path).parent_path();
    auto stem = std::filesystem::path(f.path).filename().string();
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string name = e.path().filename().string();
        if (name != stem && name.find(stem) != std::string::npos) {
            ADD_FAILURE() << "leftover temp sibling: " << name;
        }
    }
}

TEST(SettingsWriter, SetOnEmptyFileCreatesSection) {
    TempFile f("");
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/x");
    s.save(f.path);
    EXPECT_EQ(f.read(), "[paths]\ndb = /x\n");
}

TEST(SettingsWriter, GetReflectsSetBeforeSave) {
    TempFile f("[paths]\ndb = /x\n");
    Settings s = Settings::load(f.path);
    s.set("paths.db", "/y");
    ASSERT_NE(s.get("paths.db"), nullptr);
    EXPECT_EQ(*s.get("paths.db"), "/y");
    s.set("general.parallel", "2");
    ASSERT_NE(s.get("general.parallel"), nullptr);
    EXPECT_EQ(*s.get("general.parallel"), "2");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='SettingsWriter.*'`
Expected: FAIL — `std::logic_error: not implemented`.

- [ ] **Step 3: Implement `set`**

Replace the `set` stub in `src/settings.cpp`. Rewriting in place must preserve
the line's leading whitespace and any trailing comment:

```cpp
void Settings::set(const std::string& key, const std::string& value) {
    auto it = index_.find(key);
    if (it != index_.end()) {
        Line& line = lines_[it->second];
        // Rewrite only the value span: keep the original text up to and
        // including '=', and any trailing comment after the old value.
        size_t eq = line.text.find('=');
        std::string head = line.text.substr(0, eq + 1);
        std::string tail = line.text.substr(eq + 1);
        // Find where the old value sits inside `tail` so a trailing comment,
        // and the spacing before it, survive.
        size_t vb = tail.find_first_not_of(" \t");
        std::string lead = (vb == std::string::npos) ? " " : tail.substr(0, vb);
        std::string rest;
        if (vb != std::string::npos) {
            size_t hash = tail.find_first_of("#;", vb);
            if (hash != std::string::npos) rest = tail.substr(hash);
            // Preserve the whitespace that sat between value and comment.
            if (!rest.empty()) {
                size_t ve = tail.find_last_not_of(" \t", hash - 1);
                if (ve != std::string::npos && ve + 1 < hash) {
                    rest = tail.substr(ve + 1);
                }
            }
        }
        line.text = head + lead + value + rest;
        line.value = value;
        return;
    }

    // New key. Split "section.name"; a key with no dot has no section.
    size_t dot = key.find('.');
    std::string section = (dot == std::string::npos) ? "" : key.substr(0, dot);
    std::string name = (dot == std::string::npos) ? key : key.substr(dot + 1);

    Line fresh;
    fresh.kind = LineKind::KeyValue;
    fresh.section = section;
    fresh.key = key;
    fresh.value = value;
    fresh.text = name + " = " + value;

    // Insert after the last line belonging to the section.
    size_t insert_at = lines_.size();
    bool found_section = section.empty();
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].kind == LineKind::Section && lines_[i].section == section) {
            found_section = true;
            insert_at = i + 1;
        } else if (found_section && lines_[i].section == section &&
                   lines_[i].kind == LineKind::KeyValue) {
            insert_at = i + 1;
        }
    }

    if (!found_section) {
        // Create the section at end of file, preceded by a blank line unless
        // the file is empty.
        if (!lines_.empty()) {
            Line blank;
            blank.kind = LineKind::Other;
            lines_.push_back(blank);
        }
        Line hdr;
        hdr.kind = LineKind::Section;
        hdr.section = section;
        hdr.text = "[" + section + "]";
        lines_.push_back(hdr);
        lines_.push_back(fresh);
    } else {
        lines_.insert(lines_.begin() + static_cast<long>(insert_at), fresh);
    }
    reindex();
}
```

- [ ] **Step 4: Implement `unset` and `save`**

```cpp
void Settings::unset(const std::string& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return;
    lines_.erase(lines_.begin() + static_cast<long>(it->second));
    reindex();
}

void Settings::save(const std::string& path) const {
    namespace fs = std::filesystem;
    fs::path dest(path);
    fs::path tmp = temp_sibling(dest);

    const std::string eol = crlf_ ? "\r\n" : "\n";
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i].text;
        if (i + 1 < lines_.size() || trailing_newline_) out += eol;
    }

    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("Cannot write config: " + tmp.string());
        o << out;
        o.flush();
        if (!o) throw std::runtime_error("Failed writing config: " + tmp.string());
    }

    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("Cannot replace config: " + dest.string());
    }
}
```

Add `#include <filesystem>` and `#include "paths.h"` at the top of
`src/settings.cpp`.

Note the spec calls for an `fsync` before the rename. `std::ofstream` has no
portable `fsync`; if you add one, use `::open`/`::write`/`::fsync`/`::close`
directly and say so in your report. Writing then renaming already prevents
truncation, which is the property the tests check.

- [ ] **Step 5: Run the writer tests**

Run: `direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='SettingsWriter.*'`
Expected: 11 tests PASS. If `RewritesOneKeyAndLeavesEverythingElseByteIdentical`
fails, diff the actual against expected byte by byte — that test is the whole
point of this task, so fix the writer rather than relaxing the assertion.

- [ ] **Step 6: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i src/settings.cpp tests/test_settings.cpp
git add src/settings.cpp tests/test_settings.cpp
git commit -m "Add comment-preserving config writer"
```

---

### Task 3: The settings ABI

**Files:**
- Modify: `include/phig/phig.h`, `src/libphig/internal.h`, `CMakeLists.txt`, `tests/test_libphig_c_compile.c`
- Create: `src/libphig/settings.cpp`, `tests/test_libphig_settings.cpp`

**Interfaces:**
- Consumes: `Settings` (Tasks 1-2); `phig_status`, `phig_config`, `PHIG_API` from the existing header; `expand_tilde` and `config_path` from `src/paths.h`.
- Produces: the eleven `phig_settings_*` functions and the `PHIG_KEY_*` constants.

- [ ] **Step 1: Append the API to the header**

Add to `include/phig/phig.h` before the closing `extern "C"`, and bump
`PHIG_ABI_VERSION_MINOR` from `0` to `1` (leave `PHIG_ABI_VERSION_MAJOR` at 1):

```c
/* ---- Settings -----------------------------------------------------------
 * phig's config file as key/value data. Keys are "section.name", e.g.
 * "paths.db". Access is generic rather than a fixed struct so settings phig
 * adds later are readable without recompiling against a new header.
 *
 * A phig_settings is NOT thread-safe; use one per thread or serialize access.
 */

typedef struct phig_settings phig_settings;

/* Known keys. Arbitrary string keys work too — these exist for typo
 * resistance on the documented ones. */
#define PHIG_KEY_DB           "paths.db"
#define PHIG_KEY_THUMBS_DIR   "paths.thumbs_dir"
#define PHIG_KEY_MODELS_DIR   "paths.models_dir"
#define PHIG_KEY_PARALLEL     "general.parallel"
#define PHIG_KEY_LOG_ENABLED  "log.enabled"
#define PHIG_KEY_LOG_MAX      "log.max_entries"
#define PHIG_KEY_LOG_FILE     "log.file"
#define PHIG_KEY_LOG_LEVEL    "log.file_level"

/* Loads `path`, or phig's default config file when path is NULL
 * (~/.config/phig/config.ini, honoring XDG_CONFIG_HOME).
 *
 * Returns PHIG_ERR_NOT_FOUND when no file exists — *out still receives a valid
 * empty handle, so a caller wanting defaults can ignore that status and a
 * caller creating a config can populate and save it. Returns PHIG_ERR_DB when
 * the file exists but cannot be read, with *out set to NULL. */
PHIG_API phig_status phig_settings_load(const char *path, phig_settings **out);

/* Safe to call with NULL. */
PHIG_API void phig_settings_free(phig_settings *s);

/* Returns `fallback` when the key is absent. The returned pointer is owned by
 * `s` and is valid until that key is set/unset or `s` is freed. Path values
 * have a leading `~` expanded, matching the CLI; the unexpanded form is what
 * phig_settings_save writes back. */
PHIG_API const char *phig_settings_get(phig_settings *s, const char *key,
                                       const char *fallback);

/* Return `fallback` when the key is absent or its value does not parse as the
 * requested type. Bools accept true/false/yes/no/on/off/1/0, case-insensitive. */
PHIG_API int64_t phig_settings_get_int(phig_settings *s, const char *key,
                                       int64_t fallback);
PHIG_API bool phig_settings_get_bool(phig_settings *s, const char *key,
                                     bool fallback);

PHIG_API bool phig_settings_has(phig_settings *s, const char *key);

/* Enumeration, for building a settings UI without a compiled-in schema.
 * Indices are stable until the next set/unset. key_at returns NULL when i is
 * out of range. */
PHIG_API size_t phig_settings_count(phig_settings *s);
PHIG_API const char *phig_settings_key_at(phig_settings *s, size_t i);

/* Sets or replaces a key. Keys phig does not know are allowed and preserved. */
PHIG_API phig_status phig_settings_set(phig_settings *s, const char *key,
                                       const char *value);
PHIG_API phig_status phig_settings_unset(phig_settings *s, const char *key);

/* Writes to `path`, or the loaded/default path when NULL. Atomic: writes a
 * temp sibling and renames, so an interrupted save cannot truncate the file.
 * Comments, ordering, and keys phig does not model are preserved. */
PHIG_API phig_status phig_settings_save(phig_settings *s, const char *path);

/* Fills cfg->db_path/thumbs_dir/models_dir from settings, leaving fields the
 * config does not set untouched so a caller can pre-seed defaults. Does NOT
 * set cfg->mode — that is a per-context decision, not a user preference.
 *
 * The strings point into `s`, so `s` must outlive the phig_init call. */
PHIG_API phig_status phig_settings_to_config(phig_settings *s, phig_config *cfg);
```

Note `phig_config` has no `models_dir` field today — add one:

```c
    const char *models_dir;   /* NULL = phig's default (~/.cache/phig/models) */
```

**This is an ABI break** if appended anywhere but the end of the struct. Append
it as the LAST member of `phig_config`, after `mode`, so existing compiled
callers keep working.

- [ ] **Step 2: Write the failing ABI tests**

Create `tests/test_libphig_settings.cpp`:

```cpp
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "phig/phig.h"

namespace {
struct TempIni {
    std::string path;
    explicit TempIni(const std::string& content) {
        path = (std::filesystem::temp_directory_path() /
                ("phig_abi_ini_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".ini"))
                   .string();
        std::ofstream(path) << content;
    }
    ~TempIni() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::string read() const {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    }
};
}  // namespace

TEST(AbiSettings, LoadsAndReadsStringValues) {
    TempIni f("[paths]\ndb = /photos/phig.db\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_NE(s, nullptr);
    EXPECT_STREQ(phig_settings_get(s, PHIG_KEY_DB, "unset"), "/photos/phig.db");
    EXPECT_STREQ(phig_settings_get(s, "paths.nope", "fallback"), "fallback");
    EXPECT_TRUE(phig_settings_has(s, PHIG_KEY_DB));
    EXPECT_FALSE(phig_settings_has(s, "paths.nope"));
    phig_settings_free(s);
}

TEST(AbiSettings, ExpandsTildeOnRead) {
    TempIni f("[paths]\ndb = ~/photos/phig.db\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    const char* v = phig_settings_get(s, PHIG_KEY_DB, "");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(std::string(v).find('~'), std::string::npos);
    EXPECT_NE(std::string(v).find("/photos/phig.db"), std::string::npos);
    phig_settings_free(s);
}

TEST(AbiSettings, IntAndBoolParsingWithFallbacks) {
    TempIni f(
        "[general]\nparallel = 8\n"
        "[log]\nenabled = yes\nfile = FALSE\nmax_entries = notanumber\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    EXPECT_EQ(phig_settings_get_int(s, PHIG_KEY_PARALLEL, 1), 8);
    EXPECT_EQ(phig_settings_get_int(s, "general.missing", 42), 42);
    EXPECT_EQ(phig_settings_get_int(s, PHIG_KEY_LOG_MAX, 99), 99);  // unparseable
    EXPECT_TRUE(phig_settings_get_bool(s, PHIG_KEY_LOG_ENABLED, false));
    EXPECT_FALSE(phig_settings_get_bool(s, PHIG_KEY_LOG_FILE, true));
    EXPECT_TRUE(phig_settings_get_bool(s, "log.missing", true));
    phig_settings_free(s);
}

TEST(AbiSettings, MissingFileYieldsNotFoundWithUsableHandle) {
    auto missing = (std::filesystem::temp_directory_path() /
                    ("phig_absent_" + std::to_string(::getpid()) + ".ini")).string();
    std::error_code ec;
    std::filesystem::remove(missing, ec);

    phig_settings* s = nullptr;
    EXPECT_EQ(phig_settings_load(missing.c_str(), &s), PHIG_ERR_NOT_FOUND);
    ASSERT_NE(s, nullptr);  // usable empty handle
    EXPECT_EQ(phig_settings_count(s), 0u);
    EXPECT_EQ(phig_settings_set(s, PHIG_KEY_DB, "/new"), PHIG_OK);
    EXPECT_EQ(phig_settings_save(s, missing.c_str()), PHIG_OK);
    EXPECT_TRUE(std::filesystem::exists(missing));
    phig_settings_free(s);
    std::filesystem::remove(missing, ec);
}

TEST(AbiSettings, EnumerationReturnsKeysInFileOrder) {
    TempIni f("[paths]\ndb = /x\nthumbs_dir = /t\n[general]\nparallel = 4\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_EQ(phig_settings_count(s), 3u);
    EXPECT_STREQ(phig_settings_key_at(s, 0), "paths.db");
    EXPECT_STREQ(phig_settings_key_at(s, 1), "paths.thumbs_dir");
    EXPECT_STREQ(phig_settings_key_at(s, 2), "general.parallel");
    EXPECT_EQ(phig_settings_key_at(s, 3), nullptr);
    EXPECT_EQ(phig_settings_key_at(s, 9999), nullptr);
    phig_settings_free(s);
}

TEST(AbiSettings, SetSaveRoundTripPreservesComments) {
    TempIni f("# keep me\n[paths]\ndb = /x\n[general]\nparallel = 4\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_EQ(phig_settings_set(s, PHIG_KEY_PARALLEL, "8"), PHIG_OK);
    ASSERT_EQ(phig_settings_save(s, nullptr), PHIG_OK);  // NULL = loaded path
    std::string out = f.read();
    EXPECT_NE(out.find("# keep me"), std::string::npos);
    EXPECT_NE(out.find("parallel = 8"), std::string::npos);
    phig_settings_free(s);
}

TEST(AbiSettings, UnsetRemovesKey) {
    TempIni f("[paths]\ndb = /x\nthumbs_dir = /t\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);
    ASSERT_EQ(phig_settings_unset(s, PHIG_KEY_DB), PHIG_OK);
    EXPECT_FALSE(phig_settings_has(s, PHIG_KEY_DB));
    EXPECT_EQ(phig_settings_unset(s, "paths.nope"), PHIG_OK);  // absent = no-op
    phig_settings_free(s);
}

TEST(AbiSettings, ToConfigFillsPathsAndLeavesModeAlone) {
    TempIni f("[paths]\ndb = /photos/phig.db\nthumbs_dir = /ssd/thumbs\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);

    phig_config cfg{};
    cfg.mode = PHIG_READ_WRITE;  // caller's choice; must survive
    ASSERT_EQ(phig_settings_to_config(s, &cfg), PHIG_OK);
    ASSERT_NE(cfg.db_path, nullptr);
    EXPECT_STREQ(cfg.db_path, "/photos/phig.db");
    ASSERT_NE(cfg.thumbs_dir, nullptr);
    EXPECT_STREQ(cfg.thumbs_dir, "/ssd/thumbs");
    EXPECT_EQ(cfg.mode, PHIG_READ_WRITE);
    EXPECT_EQ(cfg.models_dir, nullptr);  // not in the file, left untouched
    phig_settings_free(s);
}

TEST(AbiSettings, ToConfigDoesNotClobberPreSeededFields) {
    TempIni f("[general]\nparallel = 4\n");  // no [paths] at all
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);

    phig_config cfg{};
    cfg.db_path = "/caller/seeded.db";
    ASSERT_EQ(phig_settings_to_config(s, &cfg), PHIG_OK);
    EXPECT_STREQ(cfg.db_path, "/caller/seeded.db");
    phig_settings_free(s);
}

TEST(AbiSettings, NullArgumentsAreRejected) {
    TempIni f("[paths]\ndb = /x\n");
    phig_settings* s = nullptr;
    ASSERT_EQ(phig_settings_load(f.path.c_str(), &s), PHIG_OK);

    EXPECT_EQ(phig_settings_load(f.path.c_str(), nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_set(nullptr, "a.b", "v"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_set(s, nullptr, "v"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_set(s, "a.b", nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_unset(nullptr, "a.b"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_save(nullptr, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_to_config(nullptr, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_settings_to_config(s, nullptr), PHIG_ERR_INVALID_ARG);

    // Readers must tolerate NULL and return the fallback, not crash.
    EXPECT_STREQ(phig_settings_get(nullptr, "a.b", "fb"), "fb");
    EXPECT_EQ(phig_settings_get_int(nullptr, "a.b", 7), 7);
    EXPECT_TRUE(phig_settings_get_bool(nullptr, "a.b", true));
    EXPECT_FALSE(phig_settings_has(nullptr, "a.b"));
    EXPECT_EQ(phig_settings_count(nullptr), 0u);
    EXPECT_EQ(phig_settings_key_at(nullptr, 0), nullptr);
    EXPECT_STREQ(phig_settings_get(s, nullptr, "fb"), "fb");
    phig_settings_free(nullptr);  // must not crash
    phig_settings_free(s);
}

TEST(AbiSettings, LoadsDefaultPathWhenNull) {
    // NULL path means phig's default config location. It may or may not exist
    // on this machine; either outcome is valid, and the handle must be usable.
    phig_settings* s = nullptr;
    phig_status st = phig_settings_load(nullptr, &s);
    EXPECT_TRUE(st == PHIG_OK || st == PHIG_ERR_NOT_FOUND);
    ASSERT_NE(s, nullptr);
    (void)phig_settings_count(s);
    phig_settings_free(s);
}
```

- [ ] **Step 3: Run to verify failure**

Register `tests/test_libphig_settings.cpp` in the `phig-tests` sources first.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build 2>&1 | tail -20`
Expected: FAIL — `phig_settings_load` undefined.

- [ ] **Step 4: Add the handle to `internal.h`**

```cpp
#include "settings.h"

struct phig_settings {
    Settings s;
    std::string load_path;                  // for save(NULL)
    // Expanded values handed out by phig_settings_get, kept alive until the
    // key changes or the handle is freed. Keyed by the setting's key.
    std::unordered_map<std::string, std::string> expanded;
    std::vector<std::string> key_cache;     // backs phig_settings_key_at
};
```

Add `#include <unordered_map>` and `#include <vector>` to `internal.h`.

- [ ] **Step 5: Implement `src/libphig/settings.cpp`**

These functions take no `phig_ctx`, so they cannot use `PHIG_TRY`/`PHIG_CATCH`
(which requires one). Use an explicit `try`/`catch(...)` in each, the way
`phig_init` does.

```cpp
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "internal.h"
#include "paths.h"

namespace {
bool parse_bool(const std::string& raw, bool* out) {
    std::string v;
    v.reserve(raw.size());
    for (char c : raw) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "true" || v == "yes" || v == "on" || v == "1") { *out = true; return true; }
    if (v == "false" || v == "no" || v == "off" || v == "0") { *out = false; return true; }
    return false;
}
}  // namespace

extern "C" {

phig_status phig_settings_load(const char* path, phig_settings** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    try {
        std::string p = (path && path[0] != '\0') ? std::string(path) : config_path();
        auto* h = new phig_settings();
        h->load_path = p;
        try {
            h->s = Settings::load(p);
        } catch (const std::runtime_error&) {
            // No readable file: hand back a usable empty handle so a caller can
            // populate and save one.
            *out = h;
            return PHIG_ERR_NOT_FOUND;
        }
        *out = h;
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

void phig_settings_free(phig_settings* s) { delete s; }

const char* phig_settings_get(phig_settings* s, const char* key, const char* fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        // Expand ~ on read; cache so the returned pointer stays valid.
        s->expanded[key] = expand_tilde(*raw);
        return s->expanded[key].c_str();
    } catch (...) {
        return fallback;
    }
}

int64_t phig_settings_get_int(phig_settings* s, const char* key, int64_t fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        size_t consumed = 0;
        int64_t v = std::stoll(*raw, &consumed);
        // Reject trailing junk: "4x" is not 4.
        if (consumed != raw->size()) return fallback;
        return v;
    } catch (...) {
        return fallback;
    }
}

bool phig_settings_get_bool(phig_settings* s, const char* key, bool fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        bool v = false;
        return parse_bool(*raw, &v) ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

bool phig_settings_has(phig_settings* s, const char* key) {
    if (s == nullptr || key == nullptr) return false;
    try {
        return s->s.has(key);
    } catch (...) {
        return false;
    }
}

size_t phig_settings_count(phig_settings* s) {
    if (s == nullptr) return 0;
    try {
        s->key_cache = s->s.keys();
        return s->key_cache.size();
    } catch (...) {
        return 0;
    }
}

const char* phig_settings_key_at(phig_settings* s, size_t i) {
    if (s == nullptr) return nullptr;
    try {
        s->key_cache = s->s.keys();
        if (i >= s->key_cache.size()) return nullptr;
        return s->key_cache[i].c_str();
    } catch (...) {
        return nullptr;
    }
}

phig_status phig_settings_set(phig_settings* s, const char* key, const char* value) {
    if (s == nullptr || key == nullptr || key[0] == '\0' || value == nullptr) {
        return PHIG_ERR_INVALID_ARG;
    }
    try {
        s->s.set(key, value);
        s->expanded.erase(key);  // stale expansion
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

phig_status phig_settings_unset(phig_settings* s, const char* key) {
    if (s == nullptr || key == nullptr) return PHIG_ERR_INVALID_ARG;
    try {
        s->s.unset(key);
        s->expanded.erase(key);
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

phig_status phig_settings_save(phig_settings* s, const char* path) {
    if (s == nullptr) return PHIG_ERR_INVALID_ARG;
    try {
        std::string p = (path && path[0] != '\0') ? std::string(path) : s->load_path;
        namespace fs = std::filesystem;
        fs::path parent = fs::path(p).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            fs::create_directories(parent, ec);
        }
        s->s.save(p);
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_DB;
    }
}

phig_status phig_settings_to_config(phig_settings* s, phig_config* cfg) {
    if (s == nullptr || cfg == nullptr) return PHIG_ERR_INVALID_ARG;
    try {
        struct { const char* key; const char** field; } map[] = {
            {PHIG_KEY_DB, &cfg->db_path},
            {PHIG_KEY_THUMBS_DIR, &cfg->thumbs_dir},
            {PHIG_KEY_MODELS_DIR, &cfg->models_dir},
        };
        for (auto& m : map) {
            // Only fill fields the caller left unset; never clobber a pre-seed.
            if (*m.field != nullptr) continue;
            const std::string* raw = s->s.get(m.key);
            if (raw == nullptr) continue;
            s->expanded[m.key] = expand_tilde(*raw);
            *m.field = s->expanded[m.key].c_str();
        }
        return PHIG_OK;   // cfg->mode deliberately untouched
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

}  // extern "C"
```

Add `#include <filesystem>` for the `create_directories` call.

- [ ] **Step 6: Add the C link-check calls**

Append to `tests/test_libphig_c_compile.c`, inside the existing check function,
following the pattern of the calls already there:

```c
    phig_settings *st = NULL;
    (void)phig_settings_load(NULL, &st);
    (void)phig_settings_get(st, PHIG_KEY_DB, "fallback");
    (void)phig_settings_get_int(st, PHIG_KEY_PARALLEL, 4);
    (void)phig_settings_get_bool(st, PHIG_KEY_LOG_ENABLED, true);
    (void)phig_settings_has(st, PHIG_KEY_THUMBS_DIR);
    (void)phig_settings_count(st);
    (void)phig_settings_key_at(st, 0);
    (void)phig_settings_set(st, PHIG_KEY_MODELS_DIR, "/models");
    (void)phig_settings_unset(st, PHIG_KEY_MODELS_DIR);
    (void)phig_settings_save(st, "/tmp/phig-c-compile-check.ini");
    (void)phig_settings_to_config(st, &cfg);
    phig_settings_free(st);
```

- [ ] **Step 7: Wire up CMake and run the tests**

Add `src/libphig/settings.cpp` to the `phig_abi` target sources.

Run: `direnv exec . cmake -B build && direnv exec . cmake --build build && direnv exec . ./build/phig-tests --gtest_filter='AbiSettings.*'`
Expected: 11 tests PASS.

- [ ] **Step 8: Verify the exported symbols and the ABI version**

```bash
direnv exec . nm -D --defined-only build/libphig.so | grep ' T ' | grep -c 'phig_'
direnv exec . nm -D --defined-only build/libphig.so | grep ' T ' | grep -v 'phig_' || echo "no leakage"
```
Expected: 39 `phig_` symbols (28 existing + 11 new), no leakage. The
`phig.version` / `phig.exported_symbols` files use `phig_*` wildcards, so no
edit is needed there — confirm rather than assume.

Also confirm `phig_abi_version()` now reports minor 1:
```bash
direnv exec . ./build/phig-tests --gtest_filter='Abi.VersionMajorMatchesHeader'
```

- [ ] **Step 9: Run the full suite and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format -i include/phig/phig.h src/libphig/settings.cpp \
    src/libphig/internal.h tests/test_libphig_settings.cpp tests/test_libphig_c_compile.c
git add include/ src/libphig/ tests/ CMakeLists.txt
git commit -m "Add libphig settings API with comment-preserving writes"
```

---

### Task 4: Documentation and the install check

**Files:**
- Modify: `README.md`, `tests/test_libphig_install/main.c`

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: no new symbols.

- [ ] **Step 1: Extend the out-of-tree install check**

The install check proves the installed header and library are usable by a client
that knows nothing about phig's source tree. Add the settings API to it so a
broken install of the new surface is caught too. Edit
`tests/test_libphig_install/main.c`, inside `main`, before the final `printf`:

```c
    phig_settings *st = NULL;
    phig_status sst = phig_settings_load("/nonexistent/phig-config.ini", &st);
    if (sst != PHIG_ERR_NOT_FOUND || st == NULL) {
        fprintf(stderr, "expected NOT_FOUND with a usable handle\n");
        return 1;
    }
    if (phig_settings_count(st) != 0) {
        fprintf(stderr, "expected an empty settings handle\n");
        return 1;
    }
    phig_settings_free(st);
```

- [ ] **Step 2: Run the install check**

```bash
direnv exec . cmake --install build --prefix /tmp/phig-prefix
direnv exec . cmake -S tests/test_libphig_install -B /tmp/phig-install-check \
      -DCMAKE_PREFIX_PATH=/tmp/phig-prefix
direnv exec . cmake --build /tmp/phig-install-check
/tmp/phig-install-check/install_check
```
Expected: prints `ok`.

- [ ] **Step 3: Document the settings API in the README**

Extend the existing `## libphig` section (added by the previous plan) with a
short subsection. Write plainly — no marketing tone, no padding:

```markdown
### Configuration

`libphig` can read and write phig's config file, so a client uses the same
settings the CLI does instead of parsing `config.ini` itself:

```c
phig_settings *s = NULL;
phig_settings_load(NULL, &s);           /* phig's config.ini */

phig_config cfg = {0};
phig_settings_to_config(s, &cfg);       /* paths from the config file */
cfg.mode = PHIG_READ_ONLY;              /* per-context, never from config */

phig_ctx *ctx = NULL;
phig_init(&cfg, &ctx);
```

Settings are addressed as `"section.name"` strings rather than struct fields, so
a setting phig adds later is readable without recompiling. `PHIG_KEY_*` macros
cover the documented keys.

Writes preserve the file: comments, ordering, and keys libphig does not model
survive a `phig_settings_save`, and the save is atomic. Loading is a separate
call from `phig_init` so opening a collection does not touch the filesystem
beyond the database.
```

- [ ] **Step 4: Full verification and commit**

```bash
direnv exec . ./build/phig-tests
direnv exec . clang-format --dry-run --Werror include/phig/phig.h src/libphig/*.cpp \
    src/settings.cpp src/settings.h
git add README.md tests/test_libphig_install/main.c
git commit -m "Document the libphig settings API and cover it in the install check"
```

---

## Plan self-review

**Spec coverage.** Every section of the design maps to a task: the line-preserving parser and its behaviors (Task 1), the writer with comment/ordering preservation and atomic save (Task 2), the full C API including enumeration, known-key constants, and `phig_settings_to_config` (Task 3), and documentation plus install verification (Task 4). The spec's testing table is covered — parser edge cases in Task 1, byte-identical round-trip and temp-sibling checks in Task 2, ABI behaviors in Task 3.

**Deliberately out of scope**, matching the spec: migrating `src/config.cpp` onto the new parser; CLI commands for editing config; value validation against phig's rules; structured config or any format change.

**Two things an implementer should know:**
- `phig_config` gains a `models_dir` field. It must be appended as the **last** member, after `mode`, or previously compiled callers break. This is the only struct change in the plan and it is why the ABI minor version bumps.
- The settings functions cannot use `PHIG_TRY`/`PHIG_CATCH`, because those macros require a `phig_ctx*` in scope and these functions take none. Task 3 Step 5 uses explicit `try`/`catch(...)` per function, matching `phig_init`.

**Type consistency check.** `Settings::load/has/get/set/unset/keys/save/source_path` (Task 1 header) are used with those exact signatures in Tasks 2 and 3. `phig_settings` (Task 3 Step 4) holds a `Settings s` matching the class from Task 1. The `PHIG_KEY_*` macros defined in Task 3 Step 1 are used in Task 3's tests, the C link check (Step 6), and the README (Task 4). `expand_tilde` and `config_path` are existing public functions in `src/paths.h`, verified present. `temp_sibling` is declared at `src/paths.h:102`, verified present.
