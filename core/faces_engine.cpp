#include "faces_engine.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "faces.h"
#include "hasher.h"
#include "log.h"
#include "parallel.h"
#include "scan_engine.h"  // detector_for_worker
#include "selection.h"

IdentifyResult identify_person(Database& db, const std::string& person, float threshold) {
    IdentifyResult result;

    int64_t person_id = -1;
    for (const auto& p : db.list_people()) {
        if (p.name == person) {
            person_id = p.id;
            break;
        }
    }
    if (person_id < 0) return result;
    result.person_found = true;

    // Phase 1: read and compare with no transaction open. See the header — the
    // comparison is O(candidates * references) and must not hold a write lock.
    std::vector<Database::FaceEmbeddingRow> refs = db.get_face_embeddings_for_person(person_id);
    result.references = refs.size();
    if (refs.empty()) return result;

    std::vector<Database::FaceEmbeddingRow> candidates = db.get_unlabeled_face_embeddings();
    result.candidates = candidates.size();

    // The whole matched row, not just its id: phase 2 writes against the
    // embedding it compared, so the id alone is not enough to identify the face
    // again.
    std::vector<Database::FaceEmbeddingRow> matches;
    for (const auto& c : candidates) {
        for (const auto& ref : refs) {
            if (embedding_distance(ref.embedding, c.embedding) <= threshold) {
                matches.push_back(c);
                break;
            }
        }
    }
    if (matches.empty()) return result;

    // Phase 2: one transaction for every label. Each write re-checks that the
    // row is still the face phase 1 compared, so a face deleted — or deleted and
    // replaced on the same rowid — updates zero rows instead of taking a label
    // it never earned.
    size_t labeled = 0;
    db.begin_transaction();
    try {
        for (const auto& m : matches) {
            if (db.set_face_person_if_unchanged(m.face_id, person_id, m.embedding)) ++labeled;
        }
        db.commit_transaction();
    } catch (...) {
        db.rollback_transaction();
        throw;
    }
    result.labeled = labeled;
    return result;
}

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
    if (cancel.stop()) {
        result.completed = false;
        return result;
    }
    if (images.empty()) return result;

    int num_threads;
    if (opts_.parallel > 0) {
        num_threads = opts_.parallel;
    } else {
        num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
        num_threads = std::max(num_threads, 1);
    }

    if (progress) {
        FacesProgress p;
        p.stage = FacesStage::LoadingFaceModels;
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
    std::atomic<size_t> errors{0};
    std::mutex db_mutex;

    auto publish_done = [&](const ImageInfo& img_info, const std::string& error = "",
                            const std::string& warning = "") {
        size_t d = done.fetch_add(1) + 1;
        if (progress) {
            FacesProgress p;
            p.stage = FacesStage::FileDone;
            p.done = d;
            p.total = images.size();
            p.errors = errors.load();
            p.path = img_info.path;
            p.error = error;
            p.warning = warning;
            progress->push(p);
        }
    };

    auto worker = [&](size_t idx, int worker_id) {
        const ImageInfo& img_info = images[idx];
        // decode_image_with_warning rather than decode_image: the latter logs
        // OpenCV's message and discards it, which loses the reason from a worker
        // thread. Routing it to the caller keeps the warning and the progress
        // line together, on one thread, in order.
        DecodeResult decoded = decode_image_with_warning(img_info.path);
        if (decoded.img.empty()) {
            errors.fetch_add(1);
            std::string text = "faces: " + img_info.path;
            if (!decoded.warning.empty()) text += " -- " + decoded.warning;
            // Nowhere to route it when the caller declined progress, so log it
            // here instead of dropping it.
            if (progress == nullptr) log_warn(text);
            // Still counted so the final tally reaches `total`.
            publish_done(img_info, text);
            return;
        }
        // Decoded, but the decoder complained. Reported, not counted.
        std::string warning;
        if (!decoded.warning.empty()) {
            warning = "faces: " + img_info.path + " -- " + decoded.warning;
            if (progress == nullptr) log_warn(warning);
        }
        cv::Mat img = decoded.img;

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

        publish_done(img_info, "", warning);
    };

    result.completed = parallel_for_each(images.size(), num_threads, cancel, worker);
    result.faces_detected = total_faces.load();
    result.errors = errors.load();
    return result;
}
