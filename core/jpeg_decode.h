#pragma once

#include <span>
#include <string>

#include "decode_result.h"

// The decoders below mirror OpenCV's JPEG decoder (grfmt_jpeg.cpp, loadsave.cpp,
// exif.cpp at 5.0.0) so their pixels are byte-identical to cv::imread and
// cv::imdecode with IMREAD_COLOR. Which APP1 markers EXIF orientation is read
// from follows the OpenCV linked at runtime, 4.x or 5.x. Each call has its own
// libjpeg error manager: they never throw, never write to stderr, and need no
// lock. warning holds what libjpeg would have printed (its first warning, as
// its default policy shows) and, when decoding fails, the error that stopped it.

// True when head starts with the JPEG SOI marker FF D8 FF, the signature
// cv::imread uses to pick its JPEG decoder.
bool is_jpeg_signature(std::span<const unsigned char> head);

// Decodes a JPEG file to 8-bit BGR as cv::imread does, EXIF orientation applied.
// A file that ends early decodes to imread's partial image, with the message.
DecodeResult decode_jpeg_file(const std::string& path);

// Decodes JPEG bytes to 8-bit BGR as cv::imdecode does, EXIF orientation applied.
// Bytes that end early fail with an empty image and the message, at every OpenCV
// version: 4.x's imdecode returns uninitialised rows there, which is not copied.
DecodeResult decode_jpeg_memory(std::span<const unsigned char> bytes);

// The bytes of a file, decoded as decode_jpeg_file would: bytes that end early
// decode to the same partial image, with the same message.
DecodeResult decode_jpeg_file_bytes(std::span<const unsigned char> bytes);
