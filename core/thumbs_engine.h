#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "invalid_option.h"
#include "progress_queue.h"
#include "thumbs.h"
#include "types.h"

struct ThumbsOptions {
    // The rows to act on. Empty selects nothing — a caller names its subject,
    // and forgetting to is a no-op rather than a whole-collection run. cli/
    // resolves its --match/--exclude globs through Database::ids_matching
    // before constructing this.
    std::vector<int64_t> ids;
    bool force = false;
    int parallel = -1;                   // -1 = auto
    int size = PHIG_DEFAULT_THUMB_SIZE;  // longest edge, in pixels
};

// Throws InvalidOption when a field holds a value outside its set (R73).
void validate(const ThumbsOptions& opts);

// Whether one row needs a thumbnail generated.
//
// A row with an empty sha256 is always skipped: the sha is the thumbnail cache
// key, so there is no path to write to. `force` does not override that — it
// overrides the freshness check only.
//
// Free function, not a private method, so the decision is testable without
// touching the thumbnail cache on disk.
bool thumb_needs_generating(const ImageInfo& img, bool force, int size);

// The outcome of ensure_thumb. NoSuchRow covers an unknown id and a row with no
// sha256: neither has a cache key to write to.
enum class EnsureThumb { Ok, NoSuchRow, SourceMissing, Failed };

// Makes sure one row's thumbnail exists at `size` (> 0), generating it from the
// row's file if it is not cached. A video is generated from its keyframe. This
// is the single-row, on-demand counterpart to ThumbsEngine::run, for a client
// that generates as cells scroll into view. Safe to call concurrently,
// including for the same row.
EnsureThumb ensure_thumb(Database& db, int64_t image_id, int size);

// Progress carries either a finished file or the count of selected images,
// published once before the parallel phase begins so a caller can size a
// progress bar without pre-counting.
enum class ThumbsStage { FileDone, Selected };

struct ThumbsProgress {
    ThumbsStage stage = ThumbsStage::FileDone;
    size_t done = 0, total = 0;
    size_t generated = 0, skipped = 0, errors = 0;
    std::string path;
    // Non-empty when this file failed: the already-formatted warning text, which
    // the CALLER is responsible for logging. Matches ScanProgress::error — the
    // engine does not log from a worker thread, where another worker's progress
    // line can overwrite the warning. A caller that passes no queue has nowhere
    // to route it, so the engine logs it directly instead of dropping it.
    std::string error;
    // Non-empty when the decoder complained but still produced an image — a
    // truncated JPEG that decodes partially, say. Distinct from `error`, which
    // means the file failed: this file succeeded and the caller got a
    // thumbnail. Carried separately so a caller can surface it without
    // counting it as a failure.
    std::string warning;
    // False for the terse tick emitted when a file is skipped, reproducing the
    // CLI's two progress formats.
    bool show_path = false;
};

struct ThumbsResult {
    size_t generated = 0, skipped = 0, errors = 0;
    size_t total_selected = 0;
    bool completed = true;
};

// Generates missing thumbnails for the images a database already knows about.
//
// Reads the database once up front and writes none of it back — thumbnails are
// filesystem cache artifacts, so this engine needs no transaction and no
// database mutex. Publishes to the queue if one is given; the queue may be null.
// Polls the token between items.
class ThumbsEngine {
   public:
    ThumbsEngine(const ThumbsOptions& opts, Database& db);

    ThumbsResult run(const CancelToken& cancel, ProgressQueue<ThumbsProgress>* progress);

   private:
    // By value: constructing from a temporary would otherwise dangle.
    ThumbsOptions opts_;
    Database& db_;
};
