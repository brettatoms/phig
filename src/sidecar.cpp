#include "sidecar.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace fs = std::filesystem;

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

bool is_known_sidecar_extension(const std::string& ext) {
    static const std::array<const char*, 8> kExts = {"xmp", "pp3",  "dop", "on1",
                                                     "aae", "json", "thm", "lrv"};
    std::string lower = to_lower(ext);
    if (!lower.empty() && lower[0] == '.') lower = lower.substr(1);
    for (const char* e : kExts)
        if (lower == e) return true;
    return false;
}

std::vector<fs::path> find_sidecars(const fs::path& media_file) {
    std::vector<fs::path> out;
    fs::path parent = media_file.parent_path();
    if (parent.empty()) parent = ".";
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) return out;

    const std::string media_name = media_file.filename().string();  // IMG_1234.CR2
    const std::string media_stem = media_file.stem().string();      // IMG_1234

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (p.filename() == media_file.filename()) continue;  // never itself

        const std::string ext = p.extension().string();  // ".xmp"
        if (!is_known_sidecar_extension(ext)) continue;

        const std::string base = p.stem().string();  // filename minus final ext
        // append style: base == media_name       (IMG_1234.CR2.xmp -> IMG_1234.CR2)
        if (base == media_name) {
            out.push_back(p);
            continue;
        }
        // stem style: base == media_stem (IMG_1234.xmp -> IMG_1234). Siblings
        // sharing the same stem (e.g. a RAW+JPEG pair) can each have their own
        // append-style sidecar of the same extension; when media_file has one,
        // the plain stem-style file belongs to a *different* sibling instead.
        if (base == media_stem) {
            const fs::path append_style_sibling = parent / (media_name + p.extension().string());
            if (!fs::exists(append_style_sibling, ec)) out.push_back(p);
        }
    }
    return out;
}
