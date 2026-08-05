#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"
#include "types.h"

struct ThumbsOptions {
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    bool force = false;
    int parallel = -1;  // -1 = auto
};

// Whether one row needs a thumbnail generated.
//
// A row with an empty sha256 is always skipped: the sha is the thumbnail cache
// key, so there is no path to write to. `force` does not override that — it
// overrides the freshness check only.
//
// Free function, not a private method, so the decision is testable without
// touching the thumbnail cache on disk.
bool thumb_needs_generating(const ImageInfo& img, bool force);

// Progress carries either a finished file or the count of selected images,
// published once before the parallel phase begins so a caller can size a
// progress bar without pre-counting.
enum class ThumbsStage { FileDone, Selected };

struct ThumbsProgress {
    ThumbsStage stage = ThumbsStage::FileDone;
    size_t done = 0, total = 0;
    size_t generated = 0, skipped = 0, errors = 0;
    std::string path;
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
