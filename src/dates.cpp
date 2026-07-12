#include "dates.h"

#include "sidecar.h"

DateParts parse_date(const std::string& date_str) {
    DateParts d;
    if (date_str.empty()) return d;

    // Try EXIF format: "YYYY:MM:DD HH:MM:SS"
    if (date_str.size() >= 10 && date_str[4] == ':' && date_str[7] == ':') {
        d.year = date_str.substr(0, 4);
        d.month = date_str.substr(5, 2);
        d.day = date_str.substr(8, 2);
        d.valid = (d.year != "0000" && d.year != "1970");
        return d;
    }

    // Try ISO format: "YYYY-MM-DDTHH:MM:SSZ"
    if (date_str.size() >= 10 && date_str[4] == '-' && date_str[7] == '-') {
        d.year = date_str.substr(0, 4);
        d.month = date_str.substr(5, 2);
        d.day = date_str.substr(8, 2);
        d.valid = (d.year != "0000" && d.year != "1970");
        return d;
    }

    return d;
}

// Extract a JSON string value by key (simple parser, no nesting)
std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

DateParts get_best_date(const ImageInfo& img) {
    // 1. EXIF DateTimeOriginal
    std::string dt = json_get(img.metadata_json, "DateTimeOriginal");
    DateParts d = parse_date(dt);
    if (d.valid) return d;

    // 2. EXIF CreateDate / DateTimeDigitized
    dt = json_get(img.metadata_json, "DateTimeDigitized");
    d = parse_date(dt);
    if (d.valid) return d;

    // 3. Sidecar date (e.g. f-spot .xmp, Google Takeout .json). Only consulted
    //    when the embedded EXIF has no date; a sidecar can be edited, so EXIF
    //    wins. Sits before mtime so EXIF-stripped exports still date correctly.
    for (const auto& side : find_sidecars(img.path)) {
        d = parse_date(sidecar_date(side));
        if (d.valid) return d;
    }

    // 4. File modified time
    d = parse_date(img.modified_at);
    if (d.valid) return d;

    return d;  // invalid — will go to unsorted
}

std::string get_best_date_str(const ImageInfo& img) {
    // Try EXIF DateTimeOriginal
    std::string dt = json_get(img.metadata_json, "DateTimeOriginal");
    if (!dt.empty() && dt.size() >= 10 && dt.substr(0, 4) != "0000" && dt.substr(0, 4) != "1970") {
        return dt.substr(0, 10);
    }
    // Try DateTimeDigitized
    dt = json_get(img.metadata_json, "DateTimeDigitized");
    if (!dt.empty() && dt.size() >= 10 && dt.substr(0, 4) != "0000" && dt.substr(0, 4) != "1970") {
        return dt.substr(0, 10);
    }
    // Sidecar date (f-spot .xmp / Google Takeout .json) before mtime, so search
    // display agrees with where organize files the photo.
    for (const auto& side : find_sidecars(img.path)) {
        std::string sd = sidecar_date(side);
        if (sd.size() >= 10 && sd.substr(0, 4) != "0000" && sd.substr(0, 4) != "1970") {
            return sd.substr(0, 10);
        }
    }
    // Fall back to modified_at
    if (!img.modified_at.empty() && img.modified_at.size() >= 10) {
        return img.modified_at.substr(0, 10);
    }
    return "unknown";
}
