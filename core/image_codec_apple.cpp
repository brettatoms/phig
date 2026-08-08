#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>

#include <memory>
#include <opencv2/imgproc.hpp>
#include <type_traits>

#include "image_codec_internal.h"

namespace {

struct CFDeleter {
    void operator()(CFTypeRef ref) const {
        if (ref) CFRelease(ref);
    }
};

template <typename T>
using CFPtr = std::unique_ptr<typename std::remove_pointer<T>::type, CFDeleter>;

// EXIF orientation 1-8 -> the rotation and flip that makes the image upright.
//
// This exists only on this path. ImageIO hands back unrotated pixels plus the
// orientation value, whereas libheif applies the equivalent transform itself
// and cv::imread has applied EXIF orientation since 3.4. Without this, HEICs
// would be upright on Linux and sideways on macOS.
cv::Mat apply_orientation(const cv::Mat& src, int orientation) {
    cv::Mat out;
    switch (orientation) {
        case 2:
            cv::flip(src, out, 1);
            return out;
        case 3:
            cv::rotate(src, out, cv::ROTATE_180);
            return out;
        case 4:
            cv::flip(src, out, 0);
            return out;
        case 5:
            cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE);
            cv::flip(out, out, 1);
            return out;
        case 6:
            cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE);
            return out;
        case 7:
            cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE);
            cv::flip(out, out, 1);
            return out;
        case 8:
            cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE);
            return out;
        default:
            // 1, and anything malformed: an unknown value is left alone rather
            // than guessed at.
            return src.clone();
    }
}

}  // namespace

cv::Mat codec_decode_imageio(const std::string& path) try {
    CFPtr<CFStringRef> cfpath(
        CFStringCreateWithCString(nullptr, path.c_str(), kCFStringEncodingUTF8));
    if (!cfpath) return {};

    CFPtr<CFURLRef> url(
        CFURLCreateWithFileSystemPath(nullptr, cfpath.get(), kCFURLPOSIXPathStyle, false));
    if (!url) return {};

    CFPtr<CGImageSourceRef> source(CGImageSourceCreateWithURL(url.get(), nullptr));
    if (!source) return {};

    // The reported orientation already folds in the container's own rotation
    // transforms, so this single value is the whole correction needed.
    int orientation = 1;
    CFPtr<CFDictionaryRef> props(CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr));
    if (props) {
        CFTypeRef value = CFDictionaryGetValue(props.get(), kCGImagePropertyOrientation);
        if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberIntType, &orientation);
        }
    }

    CFPtr<CGImageRef> image(CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr));
    if (!image) return {};

    const size_t width = CGImageGetWidth(image.get());
    const size_t height = CGImageGetHeight(image.get());
    if (width == 0 || height == 0) return {};

    // Draw into a known 8-bit layout rather than reading the source's own
    // format: a CGImage may be 10-bit, YCbCr, or carry an unusual alpha
    // arrangement, and the bitmap context normalizes all of it in one step.
    // No colour space conversion is requested — both backends ignore the ICC
    // profile identically so the two platforms produce the same pixels.
    cv::Mat bgra(static_cast<int>(height), static_cast<int>(width), CV_8UC4);
    CFPtr<CGColorSpaceRef> space(CGColorSpaceCreateDeviceRGB());
    if (!space) return {};

    // Both constants are cast before combining: they come from two different
    // (one anonymous) enums, and a bare `|` between them is deprecated —
    // -Wdeprecated-anon-enum-enum-conversion, which CI makes fatal via
    // -DPHIG_WERROR=ON. CGBitmapContextCreate takes a uint32_t regardless.
    const uint32_t bitmap_info = static_cast<uint32_t>(kCGImageAlphaNoneSkipFirst) |
                                 static_cast<uint32_t>(kCGBitmapByteOrder32Little);
    CFPtr<CGContextRef> ctx(
        CGBitmapContextCreate(bgra.data, width, height, 8, bgra.step, space.get(), bitmap_info));
    if (!ctx) return {};

    CGContextDrawImage(ctx.get(),
                       CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)),
                       image.get());

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return apply_orientation(bgr, orientation);
} catch (...) {
    return {};
}

#endif  // __APPLE__
