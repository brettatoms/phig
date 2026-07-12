#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "database.h"
#include "dates.h"
#include "exif.h"
#include "faces.h"
#include "selection.h"
#include "hasher.h"
#include "log.h"
#include "metadata.h"
#include "models.h"
#include "organize_exec.h"
#include "paths.h"
#include "relink.h"
#include "remove_exec.h"
#include "scanner.h"
#include "sidecar.h"
#include "thumbs.h"
#include "types.h"
#include "version.h"

namespace fs = std::filesystem;

// Graceful shutdown on Ctrl-C
static volatile std::sig_atomic_t g_interrupted = 0;
static void sigint_handler(int) { g_interrupted = 1; }

// Full invocation string, stored on the operation row for the audit trail.
static std::string join_argv(int argc, char* argv[]) {
    std::string s;
    for (int i = 0; i < argc; i++) {
        if (i) s += ' ';
        s += argv[i];
    }
    return s;
}

// ---- Helpers ----

std::string format_time(fs::file_time_type ftime) {
    // Portable conversion: compute offset between file_clock and system_clock
    auto file_now = decltype(ftime)::clock::now();
    auto sys_now = std::chrono::system_clock::now();
    auto sys_tp = sys_now + (ftime - file_now);
    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(sys_tp);
    auto time_t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm;
    gmtime_r(&time_t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  phig scan <directory> [flags]\n"
        << "  phig duplicates [flags]\n"
        << "  phig cp <destination> [flags]\n"
        << "  phig mv <destination> [flags]\n"
        << "  phig rm [--match|--exclude] [--hard|--db-only|--orphans] [--dry-run] [--yes]\n"
        << "                               Remove images: trash (default), --hard (permanent),\n"
        << "                               --db-only (keep file), --orphans (missing files)\n"
        << "  phig trash empty [--yes]     Permanently delete everything in the trash\n"
        << "  phig search [flags]\n"
        << "  phig face name <image> <name>     Name a face from a single-face photo\n"
        << "  phig face identify <name>          Label all matching faces in DB\n"
        << "  phig face rebuild [--match] [--exclude] [--parallel N]  Re-detect all faces\n"
        << "  phig face list                     List known people\n"
        << "  phig thumbs rebuild [flags]  Generate missing thumbnails\n"
        << "  phig thumbs clean            Remove orphaned thumbnails\n"
        << "  phig tag add|remove <tag> [--match|--exclude|--path]   Tag images\n"
        << "  phig tag list                                         List tags and counts\n"
        << "  phig gallery set <name>|clear [--match|--exclude|--path]  Assign/clear gallery\n"
        << "  phig gallery list                                     List galleries and counts\n"
        << "  phig fav add|remove [--match|--exclude|--path]         Favorite images\n"
        << "  phig fav list [output flags]                          List favorites\n"
        << "  phig models download\n"
        << "\nScan flags:\n"
        << "  --recursive              Scan subdirectories\n"
        << "  --on-error warn|fail     Behavior for unreadable files (default: warn)\n"
        << "  --force [what]           Re-process: hash,thumbs,faces,all (default: all)\n"
        << "  --faces                  Detect faces and compute embeddings\n"
        << "  --thumbs                 Generate thumbnails\n"
        << "  --ignore-mount-warning   Proceed even if all files are missing\n"
        << "  --media image|video|all  Restrict to one media type (default: all)\n"
        << "  --hidden                 Include hidden/temp files and directories (. or ~)\n"
        << "  --parallel N             Number of images to process in parallel (default: auto)\n"
        << "  --db <path>              Database path\n"
        << "\nDuplicates flags:\n"
        << "  --type exact|near|all    Duplicate type to find (default: all)\n"
        << "  --threshold [0-64]       Hamming distance for near duplicates (default: 5)\n"
        << "  --format text|csv|json   Output format (default: text)\n"
        << "  --output <path>          Write to file instead of stdout\n"
        << "  --match <glob>           Show groups containing matching files (repeatable, matches "
           "path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --media image|video|all  Filter by media type (default: all)\n"
        << "  --fav                    Only groups containing a favorite\n"
        << "  --db <path>              Database path\n"
        << "\nCopy/Move flags (cp, mv):\n"
        << "  --format <string>        Output path format (default: %Y/%m/%original)\n"
        << "                           Tokens: %Y %m %d %camera %make %gallery %fav %original\n"
        << "  --match <glob>           Only files matching glob (repeatable, matches path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --dry-run                Show what would happen without doing it\n"
        << "  --on-conflict skip|overwrite|rename  (default: skip)\n"
        << "  --db <path>              Database path\n"
        << "\nRemove flags (rm):\n"
        << "  --match <glob>           Remove entries matching glob (repeatable, matches path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --hard                   Delete the file from disk (permanent, no trash)\n"
        << "  --db-only                Remove the DB row only, keep the file on disk\n"
        << "  --orphans                Remove DB rows whose files are already missing\n"
        << "  --dry-run                Show what would be removed\n"
        << "  --yes                    Skip confirmation for broad removals\n"
        << "  --db <path>              Database path\n"
        << "\nSearch flags:\n"
        << "  --ext <extension>        Match file extension (e.g., jpg, png, cr2)\n"
        << "  --after <date>           Files dated after YYYY-MM-DD\n"
        << "  --before <date>          Files dated before YYYY-MM-DD\n"
        << "  --camera <string>        Substring match on EXIF camera model\n"
        << "  --make <string>          Substring match on EXIF camera make\n"
        << "  --min-size <size>        Minimum file size (e.g., 1000, 5KB, 10MB, 1GB)\n"
        << "  --max-size <size>        Maximum file size (e.g., 1000, 5KB, 10MB, 1GB)\n"
        << "  --media image|video|all  Filter by media type (default: all)\n"
        << "  --min-duration <dur>     Minimum video duration (e.g., 10s, 5m, 1h)\n"
        << "  --max-duration <dur>     Maximum video duration (e.g., 10s, 5m, 1h)\n"
        << "  --similar <image-path>   Find visually similar images (by phash)\n"
        << "  --similar-hash <hex>     Find images similar to this phash\n"
        << "  --threshold [0-64]       Hamming distance for --similar (default: 5)\n"
        << "  --face <image-path>      Find images containing this person\n"
        << "  --face-threshold <float> Distance threshold for face match (default: 0.6)\n"
        << "  --person <name>          Find images containing a named person\n"
        << "  --tag <name>             Images with this tag (repeatable, all must match)\n"
        << "  --gallery <name>         Images in this gallery\n"
        << "  --fav                    Favorited images (alias for --tag favorite)\n"
        << "  --blurry                 Low-sharpness images (blur_score <= 100)\n"
        << "  --min-sharpness <n>      Only images with blur_score >= n\n"
        << "  --max-sharpness <n>      Only images with blur_score <= n\n"
        << "  --match <glob>           Include matching files (repeatable, matches path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --limit N                Maximum number of results\n"
        << "  --count                  Show count of matches only\n"
        << "  --porcelain              Tab-separated machine output\n"
        << "  --print0                 Null-separated paths\n"
        << "  --format text|csv|json   Output format (default: text)\n"
        << "  --output <path>          Write to file\n"
        << "  --db <path>              Database path\n"
        << "\nThumbs rebuild flags:\n"
        << "  --match <glob>           Only matching files (repeatable)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable)\n"
        << "  --force                  Regenerate even if thumbnail exists\n"
        << "  --parallel N             Number of images to process in parallel (default: auto)\n"
        << "  --db <path>              Database path\n";
}

// ---- Scan command ----

struct ForceOptions {
    bool hash = false;
    bool thumbs = false;
    bool faces = false;

    bool any() const { return hash || thumbs || faces; }
    void set_all() { hash = thumbs = faces = true; }
};

ForceOptions parse_force(const std::string& value) {
    ForceOptions f;
    if (value.empty()) {
        f.set_all();
        return f;
    }
    std::istringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token == "hash")
            f.hash = true;
        else if (token == "thumbs")
            f.thumbs = true;
        else if (token == "faces")
            f.faces = true;
        else if (token == "all")
            f.set_all();
        else
            throw std::runtime_error("Unknown --force value: " + token +
                                     " (use hash,thumbs,faces,all)");
    }
    return f;
}

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

    return result;
}

int cmd_scan(const ScanOptions& opts) {
    auto start_time = std::chrono::steady_clock::now();

    Database db(opts.db_path);
    db.init_schema();

    // Helper to show paths relative to the scan directory
    std::string base_dir = fs::canonical(opts.directory).string();
    if (base_dir.back() != '/') base_dir += '/';

    auto rel_path = [&](const fs::path& p) -> std::string {
        std::string full = p.string();
        if (full.starts_with(base_dir)) {
            return full.substr(base_dir.size());
        }
        return full;
    };

    std::cout << "Scanning: " << opts.directory << (opts.recursive ? " (recursive)" : "") << "\n";
    std::cout << "Database: " << opts.db_path << "\n";

    // Collect files
    auto files = scan_directory(opts.directory, opts.recursive, opts.include_hidden);

    // Filter by media type if requested
    if (opts.media == "image") {
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const fs::path& p) {
                                       return !is_known_image_extension(p.extension().string());
                                   }),
                    files.end());
    } else if (opts.media == "video") {
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const fs::path& p) {
                                       return !is_known_video_extension(p.extension().string());
                                   }),
                    files.end());
    }

    std::cout << "Found " << files.size() << " files\n";

    // Determine thread count
    int num_threads;
    if (opts.parallel > 0) {
        num_threads = opts.parallel;
    } else if (opts.faces) {
        // With faces: conservative default (each thread loads ~150MB of models)
        num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
        num_threads = std::max(num_threads, 1);
    } else {
        num_threads = std::max(1u, std::thread::hardware_concurrency());
    }

    // Create per-thread face detectors if needed
    std::vector<std::unique_ptr<FaceDetector>> face_detectors;
    if (opts.faces) {
        std::cout << "Loading face recognition models (" << num_threads << " instances)...\n";
        for (int i = 0; i < num_threads; i++) {
            face_detectors.push_back(std::make_unique<FaceDetector>());
        }
    }

    // Install signal handler for graceful Ctrl-C
    g_interrupted = 0;
    auto prev_handler = std::signal(SIGINT, sigint_handler);

    // Process files with thread pool
    std::atomic<size_t> processed{0};
    std::atomic<size_t> skipped{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> face_count{0};
    std::atomic<size_t> thumb_count{0};
    std::atomic<size_t> video_count{0};

    if (!files.empty()) {
        std::atomic<size_t> next_file{0};

        std::mutex db_mutex;
        std::vector<ProcessResult> batch;
        const size_t batch_size = 10;  // small batches so Ctrl-C loses less work

        auto flush_batch = [&]() {
            if (batch.empty()) return;
            // Insert image metadata
            std::vector<ImageInfo> infos;
            infos.reserve(batch.size());
            for (auto& r : batch) infos.push_back(r.info);
            db.insert_batch(infos);

            // Insert faces (need image_id from DB) and auto-label known people
            for (auto& r : batch) {
                if (!r.faces.empty()) {
                    int64_t image_id = db.get_image_id(r.info.path);
                    // Rows whose bytes didn't change kept their existing faces
                    // (and person labels) through insert(); re-adding this
                    // run's detections would duplicate them. insert() cleared
                    // faces when the content changed, so has_faces == false
                    // means these detections are needed. `face rebuild` is the
                    // path for forced re-detection.
                    if (image_id >= 0 && !db.has_faces(r.info.path)) {
                        for (const auto& face : r.faces) {
                            db.insert_face(image_id, face);
                            int64_t face_id = sqlite3_last_insert_rowid(db.db_ptr());
                            db.auto_label_face(face_id, face.embedding);
                        }
                        face_count.fetch_add(r.faces.size());
                    }
                }
            }
            batch.clear();
        };

        auto worker = [&](int thread_id) {
            FaceDetector* detector =
                (!face_detectors.empty()) ? face_detectors[thread_id].get() : nullptr;
            while (!g_interrupted) {
                size_t idx = next_file.fetch_add(1);
                if (idx >= files.size()) break;

                const auto& filepath = files[idx];
                std::string canonical;
                try {
                    canonical = fs::canonical(filepath).string();
                } catch (...) {
                    errors.fetch_add(1);
                    continue;
                }

                if (has_sibling_raw(filepath)) {
                    skipped.fetch_add(1);
                    continue;
                }

                // Check if we should skip (path + mtime match)
                bool needs_faces_only = false;
                bool needs_thumbs_only = false;

                if (!opts.force.hash) {
                    std::string mtime;
                    try {
                        mtime = format_time(fs::last_write_time(filepath));
                    } catch (...) {
                        // Can't stat, will fail later
                    }

                    std::lock_guard<std::mutex> lock(db_mutex);
                    auto existing_mtime = db.get_modified_at(canonical);
                    if (existing_mtime && *existing_mtime == mtime) {
                        // File unchanged — check if we need faces or thumbs
                        bool need_faces = detector && !db.has_faces(canonical);
                        bool need_thumbs = false;
                        if (opts.thumbs) {
                            auto sha = db.get_sha256(canonical);
                            if (sha && !sha->empty()) {
                                need_thumbs = opts.force.thumbs || !thumb_exists(*sha);
                            }
                        }

                        if (need_faces) {
                            needs_faces_only = true;
                        } else if (need_thumbs && !need_faces) {
                            needs_thumbs_only = true;
                        } else {
                            skipped.fetch_add(1);
                            size_t done = processed.load() + skipped.load() + errors.load();
                            std::cout << "\r[" << done << "/" << files.size() << "] " << std::flush;
                            continue;
                        }
                    }
                }

                try {
                    if (needs_thumbs_only) {
                        // Only generate thumbnail — file is unchanged and faces are done
                        std::string sha;
                        {
                            std::lock_guard<std::mutex> lock(db_mutex);
                            auto s = db.get_sha256(canonical);
                            if (s) sha = *s;
                        }
                        if (!sha.empty()) {
                            generate_thumb(canonical, sha);
                            thumb_count.fetch_add(1);
                        }
                        processed.fetch_add(1);
                    } else if (needs_faces_only) {
                        // Only decode and detect faces — skip hash/phash/EXIF
                        cv::Mat img = decode_image(canonical);
                        if (!img.empty()) {
                            if (detector) {
                                auto faces = detector->detect(img);
                                if (!faces.empty()) {
                                    std::lock_guard<std::mutex> lock(db_mutex);
                                    // Flush any pending batch first, then insert faces in a
                                    // transaction
                                    flush_batch();
                                    // Another thread's flush_batch() may have inserted faces
                                    // for this row already; don't duplicate them.
                                    if (!db.has_faces(canonical)) {
                                        db.begin_transaction();
                                        try {
                                            int64_t image_id = db.get_image_id(canonical);
                                            if (image_id >= 0) {
                                                for (const auto& face : faces) {
                                                    db.insert_face(image_id, face);
                                                    int64_t face_id =
                                                        sqlite3_last_insert_rowid(db.db_ptr());
                                                    db.auto_label_face(face_id, face.embedding);
                                                }
                                                face_count.fetch_add(faces.size());
                                            }
                                            db.commit_transaction();
                                        } catch (...) {
                                            db.rollback_transaction();
                                            throw;
                                        }
                                    }
                                }
                            }
                            // Also generate thumbnail if needed
                            if (opts.thumbs) {
                                std::string sha;
                                {
                                    std::lock_guard<std::mutex> lock(db_mutex);
                                    auto s = db.get_sha256(canonical);
                                    if (s) sha = *s;
                                }
                                if (!sha.empty() && (opts.force.thumbs || !thumb_exists(sha))) {
                                    generate_thumb(img, sha);
                                    thumb_count.fetch_add(1);
                                }
                            }
                        }
                        processed.fetch_add(1);
                    } else {
                        auto result = process_file(filepath, detector);

                        if (result.info.media_type == "video") {
                            video_count.fetch_add(1);
                        }

                        // Generate thumbnail if requested
                        if (opts.thumbs && !result.info.sha256.empty()) {
                            if (opts.force.thumbs || !thumb_exists(result.info.sha256)) {
                                if (result.info.media_type == "video") {
                                    // For videos, extract keyframe for thumbnail
                                    try {
                                        auto kf = extract_video_keyframe(canonical);
                                        if (!kf.frame.empty()) {
                                            generate_thumb(kf.frame, result.info.sha256);
                                            thumb_count.fetch_add(1);
                                        }
                                    } catch (...) {
                                    }
                                } else {
                                    cv::Mat img = decode_image(canonical);
                                    if (!img.empty()) {
                                        generate_thumb(img, result.info.sha256);
                                        thumb_count.fetch_add(1);
                                    }
                                }
                            }
                        }

                        std::lock_guard<std::mutex> lock(db_mutex);
                        batch.push_back(std::move(result));
                        if (batch.size() >= batch_size) {
                            flush_batch();
                        }
                        processed.fetch_add(1);
                    }
                } catch (const std::exception& e) {
                    errors.fetch_add(1);
                    // Clear progress line, print warning, then resume progress
                    std::cout << "\r\033[K" << std::flush;
                    // Assemble the whole line so concurrent workers' messages
                    // don't interleave with each other or the progress line.
                    if (opts.fail_on_error) {
                        log_error("scan: " + rel_path(filepath) + " -- " + e.what());
                        throw;
                    } else {
                        log_warn("scan: " + rel_path(filepath) + " -- " + e.what());
                    }
                }

                size_t done = processed.load() + skipped.load() + errors.load();
                std::cout << "\r\033[K[" << done << "/" << files.size() << "] "
                          << rel_path(filepath) << std::flush;
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back(worker, i);
        }
        for (auto& t : threads) {
            t.join();
        }

        // Flush remaining
        flush_batch();
    }  // end if (!files.empty())

    // Restore previous signal handler
    std::signal(SIGINT, prev_handler);

    if (g_interrupted) {
        std::cout << "\n\nInterrupted. Progress saved.\n";
    }

    // Clean up entries for files that no longer exist on disk
    std::string scan_prefix = fs::canonical(opts.directory).string();
    if (scan_prefix.back() != '/') scan_prefix += '/';

    auto db_paths = db.get_paths_with_prefix(scan_prefix);
    // Only consider a row deletable when its file is genuinely gone AND its
    // parent directory still exists — a missing parent (unmounted subtree) or an
    // errored existence check keeps the row, so a transient mount/permission
    // problem can't purge metadata for intact files.
    std::vector<std::string> removed_paths = paths_safe_to_delete(db_paths);
    size_t removed_count = 0;
    int relinked_count = 0;
    int demoted_count = 0;

    if (!removed_paths.empty() && !db_paths.empty() && removed_paths.size() == db_paths.size() &&
        !opts.ignore_mount_warning) {
        log_warn("scan: all " + std::to_string(removed_paths.size()) +
                 " previously scanned files in this directory are missing.\n"
                 "This may indicate an unmounted drive or incorrect path.\n"
                 "Use --ignore-mount-warning to remove these entries from the database.");
    } else {
        // A demoted JPEG (same-basename RAW now on disk, RAW itself indexed)
        // still exists on disk, so it's never in removed_paths — but its
        // stale row must still be cleaned up. has_sibling_raw is a cheap,
        // read-only peek (no DB writes) used only to decide whether it's
        // worth opening a scan operation; demote_jpeg_rows (below) does the
        // real, authoritative work exactly once, inside that operation.
        bool maybe_demotable = std::any_of(db_paths.begin(), db_paths.end(),
                                           [](const auto& p) { return has_sibling_raw(p); });

        if (!removed_paths.empty() || maybe_demotable) {
            // Removing a row destroys user-authored metadata (tags, gallery,
            // faces) that a rescan cannot rebuild — record it like purge does,
            // with a restorable row snapshot per entry (review #3a).
            if (opts.log_enabled) {
                int64_t op_id = db.log_begin_operation("scan", opts.argv_str, iso_utc_now());
                try {
                    relinked_count = relink_moved_rows(db, removed_paths, op_id);
                    std::vector<std::string> demoted_paths =
                        demote_jpeg_rows(db, scan_prefix, op_id);
                    demoted_count = static_cast<int>(demoted_paths.size());
                    std::vector<std::string> to_delete = removed_paths;
                    to_delete.insert(to_delete.end(), demoted_paths.begin(), demoted_paths.end());
                    db.delete_paths_logged(to_delete, op_id);
                    removed_count = removed_paths.size();
                } catch (...) {
                    // delete_paths_logged rolls back its own transaction, but the
                    // operation row was autocommitted by log_begin_operation. Mark
                    // it so it doesn't linger as a stuck 'in_progress' row, matching
                    // rm's broad-selection guard (review [6]); best-effort.
                    try {
                        db.log_finish_operation(op_id, "interrupted");
                    } catch (...) {
                    }
                    throw;
                }
                db.log_finish_operation(op_id, "completed");
                db.log_prune(opts.log_max_entries);
            } else {
                relinked_count = relink_moved_rows(db, removed_paths, 0);
                std::vector<std::string> demoted_paths = demote_jpeg_rows(db, scan_prefix, 0);
                demoted_count = static_cast<int>(demoted_paths.size());
                std::vector<std::string> to_delete = removed_paths;
                to_delete.insert(to_delete.end(), demoted_paths.begin(), demoted_paths.end());
                db.delete_paths(to_delete);
                removed_count = removed_paths.size();
            }
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;

    size_t image_count = processed.load() - video_count.load();
    std::cout << "\n\nDone in " << secs << "s\n"
              << "  Processed: " << processed.load() << " (" << image_count << " images, "
              << video_count.load() << " videos)\n"
              << "  Skipped:   " << skipped.load() << "\n"
              << "  Errors:    " << errors.load() << "\n"
              << "  Removed:   " << removed_count << "\n";
    if (relinked_count > 0) {
        std::cout << "  Relinked:  " << relinked_count
                  << " (metadata moved to identical files at new paths)\n";
    }
    if (demoted_count > 0) {
        std::cout << "  Demoted:   " << demoted_count
                  << " (metadata moved from JPEG to sibling RAW)\n";
    }
    if (opts.faces) {
        std::cout << "  Faces:     " << face_count.load() << "\n";
    }
    if (opts.thumbs) {
        std::cout << "  Thumbs:    " << thumb_count.load() << "\n";
    }
    std::cout << "  Total DB:  " << db.count() << " entries\n";

    log_info("scan " + opts.directory + " done: processed=" + std::to_string(processed.load()) +
             " skipped=" + std::to_string(skipped.load()) + " errors=" +
             std::to_string(errors.load()) + " removed=" + std::to_string(removed_count));

    return 0;
}

// ---- Duplicates command ----

struct DuplicatesOptions {
    std::string type = "all";     // exact, near, all
    int threshold = 5;            // hamming distance for near duplicates
    std::string format = "text";  // text, csv, json
    std::string output;           // empty = stdout
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    std::string media;  // "image", "video", or "" (all)
    bool fav = false;   // only groups containing at least one favorite
    std::string db_path;
};

int hamming_distance(uint64_t a, uint64_t b) { return std::popcount(a ^ b); }

struct DuplicateGroup {
    std::string type;  // "exact" or "near"
    int distance;      // 0 for exact
    std::vector<ImageInfo> images;
};

std::vector<DuplicateGroup> find_near_duplicates(const std::vector<ImageInfo>& all_images,
                                                 int threshold,
                                                 const std::set<std::string>& exact_sha256s) {
    std::vector<DuplicateGroup> groups;

    // Union-find to group near duplicates
    std::vector<int> parent(all_images.size());
    std::vector<int> min_dist(all_images.size(), 0);
    for (size_t i = 0; i < parent.size(); i++) parent[i] = i;

    std::function<int(int)> find = [&](int x) -> int {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[a] = b;
    };

    for (size_t i = 0; i < all_images.size(); i++) {
        for (size_t j = i + 1; j < all_images.size(); j++) {
            // Skip pairs that are exact duplicates
            if (all_images[i].sha256 == all_images[j].sha256 &&
                exact_sha256s.count(all_images[i].sha256)) {
                continue;
            }
            int dist = hamming_distance(all_images[i].phash, all_images[j].phash);
            if (dist <= threshold) {
                unite(i, j);
            }
        }
    }

    // Collect groups
    std::map<int, std::vector<size_t>> group_map;
    for (size_t i = 0; i < all_images.size(); i++) {
        group_map[find(i)].push_back(i);
    }

    for (const auto& [root, indices] : group_map) {
        if (indices.size() < 2) continue;
        DuplicateGroup g;
        g.type = "near";
        g.distance = threshold;
        for (size_t idx : indices) {
            g.images.push_back(all_images[idx]);
        }
        groups.push_back(std::move(g));
    }

    return groups;
}

void output_text(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                 const std::vector<DuplicateGroup>& near) {
    if (!exact.empty()) {
        out << "=== Exact Duplicates (same SHA256) ===\n\n";
        for (size_t g = 0; g < exact.size(); g++) {
            out << "Group " << (g + 1) << " (" << exact[g].images.size() << " files, "
                << exact[g].images[0].file_size << " bytes each):\n";
            for (const auto& img : exact[g].images) {
                out << "  " << img.path << "\n";
            }
            out << "\n";
        }
    }

    if (!near.empty()) {
        out << "=== Near Duplicates (similar phash) ===\n\n";
        for (size_t g = 0; g < near.size(); g++) {
            out << "Group " << (g + 1) << " (" << near[g].images.size() << " files):\n";
            for (const auto& img : near[g].images) {
                out << "  " << img.path << " [" << img.width << "x" << img.height << ", "
                    << img.file_size << " bytes]\n";
            }
            out << "\n";
        }
    }

    size_t exact_files = 0, near_files = 0;
    for (const auto& g : exact) exact_files += g.images.size();
    for (const auto& g : near) near_files += g.images.size();

    out << "Summary:\n"
        << "  Exact duplicate groups: " << exact.size() << " (" << exact_files << " files)\n"
        << "  Near duplicate groups:  " << near.size() << " (" << near_files << " files)\n";
}

void output_csv(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                const std::vector<DuplicateGroup>& near) {
    out << "group,type,path,filename,sha256,phash,width,height,file_size\n";
    int group_num = 0;

    for (const auto& g : exact) {
        group_num++;
        for (const auto& img : g.images) {
            out << group_num << ",exact,\"" << img.path << "\"," << img.filename << ","
                << img.sha256 << "," << img.phash << "," << img.width << "," << img.height << ","
                << img.file_size << "\n";
        }
    }
    for (const auto& g : near) {
        group_num++;
        for (const auto& img : g.images) {
            out << group_num << ",near,\"" << img.path << "\"," << img.filename << "," << img.sha256
                << "," << img.phash << "," << img.width << "," << img.height << "," << img.file_size
                << "\n";
        }
    }
}

void output_json(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                 const std::vector<DuplicateGroup>& near) {
    auto write_groups = [&](const std::vector<DuplicateGroup>& groups, bool& first_group) {
        for (const auto& g : groups) {
            if (!first_group) out << ",";
            first_group = false;
            out << "{\"type\":\"" << g.type << "\",\"files\":[";
            for (size_t i = 0; i < g.images.size(); i++) {
                if (i > 0) out << ",";
                const auto& img = g.images[i];
                out << "{\"path\":\"" << img.path << "\""
                    << ",\"filename\":\"" << img.filename << "\""
                    << ",\"sha256\":\"" << img.sha256 << "\""
                    << ",\"phash\":" << img.phash << ",\"width\":" << img.width
                    << ",\"height\":" << img.height << ",\"file_size\":" << img.file_size << "}";
            }
            out << "]}";
        }
    };

    out << "{\"groups\":[";
    bool first = true;
    write_groups(exact, first);
    write_groups(near, first);
    out << "]}\n";
}

// Select duplicate groups: keep only groups where at least one file passes selection
std::vector<DuplicateGroup> select_groups(const std::vector<DuplicateGroup>& groups,
                                          const std::vector<std::string>& matches,
                                          const std::vector<std::string>& excludes) {
    if (matches.empty() && excludes.empty()) return groups;

    std::vector<DuplicateGroup> result;
    for (const auto& g : groups) {
        bool has_match = false;
        for (const auto& img : g.images) {
            if (passes_selection(img.path, matches, excludes)) {
                has_match = true;
                break;
            }
        }
        if (has_match) result.push_back(g);
    }
    return result;
}

int cmd_duplicates(const DuplicatesOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    bool do_exact = (opts.type == "all" || opts.type == "exact");
    bool do_near = (opts.type == "all" || opts.type == "near");

    // Compare against full DB (within path prefix), then filter results
    std::vector<DuplicateGroup> exact_groups;
    std::set<std::string> exact_sha256s;

    if (do_exact) {
        auto groups = db.get_exact_duplicates();
        for (auto& g : groups) {
            exact_sha256s.insert(g[0].sha256);
            DuplicateGroup dg;
            dg.type = "exact";
            dg.distance = 0;
            dg.images = std::move(g);
            exact_groups.push_back(std::move(dg));
        }
        exact_groups = select_groups(exact_groups, opts.matches, opts.excludes);
        if (!opts.media.empty()) {
            exact_groups.erase(std::remove_if(exact_groups.begin(), exact_groups.end(),
                                              [&](const DuplicateGroup& g) {
                                                  return std::none_of(
                                                      g.images.begin(), g.images.end(),
                                                      [&](const ImageInfo& img) {
                                                          return img.media_type == opts.media;
                                                      });
                                              }),
                               exact_groups.end());
        }
    }

    std::vector<DuplicateGroup> near_groups;
    if (do_near) {
        auto all_images = db.get_all_images();
        std::cerr << "Comparing " << all_images.size() << " images for near duplicates...\n";
        near_groups = find_near_duplicates(all_images, opts.threshold, exact_sha256s);
        near_groups = select_groups(near_groups, opts.matches, opts.excludes);
        if (!opts.media.empty()) {
            near_groups.erase(std::remove_if(near_groups.begin(), near_groups.end(),
                                             [&](const DuplicateGroup& g) {
                                                 return std::none_of(
                                                     g.images.begin(), g.images.end(),
                                                     [&](const ImageInfo& img) {
                                                         return img.media_type == opts.media;
                                                     });
                                             }),
                              near_groups.end());
        }
    }

    // Keep only groups containing at least one favorited image.
    if (opts.fav) {
        Database::SearchCriteria fc;
        fc.tags = {"favorite"};
        std::set<std::string> fav_paths;
        for (const auto& img : db.search(fc)) {
            fav_paths.insert(img.path);
        }
        auto keep_favorited = [&](std::vector<DuplicateGroup>& groups) {
            groups.erase(std::remove_if(groups.begin(), groups.end(),
                                        [&](const DuplicateGroup& g) {
                                            return std::none_of(
                                                g.images.begin(), g.images.end(),
                                                [&](const ImageInfo& img) {
                                                    return fav_paths.count(img.path) > 0;
                                                });
                                        }),
                         groups.end());
        };
        keep_favorited(exact_groups);
        keep_favorited(near_groups);
    }

    // Output
    std::ofstream file_out;
    if (!opts.output.empty()) {
        file_out.open(opts.output);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << opts.output << "\n";
            return 1;
        }
    }
    std::ostream& out = opts.output.empty() ? std::cout : file_out;

    if (opts.format == "csv") {
        output_csv(out, exact_groups, near_groups);
    } else if (opts.format == "json") {
        output_json(out, exact_groups, near_groups);
    } else {
        output_text(out, exact_groups, near_groups);
    }

    return 0;
}

// ---- Organize command ----

struct OrganizeOptions {
    std::string destination;
    std::string format = "%Y/%m/%original";
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    bool is_move = false;  // true for mv, false for cp
    bool dry_run = false;
    std::string on_conflict = "skip";  // skip, overwrite, rename
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

namespace {

std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date) {
    std::string result;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            // Check for multi-char tokens first
            if (fmt.substr(i, 9) == "%original") {
                result += img.filename;
                i += 8;
            } else if (fmt.substr(i, 7) == "%camera") {
                std::string cam = json_get(img.metadata_json, "Model");
                result += cam.empty() ? "unknown-camera" : cam;
                i += 6;
            } else if (fmt.substr(i, 5) == "%make") {
                std::string make = json_get(img.metadata_json, "Make");
                result += make.empty() ? "unknown-make" : make;
                i += 4;
            } else if (fmt.substr(i, 8) == "%gallery") {
                result += img.gallery.empty() ? "no-gallery" : img.gallery;
                i += 7;
            } else if (fmt.substr(i, 4) == "%fav") {
                result += img.favorite ? "favorites" : "unfavorited";
                i += 3;
            } else if (fmt[i + 1] == 'Y') {
                result += date.valid ? date.year : "unsorted";
                i += 1;
            } else if (fmt[i + 1] == 'm') {
                result += date.valid ? date.month : "unsorted";
                i += 1;
            } else if (fmt[i + 1] == 'd') {
                result += date.valid ? date.day : "unsorted";
                i += 1;
            } else {
                result += fmt[i];  // unknown token, keep as-is
            }
        } else {
            result += fmt[i];
        }
    }
    return result;
}

}  // anonymous namespace

int cmd_organize(const OrganizeOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    auto images = db.get_all_images();
    std::cout << "Found " << images.size() << " images in database\n";

    // Apply match/filter
    bool has_selection = !opts.matches.empty() || !opts.excludes.empty();
    if (has_selection) {
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageInfo& img) {
                                        return !passes_selection(img.path, opts.matches,
                                                               opts.excludes);
                                    }),
                     images.end());
        std::cout << "After match/filter: " << images.size() << " images\n";
    }

    // Plan moves/copies
    struct Action {
        std::string source;
        fs::path dest;
        bool skipped = false;
        std::string skip_reason;
    };
    std::vector<Action> actions;
    std::set<std::string> claimed;  // dest paths chosen this run, to catch intra-run collisions

    // All source paths in this batch. A destination equal to any OTHER image's
    // source is a real, irreplaceable original that hasn't moved yet — never
    // overwrite it (audit #1). A file may resolve to its own source (no-op).
    std::set<std::string> batch_sources;
    for (const auto& img : images) batch_sources.insert(img.path);

    for (const auto& img : images) {
        DateParts date = get_best_date(img);
        std::string rel_path = apply_format(opts.format, img, date);
        fs::path dest = fs::path(opts.destination) / rel_path;

        // Protect: (a) another image's source file in this batch, and (b) ANY
        // other real file on disk — tracked or not. Clobbering an untracked file
        // is silent data loss (audit #4); clobbering a tracked file destroys a
        // real image the DB still knows about and, with logging on, cascades its
        // faces/tags away with nothing in the log to restore them (plan 020
        // review). Overwriting only ever lands on a genuinely free path; existing
        // files are renamed around, so mv/cp never destroy irreplaceable bytes.
        const std::string& self_source = img.path;
        auto is_protected = [&](const std::string& path) -> bool {
            if (path == self_source) return false;
            if (batch_sources.count(path)) return true;
            if (fs::exists(path)) return true;
            return false;
        };

        auto resolved = resolve_conflict(dest.string(), opts.on_conflict, claimed, is_protected);
        Action act;
        act.source = img.path;
        if (resolved.empty()) {
            act.dest = dest;
            act.skipped = true;
            act.skip_reason = "already exists";
        } else {
            act.dest = resolved;
        }
        actions.push_back(std::move(act));
    }

    // Summary
    size_t to_process = 0, to_skip = 0;
    for (const auto& a : actions) {
        if (a.skipped)
            to_skip++;
        else
            to_process++;
    }

    std::string verb = opts.is_move ? "Move" : "Copy";
    std::cout << "\n"
              << verb << ": " << to_process << " files"
              << ", skip: " << to_skip << " (conflict/exists)\n";

    if (opts.dry_run) {
        std::cout << "\n[DRY RUN]\n";
        for (const auto& a : actions) {
            if (a.skipped) {
                std::cout << "  SKIP " << a.source << " → " << a.dest << " (" << a.skip_reason
                          << ")\n";
            } else {
                std::cout << "  " << verb << " " << a.source << " → " << a.dest << "\n";
            }
        }
        return 0;
    }

    // Execute. Handle SIGINT so Ctrl-C stops cleanly at a file boundary rather
    // than killing the process between a move and its DB update.
    g_interrupted = 0;
    auto prev_handler = std::signal(SIGINT, sigint_handler);

    // Open a log operation for this run unless logging is disabled. Entries are
    // appended per file inside the same transaction as each DB change (plan 020).
    int64_t op_id = 0;
    const bool logging = opts.log_enabled;
    if (logging) {
        op_id = db.log_begin_operation(opts.is_move ? "mv" : "cp", opts.argv_str, iso_utc_now());
    }

    size_t done = 0, errs = 0;
    for (const auto& a : actions) {
        if (g_interrupted) break;
        if (a.skipped) continue;

        try {
            // Disk op + DB update + optional log entry, committed together with
            // on-failure compensation so disk and DB never disagree (plan 020).
            execute_organize_action(db, a.source, a.dest.string(), opts.is_move,
                                    logging ? op_id : 0);
            done++;

            carry_sidecars(db, a.source, a.dest.string(), opts.is_move, logging ? op_id : 0);

            std::cout << "\r[" << done << "/" << to_process << "] " << std::flush;
        } catch (const std::exception& e) {
            std::cerr << "\nError: " << a.source << " → " << a.dest << ": " << e.what() << "\n";
            errs++;
        }
    }

    std::signal(SIGINT, prev_handler);

    if (logging) {
        db.log_finish_operation(op_id, g_interrupted ? "interrupted" : "completed");
        db.log_prune(opts.log_max_entries);
    }

    std::cout << "\n\n";
    if (g_interrupted) std::cout << "Interrupted. Stopped at a file boundary.\n";
    std::cout << "Done.\n"
              << "  Done: " << done << "\n"
              << "  Skipped: " << to_skip << "\n"
              << "  Errors: " << errs << "\n";

    return 0;
}

// ---- Remove command ----

int cmd_trash_empty(bool assume_yes) {
    std::string dir = trash_dir();
    if (!fs::exists(dir)) {
        std::cout << "Trash is empty.\n";
        return 0;
    }
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        n++;
    }
    if (n == 0) {
        std::cout << "Trash is empty.\n";
        return 0;
    }
    if (!assume_yes) {
        std::cout << "This will PERMANENTLY delete " << n << " item(s) from " << dir << ".\n";
        if (!::isatty(fileno(stdin))) {
            std::cerr << "Refusing without confirmation. Re-run with --yes.\n";
            return 1;
        }
        std::cout << "Type 'yes' to proceed: " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "yes") {
            std::cout << "Aborted.\n";
            return 1;
        }
    }
    for (const auto& e : fs::directory_iterator(dir)) fs::remove_all(e.path());
    std::cout << "Emptied trash (" << n << " item(s)).\n";
    return 0;
}

// ---- Search command ----

struct SearchOptions {
    std::string extension;
    std::string after;
    std::string before;
    std::string camera;
    std::string make;
    int64_t min_size = -1;
    int64_t max_size = -1;
    std::string media;  // "image", "video", or "" (all)
    int64_t min_duration_ms = -1;
    int64_t max_duration_ms = -1;
    std::string similar;       // image path for phash similarity
    std::string similar_hash;  // hex phash string
    int threshold = 5;
    std::string face;               // image path for face similarity search
    float face_threshold = 0.6;     // dlib recommended threshold
    std::string person;             // search by named person
    std::vector<std::string> tags;  // must have ALL these tags
    std::string gallery;            // exact gallery match
    double min_sharpness = -1;      // blur_score >= this
    double max_sharpness = -1;      // blur_score <= this
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    int limit = -1;
    bool count_only = false;
    bool porcelain = false;
    bool print0 = false;
    std::string format = "text";
    std::string output;
    std::string db_path;
};

namespace {

std::string format_size(int64_t bytes) {
    if (bytes >= static_cast<int64_t>(1024 * 1024)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0 * 1024.0));
        return buf;
    } else if (bytes >= 1024) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fKB", bytes / 1024.0);
        return buf;
    }
    return std::to_string(bytes) + "B";
}

int64_t parse_size(const std::string& s) {
    if (s.empty()) throw std::runtime_error("Empty size value");

    // Find where the digits end
    size_t i = 0;
    while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.')) i++;

    double num = std::stod(s.substr(0, i));
    std::string suffix = s.substr(i);

    // Lowercase the suffix
    for (auto& c : suffix) c = std::tolower(c);

    if (suffix.empty() || suffix == "b") return static_cast<int64_t>(num);
    if (suffix == "k" || suffix == "kb") return static_cast<int64_t>(num * 1024);
    if (suffix == "m" || suffix == "mb") return static_cast<int64_t>(num * 1024 * 1024);
    if (suffix == "g" || suffix == "gb") return static_cast<int64_t>(num * 1024 * 1024 * 1024);

    throw std::runtime_error("Unknown size suffix: " + suffix + " (use B, KB, MB, GB)");
}

// Parse a duration string like "10s", "5m", "1h", "1.5m" into milliseconds
int64_t parse_duration(const std::string& s) {
    if (s.empty()) throw std::runtime_error("Empty duration value");

    size_t i = 0;
    while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.')) i++;

    double num = std::stod(s.substr(0, i));
    std::string suffix = s.substr(i);
    for (auto& c : suffix) c = std::tolower(c);

    if (suffix.empty() || suffix == "ms") return static_cast<int64_t>(num);
    if (suffix == "s") return static_cast<int64_t>(num * 1000);
    if (suffix == "m") return static_cast<int64_t>(num * 60 * 1000);
    if (suffix == "h") return static_cast<int64_t>(num * 3600 * 1000);

    throw std::runtime_error("Unknown duration suffix: " + suffix + " (use ms, s, m, h)");
}

// Read stdin to a temp file, return the path. Caller should delete when done.
std::string read_stdin_to_temp() {
    auto temp_path = fs::temp_directory_path() / "phig-stdin-image";
    std::ofstream out(temp_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot create temp file: " + temp_path.string());
    }
    out << std::cin.rdbuf();
    out.close();
    if (fs::file_size(temp_path) == 0) {
        fs::remove(temp_path);
        throw std::runtime_error("No data received on stdin");
    }
    return temp_path.string();
}

uint64_t parse_hex_phash(const std::string& hex) {
    uint64_t val = 0;
    for (char c : hex) {
        val <<= 4;
        if (c >= '0' && c <= '9')
            val |= (c - '0');
        else if (c >= 'a' && c <= 'f')
            val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val |= (c - 'A' + 10);
        else
            throw std::runtime_error("Invalid hex character in phash: " + hex);
    }
    return val;
}

}  // anonymous namespace

int cmd_search(const SearchOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    std::vector<ImageInfo> results;
    bool is_similar = !opts.similar.empty() || !opts.similar_hash.empty();
    bool is_face_search = !opts.face.empty();
    bool is_person_search = !opts.person.empty();

    // Handle stdin ("-") for --face and --similar
    std::string temp_file;
    std::string face_path = opts.face;
    std::string similar_path = opts.similar;
    if (is_face_search && face_path == "-") {
        temp_file = read_stdin_to_temp();
        face_path = temp_file;
    } else if (is_similar && similar_path == "-") {
        temp_file = read_stdin_to_temp();
        similar_path = temp_file;
    }

    // Clean up temp file on exit
    struct TempCleanup {
        std::string path;
        ~TempCleanup() {
            if (!path.empty()) fs::remove(path);
        }
    } cleanup{temp_file};

    if (is_person_search) {
        // Search by named person
        auto paths = db.get_images_for_person(opts.person);
        if (opts.count_only) {
            std::cout << paths.size() << "\n";
            return 0;
        }
        if (paths.empty()) {
            std::cout << "No images found for \"" << opts.person << "\"\n";
            return 0;
        }

        // Output
        std::ofstream file_out;
        if (!opts.output.empty()) {
            file_out.open(opts.output);
            if (!file_out) {
                std::cerr << "Error: cannot open output file\n";
                return 1;
            }
        }
        std::ostream& out = opts.output.empty() ? std::cout : file_out;

        if (opts.print0) {
            for (const auto& p : paths) out << p << '\0';
        } else if (opts.porcelain) {
            for (const auto& p : paths) out << p << '\n';
        } else {
            for (const auto& p : paths) out << p << "\n";
            out << "\nFound " << paths.size() << " images of \"" << opts.person << "\"\n";
        }
        return 0;
    } else if (is_face_search) {
        // Face similarity search — decode with OpenCV, detect with dlib
        preload_face_models();
        cv::Mat query_img = decode_image(face_path);
        if (query_img.empty()) {
            log_error("Cannot decode image: " + face_path);
            return 1;
        }
        auto faces = detect_faces(query_img);
        if (faces.empty()) {
            std::cerr << "No faces detected in: " << face_path << "\n";
            return 1;
        }

        std::cerr << "Detected " << faces.size() << " face(s), searching all...\n";

        // Search for ALL faces found in the query image
        std::vector<Database::FaceSearchResult> face_results;
        std::set<std::string> seen_paths;
        int per_face_limit = opts.limit > 0 ? opts.limit : 100;

        for (const auto& face : faces) {
            auto matches = db.search_faces(face.embedding, opts.face_threshold, per_face_limit);
            for (auto& m : matches) {
                // Deduplicate by path — keep the closest match per image
                if (seen_paths.insert(m.path).second) {
                    face_results.push_back(std::move(m));
                }
            }
        }

        // Sort by distance
        std::sort(face_results.begin(), face_results.end(),
                  [](const Database::FaceSearchResult& a, const Database::FaceSearchResult& b) {
                      return a.distance < b.distance;
                  });

        if (opts.limit > 0 && static_cast<int>(face_results.size()) > opts.limit) {
            face_results.resize(opts.limit);
        }

        if (opts.count_only) {
            std::cout << face_results.size() << "\n";
            return 0;
        }

        // Convert to output — face search has its own output since it includes distance
        std::ofstream file_out;
        if (!opts.output.empty()) {
            file_out.open(opts.output);
            if (!file_out) {
                std::cerr << "Error: cannot open output file: " << opts.output << "\n";
                return 1;
            }
        }
        std::ostream& out = opts.output.empty() ? std::cout : file_out;

        if (opts.print0) {
            for (const auto& r : face_results) {
                out << r.path << '\0';
            }
        } else if (opts.porcelain) {
            for (const auto& r : face_results) {
                out << r.path << '\t' << r.distance << '\n';
            }
        } else if (opts.format == "json") {
            out << "{\"results\":[";
            for (size_t i = 0; i < face_results.size(); i++) {
                if (i > 0) out << ",";
                out << "{\"path\":\"" << face_results[i].path << "\""
                    << ",\"distance\":" << face_results[i].distance
                    << ",\"face_x\":" << face_results[i].face_x
                    << ",\"face_y\":" << face_results[i].face_y
                    << ",\"face_w\":" << face_results[i].face_w
                    << ",\"face_h\":" << face_results[i].face_h << "}";
            }
            out << "],\"count\":" << face_results.size() << "}\n";
        } else {
            for (const auto& r : face_results) {
                out << r.path << "\n"
                    << "  distance: " << r.distance << "  face: " << r.face_x << "," << r.face_y
                    << " " << r.face_w << "x" << r.face_h << "\n\n";
            }
            out << "Found " << face_results.size() << " matching faces\n";
        }
        return 0;
    } else if (is_similar) {
        // Compute or parse the query phash
        uint64_t query_phash;
        if (!similar_path.empty()) {
            auto phash_result = compute_phash(similar_path);
            query_phash = phash_result.hash;
        } else {
            query_phash = parse_hex_phash(opts.similar_hash);
        }

        // Get all images (within path prefix) and compare
        auto all_images = db.get_all_images();

        // Compute distances and filter
        struct ScoredImage {
            ImageInfo info;
            int distance;
        };
        std::vector<ScoredImage> scored;

        for (auto& img : all_images) {
            int dist = std::popcount(query_phash ^ img.phash);
            if (dist <= opts.threshold) {
                scored.push_back({std::move(img), dist});
            }
        }

        // Sort by distance (closest first)
        std::sort(scored.begin(), scored.end(), [](const ScoredImage& a, const ScoredImage& b) {
            return a.distance < b.distance;
        });

        for (auto& s : scored) {
            results.push_back(std::move(s.info));
        }
    } else {
        // Build search criteria
        Database::SearchCriteria criteria;
        criteria.extension = opts.extension;
        criteria.after = opts.after;
        criteria.before = opts.before;
        criteria.camera = opts.camera;
        criteria.make = opts.make;
        criteria.min_size = opts.min_size;
        criteria.max_size = opts.max_size;
        criteria.media = opts.media;
        criteria.min_duration_ms = opts.min_duration_ms;
        criteria.max_duration_ms = opts.max_duration_ms;
        criteria.tags = opts.tags;
        criteria.gallery = opts.gallery;
        criteria.min_sharpness = opts.min_sharpness;
        criteria.max_sharpness = opts.max_sharpness;
        // Don't apply limit in DB if we also need match/filter (apply after)
        if (opts.matches.empty() && opts.excludes.empty()) {
            criteria.limit = opts.limit;
        }
        results = db.search(criteria);
    }

    // Apply match/filter
    if (!opts.matches.empty() || !opts.excludes.empty()) {
        results.erase(std::remove_if(results.begin(), results.end(),
                                     [&](const ImageInfo& img) {
                                         return !passes_selection(img.path, opts.matches,
                                                                opts.excludes);
                                     }),
                      results.end());
    }

    // Apply limit (if not already applied in DB)
    if (opts.limit > 0 && static_cast<int>(results.size()) > opts.limit) {
        results.resize(opts.limit);
    }

    if (opts.count_only) {
        std::cout << results.size() << "\n";
        return 0;
    }

    // Output
    std::ofstream file_out;
    if (!opts.output.empty()) {
        file_out.open(opts.output);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << opts.output << "\n";
            return 1;
        }
    }
    std::ostream& out = opts.output.empty() ? std::cout : file_out;

    if (opts.print0) {
        for (const auto& img : results) {
            out << img.path << '\0';
        }
    } else if (opts.porcelain) {
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            std::string tpath = img.sha256.size() >= 2 ? get_thumb_path(img.sha256) : "";
            out << img.path << '\t' << img.width << '\t' << img.height << '\t' << img.file_size
                << '\t' << date << '\t' << camera << '\t' << tpath << '\n';
        }
    } else if (opts.format == "csv") {
        out << "path,filename,width,height,file_size,date,camera,sha256,phash,thumb_path\n";
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            std::string tpath = img.sha256.size() >= 2 ? get_thumb_path(img.sha256) : "";
            out << "\"" << img.path << "\"," << img.filename << "," << img.width << ","
                << img.height << "," << img.file_size << "," << date << ","
                << "\"" << camera << "\"," << img.sha256 << "," << img.phash << ","
                << "\"" << tpath << "\"\n";
        }
    } else if (opts.format == "json") {
        out << "{\"results\":[";
        for (size_t i = 0; i < results.size(); i++) {
            if (i > 0) out << ",";
            const auto& img = results[i];
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            std::string tpath = img.sha256.size() >= 2 ? get_thumb_path(img.sha256) : "";
            out << "{\"path\":\"" << img.path << "\""
                << ",\"filename\":\"" << img.filename << "\""
                << ",\"width\":" << img.width << ",\"height\":" << img.height
                << ",\"file_size\":" << img.file_size << ",\"date\":\"" << date << "\""
                << ",\"camera\":\"" << camera << "\""
                << ",\"sha256\":\"" << img.sha256 << "\""
                << ",\"phash\":" << img.phash << ",\"thumb_path\":\"" << tpath << "\""
                << "}";
        }
        out << "],\"count\":" << results.size() << "}\n";
    } else {
        // Human-readable
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            out << img.path << "\n"
                << "  " << img.width << "x" << img.height << "  " << format_size(img.file_size)
                << "  " << date;
            if (!camera.empty()) out << "  " << camera;
            out << "\n\n";
        }
        out << "Found " << results.size() << " images\n";
    }

    return 0;
}

// ---- Thumbs command ----

int cmd_thumbs(int argc, char* argv[], const std::string& db_path, int default_parallel) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig thumbs rebuild [flags]   Generate missing thumbnails\n"
                  << "  phig thumbs clean [--dry-run] Remove orphaned thumbnails\n";
        return 1;
    }

    std::string subcmd = argv[2];

    if (subcmd == "rebuild") {
        std::vector<std::string> matches, excludes;
        bool force = false;
        int parallel = default_parallel;

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--force") {
                force = true;
            } else if (arg == "--parallel" && i + 1 < argc) {
                parallel = std::stoi(argv[++i]);
            } else if (arg == "--db") {
                i++;  // already handled
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        auto start_time = std::chrono::steady_clock::now();

        Database db(db_path);
        db.init_schema();

        if (db.count() == 0) {
            std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
            return 1;
        }

        auto images = db.get_all_images();

        if (!matches.empty() || !excludes.empty()) {
            images.erase(std::remove_if(images.begin(), images.end(),
                                        [&](const ImageInfo& img) {
                                            return !passes_selection(img.path, matches, excludes);
                                        }),
                         images.end());
        }

        std::cout << "Generating thumbnails for " << images.size() << " images...\n";

        int num_threads;
        if (parallel > 0) {
            num_threads = parallel;
        } else {
            num_threads = std::max(1u, std::thread::hardware_concurrency());
        }

        g_interrupted = 0;
        auto prev_handler = std::signal(SIGINT, sigint_handler);

        std::atomic<size_t> next_img{0};
        std::atomic<size_t> generated{0};
        std::atomic<size_t> skipped{0};
        std::atomic<size_t> errors{0};

        auto worker = [&](int) {
            while (!g_interrupted) {
                size_t idx = next_img.fetch_add(1);
                if (idx >= images.size()) break;

                const auto& img = images[idx];
                if (img.sha256.empty()) {
                    skipped.fetch_add(1);
                    continue;
                }

                if (!force && thumb_exists(img.sha256)) {
                    skipped.fetch_add(1);
                    size_t done = generated.load() + skipped.load() + errors.load();
                    std::cout << "\r[" << done << "/" << images.size() << "] " << std::flush;
                    continue;
                }

                try {
                    bool ok = false;
                    if (img.media_type == "video") {
                        // Extract keyframe for video thumbnail
                        auto kf = extract_video_keyframe(img.path);
                        if (!kf.frame.empty()) {
                            ok = generate_thumb(kf.frame, img.sha256);
                        }
                    } else {
                        ok = generate_thumb(img.path, img.sha256);
                    }
                    if (ok) {
                        generated.fetch_add(1);
                    } else {
                        errors.fetch_add(1);
                    }
                } catch (...) {
                    errors.fetch_add(1);
                }

                size_t done = generated.load() + skipped.load() + errors.load();
                fs::path p(img.path);
                std::string display = (p.parent_path().filename() / p.filename()).string();
                std::cout << "\r\033[K[" << done << "/" << images.size() << "] " << display
                          << std::flush;
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back(worker, i);
        }
        for (auto& t : threads) t.join();

        std::signal(SIGINT, prev_handler);

        if (g_interrupted) {
            std::cout << "\n\nInterrupted. Progress saved.\n";
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;

        std::cout << "\n\nDone in " << secs << "s\n"
                  << "  Generated: " << generated.load() << "\n"
                  << "  Skipped:   " << skipped.load() << "\n"
                  << "  Errors:    " << errors.load() << "\n";

        return 0;

    } else if (subcmd == "clean") {
        bool dry_run = false;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--dry-run") {
                dry_run = true;
            } else if (arg == "--db") {
                i++;  // already handled
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        Database db(db_path);
        db.init_schema();

        auto known_hashes = db.get_all_sha256s();
        std::string thumb_cache = thumbs_dir();

        if (!fs::exists(thumb_cache)) {
            std::cout << "No thumbnail cache directory found.\n";
            return 0;
        }

        size_t removed = 0;
        size_t kept = 0;
        size_t skipped_foreign = 0;  // files that don't match the thumbnail contract

        for (auto& entry : fs::recursive_directory_iterator(thumb_cache)) {
            if (!entry.is_regular_file()) continue;

            // Only ever touch files that match phig's thumbnail naming contract
            // (<64-hex-sha256>.jpg). Anything else in the cache dir — user files,
            // other apps' data if the dir is misconfigured — is left untouched.
            std::string name = entry.path().filename().string();
            if (!is_thumbnail_filename(name)) {
                skipped_foreign++;
                continue;
            }

            std::string stem = entry.path().stem().string();
            if (known_hashes.count(stem)) {
                kept++;
            } else if (dry_run) {
                std::cout << "  would remove " << entry.path().string() << "\n";
                removed++;
            } else {
                fs::remove(entry.path());
                removed++;
            }
        }

        // Remove empty subdirectories (skip in dry-run)
        if (!dry_run) {
            for (auto& entry : fs::directory_iterator(thumb_cache)) {
                if (entry.is_directory() && fs::is_empty(entry.path())) {
                    fs::remove(entry.path());
                }
            }
        }

        std::cout << (dry_run ? "Would remove " : "Removed ") << removed << " orphaned thumbnails\n"
                  << "Kept " << kept << " thumbnails\n";
        if (skipped_foreign > 0) {
            std::cout << "Left untouched " << skipped_foreign
                      << " non-thumbnail files in the cache directory\n";
        }

        return 0;

    } else {
        std::cerr << "Unknown thumbs subcommand: " << subcmd << "\n"
                  << "Usage:\n"
                  << "  phig thumbs rebuild [flags]   Generate missing thumbnails\n"
                  << "  phig thumbs clean [--dry-run] Remove orphaned thumbnails\n";
        return 1;
    }
}

// ---- Face command ----

int cmd_face(int argc, char* argv[], const std::string& db_path, int default_parallel) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig face name <image> <name>     Name a face from a single-face photo\n"
                  << "  phig face identify <name>          Label all matching faces in DB\n"
                  << "  phig face rebuild [flags]          Re-detect all faces from scratch\n"
                  << "  phig face list                     List known people\n";
        return 1;
    }

    std::string subcmd = argv[2];
    Database db(db_path);
    db.init_schema();

    if (subcmd == "name") {
        if (argc < 5) {
            std::cerr << "Usage: phig face name <image> <name>\n";
            return 1;
        }
        std::string image_path = argv[3];
        std::string name = argv[4];

        // Detect faces in the reference image
        preload_face_models();
        cv::Mat img = decode_image(image_path);
        if (img.empty()) {
            log_error("Cannot decode image: " + image_path);
            return 1;
        }
        auto faces = detect_faces(img);

        if (faces.empty()) {
            std::cerr << "No faces detected in: " << image_path << "\n";
            return 1;
        }
        if (faces.size() > 1) {
            std::cerr << "Error: Multiple faces detected (" << faces.size()
                      << "). Please use a photo with a single face.\n";
            return 1;
        }

        // Find the closest matching face in the DB
        int64_t face_id = db.find_closest_face(faces[0].embedding, 0.6);
        if (face_id < 0) {
            // No matching face in DB — scan this image first
            std::cerr << "No matching face found in the database.\n"
                      << "Make sure the image (or similar photos) have been scanned with --faces "
                         "first.\n";
            return 1;
        }

        // Create or get person, assign to face
        int64_t person_id = db.get_or_create_person(name);
        db.set_face_person(face_id, person_id);

        std::cout << "Named face as \"" << name << "\"\n";
        return 0;

    } else if (subcmd == "identify") {
        if (argc < 4) {
            std::cerr << "Usage: phig face identify <name>\n";
            return 1;
        }
        std::string name = argv[3];

        // Get the person's reference embeddings (all faces already named as this person)
        auto people = db.list_people();
        int64_t person_id = -1;
        for (const auto& p : people) {
            if (p.name == name) {
                person_id = p.id;
                break;
            }
        }
        if (person_id < 0) {
            std::cerr << "Unknown person: " << name << "\n"
                      << "Use 'phig face name <image> <name>' first.\n";
            return 1;
        }

        // Get all face embeddings for this person (as reference)
        // Then search the entire DB for matching untagged faces
        auto all_images = db.get_all_images();
        std::cout << "Searching " << all_images.size() << " images for \"" << name << "\"...\n";

        // Get reference embeddings from already-named faces
        // We use the vec_faces table to find all faces near the named ones
        auto named_paths = db.get_images_for_person(name);
        if (named_paths.empty()) {
            std::cerr << "No reference faces found for \"" << name << "\".\n"
                      << "Use 'phig face name <image> <name>' to provide a reference.\n";
            return 1;
        }

        // Get embeddings of named faces
        // For each named face, search for similar untagged faces
        const char* ref_sql = R"(
            SELECT f.id, f.embedding FROM faces f
            WHERE f.person_id = ?
        )";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db.db_ptr(), ref_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Error querying reference faces\n";
            return 1;
        }
        sqlite3_bind_int64(stmt, 1, person_id);

        std::vector<std::array<float, 128>> ref_embeddings;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::array<float, 128> emb;
            const float* data = static_cast<const float*>(sqlite3_column_blob(stmt, 1));
            if (data) {
                std::copy(data, data + 128, emb.begin());
                ref_embeddings.push_back(emb);
            }
        }
        sqlite3_finalize(stmt);

        // Scan all untagged faces and check each against the reference embeddings.
        size_t labeled = 0;
        const char* untagged_sql = "SELECT id, embedding FROM faces WHERE person_id IS NULL";
        rc = sqlite3_prepare_v2(db.db_ptr(), untagged_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Error querying faces\n";
            return 1;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t face_id = sqlite3_column_int64(stmt, 0);
            const float* data = static_cast<const float*>(sqlite3_column_blob(stmt, 1));
            if (!data) continue;

            std::array<float, 128> emb;
            std::copy(data, data + 128, emb.begin());

            // Check against all reference embeddings
            for (const auto& ref_emb : ref_embeddings) {
                float dist = embedding_distance(ref_emb, emb);
                if (dist <= 0.6) {
                    db.set_face_person(face_id, person_id);
                    labeled++;
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);

        std::cout << "Labeled " << labeled << " additional faces as \"" << name << "\"\n";
        return 0;

    } else if (subcmd == "rebuild") {
        // Parse match/filter flags
        std::vector<std::string> matches, excludes;
        int parallel = default_parallel;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--parallel" && i + 1 < argc) {
                parallel = std::stoi(argv[++i]);
            } else if (arg == "--db") {
                i++;  // already handled
            }
        }

        auto all_images = db.get_all_images();

        // Apply match/filter
        std::vector<ImageInfo> images;
        for (const auto& img : all_images) {
            if (passes_selection(img.path, matches, excludes)) {
                images.push_back(img);
            }
        }

        if (images.empty()) {
            std::cout << "No matching images found.\n";
            return 0;
        }

        std::cout << "Rebuilding face data for " << images.size() << " images...\n";

        // Determine thread count
        int num_threads;
        if (parallel > 0) {
            num_threads = parallel;
        } else {
            num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
            num_threads = std::max(num_threads, 1);
        }

        // Create per-thread detectors
        std::cout << "Loading face recognition models (" << num_threads << " instances)...\n";
        std::vector<std::unique_ptr<FaceDetector>> detectors;
        detectors.reserve(num_threads);
        for (int i = 0; i < num_threads; i++) {
            detectors.push_back(std::make_unique<FaceDetector>());
        }

        std::atomic<size_t> next_img{0};
        std::atomic<size_t> total_faces{0};
        std::mutex db_mutex;

        auto worker = [&](int thread_id) {
            while (!g_interrupted) {
                size_t idx = next_img.fetch_add(1);
                if (idx >= images.size()) break;

                const auto& img_info = images[idx];
                cv::Mat img = decode_image(img_info.path);
                if (img.empty()) continue;

                auto faces = detectors[thread_id]->detect(img);

                {
                    std::lock_guard<std::mutex> lock(db_mutex);
                    int64_t image_id = db.get_image_id(img_info.path);
                    if (image_id >= 0) {
                        // Delete-then-reinsert must be atomic: if the delete ran
                        // outside the transaction and we were interrupted before
                        // commit, the old faces would be gone with no re-detection.
                        db.begin_transaction();
                        try {
                            db.delete_faces_for_image(image_id);
                            for (const auto& face : faces) {
                                db.insert_face(image_id, face);
                                int64_t face_id = sqlite3_last_insert_rowid(db.db_ptr());
                                db.auto_label_face(face_id, face.embedding);
                            }
                            db.commit_transaction();
                            total_faces.fetch_add(faces.size());
                        } catch (...) {
                            db.rollback_transaction();
                            throw;
                        }
                    }
                }

                fs::path p(img_info.path);
                std::string display = (p.parent_path().filename() / p.filename()).string();
                std::cout << "\r\033[K[" << (idx + 1) << "/" << images.size() << "] " << display
                          << std::flush;
            }
        };

        g_interrupted = 0;
        auto prev_handler = std::signal(SIGINT, sigint_handler);

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back(worker, i);
        }
        for (auto& t : threads) t.join();

        std::signal(SIGINT, prev_handler);

        if (g_interrupted) {
            std::cout << "\n\nInterrupted. Progress saved.\n";
        }

        std::cout << "\n\nDone. " << total_faces.load() << " faces detected.\n";
        return 0;

    } else if (subcmd == "list") {
        auto people = db.list_people();
        if (people.empty()) {
            std::cout << "No named people.\n";
            return 0;
        }
        for (const auto& p : people) {
            std::cout << p.name << " (" << p.face_count << " faces)\n";
        }
        return 0;

    } else {
        std::cerr << "Unknown face subcommand: " << subcmd << "\n";
        return 1;
    }
}

// ---- Main ----

// Resolve the images selected by an optional --path prefix plus match/filter
// globs into a list of paths, reusing the shared filter logic.
static std::vector<std::string> select_paths(Database& db, const std::string& path_prefix,
                                             const std::vector<std::string>& matches,
                                             const std::vector<std::string>& excludes) {
    std::vector<std::string> paths;
    for (const auto& img : db.get_all_images(path_prefix)) {
        if (passes_selection(img.path, matches, excludes)) {
            paths.push_back(img.path);
        }
    }
    return paths;
}

int cmd_tag(int argc, char* argv[], const std::string& db_path) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig tag add <tag> [--match|--exclude|--path]\n"
                  << "  phig tag remove <tag> [--match|--exclude|--path]\n"
                  << "  phig tag list\n";
        return 1;
    }
    std::string subcmd = argv[2];

    if (subcmd == "list") {
        Database db(db_path);
        db.init_schema();
        for (const auto& [name, count] : db.get_tag_counts()) {
            std::cout << name << " (" << count << ")\n";
        }
        return 0;
    }

    if (subcmd == "add" || subcmd == "remove") {
        if (argc < 4 || argv[3][0] == '-') {
            std::cerr << "Error: tag " << subcmd << " requires a <tag> argument\n";
            return 1;
        }
        std::string tag = argv[3];
        std::string path_prefix;
        std::vector<std::string> matches, excludes;
        for (int i = 4; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--path" && i + 1 < argc) {
                path_prefix = expand_tilde(argv[++i]);
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        if (matches.empty() && path_prefix.empty()) {
            std::cerr << "Error: tag " << subcmd << " requires --match or --path\n";
            return 1;
        }
        Database db(db_path);
        db.init_schema();
        int n = db.set_tag(select_paths(db, path_prefix, matches, excludes), tag, subcmd == "add");
        std::cout << (subcmd == "add" ? "Tagged " : "Untagged ") << n << " images (" << tag
                  << ")\n";
        return 0;
    }

    std::cerr << "Unknown tag subcommand: " << subcmd << "\n";
    return 1;
}

int cmd_gallery(int argc, char* argv[], const std::string& db_path) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig gallery set <name> [--match|--exclude|--path] [--no-overwrite]\n"
                  << "  phig gallery clear [--match|--exclude|--path]\n"
                  << "  phig gallery list\n";
        return 1;
    }
    std::string subcmd = argv[2];

    if (subcmd == "list") {
        Database db(db_path);
        db.init_schema();
        for (const auto& [name, count] : db.get_gallery_counts()) {
            std::cout << name << " (" << count << ")\n";
        }
        return 0;
    }

    if (subcmd == "set" || subcmd == "clear") {
        bool is_set = subcmd == "set";
        std::string gallery;
        int first_flag = 3;
        if (is_set) {
            if (argc < 4 || argv[3][0] == '-') {
                std::cerr << "Error: gallery set requires a <name> argument\n";
                return 1;
            }
            gallery = argv[3];
            first_flag = 4;
        }
        std::string path_prefix;
        std::vector<std::string> matches, excludes;
        bool no_overwrite = false;
        for (int i = first_flag; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--path" && i + 1 < argc) {
                path_prefix = expand_tilde(argv[++i]);
            } else if (arg == "--no-overwrite" && is_set) {
                no_overwrite = true;
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        if (matches.empty() && path_prefix.empty()) {
            std::cerr << "Error: gallery " << subcmd << " requires --match or --path\n";
            return 1;
        }
        Database db(db_path);
        db.init_schema();
        auto paths = select_paths(db, path_prefix, matches, excludes);
        if (is_set) {
            int n = db.set_gallery(paths, gallery, no_overwrite);
            std::cout << "Set gallery '" << gallery << "' on " << n << " images\n";
        } else {
            int n = db.clear_gallery(paths);
            std::cout << "Cleared gallery on " << n << " images\n";
        }
        return 0;
    }

    std::cerr << "Unknown gallery subcommand: " << subcmd << "\n";
    return 1;
}

// Favorites are the built-in `favorite` tag; these commands are ergonomic sugar.
int cmd_fav(int argc, char* argv[], const std::string& db_path) {
    if (argc < 3) {
        std::cerr
            << "Usage:\n"
            << "  phig fav add [--match|--exclude|--path]\n"
            << "  phig fav remove [--match|--exclude|--path]\n"
            << "  phig fav list [--match|--exclude] [--format|--porcelain|--print0|--output]\n";
        return 1;
    }
    std::string subcmd = argv[2];

    if (subcmd == "add" || subcmd == "remove") {
        std::string path_prefix;
        std::vector<std::string> matches, excludes;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--path" && i + 1 < argc) {
                path_prefix = expand_tilde(argv[++i]);
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        if (matches.empty() && path_prefix.empty()) {
            std::cerr << "Error: fav " << subcmd << " requires --match or --path\n";
            return 1;
        }
        Database db(db_path);
        db.init_schema();
        int n = db.set_tag(select_paths(db, path_prefix, matches, excludes), "favorite",
                           subcmd == "add");
        std::cout << (subcmd == "add" ? "Favorited " : "Unfavorited ") << n << " images\n";
        return 0;
    }

    if (subcmd == "list") {
        // Delegate to search on the favorite tag so all output modes are reused.
        SearchOptions opts;
        opts.db_path = db_path;
        opts.tags.push_back("favorite");
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--limit" && i + 1 < argc) {
                opts.limit = std::stoi(argv[++i]);
            } else if (arg == "--count") {
                opts.count_only = true;
            } else if (arg == "--porcelain") {
                opts.porcelain = true;
            } else if (arg == "--print0") {
                opts.print0 = true;
            } else if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
            } else if (arg == "--output" && i + 1 < argc) {
                opts.output = argv[++i];
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        return cmd_search(opts);
    }

    std::cerr << "Unknown fav subcommand: " << subcmd << "\n";
    return 1;
}

// Print the effective configuration (resolved paths + any config overrides),
// shown at the top of --help output.
static void print_active_config(const Config& cfg, const std::string& cfg_path, bool loaded) {
    std::cout << "Active configuration:\n";
    std::cout << "  config file:  " << cfg_path << (loaded ? "  (loaded)" : "  (not found)")
              << "\n";
    std::cout << "  db:           " << default_db_path() << (cfg.db ? "  [config]" : "  (default)")
              << "\n";
    std::cout << "  models_dir:   " << models_dir()
              << (cfg.models_dir ? "  [config]" : "  (default)") << "\n";
    std::cout << "  thumbs_dir:   " << thumbs_dir()
              << (cfg.thumbs_dir ? "  [config]" : "  (default)") << "\n";

    // Non-path values are listed only when set in the config file.
    auto line = [](const char* k, const std::string& v) {
        std::cout << "  " << k << " = " << v << "  [config]\n";
    };
    if (cfg.parallel) line("general.parallel", std::to_string(*cfg.parallel));
    if (cfg.scan_recursive) line("scan.recursive", *cfg.scan_recursive ? "true" : "false");
    if (cfg.scan_on_error) line("scan.on_error", *cfg.scan_on_error);
    if (cfg.scan_media) line("scan.media", *cfg.scan_media);
    if (cfg.scan_thumbs) line("scan.thumbs", *cfg.scan_thumbs ? "true" : "false");
    if (cfg.scan_faces) line("scan.faces", *cfg.scan_faces ? "true" : "false");
    if (cfg.organize_format) line("organize.format", *cfg.organize_format);
    if (cfg.duplicates_type) line("duplicates.type", *cfg.duplicates_type);
    if (cfg.duplicates_threshold)
        line("duplicates.threshold", std::to_string(*cfg.duplicates_threshold));
    if (cfg.faces_threshold) line("faces.threshold", std::to_string(*cfg.faces_threshold));
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    // Extract an optional global --config <path> from anywhere in the args, then
    // continue with it removed so the per-command parsers don't see it.
    std::string config_file;
    bool have_config_file = false;
    std::vector<char*> filtered_args;
    filtered_args.push_back(argv[0]);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_file = argv[++i];
            have_config_file = true;
        } else {
            filtered_args.push_back(argv[i]);
        }
    }
    argv = filtered_args.data();
    argc = static_cast<int>(filtered_args.size());

    // --version works regardless of config state.
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "phig " << PHIG_VERSION << "\n";
            return 0;
        }
    }

    // Load configuration (missing file -> defaults). Install path overrides so
    // default_db_path()/models_dir()/thumbs_dir() reflect the config everywhere.
    std::string resolved_cfg_path = have_config_file ? config_file : config_path();
    Config cfg;
    try {
        cfg = load_config(resolved_cfg_path);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    set_path_overrides(cfg.db, cfg.models_dir, cfg.thumbs_dir);

    // Diagnostic logger (plan 024): stderr sink for warnings/errors plus a
    // rotating file sink at ~/.local/state/phig/phig.log unless disabled.
    // Init before any dispatch so every instrumented site has a live logger.
    log_init(default_log_path(), !cfg.log_file || *cfg.log_file,
             cfg.log_file_level ? log_level_from_string(*cfg.log_file_level) : LogLevel::Info);

    bool cfg_loaded = fs::exists(resolved_cfg_path);

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h" || command == "help") {
        print_active_config(cfg, resolved_cfg_path, cfg_loaded);
        print_usage();
        return 0;
    }

    // Check for --help on any command
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_active_config(cfg, resolved_cfg_path, cfg_loaded);
            print_usage();
            return 0;
        }
    }

    log_info(join_argv(argc, argv));  // file-only: the stderr sink is warn+

    if (command == "scan") {
        if (argc < 3) {
            std::cerr << "Error: scan requires a directory argument\n";
            print_usage();
            return 1;
        }

        ScanOptions opts;
        opts.directory = argv[2];
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.parallel) opts.parallel = *cfg.parallel;
        if (cfg.scan_recursive) opts.recursive = *cfg.scan_recursive;
        if (cfg.scan_thumbs) opts.thumbs = *cfg.scan_thumbs;
        if (cfg.scan_faces) opts.faces = *cfg.scan_faces;
        if (cfg.scan_on_error) opts.fail_on_error = (*cfg.scan_on_error == "fail");
        if (cfg.scan_media) opts.media = (*cfg.scan_media == "all") ? "" : *cfg.scan_media;
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--recursive") {
                opts.recursive = true;
            } else if (arg == "--force") {
                // --force with optional value: --force or --force hash,thumbs
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    auto f = parse_force(argv[++i]);
                    opts.force.hash = opts.force.hash || f.hash;
                    opts.force.thumbs = opts.force.thumbs || f.thumbs;
                    opts.force.faces = opts.force.faces || f.faces;
                } else {
                    opts.force.set_all();
                }
            } else if (arg == "--faces") {
                opts.faces = true;
            } else if (arg == "--thumbs") {
                opts.thumbs = true;
            } else if (arg == "--ignore-mount-warning") {
                opts.ignore_mount_warning = true;
            } else if (arg == "--media" && i + 1 < argc) {
                opts.media = argv[++i];
                if (opts.media != "image" && opts.media != "video" && opts.media != "all") {
                    std::cerr << "Error: --media must be 'image', 'video', or 'all'\n";
                    return 1;
                }
                if (opts.media == "all") opts.media = "";
            } else if (arg == "--hidden") {
                opts.include_hidden = true;
            } else if (arg == "--parallel" && i + 1 < argc) {
                opts.parallel = std::stoi(argv[++i]);
            } else if (arg == "--on-error" && i + 1 < argc) {
                std::string val = argv[++i];
                if (val == "fail") {
                    opts.fail_on_error = true;
                } else if (val == "warn") {
                    opts.fail_on_error = false;
                } else {
                    std::cerr << "Error: --on-error must be 'warn' or 'fail'\n";
                    return 1;
                }
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_scan(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "duplicates") {
        DuplicatesOptions opts;
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.duplicates_type) opts.type = *cfg.duplicates_type;
        if (cfg.duplicates_threshold) opts.threshold = *cfg.duplicates_threshold;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--type" && i + 1 < argc) {
                opts.type = argv[++i];
                if (opts.type != "exact" && opts.type != "near" && opts.type != "all") {
                    std::cerr << "Error: --type must be 'exact', 'near', or 'all'\n";
                    return 1;
                }
            } else if (arg == "--threshold" && i + 1 < argc) {
                opts.threshold = std::stoi(argv[++i]);
            } else if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
                if (opts.format != "text" && opts.format != "csv" && opts.format != "json") {
                    std::cerr << "Error: --format must be 'text', 'csv', or 'json'\n";
                    return 1;
                }
            } else if (arg == "--output" && i + 1 < argc) {
                opts.output = argv[++i];
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--media" && i + 1 < argc) {
                opts.media = argv[++i];
                if (opts.media != "image" && opts.media != "video" && opts.media != "all") {
                    std::cerr << "Error: --media must be 'image', 'video', or 'all'\n";
                    return 1;
                }
                if (opts.media == "all") opts.media = "";
            } else if (arg == "--fav") {
                opts.fav = true;
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_duplicates(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "cp" || command == "mv") {
        if (argc < 3) {
            std::cerr << "Error: " << command << " requires a destination directory\n";
            print_usage();
            return 1;
        }

        OrganizeOptions opts;
        opts.destination = argv[2];
        opts.is_move = (command == "mv");
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.organize_format) opts.format = *cfg.organize_format;
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--on-conflict" && i + 1 < argc) {
                opts.on_conflict = argv[++i];
                if (opts.on_conflict != "skip" && opts.on_conflict != "overwrite" &&
                    opts.on_conflict != "rename") {
                    std::cerr << "Error: --on-conflict must be 'skip', 'overwrite', or 'rename'\n";
                    return 1;
                }
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_organize(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "rm") {
        RemoveOptions opts;
        opts.db_path = default_db_path();
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        int mode_flags = 0;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--hard") {
                opts.mode = RemoveMode::Hard;
                mode_flags++;
            } else if (arg == "--db-only") {
                opts.mode = RemoveMode::DbOnly;
                mode_flags++;
            } else if (arg == "--orphans") {
                opts.mode = RemoveMode::Orphans;
                mode_flags++;
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--yes" || arg == "-y") {
                opts.assume_yes = true;
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }
        if (mode_flags > 1) {
            std::cerr << "Error: --hard, --db-only, and --orphans are mutually exclusive\n";
            return 1;
        }
        try {
            return cmd_rm(opts);
        } catch (const std::exception& e) {
            log_error(std::string("rm: ") + e.what());
            return 1;
        }
    } else if (command == "trash") {
        // Only subcommand: `trash empty`.
        if (argc < 3 || std::string(argv[2]) != "empty") {
            std::cerr << "Usage: phig trash empty [--yes]\n";
            return 1;
        }
        bool yes = false;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--yes" || arg == "-y")
                yes = true;
            else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        try {
            return cmd_trash_empty(yes);
        } catch (const std::exception& e) {
            log_error(std::string("trash: ") + e.what());
            return 1;
        }
    } else if (command == "thumbs") {
        std::string thumbs_db = default_db_path();
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--db" && i + 1 < argc) {
                thumbs_db = argv[i + 1];
                break;
            }
        }
        try {
            return cmd_thumbs(argc, argv, thumbs_db, cfg.parallel.value_or(-1));
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "tag" || command == "gallery" || command == "fav") {
        std::string db_path = default_db_path();
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--db" && i + 1 < argc) {
                db_path = argv[i + 1];
                break;
            }
        }
        try {
            if (command == "tag") return cmd_tag(argc, argv, db_path);
            if (command == "gallery") return cmd_gallery(argc, argv, db_path);
            return cmd_fav(argc, argv, db_path);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "face") {
        std::string face_db = default_db_path();
        // Check for --db flag
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--db" && i + 1 < argc) {
                face_db = argv[i + 1];
                break;
            }
        }
        try {
            return cmd_face(argc, argv, face_db, cfg.parallel.value_or(-1));
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "models") {
        if (argc < 3 || std::string(argv[2]) != "download") {
            std::cerr << "Usage: phig models download\n";
            return 1;
        }
        try {
            if (download_face_models()) {
                std::cout << "All models downloaded successfully.\n";
                return 0;
            } else {
                std::cerr << "Model download failed.\n";
                return 1;
            }
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "search") {
        SearchOptions opts;
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.faces_threshold) opts.face_threshold = static_cast<float>(*cfg.faces_threshold);

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--ext" && i + 1 < argc) {
                opts.extension = argv[++i];
            } else if (arg == "--after" && i + 1 < argc) {
                opts.after = argv[++i];
            } else if (arg == "--before" && i + 1 < argc) {
                opts.before = argv[++i];
            } else if (arg == "--camera" && i + 1 < argc) {
                opts.camera = argv[++i];
            } else if (arg == "--make" && i + 1 < argc) {
                opts.make = argv[++i];
            } else if (arg == "--min-size" && i + 1 < argc) {
                opts.min_size = parse_size(argv[++i]);
            } else if (arg == "--max-size" && i + 1 < argc) {
                opts.max_size = parse_size(argv[++i]);
            } else if (arg == "--similar" && i + 1 < argc) {
                opts.similar = argv[++i];
            } else if (arg == "--similar-hash" && i + 1 < argc) {
                opts.similar_hash = argv[++i];
            } else if (arg == "--threshold" && i + 1 < argc) {
                opts.threshold = std::stoi(argv[++i]);
            } else if (arg == "--face" && i + 1 < argc) {
                opts.face = argv[++i];
            } else if (arg == "--face-threshold" && i + 1 < argc) {
                opts.face_threshold = std::stof(argv[++i]);
            } else if (arg == "--person" && i + 1 < argc) {
                opts.person = argv[++i];
            } else if (arg == "--tag" && i + 1 < argc) {
                opts.tags.push_back(argv[++i]);
            } else if (arg == "--gallery" && i + 1 < argc) {
                opts.gallery = argv[++i];
            } else if (arg == "--fav") {
                opts.tags.push_back("favorite");
            } else if (arg == "--min-sharpness" && i + 1 < argc) {
                opts.min_sharpness = std::stod(argv[++i]);
            } else if (arg == "--max-sharpness" && i + 1 < argc) {
                opts.max_sharpness = std::stod(argv[++i]);
            } else if (arg == "--blurry") {
                if (opts.max_sharpness < 0) opts.max_sharpness = 100.0;  // rough default
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--limit" && i + 1 < argc) {
                opts.limit = std::stoi(argv[++i]);
            } else if (arg == "--count") {
                opts.count_only = true;
            } else if (arg == "--porcelain") {
                opts.porcelain = true;
            } else if (arg == "--print0") {
                opts.print0 = true;
            } else if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
                if (opts.format != "text" && opts.format != "csv" && opts.format != "json") {
                    std::cerr << "Error: --format must be 'text', 'csv', or 'json'\n";
                    return 1;
                }
            } else if (arg == "--media" && i + 1 < argc) {
                opts.media = argv[++i];
                if (opts.media != "image" && opts.media != "video" && opts.media != "all") {
                    std::cerr << "Error: --media must be 'image', 'video', or 'all'\n";
                    return 1;
                }
                if (opts.media == "all") opts.media = "";
            } else if (arg == "--min-duration" && i + 1 < argc) {
                try {
                    opts.min_duration_ms = parse_duration(argv[++i]);
                } catch (const std::exception& e) {
                    log_error(command + ": invalid --min-duration: " + e.what());
                    return 1;
                }
            } else if (arg == "--max-duration" && i + 1 < argc) {
                try {
                    opts.max_duration_ms = parse_duration(argv[++i]);
                } catch (const std::exception& e) {
                    log_error(command + ": invalid --max-duration: " + e.what());
                    return 1;
                }
            } else if (arg == "--output" && i + 1 < argc) {
                opts.output = argv[++i];
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_search(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }
}
