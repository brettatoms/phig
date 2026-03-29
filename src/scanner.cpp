#include "scanner.h"
#include <algorithm>
#include <unordered_set>
#include <iostream>

namespace {

const std::unordered_set<std::string>& known_extensions() {
    static const std::unordered_set<std::string> exts = {
        // Common formats
        "jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif", "webp",
        // RAW formats
        "cr2", "cr3", "nef", "nrw", "arw", "srf", "sr2",
        "dng", "orf", "rw2", "pef", "raf", "raw", "rwl",
        "3fr", "ari", "bay", "cap", "crw", "dcr", "dcs",
        "erf", "fff", "iiq", "k25", "kdc", "mdc", "mef",
        "mos", "mrw", "obm", "ptx", "pxn", "r3d", "srw",
        "x3f",
        // Other image formats
        "heic", "heif", "avif", "jxl",
        "ico", "svg", "psd", "xcf",
        "jp2", "j2k", "jpf", "jpm", "jpx",
        "pgm", "ppm", "pbm", "pnm",
        "exr", "hdr",
    };
    return exts;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

} // anonymous namespace

bool is_known_image_extension(const std::string& ext) {
    std::string lower = to_lower(ext);
    // Strip leading dot if present
    if (!lower.empty() && lower[0] == '.') {
        lower = lower.substr(1);
    }
    return known_extensions().count(lower) > 0;
}

std::vector<fs::path> scan_directory(const std::string& dir, bool recursive) {
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

        // Skip macOS resource fork files
        if (filename.starts_with("._")) return;

        // Skip files without a known image extension
        if (!is_known_image_extension(path.extension().string())) return;

        files.push_back(path);
    };

    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(dir,
                fs::directory_options::skip_permission_denied)) {
            collect(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(dir)) {
            collect(entry);
        }
    }

    return files;
}
