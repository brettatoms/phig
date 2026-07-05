#pragma once

#include <optional>
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

// Install process-wide path overrides (typically from the config file). When set,
// default_db_path()/models_dir()/thumbs_dir() return the override instead of the
// computed XDG path. Pass std::nullopt to leave a given path computed. Called once
// at startup; also used by tests to reset state.
void set_path_overrides(const std::optional<std::string>& db,
                        const std::optional<std::string>& models,
                        const std::optional<std::string>& thumbs);
