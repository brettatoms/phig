#include "faces_engine.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "faces.h"
#include "hasher.h"
#include "parallel.h"
#include "scan_engine.h"  // detector_for_worker
#include "selection.h"

FacesEngine::FacesEngine(const FacesRebuildOptions& opts, Database& db) : opts_(opts), db_(db) {}

FacesResult FacesEngine::run(const CancelToken& cancel, ProgressQueue<FacesProgress>* progress) {
    FacesResult result;

    std::vector<ImageInfo> all = db_.get_all_images();
    std::vector<ImageInfo> images;
    for (const auto& img : all) {
        if (passes_selection(img.path, opts_.matches, opts_.excludes)) {
            images.push_back(img);
        }
    }
    result.total_selected = images.size();

    if (progress) {
        FacesProgress p;
        p.stage = FacesStage::Selected;
        p.total = images.size();
        progress->push(p);
    }
    if (images.empty()) return result;

    if (cancel.stop()) {
        result.completed = false;
        return result;
    }

    int num_threads;
    if (opts_.parallel > 0) {
        num_threads = opts_.parallel;
    } else {
        num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
        num_threads = std::max(num_threads, 1);
    }

    if (progress) {
        FacesProgress p;
        p.stage = FacesStage::LoadingModels;
        p.total = static_cast<size_t>(num_threads);
        progress->push(p);
    }

    std::vector<std::unique_ptr<FaceDetector>> detectors;
    detectors.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        detectors.push_back(std::make_unique<FaceDetector>());
    }

    std::atomic<size_t> total_faces{0};
    std::atomic<size_t> done{0};
    std::mutex db_mutex;

    auto publish_done = [&](const ImageInfo& img_info) {
        size_t d = done.fetch_add(1) + 1;
        if (progress) {
            FacesProgress p;
            p.stage = FacesStage::FileDone;
            p.done = d;
            p.total = images.size();
            p.path = img_info.path;
            progress->push(p);
        }
    };

    auto worker = [&](size_t idx, int worker_id) {
        const ImageInfo& img_info = images[idx];
        cv::Mat img = decode_image(img_info.path);
        if (img.empty()) {
            // Still counted so the final tally reaches `total`.
            publish_done(img_info);
            return;
        }

        auto faces = detector_for_worker(detectors, worker_id)->detect(img);

        {
            std::lock_guard<std::mutex> lock(db_mutex);
            int64_t image_id = db_.get_image_id(img_info.path);
            if (image_id >= 0) {
                // Delete-then-reinsert must be atomic: if the delete ran
                // outside the transaction and we were interrupted before
                // commit, the old faces would be gone with no re-detection.
                db_.begin_transaction();
                try {
                    db_.delete_faces_for_image(image_id);
                    for (const auto& face : faces) {
                        int64_t face_id = db_.insert_face(image_id, face);
                        db_.auto_label_face(face_id, face.embedding);
                    }
                    db_.commit_transaction();
                    total_faces.fetch_add(faces.size());
                } catch (...) {
                    db_.rollback_transaction();
                    throw;
                }
            }
        }

        publish_done(img_info);
    };

    result.completed = parallel_for_each(images.size(), num_threads, cancel, worker);
    result.faces_detected = total_faces.load();
    return result;
}
