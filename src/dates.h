#pragma once

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

// Best date for organizing a file, in order: EXIF DateTimeOriginal, EXIF
// DateTimeDigitized, sidecar date (xmp/Google Takeout), file modified time.
DateParts get_best_date(const ImageInfo& img);

// Best date for display (e.g. search output), same ordering as
// get_best_date, returned as a "YYYY:MM:DD" (or similar) string.
std::string get_best_date_str(const ImageInfo& img);
