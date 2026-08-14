#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

enum class FacesStage { FileDone, Selected, LoadingFaceModels };

struct FacesRebuildOptions {
    // The rows to act on. Empty selects nothing. This engine deletes and
    // re-derives face rows for everything it selects, including the person
    // labels R60 records as unrecoverable, so an unset selection meaning "the
    // whole collection" would be the worst possible default. cli/ resolves its
    // --match/--exclude globs through Database::ids_matching before filling it.
    std::vector<int64_t> ids;
    int parallel = -1;  // -1 = auto
};

// `error` is non-empty when the file could not be decoded: it is the
// already-formatted warning text, and the CALLER is responsible for logging it.
// Matches ScanProgress::error — the engine does not log from a worker thread,
// where another worker's progress line can overwrite the warning. A caller that
// passes no queue has nowhere to route it, so the engine logs it directly
// instead of dropping it.
struct FacesProgress {
    FacesStage stage = FacesStage::FileDone;
    size_t done = 0, total = 0;
    // Running count of files that failed to decode, matching the `errors` on
    // ScanProgress and ThumbsProgress so a caller can render a failure tally as
    // the run proceeds rather than only from the result.
    size_t errors = 0;
    std::string path;
    std::string error;
    // Non-empty when the decoder complained but still produced an image. The
    // file succeeded and its faces were detected; this is not counted in
    // `errors`. Carried separately so a caller can surface it without treating
    // it as a failure.
    std::string warning;
};

struct FacesResult {
    size_t total_selected = 0;
    size_t faces_detected = 0;
    // Files that could not be decoded. Counted rather than fatal: one unreadable
    // image must not abandon a rebuild over a whole collection, but it must not
    // be silent either — without this an unreadable file is indistinguishable
    // from one containing no faces.
    size_t errors = 0;
    bool completed = true;
};

struct IdentifyResult {
    bool person_found = false;
    size_t references = 0;  // already-named faces used as the comparison set
    size_t candidates = 0;  // unlabeled faces examined
    size_t labeled = 0;
};

// Labels every unlabeled face within `threshold` of any face already named as
// `person`, in one transaction.
//
// Two-phase, and deliberately so. Phase 1 reads the reference and candidate
// embeddings and compares them with NO transaction open, because the comparison
// is pure computation over every unlabeled face in the collection — holding a
// write transaction across it would block every other writer for its whole
// duration. Phase 2 commits the matches inside a single BEGIN IMMEDIATE, so the
// labels land together rather than as one implicit transaction per face.
//
// Nothing serializes the gap between the phases, so phase 2 re-checks each row
// it writes through `set_face_person_if_unchanged` instead of trusting what
// phase 1 read. faces.id is AUTOINCREMENT as of schema v8, so a candidate that
// a concurrent rebuild deleted in the gap takes its id out of circulation for
// good: the write matches no row, rather than landing on whichever face
// replaced it. A candidate another labeler named in the gap keeps that name.
// Either way the write updates zero rows, which is why `labeled` counts changed
// rows rather than statements issued. A caller needs no lock for this to be
// correct.
//
// SCALE: phase 1 materializes every unlabeled embedding (128 floats each) in
// memory. That is the wrong shape for a collection with millions of faces; plan
// 044 covers paging it. The transaction behavior here does not change when that
// is fixed.
IdentifyResult identify_person(Database& db, const std::string& person, float threshold);

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
