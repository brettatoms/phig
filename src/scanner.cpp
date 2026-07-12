#include "scanner.h"

#include <algorithm>
#include <iostream>
#include <unordered_set>

namespace {

const std::unordered_set<std::string>& raw_extensions() {
    static const std::unordered_set<std::string> exts = {
        "cr2", "cr3", "nef", "nrw", "arw", "srf", "sr2", "dng", "orf", "rw2", "pef", "raf",
        "raw", "rwl", "3fr", "ari", "bay", "cap", "crw", "dcr", "dcs", "erf", "fff", "iiq",
        "k25", "kdc", "mdc", "mef", "mos", "mrw", "obm", "ptx", "pxn", "r3d", "srw", "x3f",
    };
    return exts;
}

const std::unordered_set<std::string>& known_extensions() {
    static const std::unordered_set<std::string> exts = [] {
        std::unordered_set<std::string> s = {
            // Common formats
            "jpg",
            "jpeg",
            "png",
            "gif",
            "bmp",
            "tiff",
            "tif",
            "webp",
            // Other image formats
            "heic",
            "heif",
            "avif",
            "jxl",
            "ico",
            "svg",
            "psd",
            "xcf",
            "jp2",
            "j2k",
            "jpf",
            "jpm",
            "jpx",
            "pgm",
            "ppm",
            "pbm",
            "pnm",
            "exr",
            "hdr",
        };
        s.insert(raw_extensions().begin(), raw_extensions().end());
        return s;
    }();
    return exts;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

const std::unordered_set<std::string>& known_video_extensions() {
    static const std::unordered_set<std::string> exts = {
        "mp4", "mov", "avi", "mkv",  "webm", "m4v", "wmv",
        "flv", "3gp", "mts", "m2ts", "ts",   "mpg", "mpeg",
    };
    return exts;
}

}  // anonymous namespace

bool is_known_image_extension(const std::string& ext) {
    std::string lower = to_lower(ext);
    // Strip leading dot if present
    if (!lower.empty() && lower[0] == '.') {
        lower = lower.substr(1);
    }
    return known_extensions().count(lower) > 0;
}

bool is_raw_extension(const std::string& ext) {
    std::string lower = to_lower(ext);
    // Strip leading dot if present
    if (!lower.empty() && lower[0] == '.') {
        lower = lower.substr(1);
    }
    return raw_extensions().count(lower) > 0;
}

bool is_known_video_extension(const std::string& ext) {
    std::string lower = to_lower(ext);
    if (!lower.empty() && lower[0] == '.') {
        lower = lower.substr(1);
    }
    return known_video_extensions().count(lower) > 0;
}

bool is_known_media_extension(const std::string& ext) {
    return is_known_image_extension(ext) || is_known_video_extension(ext);
}

namespace {

bool is_jpeg_extension(const std::string& ext) {
    std::string lower = to_lower(ext);
    if (!lower.empty() && lower[0] == '.') {
        lower = lower.substr(1);
    }
    return lower == "jpg" || lower == "jpeg";
}

}  // anonymous namespace

bool has_sibling_raw(const fs::path& media_path) {
    if (!is_jpeg_extension(media_path.extension().string())) return false;

    std::error_code ec;
    fs::path dir = media_path.parent_path();
    if (!fs::exists(dir, ec) || ec) return false;

    std::string target_stem = to_lower(media_path.stem().string());

    auto it = fs::directory_iterator(dir, ec);
    if (ec) return false;
    for (const auto& end = fs::directory_iterator(); it != end; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (!entry.is_regular_file(ec)) continue;
        if (to_lower(entry.path().stem().string()) != target_stem) continue;
        if (is_raw_extension(entry.path().extension().string())) return true;
    }

    return false;
}

namespace {

// A hidden or temp name: dotfiles/dotdirs and editor/sync temp files.
// This matches a single path component's literal name, not a shell path:
// a leading '~' means a filename that actually begins with a tilde (e.g. a
// '~tmp' temp dir), never '~/...' home-directory expansion, which the shell
// resolves before phig sees the argument.
bool is_hidden_name(const std::string& name) {
    return !name.empty() && (name[0] == '.' || name[0] == '~');
}

}  // anonymous namespace

std::vector<fs::path> scan_directory(const std::string& dir, bool recursive, bool include_hidden) {
    std::vector<fs::path> files;

    if (!fs::exists(dir)) {
        throw std::runtime_error("Directory does not exist: " + dir);
    }

    if (!fs::is_directory(dir)) {
        throw std::runtime_error("Not a directory: " + dir);
    }

    auto collect = [&](const auto& entry) {
        if (!entry.is_regular_file()) return;

        const auto& path = entry.path();
        std::string filename = path.filename().string();

        // Skip hidden/temp files unless explicitly included. This also
        // covers macOS resource forks (._*) and sync temp files (.pending-*).
        if (!include_hidden && is_hidden_name(filename)) return;

        // Skip files without a known media extension
        if (!is_known_media_extension(path.extension().string())) return;

        files.push_back(path);
    };

    if (recursive) {
        // Iterate manually so hidden directories can be pruned (not descended
        // into). The scan root itself is honored even if hidden, because
        // pruning only applies to directories discovered during the walk.
        auto it =
            fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied);
        for (auto end = fs::recursive_directory_iterator(); it != end; ++it) {
            const auto& entry = *it;
            if (!include_hidden && entry.is_directory() &&
                is_hidden_name(entry.path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            collect(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dir)) {
            collect(entry);
        }
    }

    return files;
}
