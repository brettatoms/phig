#include "organize_plan.h"

std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date) {
    std::string result;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            // Check for multi-char tokens first
            if (fmt.substr(i, 9) == "%original") {
                result += img.filename;
                i += 8;
            } else if (fmt.substr(i, 7) == "%camera") {
                std::string cam = json_get(img.metadata_json, "Model");
                result += cam.empty() ? "unknown-camera" : cam;
                i += 6;
            } else if (fmt.substr(i, 5) == "%make") {
                std::string make = json_get(img.metadata_json, "Make");
                result += make.empty() ? "unknown-make" : make;
                i += 4;
            } else if (fmt.substr(i, 8) == "%gallery") {
                result += img.gallery.empty() ? "no-gallery" : img.gallery;
                i += 7;
            } else if (fmt.substr(i, 4) == "%fav") {
                result += img.favorite ? "favorites" : "unfavorited";
                i += 3;
            } else if (fmt[i + 1] == 'Y') {
                result += date.valid ? date.year : "unsorted";
                i += 1;
            } else if (fmt[i + 1] == 'm') {
                result += date.valid ? date.month : "unsorted";
                i += 1;
            } else if (fmt[i + 1] == 'd') {
                result += date.valid ? date.day : "unsorted";
                i += 1;
            } else {
                result += fmt[i];  // unknown token, keep as-is
            }
        } else {
            result += fmt[i];
        }
    }
    return result;
}
