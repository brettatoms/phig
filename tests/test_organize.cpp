#include <gtest/gtest.h>
#include "types.h"
#include "filters.h"
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace organize_helpers {

struct DateParts {
    std::string year = "unknown";
    std::string month = "unknown";
    std::string day = "unknown";
    bool valid = false;
};

DateParts parse_date(const std::string& date_str) {
    DateParts d;
    if (date_str.empty()) return d;

    if (date_str.size() >= 10 && date_str[4] == ':' && date_str[7] == ':') {
        d.year = date_str.substr(0, 4);
        d.month = date_str.substr(5, 2);
        d.day = date_str.substr(8, 2);
        d.valid = (d.year != "0000" && d.year != "1970");
        return d;
    }

    if (date_str.size() >= 10 && date_str[4] == '-' && date_str[7] == '-') {
        d.year = date_str.substr(0, 4);
        d.month = date_str.substr(5, 2);
        d.day = date_str.substr(8, 2);
        d.valid = (d.year != "0000" && d.year != "1970");
        return d;
    }

    return d;
}

std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date) {
    std::string result;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            if (fmt.substr(i, 9) == "%original") {
                result += img.filename;
                i += 8;
            } else if (fmt.substr(i, 7) == "%camera") {
                std::string cam = json_get(img.exif_json, "Model");
                result += cam.empty() ? "unknown-camera" : cam;
                i += 6;
            } else if (fmt.substr(i, 5) == "%make") {
                std::string make = json_get(img.exif_json, "Make");
                result += make.empty() ? "unknown-make" : make;
                i += 4;
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
                result += fmt[i];
            }
        } else {
            result += fmt[i];
        }
    }
    return result;
}

} // namespace organize_helpers

using namespace organize_helpers;

// ---- Glob matching tests ----

TEST(GlobTest, ExactMatch) {
    EXPECT_TRUE(glob_match("hello.jpg", "hello.jpg"));
    EXPECT_FALSE(glob_match("hello.jpg", "world.jpg"));
}

TEST(GlobTest, StarWildcard) {
    EXPECT_TRUE(glob_match("*.jpg", "photo.jpg"));
    EXPECT_TRUE(glob_match("*.jpg", "a.jpg"));
    EXPECT_FALSE(glob_match("*.jpg", "photo.png"));
}

TEST(GlobTest, StarPrefix) {
    EXPECT_TRUE(glob_match("IMG_*", "IMG_1234.jpg"));
    EXPECT_TRUE(glob_match("IMG_*", "IMG_"));
    EXPECT_FALSE(glob_match("IMG_*", "DSC_1234.jpg"));
}

TEST(GlobTest, StarMiddle) {
    EXPECT_TRUE(glob_match("photo*2024*", "photo_summer_2024.jpg"));
    EXPECT_FALSE(glob_match("photo*2024*", "photo_summer_2023.jpg"));
}

TEST(GlobTest, QuestionMark) {
    EXPECT_TRUE(glob_match("IMG_????.jpg", "IMG_1234.jpg"));
    EXPECT_FALSE(glob_match("IMG_????.jpg", "IMG_12345.jpg"));
    EXPECT_FALSE(glob_match("IMG_????.jpg", "IMG_123.jpg"));
}

TEST(GlobTest, EmptyPattern) {
    EXPECT_TRUE(glob_match("", ""));
    EXPECT_FALSE(glob_match("", "something"));
}

TEST(GlobTest, StarMatchesEmpty) {
    EXPECT_TRUE(glob_match("*", ""));
    EXPECT_TRUE(glob_match("*", "anything"));
}

TEST(GlobTest, MultipleStars) {
    EXPECT_TRUE(glob_match("*.*", "photo.jpg"));
    EXPECT_FALSE(glob_match("*.*", "noextension"));
}

// ---- Filter tests ----

TEST(FilterTest, NoFiltersPassesAll) {
    EXPECT_TRUE(passes_filters("anything.jpg", {}, {}));
}

TEST(FilterTest, MatchIncludesMatching) {
    EXPECT_TRUE(passes_filters("vacation.jpg", {"vacation*"}, {}));
    EXPECT_FALSE(passes_filters("winter.jpg", {"vacation*"}, {}));
}

TEST(FilterTest, MultipleMatches) {
    std::vector<std::string> matches = {"vacation*", "IMG_*"};
    EXPECT_TRUE(passes_filters("vacation1.jpg", matches, {}));
    EXPECT_TRUE(passes_filters("IMG_1234.jpg", matches, {}));
    EXPECT_FALSE(passes_filters("DSC_5678.jpg", matches, {}));
}

TEST(FilterTest, FilterExcludes) {
    EXPECT_FALSE(passes_filters("vacation_copy.jpg", {}, {"*_copy*"}));
    EXPECT_TRUE(passes_filters("vacation.jpg", {}, {"*_copy*"}));
}

TEST(FilterTest, MultipleFilters) {
    std::vector<std::string> filters = {"*_copy*", ".*"};
    EXPECT_FALSE(passes_filters("vacation_copy.jpg", {}, filters));
    EXPECT_FALSE(passes_filters(".hidden.jpg", {}, filters));
    EXPECT_TRUE(passes_filters("vacation.jpg", {}, filters));
}

TEST(FilterTest, FilterWinsOverMatch) {
    std::vector<std::string> matches = {"vacation*"};
    std::vector<std::string> filters = {"*_copy*"};
    EXPECT_TRUE(passes_filters("vacation.jpg", matches, filters));
    EXPECT_FALSE(passes_filters("vacation_copy.jpg", matches, filters));
}

TEST(FilterTest, MatchAndFilterTogether) {
    std::vector<std::string> matches = {"*.jpg", "*.png"};
    std::vector<std::string> filters = {"thumb_*", "*_small*"};
    EXPECT_TRUE(passes_filters("photo.jpg", matches, filters));
    EXPECT_TRUE(passes_filters("image.png", matches, filters));
    EXPECT_FALSE(passes_filters("photo.gif", matches, filters));    // not matched
    EXPECT_FALSE(passes_filters("thumb_photo.jpg", matches, filters)); // filtered
    EXPECT_FALSE(passes_filters("photo_small.png", matches, filters)); // filtered
}

TEST(FilterTest, MatchesAgainstFullPath) {
    EXPECT_TRUE(passes_filters("/home/user/Photos/vacation.jpg", {"*/Photos/*"}, {}));
    EXPECT_FALSE(passes_filters("/home/user/Backup/vacation.jpg", {"*/Photos/*"}, {}));
}

TEST(FilterTest, FilterAgainstFullPath) {
    EXPECT_FALSE(passes_filters("/home/user/Photos/thumbs/photo.jpg", {}, {"*/thumbs/*"}));
    EXPECT_TRUE(passes_filters("/home/user/Photos/photo.jpg", {}, {"*/thumbs/*"}));
}

TEST(FilterTest, MixedPathAndFilename) {
    std::vector<std::string> matches = {"*/Webcam/*", "*.png"};
    EXPECT_TRUE(passes_filters("/home/user/Pictures/Webcam/photo.jpg", matches, {}));
    EXPECT_TRUE(passes_filters("/home/user/Pictures/screenshot.png", matches, {}));
    EXPECT_FALSE(passes_filters("/home/user/Pictures/photo.jpg", matches, {}));
}

TEST(FilterTest, ExpandTilde) {
    std::string expanded = expand_tilde("~/Photos/*");
    EXPECT_FALSE(expanded.empty());
    EXPECT_NE(expanded[0], '~');
    EXPECT_TRUE(expanded.find("/Photos/*") != std::string::npos);

    // No tilde — unchanged
    EXPECT_EQ(expand_tilde("/absolute/path"), "/absolute/path");
    EXPECT_EQ(expand_tilde("relative/path"), "relative/path");
    EXPECT_EQ(expand_tilde("*.jpg"), "*.jpg");
}

// ---- Date parsing tests ----

TEST(DateParseTest, ExifFormat) {
    auto d = parse_date("2023:08:15 10:30:00");
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.year, "2023");
    EXPECT_EQ(d.month, "08");
    EXPECT_EQ(d.day, "15");
}

TEST(DateParseTest, ISOFormat) {
    auto d = parse_date("2023-08-15T10:30:00Z");
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.year, "2023");
    EXPECT_EQ(d.month, "08");
    EXPECT_EQ(d.day, "15");
}

TEST(DateParseTest, EpochDateInvalid) {
    auto d = parse_date("1970:01:01 00:00:00");
    EXPECT_FALSE(d.valid);
}

TEST(DateParseTest, ZeroDateInvalid) {
    auto d = parse_date("0000:00:00 00:00:00");
    EXPECT_FALSE(d.valid);
}

TEST(DateParseTest, EmptyString) {
    auto d = parse_date("");
    EXPECT_FALSE(d.valid);
}

TEST(DateParseTest, GarbageString) {
    auto d = parse_date("not a date");
    EXPECT_FALSE(d.valid);
}

// ---- JSON get tests ----

TEST(JsonGetTest, SimpleValue) {
    EXPECT_EQ(json_get("{\"Make\":\"Canon\"}", "Make"), "Canon");
}

TEST(JsonGetTest, MultipleKeys) {
    std::string json = "{\"Make\":\"Canon\",\"Model\":\"EOS R5\"}";
    EXPECT_EQ(json_get(json, "Make"), "Canon");
    EXPECT_EQ(json_get(json, "Model"), "EOS R5");
}

TEST(JsonGetTest, MissingKey) {
    EXPECT_EQ(json_get("{\"Make\":\"Canon\"}", "Model"), "");
}

TEST(JsonGetTest, EmptyJson) {
    EXPECT_EQ(json_get("{}", "Make"), "");
}

// ---- Format string tests ----

TEST(FormatTest, DefaultFormat) {
    ImageInfo img;
    img.filename = "photo.jpg";
    DateParts date{.year = "2023", .month = "08", .day = "15", .valid = true};

    auto result = apply_format("%Y/%m/%original", img, date);
    EXPECT_EQ(result, "2023/08/photo.jpg");
}

TEST(FormatTest, FullDateFormat) {
    ImageInfo img;
    img.filename = "photo.jpg";
    DateParts date{.year = "2023", .month = "08", .day = "15", .valid = true};

    auto result = apply_format("%Y/%m/%d/%original", img, date);
    EXPECT_EQ(result, "2023/08/15/photo.jpg");
}

TEST(FormatTest, CameraFormat) {
    ImageInfo img;
    img.filename = "photo.jpg";
    img.exif_json = "{\"Model\":\"EOS R5\",\"Make\":\"Canon\"}";
    DateParts date{.year = "2023", .month = "08", .day = "15", .valid = true};

    auto result = apply_format("%Y/%m/%camera/%original", img, date);
    EXPECT_EQ(result, "2023/08/EOS R5/photo.jpg");
}

TEST(FormatTest, MakeFormat) {
    ImageInfo img;
    img.filename = "photo.jpg";
    img.exif_json = "{\"Make\":\"Canon\"}";
    DateParts date{.year = "2023", .month = "08", .valid = true};

    auto result = apply_format("%Y/%make/%original", img, date);
    EXPECT_EQ(result, "2023/Canon/photo.jpg");
}

TEST(FormatTest, MissingCamera) {
    ImageInfo img;
    img.filename = "photo.jpg";
    img.exif_json = "{}";
    DateParts date{.year = "2023", .month = "08", .valid = true};

    auto result = apply_format("%camera/%original", img, date);
    EXPECT_EQ(result, "unknown-camera/photo.jpg");
}

TEST(FormatTest, InvalidDate) {
    ImageInfo img;
    img.filename = "photo.jpg";
    DateParts date; // invalid

    auto result = apply_format("%Y/%m/%original", img, date);
    EXPECT_EQ(result, "unsorted/unsorted/photo.jpg");
}

TEST(FormatTest, PlainText) {
    ImageInfo img;
    img.filename = "photo.jpg";
    DateParts date{.year = "2023", .month = "08", .valid = true};

    auto result = apply_format("organized/%Y-%m/%original", img, date);
    EXPECT_EQ(result, "organized/2023-08/photo.jpg");
}
