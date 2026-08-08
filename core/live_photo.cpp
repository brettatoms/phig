#include "live_photo.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

// Apple's key on the MOV half of a Live Photo, in the container's metadata
// dictionary — the same dictionary core/metadata.cpp already reads
// creation_time from for every indexed video.
constexpr const char* kContentIdentifierKey = "com.apple.quicktime.content.identifier";

}  // namespace

std::string live_photo_content_identifier(const std::string& video_path) try {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, video_path.c_str(), nullptr, nullptr) != 0) return {};

    struct Closer {
        AVFormatContext** p;
        ~Closer() { avformat_close_input(p); }
    } closer{&fmt};

    // avformat_find_stream_info is deliberately not called. The key lives in the
    // container dictionary, which open_input has already populated, while
    // find_stream_info decodes packets to probe the streams — real cost that a
    // scan would pay once per video for nothing.
    AVDictionaryEntry* tag = av_dict_get(fmt->metadata, kContentIdentifierKey, nullptr, 0);
    if (!tag || !tag->value) return {};
    return std::string(tag->value);
} catch (...) {
    return {};
}
