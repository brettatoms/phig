#include <jxl/decode.h>
#include <jxl/decode_cxx.h>

#include <cstdint>
#include <fstream>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "image_codec.h"
#include "image_codec_internal.h"

// JPEG XL needs its own backend: libheif handles HEIF-family containers and
// does not decode JXL at all. Like the libheif backend, this compiles on every
// platform even though macOS decodes through ImageIO in production, so the
// Linux path is exercised by the test suite here rather than only in CI.

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streampos size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(data.data()), size)) return {};
    return data;
}

}  // namespace

cv::Mat codec_decode_libjxl(const std::string& path) try {
    const std::vector<uint8_t> data = read_file(path);
    if (data.empty()) return {};

    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    if (!dec) return {};
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
        JXL_DEC_SUCCESS) {
        return {};
    }

    // Ask libjxl to apply the orientation itself, matching what libheif does
    // for HEIF containers and what cv::imread does for JPEG. Without this a
    // rotated JXL would come back sideways on Linux only.
    if (JxlDecoderSetUnpremultiplyAlpha(dec.get(), JXL_FALSE) != JXL_DEC_SUCCESS) return {};
    JxlDecoderSetKeepOrientation(dec.get(), JXL_FALSE);

    if (JxlDecoderSetInput(dec.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) return {};
    JxlDecoderCloseInput(dec.get());

    // Three interleaved 8-bit channels; libjxl emits RGB, converted to BGR at
    // the end to match every other decode path in core/.
    const JxlPixelFormat format = {3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlBasicInfo info{};
    cv::Mat rgb;

    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
        if (status == JXL_DEC_ERROR || status == JXL_DEC_NEED_MORE_INPUT) return {};

        if (status == JXL_DEC_BASIC_INFO) {
            if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) return {};
            if (info.xsize == 0 || info.ysize == 0) return {};
            rgb.create(static_cast<int>(info.ysize), static_cast<int>(info.xsize), CV_8UC3);
        } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
            if (rgb.empty()) return {};
            const size_t needed = rgb.total() * rgb.elemSize();
            if (JxlDecoderSetImageOutBuffer(dec.get(), &format, rgb.data, needed) !=
                JXL_DEC_SUCCESS) {
                return {};
            }
        } else if (status == JXL_DEC_FULL_IMAGE) {
            break;
        } else if (status == JXL_DEC_SUCCESS) {
            // Input ended without a full image.
            if (rgb.empty()) return {};
            break;
        }
    }

    if (rgb.empty()) return {};
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return bgr;
} catch (...) {
    return {};
}

std::vector<unsigned char> codec_jxl_exif_blob(const std::string& path) try {
    const std::vector<uint8_t> data = read_file(path);
    if (data.empty()) return {};

    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    if (!dec) return {};
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BOX) != JXL_DEC_SUCCESS) return {};
    // Boxes may be Brotli-compressed ("brob"); this makes libjxl hand back the
    // decompressed contents rather than the compressed bytes.
    JxlDecoderSetDecompressBoxes(dec.get(), JXL_TRUE);

    if (JxlDecoderSetInput(dec.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) return {};
    JxlDecoderCloseInput(dec.get());

    std::vector<unsigned char> box;
    bool in_exif_box = false;
    size_t filled = 0;
    constexpr size_t kChunk = 16384;

    // Reading a box is a fill-and-refill loop: libjxl writes into the buffer
    // supplied and reports remaining space only when the buffer is released,
    // so the buffer is grown until the box ends.
    auto release_into = [&]() {
        const size_t left = JxlDecoderReleaseBoxBuffer(dec.get());
        filled = box.size() - left;
        box.resize(filled);
    };

    for (;;) {
        const JxlDecoderStatus status = JxlDecoderProcessInput(dec.get());
        if (status == JXL_DEC_ERROR) return {};

        if (status == JXL_DEC_BOX) {
            if (in_exif_box) {
                release_into();
                break;  // the Exif box just ended
            }
            JxlBoxType type{};
            if (JxlDecoderGetBoxType(dec.get(), type, JXL_TRUE) != JXL_DEC_SUCCESS) continue;
            if (type[0] == 'E' && type[1] == 'x' && type[2] == 'i' && type[3] == 'f') {
                in_exif_box = true;
                box.resize(kChunk);
                filled = 0;
                if (JxlDecoderSetBoxBuffer(dec.get(), box.data(), box.size()) != JXL_DEC_SUCCESS) {
                    return {};
                }
            }
        } else if (status == JXL_DEC_BOX_NEED_MORE_OUTPUT) {
            const size_t left = JxlDecoderReleaseBoxBuffer(dec.get());
            filled = box.size() - left;
            box.resize(box.size() + kChunk);
            if (JxlDecoderSetBoxBuffer(dec.get(), box.data() + filled, box.size() - filled) !=
                JXL_DEC_SUCCESS) {
                return {};
            }
        } else if (status == JXL_DEC_SUCCESS) {
            if (in_exif_box) release_into();
            break;
        } else if (status == JXL_DEC_NEED_MORE_INPUT) {
            return {};
        }
    }

    if (box.empty()) return {};

    // A JXL Exif box carries the same 4-byte big-endian offset-to-TIFF prefix
    // that HEIF uses, so the trimming rule is shared with the libheif path.
    return codec_trim_to_tiff_header(box);
} catch (...) {
    return {};
}
