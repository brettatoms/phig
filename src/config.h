#pragma once

#include <optional>
#include <string>

// Parsed configuration. Every field is optional: unset means "not specified in
// the config file", so callers apply their own built-in default (CLI flags then
// override config values).
struct Config {
    std::optional<std::string> db;
    std::optional<std::string> models_dir;
    std::optional<std::string> thumbs_dir;
    std::optional<int>         parallel;
    std::optional<bool>        scan_recursive;
    std::optional<std::string> scan_on_error;       // "warn" | "fail"
    std::optional<std::string> scan_media;          // "image" | "video" | "all"
    std::optional<bool>        scan_thumbs;
    std::optional<bool>        scan_faces;
    std::optional<std::string> organize_format;
    std::optional<std::string> duplicates_type;     // "exact" | "near" | "all"
    std::optional<int>         duplicates_threshold; // 0-64
    std::optional<double>      faces_threshold;
};

// Load configuration from an INI file at `path`.
//   - Missing file  -> a Config with all fields unset (defaults).
//   - Recognized keys are parsed, validated, and populated. Path values have a
//     leading ~ expanded.
//   - Unknown keys   -> warning on stderr, ignored.
// Throws std::runtime_error on a malformed file, an excluded key
// (force/dry_run/on_conflict/ignore_mount_warning), or an invalid value.
Config load_config(const std::string& path);
