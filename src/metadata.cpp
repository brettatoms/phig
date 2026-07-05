#include "metadata.h"

#include <sstream>
#include <unistd.h>
#include <fcntl.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/display.h>
}

#include <cmath>
#include <cstdio>

namespace {

// Escape a string for embedding in a JSON string literal.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

} // namespace

VideoMetadata extract_video_metadata(const std::string& path) {
    VideoMetadata meta;

    // Suppress ffmpeg's stderr warnings (e.g., "moov atom not found")
    av_log_set_level(AV_LOG_QUIET);

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr) != 0) {
        return meta;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return meta;
    }

    // Duration
    if (fmt_ctx->duration > 0) {
        meta.duration_ms = fmt_ctx->duration / (AV_TIME_BASE / 1000);
    }

    // Creation date from container metadata
    AVDictionaryEntry* tag = av_dict_get(fmt_ctx->metadata, "creation_time", nullptr, 0);
    if (tag) {
        meta.creation_date = tag->value;
    }

    // Find video and audio streams
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && meta.video_codec.empty()) {
            const AVCodecDescriptor* desc = avcodec_descriptor_get(codecpar->codec_id);
            if (desc) {
                meta.video_codec = desc->name;
            }
            meta.width = codecpar->width;
            meta.height = codecpar->height;

            // FPS
            if (stream->avg_frame_rate.den > 0) {
                meta.fps = av_q2d(stream->avg_frame_rate);
            } else if (stream->r_frame_rate.den > 0) {
                meta.fps = av_q2d(stream->r_frame_rate);
            }

            // Rotation from side data
            const AVPacketSideData* sd = nullptr;
            for (int j = 0; j < stream->codecpar->nb_coded_side_data; j++) {
                if (stream->codecpar->coded_side_data[j].type == AV_PKT_DATA_DISPLAYMATRIX) {
                    sd = &stream->codecpar->coded_side_data[j];
                    break;
                }
            }
            if (sd && sd->size >= 9 * sizeof(int32_t)) {
                const int32_t* matrix = reinterpret_cast<const int32_t*>(sd->data);
                double angle = av_display_rotation_get(matrix);
                if (!std::isnan(angle)) {
                    meta.rotation = static_cast<int>(std::lround(angle));
                    meta.rotation %= 360;
                    if (meta.rotation < 0) meta.rotation += 360;
                }
            }
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && meta.audio_codec.empty()) {
            const AVCodecDescriptor* desc = avcodec_descriptor_get(codecpar->codec_id);
            if (desc) {
                meta.audio_codec = desc->name;
            }
        }
    }

    avformat_close_input(&fmt_ctx);
    return meta;
}

std::string video_metadata_to_json(const VideoMetadata& meta) {
    std::ostringstream out;
    out << "{";

    bool first = true;
    auto add_str = [&](const char* key, const std::string& val) {
        if (val.empty()) return;
        if (!first) out << ",";
        first = false;
        out << "\"" << key << "\":\"" << json_escape(val) << "\"";
    };
    auto add_int = [&](const char* key, int64_t val) {
        if (val == 0) return;
        if (!first) out << ",";
        first = false;
        out << "\"" << key << "\":" << val;
    };
    auto add_double = [&](const char* key, double val) {
        if (val == 0.0) return;
        if (!first) out << ",";
        first = false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", val);
        out << "\"" << key << "\":" << buf;
    };

    add_str("video_codec", meta.video_codec);
    add_str("audio_codec", meta.audio_codec);
    add_double("fps", meta.fps);
    add_int("rotation", meta.rotation);
    add_str("DateTimeOriginal", meta.creation_date);

    out << "}";
    return out.str();
}
