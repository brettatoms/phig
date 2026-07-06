#pragma once

#include <optional>
#include <set>
#include <string>

// XDG base directories, each returning phig's per-application subdirectory.
// Honor the corresponding XDG_* env var when set and non-empty; otherwise fall
// back to the XDG default under $HOME (on all platforms). Throw
// std::runtime_error when neither the XDG var nor HOME is available.
std::string config_dir();  // $XDG_CONFIG_HOME or ~/.config,      + /phig
std::string data_dir();    // $XDG_DATA_HOME   or ~/.local/share, + /phig
std::string cache_dir();   // $XDG_CACHE_HOME  or ~/.cache,       + /phig

// Derived paths.
std::string default_db_path();  // data_dir()  + /phig.db
std::string models_dir();       // cache_dir() + /models
std::string thumbs_dir();       // cache_dir() + /thumbs
std::string config_path();      // config_dir() + /config.ini

// Expand a leading ~ to $HOME. Paths not starting with ~ are returned unchanged,
// as are ~ paths when HOME is unset.
std::string expand_tilde(const std::string& path);

// Move a file from source to dest, creating dest's parent directories. Uses
// rename(2) on the fast path; when source and dest are on different filesystems
// (rename fails with EXDEV) falls back to copy-then-remove. Overwrites an
// existing dest. Throws std::filesystem_error on failure.
void move_file(const std::string& source, const std::string& dest);

// Copy source to dest atomically: copy into a temp file in dest's directory,
// then rename it into place only after the copy fully succeeds. A crash or
// error mid-copy leaves the temp behind (best-effort removed) and never
// truncates an existing dest. Creates dest's parent dirs. Throws on failure.
void atomic_copy_file(const std::string& source, const std::string& dest);

// Write `size` bytes atomically to dest via temp-then-rename (see above).
// Returns false on any I/O error (temp is removed); true once dest holds the
// complete bytes. Creates dest's parent dirs.
bool atomic_write_file(const std::string& dest, const void* data, size_t size);

// Resolve a destination path against the on_conflict policy ("skip",
// "overwrite", or "rename"). A path conflicts if it exists on disk OR is already
// present in `claimed` (destinations chosen by earlier files in the same run) —
// this catches two source files that resolve to the same destination within one
// batch, not just collisions with pre-existing files. Returns the resolved path
// (added to `claimed`), or an empty path meaning "skip this file".
std::string resolve_conflict(const std::string& dest, const std::string& on_conflict,
                             std::set<std::string>& claimed);

// Install process-wide path overrides (typically from the config file). When set,
// default_db_path()/models_dir()/thumbs_dir() return the override instead of the
// computed XDG path. Pass std::nullopt to leave a given path computed. Called once
// at startup; also used by tests to reset state.
void set_path_overrides(const std::optional<std::string>& db,
                        const std::optional<std::string>& models,
                        const std::optional<std::string>& thumbs);
