#include "thumbs_engine.h"

#include "thumbs.h"

bool thumb_needs_generating(const ImageInfo& img, bool force) {
    if (img.sha256.empty()) return false;
    if (force) return true;
    return !thumb_exists(img.sha256);
}
