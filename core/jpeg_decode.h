#pragma once

#include <span>
#include <string>

#include "decode_result.h"

// The decoders below mirror OpenCV's JPEG decoder (grfmt_jpeg.cpp, loadsave.cpp,
// exif.cpp at 5.0.0) so their pixels are byte-identical to cv::imread and
// cv::imdecode with IMREAD_COLOR. Each call has its own libjpeg error manager:
// they never throw, never write to stderr, and need no lock. warning holds what
// libjpeg would have printed (its first warning, as its default policy shows)
// and, when decoding fails, the error that stopped it.

// True when head starts with the JPEG SOI marker FF D8 FF, the signature
// cv::imread uses to pick its JPEG decoder.
bool is_jpeg_signature(std::span<const unsigned char> head);

// Decodes a JPEG file to 8-bit BGR as cv::imread does, EXIF orientation applied.
// A file that ends early decodes to imread's partial image, with the message.
DecodeResult decode_jpeg_file(const std::string& path);

// Decodes JPEG bytes to 8-bit BGR as cv::imdecode does, EXIF orientation applied.
// Unlike decode_jpeg_file, bytes that end early fail with an empty image.
DecodeResult decode_jpeg_memory(std::span<const unsigned char> bytes);
