#pragma once

#include <filesystem>
#include <string>

#include "types.h"

// Parse a date string from EXIF or mtime, return components
struct DateParts {
    std::string year = "unknown";
    std::string month = "unknown";
    std::string day = "unknown";
    bool valid = false;
};

DateParts parse_date(const std::string& date_str);

// Extract a JSON string value by key (simple parser, no nesting)
std::string json_get(const std::string& json, const std::string& key);

// Where get_best_date found a file's date.
enum class DateSource { Exif, Sidecar, Mtime, None };

// Best date for organizing a file, in order: EXIF DateTimeOriginal, EXIF
// DateTimeDigitized, sidecar date (xmp/Google Takeout), file modified time.
DateParts get_best_date(const ImageInfo& img);

// get_best_date, also reporting which of the four sources supplied the date.
DateParts get_best_date(const ImageInfo& img, DateSource& source);

// Best date for display (e.g. search output), same ordering as
// get_best_date, returned as a "YYYY:MM:DD" (or similar) string.
std::string get_best_date_str(const ImageInfo& img);

// Formats a filesystem timestamp as "YYYY-MM-DDTHH:MM:SSZ" (UTC). This is the
// form stored in images.modified_at, and scan's mtime-skip check compares a
// freshly formatted value against the stored one — changing the format would
// make every tracked file look modified.
std::string format_time(std::filesystem::file_time_type ftime);
