#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "faces.h"
#include "types.h"

struct ForceOptions {
    bool hash = false;
    bool thumbs = false;
    bool faces = false;

    bool any() const { return hash || thumbs || faces; }
    void set_all() { hash = thumbs = faces = true; }
};

struct ScanOptions {
    std::string directory;
    bool recursive = false;
    bool fail_on_error = false;
    ForceOptions force;
    bool faces = false;
    bool thumbs = false;
    bool ignore_mount_warning = false;
    std::string media;  // "", "image", or "video" ("" = all)
    bool include_hidden = false;
    int parallel = -1;  // -1 = auto
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

struct ProcessResult {
    ImageInfo info;
    std::vector<FaceInfo> faces;  // empty if faces not requested
};

// Hashes, phashes, and extracts metadata for one file, optionally detecting
// faces. Throws std::runtime_error if the file cannot be decoded.
ProcessResult process_file(const std::filesystem::path& filepath, FaceDetector* face_detector);
