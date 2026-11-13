#include "organize_plan.h"

#include <string_view>

namespace {

// The tokens apply_format expands. A token that is a prefix of another comes after it,
// so "%make" is not read as "%m".
constexpr std::string_view kTokens[] = {"%original", "%camera", "%gallery", "%make",
                                        "%fav",      "%Y",      "%m",       "%d"};

std::string_view token_at(const std::string& fmt, size_t i) {
    for (std::string_view t : kTokens) {
        if (fmt.compare(i, t.size(), t) == 0) return t;
    }
    return {};
}

// A value from the file or its row, made into one path component: a separator becomes
// '_' and a control character is dropped, so EXIF or a gallery name cannot add
// directories or climb out of the destination (R87). A value that is then empty, "."
// or ".." takes the token's placeholder.
std::string component(const std::string& value, const char* placeholder) {
    std::string out;
    for (char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\') {
            out += '_';
        } else if (u >= 0x20 && u != 0x7f) {
            out += c;
        }
    }
    if (out.empty() || out == "." || out == "..") return placeholder;
    return out;
}

}  // namespace

std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date) {
    std::string result;
    for (size_t i = 0; i < fmt.size(); i++) {
        const std::string_view t = fmt[i] == '%' ? token_at(fmt, i) : std::string_view{};
        if (t.empty()) {
            result += fmt[i];
            continue;
        }
        if (t == "%original") {
            result += img.filename;
        } else if (t == "%camera") {
            result += component(json_get(img.metadata_json, "Model"), "unknown-camera");
        } else if (t == "%make") {
            result += component(json_get(img.metadata_json, "Make"), "unknown-make");
        } else if (t == "%gallery") {
            result += component(img.gallery, "no-gallery");
        } else if (t == "%fav") {
            result += img.favorite ? "favorites" : "unfavorited";
        } else if (t == "%Y") {
            result += date.valid ? date.year : "unsorted";
        } else if (t == "%m") {
            result += date.valid ? date.month : "unsorted";
        } else if (t == "%d") {
            result += date.valid ? date.day : "unsorted";
        }
        i += t.size() - 1;
    }
    return result;
}

std::string first_unknown_token(const std::string& fmt) {
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') continue;
        const std::string_view t = token_at(fmt, i);
        if (t.empty()) return fmt.substr(i, 2);
        i += t.size() - 1;
    }
    return "";
}
