#pragma once

#include <string>
#include <cstdint>

struct VideoMetadata {
    int64_t duration_ms = 0;
    int width = 0;
    int height = 0;
    std::string video_codec;
    std::string audio_codec;
    double fps = 0.0;
    int rotation = 0;
    std::string creation_date;  // ISO or EXIF-style date string
};

// Extract video metadata using libavformat.
// Returns a VideoMetadata struct. Fields are empty/zero on failure.
VideoMetadata extract_video_metadata(const std::string& path);

// Serialize VideoMetadata to a JSON string for the metadata column.
std::string video_metadata_to_json(const VideoMetadata& meta);
