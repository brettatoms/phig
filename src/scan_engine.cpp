#include "scan_engine.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "database.h"
#include "dates.h"
#include "exif.h"
#include "hasher.h"
#include "metadata.h"
#include "scanner.h"

namespace fs = std::filesystem;

ProcessResult process_file(const fs::path& filepath, FaceDetector* face_detector) {
    ProcessResult result;
    auto& info = result.info;

    info.path = fs::canonical(filepath).string();
    info.filename = filepath.filename().string();
    info.extension = filepath.extension().string();
    if (!info.extension.empty() && info.extension[0] == '.') {
        info.extension = info.extension.substr(1);
    }
    // Lowercase extension
    std::transform(info.extension.begin(), info.extension.end(), info.extension.begin(), ::tolower);

    // File metadata
    info.file_size = fs::file_size(filepath);
    info.modified_at = format_time(fs::last_write_time(filepath));

    // SHA256
    info.sha256 = compute_sha256(info.path);

    bool is_video = is_known_video_extension(info.extension);

    if (is_video) {
        info.media_type = "video";

        // Video phash from keyframe
        auto phash_result = compute_video_phash(info.path);
        info.phash = phash_result.hash;
        info.width = phash_result.width;
        info.height = phash_result.height;

        // Video metadata via libavformat
        auto vmeta = extract_video_metadata(info.path);
        info.duration_ms = vmeta.duration_ms;
        info.metadata_json = video_metadata_to_json(vmeta);

        // No face detection for videos
    } else {
        info.media_type = "image";

        // Decode image once
        cv::Mat img = decode_image(info.path);
        if (img.empty()) {
            throw std::runtime_error("Cannot decode image: " + info.path);
        }

        // Phash from decoded image
        auto phash_result = compute_phash(img);
        info.phash = phash_result.hash;
        info.width = phash_result.width;
        info.height = phash_result.height;

        // Sharpness score (essentially free on the already-decoded image).
        info.blur_score = compute_blur_score(img);

        // Face detection from same decoded image (no re-decode)
        if (face_detector) {
            result.faces = face_detector->detect(img);
        }

        // EXIF / image metadata
        info.metadata_json = extract_exif_json(info.path);
    }

    info.effective_date = compute_effective_date(info.metadata_json, info.modified_at);

    return result;
}
