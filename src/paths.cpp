#include "paths.h"

#include <cstdlib>
#include <stdexcept>

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

} // namespace

std::string config_dir() { return xdg_base("XDG_CONFIG_HOME", "/.config") + "/phig"; }
std::string data_dir() { return xdg_base("XDG_DATA_HOME", "/.local/share") + "/phig"; }
std::string cache_dir() { return xdg_base("XDG_CACHE_HOME", "/.cache") + "/phig"; }

std::string default_db_path() { return data_dir() + "/phig.db"; }
std::string models_dir() { return cache_dir() + "/models"; }
std::string thumbs_dir() { return cache_dir() + "/thumbs"; }
std::string config_path() { return config_dir() + "/config.ini"; }

std::string expand_tilde(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}
