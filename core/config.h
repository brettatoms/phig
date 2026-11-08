#pragma once

#include <optional>
#include <string>

// Parsed configuration. Every field is optional: unset means "not specified in
// the config file", so callers apply their own built-in default (CLI flags then
// override config values).
struct Config {
    std::optional<std::string> catalog;  // [paths] catalog: the default catalog directory
    std::optional<std::string> models_dir;
    std::optional<int> parallel;
    std::optional<bool> scan_recursive;
    std::optional<std::string> scan_on_error;  // "warn" | "fail"
    std::optional<std::string> scan_media;     // "image" | "video" | "all"
    std::optional<bool> scan_thumbs;
    std::optional<bool> scan_faces;
    std::optional<std::string> organize_format;
    std::optional<std::string> duplicates_type;  // "exact" | "near" | "all"
    std::optional<int> duplicates_threshold;     // 0-64
    std::optional<double> faces_threshold;
    std::optional<bool> log_enabled;  // [log] enabled — default true (unset)
    std::optional<int>
        log_max_entries;           // [log] max_entries — default 100000 (unset); 0 = unlimited
    std::optional<bool> log_file;  // [log] file — default true (unset)
    std::optional<std::string>
        log_file_level;  // [log] file_level — "info" | "warn" | "error", default info (unset)
};

// Load configuration from an INI file at `path`.
//   - Missing file  -> a Config with all fields unset (defaults).
//   - Recognized keys are parsed, validated, and populated. Path values have a
//     leading ~ expanded.
//   - Unknown keys   -> warning on stderr, ignored.
// Throws std::runtime_error on a malformed file, an excluded key
// (force/dry_run/on_conflict/ignore_mount_warning), or an invalid value.
Config load_config(const std::string& path);
