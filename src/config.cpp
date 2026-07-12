#include "config.h"

#include <cctype>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

#include "INIReader.h"
#include "paths.h"

namespace {

// Lowercase and map '-' to '_' so keys/sections match regardless of the
// separator style the user chose (INIReader already lowercases).
std::string normalize(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '-') c = '_';
    }
    return s;
}

[[noreturn]] void fail(const std::string& path, const std::string& section, const std::string& key,
                       const std::string& msg) {
    throw std::runtime_error("config " + path + ": [" + section + "] " + key + ": " + msg);
}

int parse_int(const std::string& path, const std::string& sec, const std::string& key,
              const std::string& val) {
    try {
        size_t pos = 0;
        int n = std::stoi(val, &pos);
        if (pos != val.size()) throw std::invalid_argument("trailing characters");
        return n;
    } catch (const std::exception&) {
        fail(path, sec, key, "expected an integer, got '" + val + "'");
    }
}

double parse_real(const std::string& path, const std::string& sec, const std::string& key,
                  const std::string& val) {
    try {
        size_t pos = 0;
        double d = std::stod(val, &pos);
        if (pos != val.size()) throw std::invalid_argument("trailing characters");
        return d;
    } catch (const std::exception&) {
        fail(path, sec, key, "expected a number, got '" + val + "'");
    }
}

bool parse_bool(const std::string& path, const std::string& sec, const std::string& key,
                const std::string& val) {
    std::string v = normalize(val);
    if (v == "true" || v == "yes" || v == "on" || v == "1") return true;
    if (v == "false" || v == "no" || v == "off" || v == "0") return false;
    fail(path, sec, key, "expected a boolean (true/false), got '" + val + "'");
}

std::string parse_enum(const std::string& path, const std::string& sec, const std::string& key,
                       const std::string& val, std::initializer_list<const char*> allowed) {
    std::string v = normalize(val);
    std::string list;
    for (const char* a : allowed) {
        if (v == a) return v;
        list += (list.empty() ? "" : ", ");
        list += a;
    }
    fail(path, sec, key, "must be one of {" + list + "}, got '" + val + "'");
}

}  // namespace

Config load_config(const std::string& path) {
    Config cfg;

    INIReader reader(path);
    int err = reader.ParseError();
    if (err == -1) {
        return cfg;  // no file -> defaults
    }
    if (err != 0) {
        throw std::runtime_error("config " + path + ": " + reader.ParseErrorMessage());
    }

    for (const std::string& section : reader.Sections()) {
        const std::string sec = normalize(section);
        for (const std::string& key : reader.Keys(section)) {
            const std::string k = normalize(key);
            const std::string val = reader.Get(section, key, "");

            // Never configurable — set these per-command on the CLI.
            if (k == "force" || k == "dry_run" || k == "on_conflict" ||
                k == "ignore_mount_warning") {
                fail(path, section, key,
                     "is not configurable; set it per-command on the command line");
            }

            if (sec == "paths" && k == "db") {
                cfg.db = expand_tilde(val);
            } else if (sec == "paths" && k == "models_dir") {
                cfg.models_dir = expand_tilde(val);
            } else if (sec == "paths" && k == "thumbs_dir") {
                cfg.thumbs_dir = expand_tilde(val);
            } else if (sec == "general" && k == "parallel") {
                int n = parse_int(path, section, key, val);
                if (n < 1) fail(path, section, key, "must be >= 1, got " + val);
                cfg.parallel = n;
            } else if (sec == "scan" && k == "recursive") {
                cfg.scan_recursive = parse_bool(path, section, key, val);
            } else if (sec == "scan" && k == "on_error") {
                cfg.scan_on_error = parse_enum(path, section, key, val, {"warn", "fail"});
            } else if (sec == "scan" && k == "media") {
                cfg.scan_media = parse_enum(path, section, key, val, {"image", "video", "all"});
            } else if (sec == "scan" && k == "thumbs") {
                cfg.scan_thumbs = parse_bool(path, section, key, val);
            } else if (sec == "scan" && k == "faces") {
                cfg.scan_faces = parse_bool(path, section, key, val);
            } else if (sec == "organize" && k == "format") {
                if (val.empty()) fail(path, section, key, "must not be empty");
                cfg.organize_format = val;
            } else if (sec == "duplicates" && k == "type") {
                cfg.duplicates_type = parse_enum(path, section, key, val, {"exact", "near", "all"});
            } else if (sec == "duplicates" && k == "threshold") {
                int n = parse_int(path, section, key, val);
                if (n < 0 || n > 64)
                    fail(path, section, key, "must be between 0 and 64, got " + val);
                cfg.duplicates_threshold = n;
            } else if (sec == "faces" && k == "threshold") {
                cfg.faces_threshold = parse_real(path, section, key, val);
            } else if (sec == "log" && k == "enabled") {
                cfg.log_enabled = parse_bool(path, section, key, val);
            } else if (sec == "log" && k == "max_entries") {
                int n = parse_int(path, section, key, val);
                if (n < 0) fail(path, section, key, "must be >= 0, got " + val);
                cfg.log_max_entries = n;
            } else if (sec == "log" && k == "file") {
                cfg.log_file = parse_bool(path, section, key, val);
            } else if (sec == "log" && k == "file_level") {
                cfg.log_file_level = parse_enum(path, section, key, val, {"info", "warn", "error"});
            } else {
                std::cerr << "Warning: config " << path << ": unknown key [" << section << "] "
                          << key << " (ignored)\n";
            }
        }
    }

    return cfg;
}
