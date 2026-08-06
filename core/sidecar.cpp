#include "sidecar.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "scanner.h"

namespace fs = std::filesystem;

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Value of attribute `attr="..."` in `xml`, or "".
std::string attr_value(const std::string& xml, const std::string& attr) {
    std::string key = attr + "=\"";
    auto pos = xml.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    auto end = xml.find('"', pos);
    if (end == std::string::npos) return "";
    return xml.substr(pos, end - pos);
}

// Format y/m/d/H/M/S into "YYYY:MM:DD HH:MM:SS", or "" if any field is out of
// range. Callers pass unvalidated sscanf output from sidecar files phig does not
// control, so a nonsense year like 99999 must be rejected rather than formatted:
// it would not fit the fixed EXIF shape and would silently yield a wrong date.
std::string exif_shape(int y, int mo, int d, int h, int mi, int s) {
    if (y < 1 || y > 9999) return "";
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return "";
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) return "";  // 60 = leap second
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    return buf;
}

// Parse the two XMP value shapes into EXIF shape, or "".
std::string normalize_xmp_date(const std::string& v) {
    if (v.empty()) return "";
    // ISO: "YYYY-MM-DDThh:mm:ss"
    if (v.size() >= 19 && v[4] == '-' && v[7] == '-' && (v[10] == 'T' || v[10] == ' ')) {
        int y, mo, d, h, mi, s;
        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): checked via == 6 below.
        if (std::sscanf(v.c_str(), "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6)
            return exif_shape(y, mo, d, h, mi, s);
        return "";
    }
    // f-spot/US: "M/D/YYYY h:mm:ss AM|PM"
    {
        int mo, d, y, h, mi, s;
        char ampm[3] = {0, 0, 0};
        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion): checked via >= 6 below.
        int n = std::sscanf(v.c_str(), "%d/%d/%d %d:%d:%d %2s", &mo, &d, &y, &h, &mi, &s, ampm);
        if (n >= 6) {
            if (n == 7) {
                if ((ampm[0] == 'P' || ampm[0] == 'p') && h < 12) h += 12;
                if ((ampm[0] == 'A' || ampm[0] == 'a') && h == 12) h = 0;
            }
            return exif_shape(y, mo, d, h, mi, s);
        }
    }
    return "";
}

std::string json_string_value(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
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
        // append-style sidecar of the same extension; when media_file has one
        // AND a same-stem sibling media file exists to claim the plain file,
        // the stem-style file belongs to that *other* sibling instead. If
        // media_file is alone (no same-stem sibling media file), it keeps the
        // stem-style sidecar even when it also has its own append-style one —
        // ceding it would strand the sidecar and, if it carries the date,
        // make the photo date by mtime instead.
        if (base == media_stem) {
            const fs::path append_style_sibling = parent / (media_name + p.extension().string());
            bool has_own_append = fs::exists(append_style_sibling, ec);
            bool sibling_media_exists = false;
            if (has_own_append) {
                for (const auto& sib : fs::directory_iterator(parent, ec)) {
                    if (!sib.is_regular_file()) continue;
                    const fs::path& sp = sib.path();
                    if (sp.filename() == media_file.filename()) continue;
                    if (sp.stem().string() == media_stem &&
                        is_known_media_extension(sp.extension().string())) {
                        sibling_media_exists = true;
                        break;
                    }
                }
            }
            if (!has_own_append || !sibling_media_exists) out.push_back(p);
        }
    }
    return out;
}

std::string sidecar_date(const fs::path& sidecar_file) {
    std::string ext = sidecar_file.extension().string();
    std::string content = read_all(sidecar_file);
    if (content.empty()) return "";

    std::string lower_ext = to_lower(ext);
    if (!lower_ext.empty() && lower_ext[0] == '.') lower_ext = lower_ext.substr(1);

    if (lower_ext == "json") {
        // Google Takeout: photoTakenTime.timestamp is epoch seconds (UTC).
        // Scope the search to the photoTakenTime object so a sibling
        // creationTime.timestamp (upload time) is never picked by mistake.
        std::string ts;
        auto obj = content.find("\"photoTakenTime\"");
        if (obj != std::string::npos) {
            ts = json_string_value(content.substr(obj), "timestamp");
        }
        if (!ts.empty()) {
            std::time_t t = static_cast<std::time_t>(std::strtoll(ts.c_str(), nullptr, 10));
            std::tm tm{};
            gmtime_r(&t, &tm);
            return exif_shape(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                              tm.tm_sec);
        }
        return "";
    }

    // XMP: prefer CreateDate, then DateTimeOriginal.
    std::string v = attr_value(content, "xmp:CreateDate");
    std::string d = normalize_xmp_date(v);
    if (!d.empty()) return d;
    v = attr_value(content, "exif:DateTimeOriginal");
    return normalize_xmp_date(v);
}
