#include "thumbs.h"
#include "hasher.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static const int THUMB_SIZE = 512;
static const int JPEG_QUALITY = 80;

std::string get_thumb_cache_dir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            throw std::runtime_error("Cannot determine home directory");
        }
        base = std::string(home) + "/.cache";
    }
    return base + "/phig/thumbs";
}

std::string get_thumb_path(const std::string& sha256) {
    if (sha256.size() < 2) return "";
    std::string dir = get_thumb_cache_dir();
    return dir + "/" + sha256.substr(0, 2) + "/" + sha256 + ".jpg";
}

bool thumb_exists(const std::string& sha256) {
    std::string path = get_thumb_path(sha256);
    return !path.empty() && fs::exists(path);
}

bool generate_thumb(const cv::Mat& img, const std::string& sha256) {
    if (img.empty() || sha256.empty()) return false;

    // Compute resize dimensions (512px on longest edge, preserve aspect ratio)
    int w = img.cols;
    int h = img.rows;
    double scale;
    if (w >= h) {
        scale = static_cast<double>(THUMB_SIZE) / w;
    } else {
        scale = static_cast<double>(THUMB_SIZE) / h;
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

    // Write to cache path
    std::string path = get_thumb_path(sha256);
    if (path.empty()) return false;

    fs::create_directories(fs::path(path).parent_path());

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return out.good();
}

bool generate_thumb(const std::string& image_path, const std::string& sha256) {
    cv::Mat img = decode_image(image_path);
    if (img.empty()) return false;
    return generate_thumb(img, sha256);
}
