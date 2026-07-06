#include "paths.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

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

std::optional<std::string> g_db_override;
std::optional<std::string> g_models_override;
std::optional<std::string> g_thumbs_override;

}  // namespace

std::string config_dir() { return xdg_base("XDG_CONFIG_HOME", "/.config") + "/phig"; }
std::string data_dir() { return xdg_base("XDG_DATA_HOME", "/.local/share") + "/phig"; }
std::string cache_dir() { return xdg_base("XDG_CACHE_HOME", "/.cache") + "/phig"; }

std::string default_db_path() { return g_db_override ? *g_db_override : data_dir() + "/phig.db"; }
std::string models_dir() {
    return g_models_override ? *g_models_override : cache_dir() + "/models";
}
std::string thumbs_dir() {
    return g_thumbs_override ? *g_thumbs_override : cache_dir() + "/thumbs";
}
std::string config_path() { return config_dir() + "/config.ini"; }

void set_path_overrides(const std::optional<std::string>& db,
                        const std::optional<std::string>& models,
                        const std::optional<std::string>& thumbs) {
    g_db_override = db;
    g_models_override = models;
    g_thumbs_override = thumbs;
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

void move_file(const std::string& source, const std::string& dest) {
    namespace fs = std::filesystem;
    fs::path src(source), dst(dest);
    fs::create_directories(dst.parent_path());

    std::error_code ec;
    fs::rename(src, dst, ec);
    if (ec == std::errc::cross_device_link) {
        // Source and dest are on different filesystems; rename(2) can't cross a
        // device boundary. Fall back to an atomic copy (temp-then-rename so a
        // failed/interrupted copy never truncates an existing dest) + remove.
        atomic_copy_file(source, dest);
        fs::remove(src);
    } else if (ec) {
        throw fs::filesystem_error("cannot rename", src, dst, ec);
    }
}

namespace {
// A temp path in the same directory as dest (so the final rename stays on one
// filesystem and is atomic). Includes the pid and a per-process counter so
// concurrent writers and repeated calls don't collide.
std::filesystem::path temp_sibling(const std::filesystem::path& dest) {
    static unsigned long counter = 0;
    std::string name = "." + dest.filename().string() + ".phig-tmp." +
                       std::to_string(static_cast<long>(getpid())) + "." +
                       std::to_string(counter++);
    return dest.parent_path() / name;
}
}  // namespace

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
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw fs::filesystem_error("atomic_copy_file: rename failed", tmp, dst, ec);
    }
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

std::vector<std::string> paths_safe_to_delete(const std::vector<std::string>& db_paths) {
    namespace fs = std::filesystem;
    std::vector<std::string> deletable;
    for (const auto& p : db_paths) {
        std::error_code ec;
        bool exists = fs::exists(p, ec);
        if (ec) continue;      // existence check errored (EACCES/EIO) -> keep
        if (exists) continue;  // file still there -> keep

        // File is gone. Only delete if its parent directory still exists — a
        // missing parent means the subtree is unmounted/unreachable, not that
        // this file was individually removed.
        fs::path parent = fs::path(p).parent_path();
        bool parent_exists = fs::exists(parent, ec);
        if (ec || !parent_exists) continue;  // parent gone/unknown -> keep

        deletable.push_back(p);
    }
    return deletable;
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
