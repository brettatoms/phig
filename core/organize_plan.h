#pragma once

#include <string>

#include "dates.h"
#include "types.h"

// Expands an organize format string against one image.
//
// Tokens: %original (filename), %camera (EXIF Model), %make (EXIF Make),
// %gallery (gallery name), %fav (favorite state), %Y/%m/%d (date parts).
// Missing values fall back to "unknown-camera", "unknown-make", "no-gallery",
// "unfavorited", or "unsorted" for dates. %camera, %make and %gallery each
// expand to a single path component: '/' and '\' become '_', control
// characters are dropped, and a value left empty, "." or ".." takes its
// fallback. A format with an unknown token is refused before this runs
// (validate(const OrganizeOptions&)).
std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date);

// The first %-sequence in `fmt` that is not a token above, or "" when there is none.
// A '%' at the end of the format is returned as "%".
std::string first_unknown_token(const std::string& fmt);
