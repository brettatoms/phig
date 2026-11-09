#include "live_photo.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "av_log_capture.h"
#include "exif.h"
#include "live_photo_internal.h"
#include "scanner.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

// True only for the 8-4-4-4-12 hex shape. A length check alone is not enough:
// the needle comes from an untrusted container, and 36 spaces or 36 repeated
// bytes would otherwise be accepted and could match a run inside some vendor's
// MakerNote padding. The whole safety argument for containment rests on the
// identifier being a real UUID, so it is enforced rather than assumed.
bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

// Apple's key on the MOV half of a Live Photo, in the container's metadata
// dictionary — the same dictionary core/metadata.cpp already reads
// creation_time from for every indexed video.
constexpr const char* kContentIdentifierKey = "com.apple.quicktime.content.identifier";

}  // namespace

std::string live_photo_content_identifier(const std::string& video_path) try {
    // This runs before any worker reads metadata (the scan pre-pass) and
    // organize never does, so FFmpeg's callback is installed here too.
    install_av_log_callback();

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
    std::string id(tag->value);
    return is_uuid(id) ? id : std::string{};
} catch (...) {
    return {};
}

bool exif_blob_contains_identifier(const std::vector<unsigned char>& blob, const std::string& id) {
    // Anything not shaped like a UUID is refused outright: an empty needle
    // would match every still and pair one video with all of them, and an
    // arbitrary 36-byte run invites accidental matches in MakerNote padding.
    if (!is_uuid(id) || blob.size() < id.size()) return false;
    // Compared as unsigned on both sides. Left implicit, the blob's unsigned
    // char and the needle's char promote differently, so any byte >= 0x80 could
    // never match on a signed-char platform.
    return std::search(blob.begin(), blob.end(), id.begin(), id.end(), [](unsigned char a, char b) {
               return a == static_cast<unsigned char>(b);
           }) != blob.end();
}

bool still_has_identifier(const std::string& still_path, const std::string& id) try {
    if (!is_uuid(id)) return false;
    return exif_blob_contains_identifier(extract_maker_note(still_path), id);
} catch (...) {
    return false;
}

std::vector<LivePhotoPair> find_live_photo_pairs(const std::vector<std::string>& paths) try {
    // Grouped by directory, because a still and its video are written together
    // and the alternative is pathological: pairing across a whole walk would
    // read every still's MakerNote as soon as one Live Photo appeared anywhere
    // in the collection. Scoping bounds that to directories that contain one.
    std::unordered_map<std::string, std::vector<const std::string*>> by_dir;
    for (const auto& p : paths) {
        by_dir[std::filesystem::path(p).parent_path().string()].push_back(&p);
    }

    std::vector<LivePhotoPair> pairs;
    for (const auto& [dir, group] : by_dir) {
        (void)dir;

        // Identifiers first. A container open, no decode.
        std::vector<std::pair<const std::string*, std::string>> videos;
        for (const std::string* p : group) {
            if (!is_known_video_extension(std::filesystem::path(*p).extension().string())) continue;
            std::string id = live_photo_content_identifier(*p);
            if (id.size() == 36) videos.emplace_back(p, std::move(id));
        }

        // Nothing here claims to be a Live Photo, so no still in this directory
        // is opened at all — the common case for any collection without Apple
        // media, and what keeps this off the cost of an ordinary scan.
        if (videos.empty()) continue;

        // Each still's MakerNote is read exactly once and matched against every
        // identifier. Reading per (video, still) instead would reintroduce the
        // quadratic shape plan 044 measured in has_sibling_raw.
        std::unordered_map<std::string, std::vector<const std::string*>> claimants;
        for (const std::string* p : group) {
            if (!is_known_image_extension(std::filesystem::path(*p).extension().string())) continue;
            // A JPEG demoted to a RAW's attachment has no row of its own, so
            // pairing a video to it would attach the video to something scan
            // never indexed — leaving both unreachable.
            if (has_sibling_raw(*p)) continue;
            const std::vector<unsigned char> mnote = extract_maker_note(*p);
            if (mnote.empty()) continue;
            for (const auto& [vpath, id] : videos) {
                (void)vpath;
                if (exif_blob_contains_identifier(mnote, id)) claimants[id].push_back(p);
            }
        }

        // Ambiguity is refused in both directions. One still claiming many
        // videos is as wrong as many stills claiming one: scan would skip every
        // such video while organize, which refuses the same ambiguity, would
        // carry none of them — stranding files on disk that phig has
        // deliberately declined to index.
        std::vector<LivePhotoPair> dir_pairs;
        for (const auto& [vpath, id] : videos) {
            auto it = claimants.find(id);
            // Exactly one claimant, or the video stays an independent row.
            // Note two videos sharing one identifier land the same still in the
            // vector twice, which this also refuses — deliberately, not by
            // accident.
            if (it == claimants.end() || it->second.size() != 1) continue;
            dir_pairs.push_back({*it->second.front(), *vpath});
        }

        std::unordered_map<std::string, int> videos_per_still;
        for (const auto& p : dir_pairs) videos_per_still[p.still]++;
        for (auto& p : dir_pairs) {
            if (videos_per_still[p.still] == 1) pairs.push_back(std::move(p));
        }
    }
    return pairs;
} catch (...) {
    return {};
}

namespace {

// Identifiers of the videos in one directory, cached across calls.
//
// Organize calls live_photo_video_in once per moved file, and each call would
// otherwise open every video in the source directory: an iPhone dump of 2,000
// stills beside 2,000 MOVs is ~2,000,000 container opens for one `mv`. The
// cache makes it one open per video per directory.
//
// thread_local because organize is single-threaded today and this avoids a
// lock; entries are re-validated below rather than trusted, since organize is
// moving files out of these very directories as it runs.
struct DirVideoCache {
    std::string dir;
    std::vector<std::pair<std::string, std::filesystem::path>> videos;  // (identifier, path)
};

const std::vector<std::pair<std::string, std::filesystem::path>>& videos_in_dir(
    const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    thread_local DirVideoCache cache;

    const std::string key = dir.string();
    if (cache.dir == key) return cache.videos;

    cache.dir = key;
    cache.videos.clear();
    std::error_code dir_ec;
    for (const auto& entry : fs::directory_iterator(dir, dir_ec)) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
        if (!is_known_video_extension(entry.path().extension().string())) continue;
        std::string id = live_photo_content_identifier(entry.path().string());
        if (!id.empty()) cache.videos.emplace_back(std::move(id), entry.path());
    }
    return cache.videos;
}

}  // namespace

std::filesystem::path live_photo_video_in(const std::filesystem::path& still_for_metadata,
                                          const std::filesystem::path& search_dir) try {
    namespace fs = std::filesystem;

    // The gate: no MakerNote means nothing further is read — no directory
    // listing, no video opened. Note this catches every image with no vendor
    // MakerNote at all, not only non-Apple ones: a Canon or Nikon still does
    // carry one and will reach the loop below.
    const std::vector<unsigned char> mnote = extract_maker_note(still_for_metadata.string());
    if (mnote.empty()) return {};

    fs::path found;
    int matches = 0;
    for (const auto& [id, path] : videos_in_dir(search_dir)) {
        if (!exif_blob_contains_identifier(mnote, id)) continue;
        // Re-checked rather than trusted: organize moves files out of this
        // directory as it goes, so a cached entry may already be gone.
        std::error_code ec;
        if (!fs::exists(path, ec) || ec) continue;
        found = path;
        if (++matches > 1) return {};  // ambiguous: carry nothing
    }
    return matches == 1 ? found : fs::path{};
} catch (...) {
    return {};
}

std::filesystem::path live_photo_video_for(const std::filesystem::path& still_path) {
    return live_photo_video_in(still_path, still_path.parent_path());
}

std::filesystem::path live_photo_still_for(const std::filesystem::path& video_path) try {
    namespace fs = std::filesystem;

    // The gate: an ordinary clip carries no identifier, so no still is opened.
    const std::string id = live_photo_content_identifier(video_path.string());
    if (id.size() != 36) return {};

    fs::path found;
    int matches = 0;
    // Fresh error_code per entry — see live_photo_video_in.
    std::error_code dir_ec;
    for (const auto& entry : fs::directory_iterator(video_path.parent_path(), dir_ec)) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
        if (!is_known_image_extension(entry.path().extension().string())) continue;

        const std::vector<unsigned char> mnote = extract_maker_note(entry.path().string());
        if (mnote.empty()) continue;
        if (!exif_blob_contains_identifier(mnote, id)) continue;

        found = entry.path();
        if (++matches > 1) return {};  // ambiguous: demote nothing
    }
    return matches == 1 ? found : fs::path{};
} catch (...) {
    return {};
}
