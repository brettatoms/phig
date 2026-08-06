#include "thumbs.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "hasher.h"
#include "paths.h"

namespace fs = std::filesystem;

static const int JPEG_QUALITY = 80;

std::string get_thumb_path(const std::string& sha256, int size) {
    if (sha256.size() < 2 || size <= 0) return "";
    std::string dir = thumbs_dir();
    return dir + "/" + std::to_string(size) + "/" + sha256.substr(0, 2) + "/" + sha256 + ".jpg";
}

bool thumb_exists(const std::string& sha256, int size) {
    std::string path = get_thumb_path(sha256, size);
    return !path.empty() && fs::exists(path);
}

bool generate_thumb(const cv::Mat& img, const std::string& sha256, int size) {
    if (img.empty() || sha256.empty() || size <= 0) return false;

    // Resize so the longest edge is `size`, preserving aspect ratio.
    int w = img.cols;
    int h = img.rows;
    double scale;
    if (w >= h) {
        scale = static_cast<double>(size) / w;
    } else {
        scale = static_cast<double>(size) / h;
    }

    // Don't upscale
    if (scale >= 1.0) {
        scale = 1.0;
    }

    int new_w = static_cast<int>(w * scale);
    int new_h = static_cast<int>(h * scale);

    cv::Mat resized;
    cv::resize(img, resized, cv::Size(new_w, new_h), 0, 0, cv::INTER_AREA);

    // Encode as JPEG
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, JPEG_QUALITY};
    std::vector<uchar> buf;
    if (!cv::imencode(".jpg", resized, buf, params)) {
        return false;
    }

    // Write to cache path atomically: a crash mid-write must not leave a
    // truncated thumb that thumb_exists() would treat as valid forever.
    std::string path = get_thumb_path(sha256, size);
    if (path.empty()) return false;

    return atomic_write_file(path, buf.data(), buf.size());
}

bool generate_thumb(const std::string& image_path, const std::string& sha256, int size) {
    cv::Mat img = decode_image(image_path);
    if (img.empty()) return false;
    return generate_thumb(img, sha256, size);
}

int migrate_flat_thumb_cache() {
    // Before sizes were part of the path, thumbnails lived at
    // <thumbs>/ab/<sha>.jpg and were all PHIG_DEFAULT_THUMB_SIZE. Move them into
    // <thumbs>/512/ab/<sha>.jpg so they stay usable instead of being stranded
    // while a parallel tree fills up. A rename, never a re-encode.
    //
    // Idempotent: once moved there are no two-hex dirs left at the top level, so
    // a second call finds nothing. Safe to run on every startup.
    std::string root = thumbs_dir();
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return 0;

    int moved = 0;
    std::vector<fs::path> stale_dirs;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        // A size dir is all digits; a legacy fanout dir is exactly two hex chars.
        std::string name = entry.path().filename().string();
        if (name.size() != 2) continue;
        if (!std::isxdigit(static_cast<unsigned char>(name[0])) ||
            !std::isxdigit(static_cast<unsigned char>(name[1])))
            continue;

        for (const auto& file : fs::directory_iterator(entry.path(), ec)) {
            if (ec) break;
            if (!file.is_regular_file()) continue;
            if (!is_thumbnail_filename(file.path().filename().string())) continue;

            std::string sha = file.path().stem().string();
            std::string dest = get_thumb_path(sha, PHIG_DEFAULT_THUMB_SIZE);
            if (dest.empty()) continue;
            fs::create_directories(fs::path(dest).parent_path(), ec);
            // Skip rather than clobber if the destination somehow exists: the
            // bytes there are already a valid thumbnail of this size.
            if (fs::exists(dest, ec)) continue;
            fs::rename(file.path(), dest, ec);
            if (!ec) moved++;
            ec.clear();
        }
        stale_dirs.push_back(entry.path());
    }

    // Remove the now-empty legacy fanout dirs. remove() only succeeds on an
    // empty directory, so anything we did not move is left untouched.
    for (const auto& dir : stale_dirs) {
        fs::remove(dir, ec);
        ec.clear();
    }
    return moved;
}

bool is_thumbnail_filename(const std::string& filename) {
    // Exactly "<64 lowercase hex>.jpg".
    static const std::string ext = ".jpg";
    if (filename.size() != 64 + ext.size()) return false;
    if (filename.compare(64, ext.size(), ext) != 0) return false;
    for (size_t i = 0; i < 64; i++) {
        char c = filename[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}
