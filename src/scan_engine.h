#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "faces.h"
#include "progress_queue.h"
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

// One progress update, published as a file finishes. `path` is the full path of
// that file; the caller shortens it for display. `show_path` is false for the
// terse tick emitted when a file is skipped outright, which reproduces the
// CLI's two progress formats.
//
// `error` is non-empty when the file failed: it is the already-formatted
// warning text for that file. The engine has already logged it via log_warn, so
// a caller with a log sink can ignore it; a caller rendering to a terminal
// prints it alongside this update. Carrying it here rather than printing from
// the worker is what keeps a warning from being erased by another thread's
// progress line.
//
// Two updates carry a `stage` instead of a finished file: the engine publishes
// one once the file walk is done (`total` is the file count) and one before it
// loads face models (`total` is the number of detector instances). They travel
// on the queue so a caller renders them in the order the work happens rather
// than guessing when the walk finished. Wording is the caller's; the engine
// carries no user-facing prose.
enum class ScanStage { FileDone, Walked, LoadingFaceModels };

struct ScanProgress {
    ScanStage stage = ScanStage::FileDone;
    size_t done = 0, total = 0;
    size_t processed = 0, skipped = 0, errors = 0;
    std::string path;
    std::string error;
    bool show_path = false;
};

// Everything the caller needs to render a summary.
struct ScanResult {
    size_t processed = 0, skipped = 0, errors = 0;
    size_t images = 0, videos = 0, faces = 0, thumbs = 0;
    size_t removed = 0, relinked = 0, demoted = 0;
    size_t total_files = 0;
    bool completed = true;
    bool mount_warning = false;
};

// Scans a directory into a database: walks files, decides what needs
// reprocessing, hashes and extracts metadata in parallel, and reconciles rows
// for files that have moved or disappeared.
//
// Does no I/O of its own beyond the database and the diagnostic log. Progress is
// published to the queue if one is given; cancellation is polled from the token.
// The queue may be null. The engine owns the mutex serializing database access,
// so a caller must not touch `db` while run() is executing.
//
// SAFETY — cancellation covers the parallel worker phase ONLY. The reconcile
// phase deletes user-authored metadata (tags, gallery, faces) inside one logged
// operation and reads no CancelToken, so a cancellation arriving during it would
// be silently swallowed rather than stopping anything. A caller that maps a
// signal onto the token must therefore stop doing so before reconcile begins,
// which is what `run_workers` / `run_reconcile` exist for: they are the same
// work `run()` does, split at exactly that boundary so the caller can drop its
// signal handler in between. `run()` is the convenience form for a caller with
// no signal handler to drop (tests, an in-process library user).
class ScanEngine {
   public:
    ScanEngine(const ScanOptions& opts, Database& db);

    ScanResult run(const CancelToken& cancel, ProgressQueue<ScanProgress>* progress);

    // Phase 1: walk, then hash and extract in parallel. Cancellable. Throws the
    // first worker exception when `fail_on_error` is set, after flushing the
    // batch that completed.
    ScanResult run_workers(const CancelToken& cancel, ProgressQueue<ScanProgress>* progress);

    // Phase 2: reconcile rows for files that moved or disappeared — relink, then
    // demote, then delete. Not cancellable; see the class comment. Fills in
    // removed/relinked/demoted/mount_warning on `result` and logs the summary.
    void run_reconcile(ScanResult& result);

   private:
    // By value, not by reference: `ScanEngine engine(build_options(), db);`
    // otherwise leaves a dangling reference once the temporary dies at the
    // semicolon, and the options are a handful of strings.
    ScanOptions opts_;
    Database& db_;
};
