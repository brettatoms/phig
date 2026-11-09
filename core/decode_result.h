#pragma once

#include <opencv2/core.hpp>
#include <string>

// Which bytes a decode read: the image file itself, a RAW's sidecar JPEG, the
// RAW's embedded preview, or a full RAW decode.
enum class DecodeSource { File, SidecarJpeg, RawPreview, RawFull };

// The frame a decoded image is in. Face boxes are measured in this frame.
struct DecodeFrame {
    DecodeSource source = DecodeSource::File;
    int width = 0, height = 0;  // of the decoded, oriented image
    std::string source_sha256;  // SidecarJpeg and RawPreview, when requested; else empty
};

// A decoded image and every decoder message that belongs to this decode. An
// empty img means the decode failed; an empty warning means nothing was said.
struct DecodeResult {
    cv::Mat img;
    std::string warning;
    DecodeFrame frame;
};

// The stable lowercase name of a source, as stored and logged.
inline const char* decode_source_name(DecodeSource s) {
    switch (s) {
        case DecodeSource::File:
            return "file";
        case DecodeSource::SidecarJpeg:
            return "sidecar_jpeg";
        case DecodeSource::RawPreview:
            return "raw_preview";
        case DecodeSource::RawFull:
            return "raw_full";
    }
    return "file";
}
