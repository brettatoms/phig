#include <libheif/heif.h>

#include <cstdint>
#include <opencv2/imgproc.hpp>

#include "image_codec_internal.h"

namespace {

// Releases a libheif object on every exit path, including one taken because
// OpenCV threw partway through. libheif's types are C, so this is the only
// place in the file that has to think about ownership.
//
// `auto Release` rather than a written-out `void (*)(T*)`: libheif is not
// consistent about constness across its own API or across versions —
// heif_context_free takes a mutable pointer while heif_image_release and
// heif_image_handle_release take const ones in 1.23, and the const qualifiers
// arrived after the 1.16 floor. Deducing the signature accepts all of them.
template <typename T, auto Release>
class HeifPtr {
   public:
    HeifPtr() = default;
    ~HeifPtr() {
        if (p_) Release(p_);
    }
    HeifPtr(const HeifPtr&) = delete;
    HeifPtr& operator=(const HeifPtr&) = delete;

    T** out() { return &p_; }
    T* get() const { return p_; }
    void reset(T* p) { p_ = p; }

   private:
    T* p_ = nullptr;
};

}  // namespace

cv::Mat codec_decode_libheif(const std::string& path) try {
    HeifPtr<heif_context, heif_context_free> ctx;
    ctx.reset(heif_context_alloc());
    if (!ctx.get()) return {};

    if (heif_context_read_from_file(ctx.get(), path.c_str(), nullptr).code != heif_error_Ok) {
        return {};
    }

    HeifPtr<heif_image_handle, heif_image_handle_release> handle;
    if (heif_context_get_primary_image_handle(ctx.get(), handle.out()).code != heif_error_Ok) {
        return {};
    }

    // Interleaved 8-bit RGB. Container rotation and mirror transforms are
    // applied by default, which is what makes this agree with cv::imread
    // without any orientation handling here.
    HeifPtr<heif_image, heif_image_release> img;
    if (heif_decode_image(handle.get(), img.out(), heif_colorspace_RGB, heif_chroma_interleaved_RGB,
                          nullptr)
            .code != heif_error_Ok) {
        return {};
    }

    const int width = heif_image_get_width(img.get(), heif_channel_interleaved);
    const int height = heif_image_get_height(img.get(), heif_channel_interleaved);
    if (width <= 0 || height <= 0) return {};

    int stride = 0;
    const uint8_t* data =
        heif_image_get_plane_readonly(img.get(), heif_channel_interleaved, &stride);
    if (!data || stride <= 0) return {};

    // Wrap libheif's buffer without copying, then convert. cvtColor allocates
    // its own output, so the returned Mat does not alias memory owned by `img`.
    const cv::Mat rgb(height, width, CV_8UC3, const_cast<uint8_t*>(data),
                      static_cast<size_t>(stride));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    return bgr;
} catch (...) {
    return {};
}
