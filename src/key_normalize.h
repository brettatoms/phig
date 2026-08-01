#pragma once

#include <cctype>
#include <string>

// Lowercase and map '-' to '_', so a key/section matches regardless of the
// separator style or casing the user wrote it in. Shared by config.cpp's INI
// loader (CLI-facing) and Settings (libphig-facing) — both parse the same
// config file and must resolve a key to the same identity, or a GUI edit and
// a CLI read disagree about which line is which.
inline std::string normalize_key(const std::string& key) {
    std::string out = key;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '-') c = '_';
    }
    return out;
}
