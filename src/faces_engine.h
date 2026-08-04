#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

enum class FacesStage { FileDone, Selected, LoadingFaceModels };

struct FacesRebuildOptions {
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    int parallel = -1;  // -1 = auto
};

struct FacesProgress {
    FacesStage stage = FacesStage::FileDone;
    size_t done = 0, total = 0;
    std::string path;
};

struct FacesResult {
    size_t total_selected = 0;
    size_t faces_detected = 0;
    bool completed = true;
};

// Re-detects faces for images a database already knows about, replacing each
// image's existing face rows.
//
// Per image the delete and the reinsert run in ONE transaction: if the delete
// ran outside it and the process were interrupted before commit, that image's
// faces would be gone with no re-detection to restore them. The engine owns the
// mutex serializing database access, so a caller must not touch `db` while
// run() is executing.
//
// Each worker gets its own FaceDetector: cv::FaceDetectorYN is not thread-safe
// and holds ~150MB of mutable model state, so sharing one across workers is a
// data race. Publishes to the queue if one is given; the queue may be null.
class FacesEngine {
   public:
    FacesEngine(const FacesRebuildOptions& opts, Database& db);

    FacesResult run(const CancelToken& cancel, ProgressQueue<FacesProgress>* progress);

   private:
    // By value: constructing from a temporary would otherwise dangle.
    FacesRebuildOptions opts_;
    Database& db_;
};
