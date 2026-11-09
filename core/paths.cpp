#include "paths.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

std::string iso_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

namespace {

// Resolve an XDG base directory: use $var when set and non-empty, otherwise
// $HOME + home_fallback. Empty-string env vars are treated as unset.
std::string xdg_base(const char* var, const char* home_fallback) {
    const char* xdg = std::getenv(var);
    if (xdg && xdg[0] != '\0') {
        return xdg;
    }
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("Cannot determine home directory");
    }
    return std::string(home) + home_fallback;
}

// Guards g_catalog and g_models_override: thumbnail workers read them while
// phig_init may install a catalog on another thread.
std::mutex g_paths_mutex;
std::optional<CatalogPaths> g_catalog;
std::optional<std::string> g_models_override;

}  // namespace

std::string config_dir() { return xdg_base("XDG_CONFIG_HOME", "/.config") + "/phig"; }
std::string data_dir() { return xdg_base("XDG_DATA_HOME", "/.local/share") + "/phig"; }
std::string cache_dir() { return xdg_base("XDG_CACHE_HOME", "/.cache") + "/phig"; }
std::string state_dir() { return xdg_base("XDG_STATE_HOME", "/.local/state") + "/phig"; }

std::string lock_path(const std::string& db_path) { return db_path + ".lock"; }

CatalogPaths catalog_paths(const std::string& dir) {
    CatalogPaths p;
    p.dir = dir;
    p.db = dir + "/phig.db";
    p.lock = lock_path(p.db);
    p.trash = dir + "/trash";
    p.thumbs = dir + "/thumbs";
    p.log = dir + "/phig.log";
    return p;
}

std::string default_catalog_dir() { return data_dir(); }

void set_catalog(const CatalogPaths& paths) {
    std::lock_guard<std::mutex> guard(g_paths_mutex);
    g_catalog = paths;
}
void clear_catalog() {
    std::lock_guard<std::mutex> guard(g_paths_mutex);
    g_catalog.reset();
}
CatalogPaths current_catalog() {
    {
        std::lock_guard<std::mutex> guard(g_paths_mutex);
        if (g_catalog) return *g_catalog;
    }
    return catalog_paths(default_catalog_dir());
}

std::string default_db_path() { return current_catalog().db; }
std::string thumbs_dir() { return current_catalog().thumbs; }
std::string trash_dir() { return current_catalog().trash; }
std::string default_log_path() { return current_catalog().log; }

std::string models_dir() {
    {
        std::lock_guard<std::mutex> guard(g_paths_mutex);
        if (g_models_override) return *g_models_override;
    }
    return cache_dir() + "/models";
}
std::string config_path() { return config_dir() + "/config.ini"; }

void set_models_dir(const std::optional<std::string>& dir) {
    std::lock_guard<std::mutex> guard(g_paths_mutex);
    g_models_override = dir;
}

std::string trash_path_for(const std::string& filename) {
    namespace fs = std::filesystem;
    // 8 random hex chars (32 bits) inserted before the extension so the trash
    // name is unique without scanning for a free slot (no TOCTOU race). The
    // caller moves via move_file(), which never clobbers and retries with a
    // fresh name on the astronomically-rare collision.
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);
    char hex[9];
    for (int i = 0; i < 8; i++) hex[i] = "0123456789abcdef"[dist(rng)];
    hex[8] = '\0';

    fs::path name(filename);
    std::string ext = name.extension().string();  // ".jpg" or ""
    std::string stem = name.stem().string();      // "IMG_1234" (or full name if no ext)
    std::string tagged = stem + "." + hex + ext;  // IMG_1234.<hex>.jpg  /  NOEXT.<hex>
    return (fs::path(trash_dir()) / tagged).string();
}

std::string expand_tilde(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

// A temp path in the same directory as dest (so the final rename stays on one
// filesystem and is atomic). Includes the pid and a per-process counter so
// concurrent writers and repeated calls don't collide.
std::filesystem::path temp_sibling(const std::filesystem::path& dest) {
    static std::atomic<unsigned long> counter{0};
    std::string name = "." + dest.filename().string() + ".phig-tmp." +
                       std::to_string(static_cast<long>(getpid())) + "." +
                       std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return dest.parent_path() / name;
}

namespace {
// Flush a file's bytes to stable storage. Returns false on failure.
bool fsync_file(const std::filesystem::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    int rc = ::fsync(fd);
    ::close(fd);
    return rc == 0;
}

// Best-effort: make a completed rename in `dir` durable. Failure is ignored —
// the data itself was already fsynced.
void fsync_dir(const std::filesystem::path& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

// Rename that NEVER overwrites dest. Returns 0 on success or an errno:
// EEXIST = dest exists, EXDEV = cross-device (caller falls back), other =
// real failure. Uses the kernel's atomic no-replace rename where available,
// then a hard-link emulation, then (for filesystems with neither, e.g. FAT)
// a check-then-rename with a small TOCTOU window — still far safer than a
// blind rename. renameat2()/RENAME_NOREPLACE are glibc symbols declared in
// <cstdio> (under _GNU_SOURCE, which g++ defines by default), not <fcntl.h>;
// <fcntl.h> is included above only for AT_FDCWD/O_DIRECTORY.
int rename_noreplace(const char* src, const char* dst) {
#if defined(__linux__)
    if (::renameat2(AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE) == 0) return 0;
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP) return errno;
#elif defined(__APPLE__)
    if (::renamex_np(src, dst, RENAME_EXCL) == 0) return 0;
    if (errno != ENOTSUP) return errno;
#endif
    if (::link(src, dst) == 0) {
        if (::unlink(src) != 0) return errno;
        return 0;
    }
    if (errno == EEXIST) return EEXIST;
    std::error_code ec;
    if (std::filesystem::exists(dst, ec)) return EEXIST;
    if (::rename(src, dst) == 0) return 0;
    return errno;
}

// Writes every byte of `data` to `fd`, retrying on EINTR and short writes.
// Returns 0 or the errno of the failing write.
int write_all(int fd, std::string_view data) {
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno;
        }
        written += static_cast<size_t>(n);
    }
    return 0;
}
}  // namespace

void move_file(const std::string& source, const std::string& dest) {
    namespace fs = std::filesystem;
    fs::path src(source), dst(dest);
    fs::create_directories(dst.parent_path());

    int err = rename_noreplace(source.c_str(), dest.c_str());
    if (err == 0) return;
    if (err == EEXIST || err == ENOTEMPTY) {
        // A file appeared at dest after conflict planning (concurrent phig
        // run, sync app, ...). Never overwrite it — surface a per-file error.
        throw fs::filesystem_error("move_file: destination already exists", src, dst,
                                   std::make_error_code(std::errc::file_exists));
    }
    if (err != EXDEV) {
        throw fs::filesystem_error("cannot rename", src, dst,
                                   std::error_code(err, std::generic_category()));
    }

    // Cross-device: copy to a temp sibling of dest, fsync so the bytes are
    // durable BEFORE the source is unlinked (a power loss must never leave
    // the only copy truncated), then no-replace rename into place.
    fs::path tmp = temp_sibling(dst);
    std::error_code ec;
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::error_code copy_err = ec;
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        throw fs::filesystem_error("move_file: copy failed", src, dst, copy_err);
    }
    if (!fsync_file(tmp)) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("move_file: fsync failed", tmp, dst,
                                   std::error_code(errno, std::generic_category()));
    }
    err = rename_noreplace(tmp.string().c_str(), dest.c_str());
    if (err != 0) {
        fs::remove(tmp, ec);
        if (err == EEXIST) {
            throw fs::filesystem_error("move_file: destination already exists", src, dst,
                                       std::make_error_code(std::errc::file_exists));
        }
        throw fs::filesystem_error("move_file: rename failed", tmp, dst,
                                   std::error_code(err, std::generic_category()));
    }
    fsync_dir(dst.parent_path());
    fs::remove(src);
}

std::string unique_backup_path(const std::string& dest) {
    // Same pid+counter scheme as temp_sibling so a backup name can never collide
    // with a stale backup from a prior crashed run, a concurrent phig process, or
    // a real file the user happens to own. Distinct suffix so it's identifiable.
    namespace fs = std::filesystem;
    static unsigned long counter = 0;
    fs::path d(dest);
    std::string name = "." + d.filename().string() + ".phig-bak." +
                       std::to_string(static_cast<long>(getpid())) + "." +
                       std::to_string(counter++);
    return (d.parent_path() / name).string();
}

void atomic_copy_file(const std::string& source, const std::string& dest) {
    namespace fs = std::filesystem;
    fs::path dst(dest);
    fs::create_directories(dst.parent_path());
    fs::path tmp = temp_sibling(dst);

    std::error_code ec;
    // overwrite_existing on the TEMP guards against a stale temp from a prior
    // crashed run; the real dest is only touched by the atomic rename below.
    fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("atomic_copy_file: copy failed", source, dst, ec);
    }
    // User data is being placed: make the bytes durable before the rename
    // publishes the path, so a power loss can't leave a valid-looking
    // truncated dest. (Thumbnails skip this — regenerable cache.)
    if (!fsync_file(tmp)) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("atomic_copy_file: fsync failed", tmp, dst,
                                   std::error_code(errno, std::generic_category()));
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("atomic_copy_file: rename failed", tmp, dst, ec);
    }
    fsync_dir(dst.parent_path());
}

bool atomic_write_file(const std::string& dest, const void* data, size_t size) {
    namespace fs = std::filesystem;
    fs::path dst(dest);
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = temp_sibling(dst);

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }  // close before rename

    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void write_output_file(const std::string& path, std::string_view contents, bool replace) {
    namespace fs = std::filesystem;
    const std::string tmp = temp_sibling(fs::path(path)).string();
    int fd;
    do {
        fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Cannot write " + path);
    }
    int err = write_all(fd, contents);
    if (::close(fd) != 0 && err == 0) err = errno;
    if (err != 0) {
        ::unlink(tmp.c_str());
        throw std::system_error(err, std::generic_category(), "Cannot write " + path);
    }
    if (replace) {
        if (::rename(tmp.c_str(), path.c_str()) != 0) {
            err = errno;
            ::unlink(tmp.c_str());
            throw std::system_error(err, std::generic_category(), "Cannot write " + path);
        }
        return;
    }
    err = rename_noreplace(tmp.c_str(), path.c_str());
    if (err != 0) {
        ::unlink(tmp.c_str());
        if (err == EEXIST) throw OutputExists(path);
        throw std::system_error(err, std::generic_category(), "Cannot write " + path);
    }
}

std::string write_private_temp_file(const std::string& prefix, std::string_view contents) {
    namespace fs = std::filesystem;
    std::string name = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
    std::vector<char> buf(name.begin(), name.end());
    buf.push_back('\0');
    // mkstemp creates with O_CREAT | O_EXCL and mode 0600: it never opens a
    // path that already exists, so a planted file or symlink is never used.
    int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        int err = errno;
        throw std::system_error(
            err, std::generic_category(),
            "Cannot create a temporary file in " + fs::temp_directory_path().string());
    }
    const std::string path(buf.data());
    int err = write_all(fd, contents);
    ::close(fd);
    if (err != 0) {
        ::unlink(path.c_str());
        throw std::system_error(err, std::generic_category(), "Cannot write " + path);
    }
    return path;
}

SafeToDeleteResult paths_safe_to_delete(const std::vector<std::string>& db_paths) {
    namespace fs = std::filesystem;
    SafeToDeleteResult result;
    for (const auto& p : db_paths) {
        std::error_code ec;
        bool exists = fs::exists(p, ec);
        if (ec) {
            result.check_failed.push_back(p);  // EACCES/EIO -> unknown state
            continue;
        }
        if (exists) continue;  // file still there -> not an orphan

        // File is gone. Only deletable if its parent directory still exists — a
        // missing parent means the subtree is unmounted/unreachable, not that
        // this file was individually removed.
        fs::path parent = fs::path(p).parent_path();
        bool parent_exists = fs::exists(parent, ec);
        if (ec) {
            result.check_failed.push_back(p);
            continue;
        }
        if (!parent_exists) {
            result.parent_missing.push_back(p);
            continue;
        }

        result.deletable.push_back(p);
    }
    return result;
}

std::string resolve_conflict(const std::string& dest, const std::string& on_conflict,
                             std::set<std::string>& claimed,
                             const std::function<bool(const std::string&)>& is_protected) {
    namespace fs = std::filesystem;
    auto taken = [&](const fs::path& p) { return fs::exists(p) || claimed.count(p.string()) > 0; };

    fs::path d(dest);
    // A free, unprotected path is chosen directly. A protected path (another
    // batch source, or an untracked on-disk file) is never chosen even when it
    // looks "free" of claims — treat it as a conflict and rename around it.
    if (!taken(d) && !is_protected(d.string())) {
        claimed.insert(d.string());
        return d.string();
    }

    if (on_conflict == "overwrite") {
        // Overwrite existing files, but never clobber a protected path and never
        // clobber a path already claimed this run (two sources -> one dest).
        // Both cases fall through to renaming so the other file survives.
        if (claimed.count(d.string()) == 0 && !is_protected(d.string())) {
            claimed.insert(d.string());
            return d.string();
        }
    } else if (on_conflict == "skip") {
        return "";  // empty = skip
    }

    // rename: append numeric suffix until we find a free name
    auto stem = d.stem().string();
    auto ext = d.extension().string();
    auto parent = d.parent_path();
    for (int i = 1; i < 10000; i++) {
        std::string name = stem;
        name += "_";
        name += std::to_string(i);
        name += ext;
        fs::path candidate = parent / name;
        if (!taken(candidate) && !is_protected(candidate.string())) {
            claimed.insert(candidate.string());
            return candidate.string();
        }
    }

    return "";  // give up
}
