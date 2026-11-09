#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Current UTC time as an ISO-8601 timestamp ("YYYY-MM-DDTHH:MM:SSZ"), for
// activity-log operation rows (plan 020).
std::string iso_utc_now();

// XDG base directories, each returning phig's per-application subdirectory.
// Honor the corresponding XDG_* env var when set and non-empty; otherwise fall
// back to the XDG default under $HOME (on all platforms). Throw
// std::runtime_error when neither the XDG var nor HOME is available.
std::string config_dir();  // $XDG_CONFIG_HOME or ~/.config,      + /phig
std::string data_dir();    // $XDG_DATA_HOME   or ~/.local/share, + /phig
std::string cache_dir();   // $XDG_CACHE_HOME  or ~/.cache,       + /phig
std::string state_dir();   // $XDG_STATE_HOME  or ~/.local/state, + /phig

// A catalog is the directory holding what phig keeps about one collection:
// its database, the database's writer lock, the trash, the thumbnail cache
// and the diagnostic log. The images themselves live wherever they were
// scanned. Face models and the config file are user-level and not part of it.
struct CatalogPaths {
    std::string dir;
    std::string db;      // dir + "/phig.db"
    std::string lock;    // lock_path(db)
    std::string trash;   // dir + "/trash"
    std::string thumbs;  // dir + "/thumbs"
    std::string log;     // dir + "/phig.log", rotating to phig.1.log
};

// The advisory writer lock for the database at db_path: db_path + ".lock".
std::string lock_path(const std::string& db_path);

CatalogPaths catalog_paths(const std::string& dir);

// The catalog used when none is named: data_dir().
std::string default_catalog_dir();

// The process's catalog. Installed once at startup, by the CLI from its
// flags, environment and config, by phig_init and by the gtk client; one
// process works on one catalog. Until one is installed, current_catalog()
// is catalog_paths(default_catalog_dir()), computed on each call so a
// test that changes XDG_DATA_HOME sees the change. Safe from any thread: the
// installed catalog and models directory are copied out under a mutex.
void set_catalog(const CatalogPaths& paths);
void clear_catalog();
CatalogPaths current_catalog();

// Paths inside current_catalog(), for code that needs only one of them.
std::string default_db_path();
std::string thumbs_dir();
std::string trash_dir();
std::string default_log_path();

std::string models_dir();   // the set_models_dir() value, else cache_dir() + /models
std::string config_path();  // config_dir() + /config.ini

// Overrides models_dir(); std::nullopt restores the default.
void set_models_dir(const std::optional<std::string>& dir);

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

// Thrown by write_output_file when something is already at the path and the
// caller did not ask to replace it.
class OutputExists : public std::runtime_error {
   public:
    explicit OutputExists(const std::string& path)
        : std::runtime_error(path + " already exists"), path_(path) {}
    const std::string& path() const { return path_; }

   private:
    std::string path_;
};

// Writes `contents` to `path` through a temporary sibling and a rename, so the
// path never holds a partial file. Without `replace`, the rename refuses to
// replace anything at `path`, a symlink included, and throws OutputExists.
// With it, the rename replaces the directory entry: a symlink is replaced
// itself, never the file it points at. Throws on any other failure.
void write_output_file(const std::string& path, std::string_view contents, bool replace);

// Creates a new file in the temporary directory with a unique name starting
// with `prefix`, exclusively and readable only by the owner, writes
// `contents`, and returns its path. Never opens an existing path, so a file or
// symlink planted in the temporary directory is never written through.
// The caller removes the file.
std::string write_private_temp_file(const std::string& prefix, std::string_view contents);

// How `paths_safe_to_delete` classified a set of DB-tracked paths. Paths whose
// file still exists are not collected: that's the normal case and there is
// nothing to report about it.
//
// `parent_missing` and `check_failed` carry paths rather than counts so a caller
// can list them, and they are kept apart because they warrant different
// treatment: a missing parent is a heuristic judgement a caller may override,
// while a failed check is an unknown state that must never be overridden.
struct SafeToDeleteResult {
    std::vector<std::string> deletable;       // file gone, parent present
    std::vector<std::string> parent_missing;  // possible unmount; overridable
    std::vector<std::string> check_failed;    // fs::exists errored (EACCES/EIO)
};

// From a set of DB-tracked paths, classify each as "no longer on disk" or not.
// A path is deletable ONLY when the file is genuinely gone AND its parent
// directory still exists. A missing parent directory means the subtree is
// unmounted or unreachable (not that the file was deleted), and an existence
// check that errors (EACCES/EIO) is an unknown state — so a transient
// mount/permission problem never purges rows for intact files.
SafeToDeleteResult paths_safe_to_delete(const std::vector<std::string>& db_paths);

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

// Internal: temporary sibling path for atomic writes (exposed for testing).
// A temp path in the same directory as dest (so the final rename stays on one
// filesystem and is atomic). Includes the pid and a per-process counter so
// concurrent writers and repeated calls don't collide.
std::filesystem::path temp_sibling(const std::filesystem::path& dest);
