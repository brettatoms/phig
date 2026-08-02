#pragma once

#include <string>

#include "dates.h"
#include "types.h"

// Expands an organize format string against one image.
//
// Tokens: %original (filename), %camera (EXIF Model), %make (EXIF Make),
// %gallery (gallery name), %fav (favorite state), %Y/%m/%d (date parts).
// Unknown %-sequences are emitted verbatim. Missing values fall back to
// "unknown-camera", "unknown-make", "no-gallery", "unfavorited", or
// "unsorted" for dates.
std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date);
