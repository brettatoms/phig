#include "raw.h"

#include <libraw/libraw.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <span>
#include <vector>

#include "hasher.h"
#include "jpeg_decode.h"

namespace fs = std::filesystem;

namespace {

// The sha256 of bytes already in memory.
std::string sha256_of(const unsigned char* data, size_t size) {
    Sha256Ctx ctx;
    sha256_init(ctx);
    sha256_update(ctx, data, size);
    return sha256_final(ctx);
}

std::vector<unsigned char> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Tier 1: a same-basename JPEG (.jpg/.jpeg, case-insensitive) beside the RAW.
// Pixels are what cv::imread gave for the file, including the partial image of
// a truncated one.
DecodeResult try_sidecar_jpeg(const std::string& raw_path, bool hash_source) {
    fs::path path(raw_path);
    fs::path stem_path = path.parent_path() / path.stem();

    for (const char* ext : {".jpg", ".jpeg", ".JPG", ".JPEG"}) {
        fs::path candidate = stem_path;
        candidate += ext;
        std::error_code ec;
        if (!fs::exists(candidate, ec)) continue;

        DecodeResult result;
        std::vector<unsigned char> bytes;
        bool is_jpeg = false;
        if (hash_source) {
            bytes = read_file(candidate);
            is_jpeg = is_jpeg_signature(bytes);
        } else {
            unsigned char head[3] = {0, 0, 0};
            std::ifstream f(candidate, std::ios::binary);
            f.read(reinterpret_cast<char*>(head), 3);
            is_jpeg = is_jpeg_signature(std::span<const unsigned char>(head, f.gcount()));
        }
        if (is_jpeg) {
            result =
                hash_source ? decode_jpeg_file_bytes(bytes) : decode_jpeg_file(candidate.string());
        } else {
            ensure_opencv_log_level();
            result.img = cv::imread(candidate.string(), cv::IMREAD_COLOR);
            result.frame.width = result.img.cols;
            result.frame.height = result.img.rows;
        }
        if (result.img.empty()) continue;
        result.frame.source = DecodeSource::SidecarJpeg;
        if (hash_source) result.frame.source_sha256 = sha256_of(bytes.data(), bytes.size());
        return result;
    }
    return {};
}

// Tier 2: the RAW's embedded JPEG preview via LibRaw's thumbnail extraction.
DecodeResult try_embedded_preview(LibRaw& ip, bool hash_source) {
    try {
        if (ip.unpack_thumb() != LIBRAW_SUCCESS) {
            return {};
        }
        const auto& thumb = ip.imgdata.thumbnail;
        if (thumb.tformat != LIBRAW_THUMBNAIL_JPEG || thumb.thumb == nullptr ||
            thumb.tlength == 0) {
            return {};
        }
        const auto* data = reinterpret_cast<const unsigned char*>(thumb.thumb);
        DecodeResult result =
            decode_jpeg_memory(std::span<const unsigned char>(data, thumb.tlength));
        if (result.img.empty()) return {};
        result.frame.source = DecodeSource::RawPreview;
        if (hash_source) result.frame.source_sha256 = sha256_of(data, thumb.tlength);
        return result;
    } catch (...) {
        return {};
    }
}

// Tier 3: full LibRaw decode.
DecodeResult try_full_decode(LibRaw& ip) {
    try {
        if (ip.unpack() != LIBRAW_SUCCESS) {
            return {};
        }
        if (ip.dcraw_process() != LIBRAW_SUCCESS) {
            return {};
        }
        int err = 0;
        libraw_processed_image_t* im = ip.dcraw_make_mem_image(&err);
        if (im == nullptr) {
            return {};
        }
        cv::Mat img;
        if (im->type == LIBRAW_IMAGE_BITMAP && im->colors == 3) {
            cv::Mat rgb(im->height, im->width, CV_8UC3, im->data);
            cv::cvtColor(rgb, img, cv::COLOR_RGB2BGR);
            img = img.clone();
        }
        LibRaw::dcraw_clear_mem(im);
        DecodeResult result;
        result.img = img;
        result.frame = {DecodeSource::RawFull, img.cols, img.rows, {}};
        return result;
    } catch (...) {
        return {};
    }
}

}  // namespace

DecodeResult raw_decode_result(const std::string& raw_path, bool hash_source) {
    try {
        DecodeResult result = try_sidecar_jpeg(raw_path, hash_source);
        if (!result.img.empty()) {
            return result;
        }

        // Heap, not stack: sizeof(LibRaw) is ~750 KB, and macOS gives secondary
        // threads a 512 KB stack by default (Linux gives 8 MB). As a local this
        // overflows the stack of any worker thread that decodes a RAW — a bus
        // error before a single pixel is read, since the cost is the object
        // itself and not the decode.
        auto ip = std::make_unique<LibRaw>();
        if (ip->open_file(raw_path.c_str()) != LIBRAW_SUCCESS) {
            return {};
        }

        result = try_embedded_preview(*ip, hash_source);
        if (!result.img.empty()) {
            return result;
        }

        return try_full_decode(*ip);
    } catch (...) {
        return {};
    }
}

cv::Mat raw_decode(const std::string& raw_path) { return raw_decode_result(raw_path).img; }
