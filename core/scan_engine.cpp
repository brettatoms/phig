#include "scan_engine.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "dates.h"
#include "exif.h"
#include "hasher.h"
#include "log.h"
#include "metadata.h"
#include "parallel.h"
#include "paths.h"
#include "relink.h"
#include "scanner.h"
#include "thumbs.h"

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

FaceDetector* detector_for_worker(const std::vector<std::unique_ptr<FaceDetector>>& detectors,
                                  int worker_id) {
    if (detectors.empty()) return nullptr;  // faces not requested
    if (worker_id < 0 || static_cast<size_t>(worker_id) >= detectors.size()) {
        throw std::out_of_range("detector_for_worker: worker_id " + std::to_string(worker_id) +
                                " outside [0, " + std::to_string(detectors.size()) + ")");
    }
    return detectors[static_cast<size_t>(worker_id)].get();
}

ScanEngine::ScanEngine(const ScanOptions& opts, Database& db) : opts_(opts), db_(db) {}

ScanResult ScanEngine::run(const CancelToken& cancel, ProgressQueue<ScanProgress>* progress) {
    ScanResult result = run_workers(cancel, progress);
    run_reconcile(result);
    return result;
}

ScanResult ScanEngine::run_workers(const CancelToken& cancel,
                                   ProgressQueue<ScanProgress>* progress) {
    ScanResult result;

    // Paths are shortened against the scan directory in log messages, matching
    // what the caller shows on screen.
    std::string base_dir = fs::canonical(opts_.directory).string();
    if (base_dir.back() != '/') base_dir += '/';
    auto rel_path = [&](const fs::path& p) -> std::string {
        std::string full = p.string();
        if (full.starts_with(base_dir)) return full.substr(base_dir.size());
        return full;
    };

    // Collect files
    auto files = scan_directory(opts_.directory, opts_.recursive, opts_.include_hidden);

    // Filter by media type if requested
    if (opts_.media == "image") {
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const fs::path& p) {
                                       return !is_known_image_extension(p.extension().string());
                                   }),
                    files.end());
    } else if (opts_.media == "video") {
        files.erase(std::remove_if(files.begin(), files.end(),
                                   [](const fs::path& p) {
                                       return !is_known_video_extension(p.extension().string());
                                   }),
                    files.end());
    }
    result.total_files = files.size();

    // Determine thread count
    int num_threads;
    if (opts_.parallel > 0) {
        num_threads = opts_.parallel;
    } else if (opts_.faces) {
        // With faces: conservative default (each thread loads ~150MB of models)
        num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
        num_threads = std::max(num_threads, 1);
    } else {
        num_threads = std::max(1u, std::thread::hardware_concurrency());
    }

    std::atomic<size_t> processed{0};
    std::atomic<size_t> skipped{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> face_count{0};
    std::atomic<size_t> thumb_count{0};
    std::atomic<size_t> video_count{0};

    auto publish = [&](const fs::path& filepath, bool show_path, const std::string& error) {
        if (progress == nullptr) return;
        ScanProgress p;
        p.stage = ScanStage::FileDone;
        p.processed = processed.load();
        p.skipped = skipped.load();
        p.errors = errors.load();
        p.done = p.processed + p.skipped + p.errors;
        p.total = files.size();
        p.path = filepath.string();
        p.show_path = show_path;
        p.error = error;
        progress->push(std::move(p));
    };

    auto publish_stage = [&](ScanStage stage, size_t total) {
        if (progress == nullptr) return;
        ScanProgress p;
        p.stage = stage;
        p.total = total;
        progress->push(std::move(p));
    };

    // NOTE — this is the first thing pushed, and ProgressQueue evicts the OLDEST
    // item when full, so under sustained back-pressure it is the first casualty.
    // That inverts the queue's intent, where a stale progress tick is the
    // worthless item and this one-shot notice is not. Accepted rather than fixed:
    // losing it needs a consumer stalled for a full capacity's worth of file
    // completions (1024 in the CLI), and each completion costs a hash plus a
    // decode. A caller that cannot tolerate the loss reads ScanResult::total_files
    // instead, which is never dropped.
    publish_stage(ScanStage::Walked, files.size());

    // One FaceDetector per worker, never a shared one: each holds ~150MB of
    // mutable OpenCV DNN state and cv::FaceDetectorYN is not thread-safe, so
    // sharing an instance across workers is a data race. `process_one` indexes
    // this vector by worker_id, which is only sound while size() == num_threads.
    std::vector<std::unique_ptr<FaceDetector>> face_detectors;
    if (opts_.faces) {
        publish_stage(ScanStage::LoadingFaceModels, static_cast<size_t>(num_threads));
        for (int i = 0; i < num_threads; i++) {
            face_detectors.push_back(std::make_unique<FaceDetector>());
        }
    }

    if (!files.empty()) {
        std::mutex db_mutex;
        std::vector<ProcessResult> batch;
        const size_t batch_size = 10;  // small batches so cancellation loses less work

        auto flush_batch = [&]() {
            if (batch.empty()) return;

            // One transaction for the images and their faces: a throw partway
            // through the face loop would otherwise leave some rows committed
            // and others not, with batch never cleared. insert() is called
            // directly rather than via insert_batch(), which opens its own
            // transaction and cannot nest inside this one.
            db_.begin_transaction();
            try {
                for (auto& r : batch) db_.insert(r.info);

                // Insert faces (need image_id from DB) and auto-label known people
                for (auto& r : batch) {
                    if (!r.faces.empty()) {
                        int64_t image_id = db_.get_image_id(r.info.path);
                        // Rows whose bytes didn't change kept their existing faces
                        // (and person labels) through insert(); re-adding this
                        // run's detections would duplicate them. insert() cleared
                        // faces when the content changed, so has_faces == false
                        // means these detections are needed. `face rebuild` is the
                        // path for forced re-detection.
                        if (image_id >= 0 && !db_.has_faces(r.info.path)) {
                            for (const auto& face : r.faces) {
                                int64_t face_id = db_.insert_face(image_id, face);
                                db_.auto_label_face(face_id, face.embedding);
                            }
                            face_count.fetch_add(r.faces.size());
                        }
                    }
                }
                db_.commit_transaction();
            } catch (...) {
                db_.rollback_transaction();
                throw;
            }
            batch.clear();
        };

        auto process_one = [&](size_t idx, int worker_id) {
            FaceDetector* detector = detector_for_worker(face_detectors, worker_id);

            const auto& filepath = files[idx];
            std::string canonical;
            try {
                canonical = fs::canonical(filepath).string();
            } catch (...) {
                errors.fetch_add(1);
                return;
            }

            if (has_sibling_raw(filepath)) {
                skipped.fetch_add(1);
                return;
            }

            // Check if we should skip (path + mtime match)
            bool needs_faces_only = false;
            bool needs_thumbs_only = false;
            bool skip_outright = false;

            if (!opts_.force.hash) {
                std::string mtime;
                try {
                    mtime = format_time(fs::last_write_time(filepath));
                } catch (...) {
                    // Can't stat, will fail later
                }

                {
                    std::lock_guard<std::mutex> lock(db_mutex);
                    auto existing_mtime = db_.get_modified_at(canonical);
                    if (existing_mtime && *existing_mtime == mtime) {
                        // File unchanged — check if we need faces or thumbs
                        bool need_faces = detector && !db_.has_faces(canonical);
                        bool need_thumbs = false;
                        if (opts_.thumbs) {
                            auto sha = db_.get_sha256(canonical);
                            if (sha && !sha->empty()) {
                                need_thumbs = opts_.force.thumbs || !thumb_exists(*sha);
                            }
                        }

                        if (need_faces) {
                            needs_faces_only = true;
                        } else if (need_thumbs && !need_faces) {
                            needs_thumbs_only = true;
                        } else {
                            skipped.fetch_add(1);
                            skip_outright = true;
                        }
                    }
                }
                // Publish outside the lock: nothing may be emitted to a caller
                // while a database lock is held, or a slow consumer would stall
                // every worker.
                if (skip_outright) {
                    publish(filepath, false, "");
                    return;
                }
            }

            std::string error_text;
            try {
                if (needs_thumbs_only) {
                    // Only generate thumbnail — file is unchanged and faces are done
                    std::string sha;
                    {
                        std::lock_guard<std::mutex> lock(db_mutex);
                        auto s = db_.get_sha256(canonical);
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
                                if (!db_.has_faces(canonical)) {
                                    db_.begin_transaction();
                                    try {
                                        int64_t image_id = db_.get_image_id(canonical);
                                        if (image_id >= 0) {
                                            for (const auto& face : faces) {
                                                int64_t face_id = db_.insert_face(image_id, face);
                                                db_.auto_label_face(face_id, face.embedding);
                                            }
                                            face_count.fetch_add(faces.size());
                                        }
                                        db_.commit_transaction();
                                    } catch (...) {
                                        db_.rollback_transaction();
                                        throw;
                                    }
                                }
                            }
                        }
                        // Also generate thumbnail if needed
                        if (opts_.thumbs) {
                            std::string sha;
                            {
                                std::lock_guard<std::mutex> lock(db_mutex);
                                auto s = db_.get_sha256(canonical);
                                if (s) sha = *s;
                            }
                            if (!sha.empty() && (opts_.force.thumbs || !thumb_exists(sha))) {
                                generate_thumb(img, sha);
                                thumb_count.fetch_add(1);
                            }
                        }
                    }
                    processed.fetch_add(1);
                } else {
                    auto proc = process_file(filepath, detector);

                    if (proc.info.media_type == "video") {
                        video_count.fetch_add(1);
                    }

                    // Generate thumbnail if requested
                    if (opts_.thumbs && !proc.info.sha256.empty()) {
                        if (opts_.force.thumbs || !thumb_exists(proc.info.sha256)) {
                            if (proc.info.media_type == "video") {
                                // For videos, extract keyframe for thumbnail
                                try {
                                    auto kf = extract_video_keyframe(canonical);
                                    if (!kf.frame.empty()) {
                                        generate_thumb(kf.frame, proc.info.sha256);
                                        thumb_count.fetch_add(1);
                                    }
                                } catch (...) {
                                }
                            } else {
                                cv::Mat img = decode_image(canonical);
                                if (!img.empty()) {
                                    generate_thumb(img, proc.info.sha256);
                                    thumb_count.fetch_add(1);
                                }
                            }
                        }
                    }

                    std::lock_guard<std::mutex> lock(db_mutex);
                    batch.push_back(std::move(proc));
                    if (batch.size() >= batch_size) {
                        flush_batch();
                    }
                    processed.fetch_add(1);
                }
            } catch (const std::exception& e) {
                errors.fetch_add(1);
                // Assemble the whole line so concurrent workers' messages don't
                // interleave with each other, and hand it to the caller on the
                // progress update rather than printing it here — the caller
                // renders the warning and the progress line together, on one
                // thread, in order.
                error_text = "scan: " + rel_path(filepath) + " -- " + e.what();
                if (opts_.fail_on_error) {
                    log_error(error_text);
                    throw;
                }
                // Deliberately not logged here. The caller logs it as it renders
                // the matching progress update, so the warning and the progress
                // line are emitted by one thread in order — logging from the
                // worker let another worker's progress line overwrite the
                // warning on a terminal. A caller that passes no queue gets no
                // per-file warning, only the aggregate error count; that is the
                // documented cost of declining progress.
                if (progress == nullptr) log_warn(error_text);
            }

            publish(filepath, true, error_text);
        };

        try {
            result.completed = parallel_for_each(files.size(), num_threads, cancel, process_one);
        } catch (...) {
            // --on-error fail: flush what completed, then let the caller report
            // and exit non-zero. This exception used to escape a std::thread
            // lambda and call std::terminate, aborting mid-write with an
            // uncheckpointed WAL.
            //
            // If the original failure came from inside flush_batch itself (a DB
            // or face-insert error, not a decode error), the batch that caused it
            // is still sitting there un-cleared, so this recovery flush would
            // re-run the same failing work and throw again — before the caller
            // ever reports the original error. Catch that second failure and log
            // it rather than let it escape; the batch's rows are left unflushed
            // and reported as such instead of being silently dropped.
            {
                std::lock_guard<std::mutex> lock(db_mutex);
                try {
                    flush_batch();
                } catch (const std::exception& flush_e) {
                    log_error("scan: recovery flush also failed, " + std::to_string(batch.size()) +
                              " row(s) unflushed -- " + flush_e.what());
                }
            }
            throw;
        }

        // Flush remaining
        flush_batch();
    }  // end if (!files.empty())

    result.processed = processed.load();
    result.skipped = skipped.load();
    result.errors = errors.load();
    result.videos = video_count.load();
    result.images = result.processed - result.videos;
    result.faces = face_count.load();
    result.thumbs = thumb_count.load();
    return result;
}

// Clean up entries for files that no longer exist on disk. Runs after the
// worker phase and is deliberately not cancellable: it deletes user-authored
// metadata (tags, gallery, faces) inside one logged operation, and abandoning it
// halfway would leave the operation row stuck in_progress.
void ScanEngine::run_reconcile(ScanResult& result) {
    std::string scan_prefix = fs::canonical(opts_.directory).string();
    if (scan_prefix.back() != '/') scan_prefix += '/';

    auto db_paths = db_.get_paths_with_prefix(scan_prefix);
    // Only consider a row deletable when its file is genuinely gone AND its
    // parent directory still exists — a missing parent (unmounted subtree) or an
    // errored existence check keeps the row, so a transient mount/permission
    // problem can't purge metadata for intact files.
    //
    // --ignore-mount-warning means "proceed past the mount heuristic", so it also
    // treats a missing parent as a genuine deletion (same meaning the flag has on
    // `rm --orphans`). A path whose existence couldn't be determined at all is
    // never included: that's an unknown state, not a judgement about mounting.
    SafeToDeleteResult safe = paths_safe_to_delete(db_paths);
    std::vector<std::string> removed_paths = std::move(safe.deletable);
    if (opts_.ignore_mount_warning) {
        removed_paths.insert(removed_paths.end(), safe.parent_missing.begin(),
                             safe.parent_missing.end());
    }
    result.skipped_parent_missing = opts_.ignore_mount_warning ? 0 : safe.parent_missing.size();
    result.skipped_check_failed = safe.check_failed.size();
    size_t removed_count = 0;
    int relinked_count = 0;
    int demoted_count = 0;

    if (!removed_paths.empty() && !db_paths.empty() && removed_paths.size() == db_paths.size() &&
        !opts_.ignore_mount_warning) {
        result.mount_warning = true;
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
            if (opts_.log_enabled) {
                int64_t op_id = db_.log_begin_operation("scan", opts_.argv_str, iso_utc_now());
                try {
                    relinked_count = relink_moved_rows(db_, removed_paths, op_id);
                    std::vector<std::string> demoted_paths =
                        demote_jpeg_rows(db_, scan_prefix, op_id);
                    demoted_count = static_cast<int>(demoted_paths.size());
                    std::vector<std::string> to_delete = removed_paths;
                    to_delete.insert(to_delete.end(), demoted_paths.begin(), demoted_paths.end());
                    db_.delete_paths_logged(to_delete, op_id);
                    removed_count = removed_paths.size();
                } catch (...) {
                    // delete_paths_logged rolls back its own transaction, but the
                    // operation row was autocommitted by log_begin_operation. Mark
                    // it so it doesn't linger as a stuck 'in_progress' row, matching
                    // rm's broad-selection guard (review [6]); best-effort.
                    try {
                        db_.log_finish_operation(op_id, "interrupted");
                    } catch (...) {
                    }
                    throw;
                }
                db_.log_finish_operation(op_id, "completed");
                db_.log_prune(opts_.log_max_entries);
            } else {
                relinked_count = relink_moved_rows(db_, removed_paths, 0);
                std::vector<std::string> demoted_paths = demote_jpeg_rows(db_, scan_prefix, 0);
                demoted_count = static_cast<int>(demoted_paths.size());
                std::vector<std::string> to_delete = removed_paths;
                to_delete.insert(to_delete.end(), demoted_paths.begin(), demoted_paths.end());
                db_.delete_paths(to_delete);
                removed_count = removed_paths.size();
            }
        }
    }

    result.removed = removed_count;
    result.relinked = static_cast<size_t>(relinked_count);
    result.demoted = static_cast<size_t>(demoted_count);

    log_info("scan " + opts_.directory + " done: processed=" + std::to_string(result.processed) +
             " skipped=" + std::to_string(result.skipped) + " errors=" +
             std::to_string(result.errors) + " removed=" + std::to_string(result.removed));
}
