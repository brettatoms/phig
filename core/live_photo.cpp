#include "live_photo.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>

#include "exif.h"
#include "live_photo_internal.h"
#include "scanner.h"

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

bool exif_blob_contains_identifier(const std::vector<unsigned char>& blob, const std::string& id) {
    // Apple's identifier is a 36-character UUID. Anything else is refused
    // outright: an empty needle would match every still and pair one video with
    // all of them, and a short one invites accidental matches.
    if (id.size() != 36 || blob.size() < id.size()) return false;
    return std::search(blob.begin(), blob.end(), id.begin(), id.end()) != blob.end();
}

bool still_has_identifier(const std::string& still_path, const std::string& id) try {
    if (id.size() != 36) return false;
    return exif_blob_contains_identifier(extract_maker_note(still_path), id);
} catch (...) {
    return false;
}

std::vector<LivePhotoPair> find_live_photo_pairs(const std::vector<std::string>& paths) try {
    // Pass 1: identifiers from the videos. Cheap — a container open, no decode.
    std::vector<std::pair<std::string, std::string>> videos;  // (path, identifier)
    for (const auto& p : paths) {
        const std::string ext = std::filesystem::path(p).extension().string();
        if (!is_known_video_extension(ext)) continue;
        std::string id = live_photo_content_identifier(p);
        if (id.size() == 36) videos.emplace_back(p, std::move(id));
    }

    // Nothing claims to be a Live Photo, so no still is touched at all. This is
    // the common case for any collection without Apple media, and it keeps the
    // cost off collections that gain nothing from pairing.
    if (videos.empty()) return {};

    // Pass 2: each still's MakerNote is read exactly once, then matched against
    // every identifier. Reading per (video, still) instead would reintroduce the
    // quadratic shape plan 044 measured.
    std::unordered_map<std::string, std::vector<std::string>> claimants;  // id -> stills
    for (const auto& p : paths) {
        const std::string ext = std::filesystem::path(p).extension().string();
        if (!is_known_image_extension(ext)) continue;
        const std::vector<unsigned char> mnote = extract_maker_note(p);
        if (mnote.empty()) continue;
        for (const auto& [vpath, id] : videos) {
            (void)vpath;
            if (exif_blob_contains_identifier(mnote, id)) claimants[id].push_back(p);
        }
    }

    std::vector<LivePhotoPair> pairs;
    for (const auto& [vpath, id] : videos) {
        auto it = claimants.find(id);
        // Exactly one claimant, or the video stays independent.
        if (it == claimants.end() || it->second.size() != 1) continue;
        pairs.push_back({it->second.front(), vpath});
    }
    return pairs;
} catch (...) {
    return {};
}
