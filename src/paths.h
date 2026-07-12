#pragma once

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

// XDG base directories, each returning phig's per-application subdirectory.
// Honor the corresponding XDG_* env var when set and non-empty; otherwise fall
// back to the XDG default under $HOME (on all platforms). Throw
// std::runtime_error when neither the XDG var nor HOME is available.
std::string config_dir();  // $XDG_CONFIG_HOME or ~/.config,      + /phig
std::string data_dir();    // $XDG_DATA_HOME   or ~/.local/share, + /phig
std::string cache_dir();   // $XDG_CACHE_HOME  or ~/.cache,       + /phig
std::string state_dir();   // $XDG_STATE_HOME  or ~/.local/state, + /phig

// Derived paths.
std::string default_db_path();   // data_dir()  + /phig.db
std::string models_dir();        // cache_dir() + /models
std::string thumbs_dir();        // cache_dir() + /thumbs
std::string config_path();       // config_dir() + /config.ini
std::string default_log_path();  // state_dir() + /phig.log

std::string trash_dir();    // data_dir() + /trash  (phig-managed trash)

// A non-colliding absolute path inside trash_dir() for a file named `filename`:
// trash_dir()/filename if free, else trash_dir()/filename.1, .2, … The returned
// path does not exist yet. Does NOT create trash_dir().
std::string trash_path_for(const std::string& filename);

// Expand a leading ~ to $HOME. Paths not starting with ~ are returned unchanged,
// as are ~ paths when HOME is unset.
std::string expand_tilde(const std::string& path);

// Move a file from source to dest, creating dest's parent directories. Never
// overwrites an existing dest — throws std::filesystem_error (errc::file_exists)
// if dest already exists. Same-device moves use a no-replace rename; cross-device
// moves (EXDEV) fall back to a fsync'd copy-then-remove. Throws
// std::filesystem_error on any other failure.
void move_file(const std::string& source, const std::string& dest);

// Copy source to dest atomically: copy into a temp file in dest's directory,
// then rename it into place only after the copy fully succeeds. A crash or
// error mid-copy leaves the temp behind (best-effort removed) and never
// truncates an existing dest. Creates dest's parent dirs. Throws on failure.
void atomic_copy_file(const std::string& source, const std::string& dest);

// A unique sibling path for temporarily backing up an existing file before an
// overwrite (pid + per-process counter, like the atomic-copy temp names), so the
// backup can never clobber a stale backup, a concurrent process's file, or a
// real file. Does not create anything — just returns the path.
std::string unique_backup_path(const std::string& dest);

// Write `size` bytes atomically to dest via temp-then-rename (see above).
// Returns false on any I/O error (temp is removed); true once dest holds the
// complete bytes. Creates dest's parent dirs.
bool atomic_write_file(const std::string& dest, const void* data, size_t size);

// From a set of DB-tracked paths, return the subset that is safe to delete as
// "no longer on disk". A path is deletable ONLY when the file is genuinely gone
// AND its parent directory still exists. A missing parent directory means the
// subtree is unmounted or unreachable (not that the file was deleted), and an
// existence check that errors (EACCES/EIO) is treated as "unknown, keep" — so a
// transient mount/permission problem never purges rows for intact files.
std::vector<std::string> paths_safe_to_delete(const std::vector<std::string>& db_paths);

// Resolve a destination path against the on_conflict policy ("skip",
// "overwrite", or "rename"). A path conflicts if it exists on disk OR is already
// present in `claimed` (destinations chosen by earlier files in the same run) —
// this catches two source files that resolve to the same destination within one
// batch, not just collisions with pre-existing files. Returns the resolved path
// (added to `claimed`), or an empty path meaning "skip this file".
//
// `is_protected(path)` guards paths that must NEVER be overwritten even under
// the "overwrite" policy — e.g. another image's not-yet-moved source file, or a
// real file on disk that the database doesn't track. When the resolved dest is
// protected, "overwrite" falls through to renaming so the protected file
// survives. Pass a predicate that always returns false to disable the guard.
std::string resolve_conflict(const std::string& dest, const std::string& on_conflict,
                             std::set<std::string>& claimed,
                             const std::function<bool(const std::string&)>& is_protected);

// Install process-wide path overrides (typically from the config file). When set,
// default_db_path()/models_dir()/thumbs_dir() return the override instead of the
// computed XDG path. Pass std::nullopt to leave a given path computed. Called once
// at startup; also used by tests to reset state.
void set_path_overrides(const std::optional<std::string>& db,
                        const std::optional<std::string>& models,
                        const std::optional<std::string>& thumbs);
