#include "raw.h"

#include <libraw/libraw.h>

#include <filesystem>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

namespace {

// Tier 1: a same-basename JPEG (.jpg/.jpeg, case-insensitive) beside the RAW.
cv::Mat try_sidecar_jpeg(const std::string& raw_path) {
    fs::path path(raw_path);
    fs::path stem_path = path.parent_path() / path.stem();

    for (const char* ext : {".jpg", ".jpeg", ".JPG", ".JPEG"}) {
        fs::path candidate = stem_path;
        candidate += ext;
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            cv::Mat img = cv::imread(candidate.string(), cv::IMREAD_COLOR);
            if (!img.empty()) {
                return img;
            }
        }
    }
    return cv::Mat();
}

// Tier 2: the RAW's embedded JPEG preview via LibRaw's thumbnail extraction.
cv::Mat try_embedded_preview(LibRaw& ip) {
    try {
        if (ip.unpack_thumb() != LIBRAW_SUCCESS) {
            return cv::Mat();
        }
        const auto& thumb = ip.imgdata.thumbnail;
        if (thumb.tformat != LIBRAW_THUMBNAIL_JPEG || thumb.thumb == nullptr ||
            thumb.tlength == 0) {
            return cv::Mat();
        }
        cv::Mat buf(1, static_cast<int>(thumb.tlength), CV_8UC1,
                    reinterpret_cast<unsigned char*>(thumb.thumb));
        cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
        return img;
    } catch (...) {
        return cv::Mat();
    }
}

// Tier 3: full LibRaw decode.
cv::Mat try_full_decode(LibRaw& ip) {
    try {
        if (ip.unpack() != LIBRAW_SUCCESS) {
            return cv::Mat();
        }
        if (ip.dcraw_process() != LIBRAW_SUCCESS) {
            return cv::Mat();
        }
        int err = 0;
        libraw_processed_image_t* im = ip.dcraw_make_mem_image(&err);
        if (im == nullptr) {
            return cv::Mat();
        }
        cv::Mat img;
        if (im->type == LIBRAW_IMAGE_BITMAP && im->colors == 3) {
            cv::Mat rgb(im->height, im->width, CV_8UC3, im->data);
            cv::cvtColor(rgb, img, cv::COLOR_RGB2BGR);
            img = img.clone();
        }
        LibRaw::dcraw_clear_mem(im);
        return img;
    } catch (...) {
        return cv::Mat();
    }
}

}  // namespace

cv::Mat raw_decode(const std::string& raw_path) {
    try {
        cv::Mat img = try_sidecar_jpeg(raw_path);
        if (!img.empty()) {
            return img;
        }

        // Heap, not stack: sizeof(LibRaw) is ~750 KB, and macOS gives secondary
        // threads a 512 KB stack by default (Linux gives 8 MB). As a local this
        // overflows the stack of any worker thread that decodes a RAW — a bus
        // error before a single pixel is read, since the cost is the object
        // itself and not the decode.
        auto ip = std::make_unique<LibRaw>();
        if (ip->open_file(raw_path.c_str()) != LIBRAW_SUCCESS) {
            return cv::Mat();
        }

        img = try_embedded_preview(*ip);
        if (!img.empty()) {
            return img;
        }

        return try_full_decode(*ip);
    } catch (...) {
        return cv::Mat();
    }
}
