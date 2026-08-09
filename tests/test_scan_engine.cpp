#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "faces.h"
#include "models.h"
#include "progress_queue.h"
#include "scan_engine.h"
#include "scanner.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {
// A scratch directory with N tiny copies of a real fixture, removed on scope exit.
struct ScratchCollection {
    fs::path dir;
    std::string db_path;

    explicit ScratchCollection(int n) {
        // make_temp_dir, not fs::temp_directory_path() directly: it returns a
        // canonical path, which ScanEngine's fs::canonical-built prefix has to
        // match. See ReconcileOrderScratch below for the macOS symlink detail.
        dir = make_temp_dir("scan-engine");
        db_path = (dir / "phig.db").string();
        for (int i = 0; i < n; ++i) {
            fs::copy_file("tests/data/test_red.mov", dir / ("v" + std::to_string(i) + ".mov"));
        }
    }
    ~ScratchCollection() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    ScanOptions options() const {
        ScanOptions o;
        o.directory = dir.string();
        o.db_path = db_path;
        o.recursive = false;
        o.parallel = 2;
        o.log_enabled = false;  // no XDG_DATA_HOME dependency in these tests
        return o;
    }
};

// Drains a queue on its own thread so the engine's pushes are consumed as they
// happen, which is how a real caller uses it.
class Drain {
   public:
    explicit Drain(ProgressQueue<ScanProgress>& q) : q_(q) {
        t_ = std::thread([this] {
            ScanProgress p;
            while (q_.pop(p)) seen_.push_back(p);
        });
    }
    // Call after run() returns.
    std::vector<ScanProgress> finish() {
        q_.close();
        t_.join();
        return seen_;
    }

   private:
    ProgressQueue<ScanProgress>& q_;
    std::thread t_;
    std::vector<ScanProgress> seen_;
};
}  // namespace

TEST(ScanEngine, ProcessesEveryFileOnAFirstScan) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanEngine engine(c.options(), db);
    ScanResult r = engine.run(cancel, nullptr);

    EXPECT_EQ(r.processed, 6u);
    EXPECT_EQ(r.skipped, 0u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_EQ(r.videos, 6u) << "a .mov must be counted as a video, not an image";
    EXPECT_EQ(r.images, 0u);
    EXPECT_EQ(r.total_files, 6u);
    EXPECT_TRUE(r.completed);
    EXPECT_EQ(db.count(), 6);

    // Every row must be fully populated, not just present: this is what catches a
    // flush that commits images but not their derived columns.
    for (const auto& img : db.get_all_images()) {
        EXPECT_FALSE(img.sha256.empty()) << img.path;
        EXPECT_EQ(img.media_type, "video") << img.path;
        EXPECT_FALSE(img.modified_at.empty()) << img.path;
    }
}

TEST(ScanEngine, SkipsUnchangedFilesOnASecondScan) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanEngine(c.options(), db).run(cancel, nullptr);
    ScanResult second = ScanEngine(c.options(), db).run(cancel, nullptr);

    EXPECT_EQ(second.processed, 0u) << "unchanged files must not be reprocessed";
    EXPECT_EQ(second.skipped, 6u);
    EXPECT_EQ(second.errors, 0u);
    EXPECT_EQ(db.count(), 6);
}

TEST(ScanEngine, AChangedMtimeReprocessesOnlyThatFile) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);

    // Touch one file so its recorded mtime no longer matches.
    fs::last_write_time(c.dir / "v3.mov",
                        fs::last_write_time(c.dir / "v3.mov") + std::chrono::seconds(120));

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.processed, 1u) << "only the touched file may be reprocessed";
    EXPECT_EQ(r.skipped, 5u);
    EXPECT_EQ(db.count(), 6);
}

TEST(ScanEngine, ForceHashReprocessesEverything) {
    ScratchCollection c(4);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanEngine(c.options(), db).run(cancel, nullptr);

    ScanOptions forced = c.options();
    forced.force.hash = true;
    ScanResult r = ScanEngine(forced, db).run(cancel, nullptr);
    EXPECT_EQ(r.processed, 4u);
    EXPECT_EQ(r.skipped, 0u);
}

TEST(ScanEngine, ReportsProgressThroughTheQueue) {
    ScratchCollection c(5);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    Drain drain(q);
    ScanResult r = ScanEngine(c.options(), db).run(cancel, &q);
    auto seen = drain.finish();

    EXPECT_EQ(r.processed, 5u);
    ASSERT_FALSE(seen.empty()) << "the engine must publish progress";

    // The walk notice must come first, before any file update, and carry the
    // file count — this is what the CLI's "Found N files" line is rendered from.
    EXPECT_EQ(seen.front().stage, ScanStage::Walked);
    EXPECT_EQ(seen.front().total, 5u);

    size_t file_updates = 0;
    for (const auto& s : seen) {
        if (s.stage != ScanStage::FileDone) continue;
        ++file_updates;
        EXPECT_EQ(s.total, 5u);
        EXPECT_LE(s.done, 5u);
        EXPECT_GE(s.done, 1u) << "a file update must account for at least itself";
        EXPECT_FALSE(s.path.empty()) << "a file update must name its file";
        EXPECT_TRUE(s.show_path) << "a processed file shows its path";
        EXPECT_EQ(s.done, s.processed + s.skipped + s.errors);
    }
    EXPECT_EQ(file_updates, 5u) << "one update per file";
    EXPECT_EQ(seen.back().stage, ScanStage::FileDone);
    EXPECT_EQ(seen.back().done, 5u) << "the last update must report completion";
}

TEST(ScanEngine, SkipTicksCarryNoPathSoTheCliRendersTheTerseForm) {
    ScratchCollection c(4);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);

    ProgressQueue<ScanProgress> q(1024);
    Drain drain(q);
    ScanResult r = ScanEngine(c.options(), db).run(cancel, &q);
    auto seen = drain.finish();

    ASSERT_EQ(r.skipped, 4u);
    size_t ticks = 0;
    for (const auto& s : seen) {
        if (s.stage != ScanStage::FileDone) continue;
        ++ticks;
        EXPECT_FALSE(s.show_path) << "an unchanged file gets the terse tick, not a path";
    }
    EXPECT_EQ(ticks, 4u);
}

TEST(ScanEngine, RunsWithoutAQueueWhenProgressIsNotWanted) {
    ScratchCollection c(3);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    // nullptr must be accepted — a caller that does not want progress should
    // not have to construct a queue to discard.
    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.processed, 3u);
    EXPECT_EQ(db.count(), 3);
}

TEST(ScanEngine, AnAlreadyCancelledTokenProcessesNothing) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    cancel.cancel();

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.processed, 0u);
    EXPECT_EQ(db.count(), 0) << "cancellation before any work must write nothing";
}

TEST(ScanEngine, CancellationLeavesTheDatabaseConsistent) {
    ScratchCollection c(40);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    // Cancel from a consumer thread once a few files are done.
    std::thread canceller([&] {
        ScanProgress p;
        while (q.pop(p)) {
            if (p.stage == ScanStage::FileDone && p.done >= 3) {
                cancel.cancel();
                break;
            }
        }
    });
    ScanResult r = ScanEngine(c.options(), db).run(cancel, &q);
    q.close();
    canceller.join();

    EXPECT_FALSE(r.completed);
    EXPECT_LT(r.processed, 40u) << "cancellation must stop work short of the whole collection";
    // Every row that exists must be complete and readable — a partial scan must
    // not leave a half-written row behind.
    auto rows = db.get_all_images();
    for (const auto& img : rows) {
        EXPECT_FALSE(img.sha256.empty()) << img.path << " has no hash";
        EXPECT_FALSE(img.path.empty());
    }
    // A cancelled run must still have committed the batches it completed, and
    // must never claim more rows than it processed.
    EXPECT_LE(static_cast<size_t>(rows.size()), r.processed);
}

TEST(ScanEngine, CountsAnUndecodableFileAsAnErrorAndContinues) {
    ScratchCollection c(4);
    // A text file with an image extension cannot be decoded.
    {
        std::ofstream(c.dir / "bad.jpg") << "not an image";
    }
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.errors, 1u);
    EXPECT_EQ(r.processed, 4u) << "one bad file must not stop the others";
    EXPECT_TRUE(r.completed);
    EXPECT_EQ(db.count(), 4) << "the undecodable file must not get a row";
}

TEST(ScanEngine, TheFailingFilesWarningReachesTheCallerOnTheQueue) {
    ScratchCollection c(3);
    {
        std::ofstream(c.dir / "broken.jpg") << "not an image";
    }
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    Drain drain(q);
    ScanResult r = ScanEngine(c.options(), db).run(cancel, &q);
    auto seen = drain.finish();

    ASSERT_EQ(r.errors, 1u);
    // Exactly one update must carry the error text, it must name the file, and
    // every other update must carry none. Without this the caller can print
    // "Errors: 1" and never say which file or why.
    std::vector<std::string> errors;
    for (const auto& s : seen) {
        if (!s.error.empty()) errors.push_back(s.error);
    }
    ASSERT_EQ(errors.size(), 1u) << "one failure, one error message";
    EXPECT_NE(errors[0].find("broken.jpg"), std::string::npos)
        << "the message must name the file: " << errors[0];
    EXPECT_NE(errors[0].find("scan: "), std::string::npos) << errors[0];
    // The named file is relative to the scan directory, matching what the CLI
    // shows: "scan: broken.jpg -- ...", not "scan: /tmp/.../broken.jpg -- ...".
    EXPECT_EQ(errors[0].substr(0, std::string("scan: broken.jpg").size()), "scan: broken.jpg")
        << errors[0];
}

TEST(ScanEngine, FailOnErrorThrowsAndCommitsWhatCompleted) {
    // The bad file is written first and the good copies after it, so that on a
    // filesystem whose readdir order is not creation order the good files still
    // have a chance of being walked first. Where they land is then measured
    // rather than assumed, because scan_directory does not sort.
    ScratchCollection c(0);
    const fs::path bad = c.dir / "broken.jpg";
    {
        std::ofstream(bad) << "not an image";
    }
    for (int i = 0; i < 3; ++i) {
        fs::copy_file("tests/data/test_red.mov", c.dir / ("v" + std::to_string(i) + ".mov"));
    }

    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanOptions o = c.options();
    o.fail_on_error = true;
    o.parallel = 1;  // one worker, so the walk order is the processing order

    auto walk = scan_directory(o.directory, o.recursive, o.include_hidden);
    size_t good_before = walk.size();
    for (size_t i = 0; i < walk.size(); ++i) {
        if (walk[i].filename() == bad.filename()) {
            good_before = i;
            break;
        }
    }
    ASSERT_LT(good_before, walk.size()) << "the bad file must appear in the walk";
    ASSERT_GT(good_before, 0u) << "at least one good file must be walked before the bad one, or "
                                  "nothing completes before the throw and the assertion below "
                                  "would hold even with the recovery flush removed";

    EXPECT_THROW(ScanEngine(o, db).run(cancel, nullptr), std::exception);
    // Whatever completed before the throw must have been flushed, not dropped.
    // The batch never reaches batch_size (10) with only 4 files, so the engine's
    // recovery flush after catching the worker exception is the only thing that
    // can have committed these rows. Delete that flush and this drops to 0.
    EXPECT_EQ(static_cast<size_t>(db.count()), good_before)
        << "work completed before the failure must be committed";
    for (const auto& img : db.get_all_images()) {
        EXPECT_NE(img.path.find(".mov"), std::string::npos)
            << "the undecodable file must not have produced a row: " << img.path;
    }
}

TEST(ScanEngine, RemovesRowsForFilesThatDisappeared) {
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 6);

    // Delete two files, leaving four, so the mount-loss guard does not fire.
    fs::remove(c.dir / "v0.mov");
    fs::remove(c.dir / "v1.mov");

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_EQ(r.removed, 2u);
    EXPECT_FALSE(r.mount_warning);
    EXPECT_EQ(db.count(), 4);
    // The four survivors must be exactly the files still on disk.
    for (const auto& img : db.get_all_images()) {
        EXPECT_TRUE(fs::exists(img.path)) << img.path << " was kept but is gone";
    }
}

TEST(ScanEngine, RunWorkersAloneDeletesNothing) {
    // SAFETY property: the row-deleting phase is a separate call, so a caller
    // that has a signal handler mapped onto the CancelToken can drop it in
    // between. If reconcile ever moves back inside run_workers the CLI's guard
    // would still be installed while metadata is deleted, and a Ctrl-C there
    // would be swallowed by a token that phase does not read.
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 6);

    fs::remove(c.dir / "v0.mov");
    fs::remove(c.dir / "v1.mov");

    ScanEngine engine(c.options(), db);
    ScanResult r = engine.run_workers(cancel, nullptr);
    EXPECT_EQ(r.removed, 0u) << "run_workers must not reconcile";
    EXPECT_EQ(db.count(), 6) << "run_workers must not delete a single row";

    engine.run_reconcile(r);
    EXPECT_EQ(r.removed, 2u);
    EXPECT_EQ(db.count(), 4);
}

TEST(ScanEngine, ReconcileStillRunsAfterCancellation) {
    // Cancellation stops the worker phase, not the cleanup phase: a run that was
    // interrupted still removes rows for files that are gone. Skipping cleanup on
    // cancellation would leave a rescan-after-Ctrl-C reporting Removed: 0.
    ScratchCollection c(6);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 6);

    fs::remove(c.dir / "v0.mov");
    fs::remove(c.dir / "v1.mov");

    cancel.cancel();
    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_FALSE(r.completed);
    EXPECT_EQ(r.processed, 0u) << "no file may be processed after cancellation";
    EXPECT_EQ(r.removed, 2u) << "cleanup must run even though the scan was cancelled";
    EXPECT_EQ(db.count(), 4);
}

TEST(ScanEngine, MountLossGuardKeepsRowsWhenEveryFileVanishes) {
    ScratchCollection c(5);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 5);

    for (int i = 0; i < 5; ++i) {
        fs::remove(c.dir / ("v" + std::to_string(i) + ".mov"));
    }

    ScanResult r = ScanEngine(c.options(), db).run(cancel, nullptr);
    EXPECT_TRUE(r.mount_warning) << "all rows vanishing should trip the guard";
    EXPECT_EQ(r.removed, 0u);
    EXPECT_EQ(db.count(), 5) << "an unmounted drive must not wipe metadata";
}

TEST(ScanEngine, IgnoreMountWarningOverridesTheGuard) {
    ScratchCollection c(5);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ScanEngine(c.options(), db).run(cancel, nullptr);
    ASSERT_EQ(db.count(), 5);

    for (int i = 0; i < 5; ++i) {
        fs::remove(c.dir / ("v" + std::to_string(i) + ".mov"));
    }

    ScanOptions o = c.options();
    o.ignore_mount_warning = true;
    ScanResult r = ScanEngine(o, db).run(cancel, nullptr);
    EXPECT_FALSE(r.mount_warning);
    EXPECT_EQ(r.removed, 5u);
    EXPECT_EQ(db.count(), 0);
}

// Inserts a row directly (no real scan) so a "file moved" state can be built
// without decoding anything: extension alone drives RAW/JPEG classification.
// When on_disk, also writes the file the row points at.
static std::string insert_row(Database& db, const fs::path& dir, const std::string& rel,
                              const std::string& sha, bool on_disk) {
    fs::path p = dir / rel;
    if (on_disk) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "bytes-" << sha;
    } else {
        fs::create_directories(p.parent_path());  // parent must exist for reconcile to delete it
    }
    ImageInfo info;
    info.path = p.string();
    info.filename = p.filename().string();
    info.extension = p.extension().string().substr(1);
    info.file_size = 10;
    info.created_at = "2024-01-01T00:00:00Z";
    info.modified_at = "2024-01-01T00:00:00Z";
    info.sha256 = sha;
    info.media_type = "image";
    db.insert(info);
    return p.string();
}

// Scratch dir + XDG_STATE_HOME redirect for one reconcile-ordering test.
// Destructor covers every exit path (normal return, ASSERT early-return,
// thrown exception) so a failure never leaves the env var pointing at a
// removed directory or leaks the directory itself.
struct ReconcileOrderScratch {
    fs::path dir;
    std::string orig_state;
    bool had_orig_state;

    ReconcileOrderScratch() {
        // Via make_temp_dir, not fs::temp_directory_path() directly: the helper
        // returns a canonical path, and ScanEngine::run_reconcile builds its
        // prefix with fs::canonical. On macOS the temp root is under /var, a
        // symlink to /private/var, so a raw path never matches the stored rows
        // and reconcile silently finds nothing to do.
        dir = make_temp_dir("reconcile-order");

        const char* xdg = std::getenv("XDG_STATE_HOME");
        had_orig_state = xdg != nullptr;
        if (xdg) orig_state = xdg;
        setenv("XDG_STATE_HOME", dir.c_str(), 1);
    }
    ~ReconcileOrderScratch() {
        if (had_orig_state) {
            setenv("XDG_STATE_HOME", orig_state.c_str(), 1);
        } else {
            unsetenv("XDG_STATE_HOME");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// Drives the same relink/demote overlap through ScanEngine::run_reconcile
// with the given log_enabled setting, and returns the surviving gallery value
// on the RAW row both operations target. Shared by the logged- and
// unlogged-branch tests below.
static std::string run_reconcile_order_probe(bool log_enabled) {
    ReconcileOrderScratch scratch;
    const fs::path& dir = scratch.dir;

    std::string db_path = (dir / "phig.db").string();
    Database db(db_path);
    db.init_schema();

    std::string raw_old = insert_row(db, dir, "old/a.cr2", "rawSha", /*on_disk=*/false);
    std::string raw_new = insert_row(db, dir, "new/a.cr2", "rawSha", /*on_disk=*/true);
    std::string jpeg = insert_row(db, dir, "new/a.jpg", "jpegSha", /*on_disk=*/true);
    db.set_tag({raw_old}, "favorite", true);
    db.set_gallery({raw_old}, "trip1", false);
    db.set_gallery({jpeg}, "trip2", false);

    ScanOptions o;
    o.directory = dir.string();
    o.db_path = db_path;
    o.log_enabled = log_enabled;
    o.argv_str = "phig scan test";

    ScanEngine engine(o, db);
    ScanResult r;
    engine.run_reconcile(r);

    EXPECT_FALSE(r.mount_warning);
    EXPECT_EQ(r.relinked, 1u);
    EXPECT_EQ(r.demoted, 1u);
    EXPECT_EQ(r.removed, 1u);  // raw_old; demoted rows are counted separately in r.demoted
    EXPECT_EQ(db.count(), 1);

    auto tags = db.get_tags_for_image(raw_new);
    EXPECT_EQ(tags.size(), 1u);
    if (!tags.empty()) EXPECT_EQ(tags[0], "favorite");

    std::string gallery;
    for (const auto& img : db.get_all_images()) {
        if (img.path == raw_new) gallery = img.gallery;
    }
    return gallery;
}

TEST(ScanEngine, ReconcileRunsRelinkBeforeDemoteOnSharedTarget) {
    // Integration-level sibling of RelinkTest.RelinkRunsBeforeDemoteOnSharedTarget
    // (tests/test_relink.cpp): same overlap state, driven through
    // ScanEngine::run_reconcile instead of calling relink_moved_rows/
    // demote_jpeg_rows directly, so a swap of the two calls inside reconcile's
    // logged branch is observable here.
    //
    // A RAW file moved (old/a.cr2 -> new/a.cr2, same sha) in the same scan
    // that also finds a JPEG sitting next to the RAW's new location. Both
    // relink and demote transfer metadata onto the RAW row at new/a.cr2;
    // gallery transfer only fills a NULL target, so whichever runs first
    // determines the surviving gallery.
    std::string gallery = run_reconcile_order_probe(/*log_enabled=*/true);
    EXPECT_EQ(gallery, "trip1") << "relink must fill the gallery before demote sees it, "
                                   "otherwise the JPEG's gallery wins instead";
}

TEST(ScanEngine, ReconcileRunsRelinkBeforeDemoteOnSharedTargetUnlogged) {
    // Same state and ordering claim as the logged-branch test above, but with
    // log_enabled = false, which takes reconcile's other branch — a separate
    // relink_moved_rows/demote_jpeg_rows call pair with the same ordering.
    std::string gallery = run_reconcile_order_probe(/*log_enabled=*/false);
    EXPECT_EQ(gallery, "trip1") << "relink must fill the gallery before demote sees it, "
                                   "otherwise the JPEG's gallery wins instead";
}

TEST(ScanEngine, MediaFilterExcludesTheOtherType) {
    ScratchCollection c(3);
    {
        std::ofstream(c.dir / "note.jpg") << "not an image";
    }
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanOptions o = c.options();
    o.media = "video";
    ScanResult r = ScanEngine(o, db).run(cancel, nullptr);
    EXPECT_EQ(r.total_files, 3u) << "the .jpg must be filtered out before processing";
    EXPECT_EQ(r.errors, 0u) << "a filtered-out undecodable file cannot produce an error";
    EXPECT_EQ(r.processed, 3u);
}

// ---- Per-worker FaceDetector isolation ----
//
// `worker_id` exists so each worker thread gets its OWN FaceDetector: the models
// carry ~150MB of mutable OpenCV DNN state and cv::FaceDetectorYN is not
// thread-safe, so sharing one instance across N threads is a data race. This is
// also the only reason the engine needs parallel_for_each's two-argument
// callback. The two tests below pin the two halves of that invariant.

TEST(ScanEngine, DetectorForWorkerGivesEachWorkerItsOwnInstance) {
    // The mapping worker_id -> detector, tested without the models: constructing a
    // real FaceDetector requires them, but the selection logic does not, so it
    // lives in a free function. Distinct non-null addresses stand in for the real
    // instances; nothing is constructed or destroyed, so they are released.
    std::vector<std::unique_ptr<FaceDetector>> detectors;
    std::vector<FaceDetector*> raw;
    for (int i = 0; i < 4; ++i) {
        auto* p = static_cast<FaceDetector*>(::operator new(sizeof(FaceDetector)));
        raw.push_back(p);
        detectors.emplace_back(p);
    }

    // Each worker must get ITS OWN instance. Collapsing the index (the classic
    // `worker_id * 0` slip) hands worker 0's detector to every worker, which is a
    // data race on ~150MB of mutable OpenCV model state.
    for (int w = 0; w < 4; ++w) {
        EXPECT_EQ(detector_for_worker(detectors, w), raw[w]) << "worker " << w;
    }
    // Stated as distinctness too, so the test reads as the invariant it protects.
    std::set<FaceDetector*> seen;
    for (int w = 0; w < 4; ++w) seen.insert(detector_for_worker(detectors, w));
    EXPECT_EQ(seen.size(), 4u) << "workers must not share a FaceDetector";

    // Out of range must throw rather than quietly return a plausible instance.
    EXPECT_THROW(detector_for_worker(detectors, 4), std::out_of_range);
    EXPECT_THROW(detector_for_worker(detectors, -1), std::out_of_range);

    for (auto& d : detectors) ::operator delete(d.release());
}

TEST(ScanEngine, DetectorForWorkerIsNullWhenFacesWereNotRequested) {
    std::vector<std::unique_ptr<FaceDetector>> none;
    EXPECT_EQ(detector_for_worker(none, 0), nullptr);
    EXPECT_EQ(detector_for_worker(none, 7), nullptr) << "no detectors means no faces, not a throw";
}

TEST(ScanEngine, ConstructsOneFaceDetectorPerWorker) {
    // The announced count is observable without models, because the engine
    // publishes it before it starts loading.
    ScratchCollection c(3);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    ScanOptions o = c.options();
    o.faces = true;
    o.parallel = 3;

    Drain drain(q);
    try {
        ScanEngine(o, db).run_workers(cancel, &q);
    } catch (const std::exception&) {
        // Expected where models are absent: the detector constructor throws. The
        // stage update below was already published, which is the point.
    }
    auto seen = drain.finish();

    size_t loading_updates = 0;
    for (const auto& s : seen) {
        if (s.stage != ScanStage::LoadingFaceModels) continue;
        ++loading_updates;
        EXPECT_EQ(s.total, 3u) << "one FaceDetector per worker, not a shared one";
    }
    ASSERT_EQ(loading_updates, 1u) << "faces mode must announce model loading exactly once";
}

TEST(ScanEngine, DoesNotAnnounceModelLoadingWithoutTheFacesFlag) {
    // The counterpart: no faces flag, no detectors, no announcement. Without this
    // the test above could be satisfied by always publishing the stage.
    ScratchCollection c(3);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;
    ProgressQueue<ScanProgress> q(1024);

    Drain drain(q);
    ScanEngine(c.options(), db).run_workers(cancel, &q);
    auto seen = drain.finish();

    for (const auto& s : seen) {
        EXPECT_NE(s.stage, ScanStage::LoadingFaceModels)
            << "model loading must not be announced when --faces was not given";
    }
}

TEST(ScanEngine, FacesModeFailsClearlyWhenModelsAreMissing) {
    if (face_models_exist()) {
        GTEST_SKIP() << "models are installed; this test covers the missing-models path";
    }
    ScratchCollection c(2);
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanOptions o = c.options();
    o.faces = true;
    o.parallel = 2;

    // The failure must be the actionable "run phig models download" message, and
    // it must arrive before any file is touched, so nothing is half-written.
    try {
        ScanEngine(o, db).run_workers(cancel, nullptr);
        FAIL() << "faces mode must not silently proceed without models";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("models download"), std::string::npos) << e.what();
    }
    EXPECT_EQ(db.count(), 0) << "nothing may be written when model loading fails";
}

TEST(ScanEngine, DetectsFacesWithMultipleWorkers) {
    // Exercises the real `detector_for_worker(face_detectors, worker_id)` CALL with
    // more than one worker — the one piece the unit test above cannot reach, since
    // it verifies the mapping but not that the call site passes the right worker_id.
    //
    // The two fixtures have DIFFERENT dimensions on purpose. FaceDetector caches the
    // last input size and calls setInputSize only when it changes, so two workers
    // sharing one instance interleave that cached size with detect(). Identical
    // images would leave the cached size constant and hide the sharing entirely.
    if (!face_models_exist()) {
        GTEST_SKIP() << "face models not found at " << yunet_model_path()
                     << "; run `phig models download` to enable this test";
    }
    // Downscaled from OpenCV's BSD-licensed sample data (samples/data/lena.jpg and
    // messi5.jpg), the only images to hand that YuNet actually detects. Committed
    // rather than read from the OpenCV package, which is not a declared dependency.
    const fs::path small = "tests/data/faces_small.jpg";  // 320x320, 1 face
    const fs::path large = "tests/data/faces_large.jpg";  // 448x280, 2 faces
    ASSERT_TRUE(fs::exists(small)) << small;
    ASSERT_TRUE(fs::exists(large)) << large;

    ScratchCollection c(0);
    // Alternate the two sizes so the workers keep forcing setInputSize changes.
    const int PAIRS = 6;
    for (int i = 0; i < PAIRS; ++i) {
        fs::copy_file(small, c.dir / ("s" + std::to_string(i) + ".jpg"));
        fs::copy_file(large, c.dir / ("l" + std::to_string(i) + ".jpg"));
    }
    Database db(c.db_path);
    db.init_schema();
    CancelToken cancel;

    ScanOptions o = c.options();
    o.faces = true;
    o.parallel = 2;

    ScanResult r = ScanEngine(o, db).run_workers(cancel, nullptr);
    EXPECT_EQ(r.errors, 0u) << "sharing one detector across workers races on cached input size";
    EXPECT_EQ(r.processed, static_cast<size_t>(2 * PAIRS));
    // 1 face per small + 2 per large. An exact count is the assertion with teeth:
    // a shared detector run against alternating input sizes drops or duplicates
    // detections rather than failing outright.
    EXPECT_EQ(r.faces, static_cast<size_t>(3 * PAIRS))
        << "every face must be recorded exactly once";
    for (const auto& img : db.get_all_images()) {
        EXPECT_TRUE(db.has_faces(img.path)) << img.path << " got no faces";
    }
}

// ---- Live Photo pairing (plan 038) ----
//
// A Live Photo is one shot in two files. The still is indexed and the MOV is a
// carried attachment, so a scan must produce one row, not two. Real exported
// files only: a synthetic MOV can carry the identifier but no committable
// fixture has an Apple MakerNote, so the still half cannot be faked.

namespace {
std::string live_samples() { return std::string(getenv("HOME") ?: "") + "/phig-live-samples"; }
}  // namespace

TEST(ScanEngineLivePhoto, IndexesTheStillAndNotItsPairedVideo) {
    const std::string samples = live_samples();
    if (!fs::exists(samples + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples";

    fs::path dir = make_temp_dir("scan-live");
    fs::copy_file(samples + "/IMG_1112.HEIC", dir / "IMG_1112.HEIC");
    fs::copy_file(samples + "/IMG_1112.MOV", dir / "IMG_1112.MOV");

    ScanOptions o;
    o.directory = dir.string();
    o.db_path = (dir / "phig.db").string();
    o.recursive = false;
    o.parallel = 2;
    o.log_enabled = false;

    Database db(o.db_path);
    db.init_schema();
    ScanEngine engine(o, db);
    CancelToken token;
    engine.run(token, nullptr);

    auto rows = db.get_all_images();
    ASSERT_EQ(rows.size(), 1u) << "a Live Photo must index as one row, not two";
    EXPECT_EQ(fs::path(rows[0].path).filename(), "IMG_1112.HEIC") << "the still is the master";
    EXPECT_TRUE(fs::exists(dir / "IMG_1112.MOV")) << "the video must stay on disk, just unindexed";

    fs::remove_all(dir);
}

// The guard that matters most: an ordinary camera clip carries no identifier
// and must keep its own row. This is the MVI_*.MOV mis-pairing that ruled out
// same-basename matching.
TEST(ScanEngineLivePhoto, LeavesAnOrdinaryVideoIndexed) {
    fs::path dir = make_temp_dir("scan-live-plain");
    fs::copy_file("tests/data/test_red.mov", dir / "IMG_0001.MOV");
    fs::copy_file("tests/data/orient_1.heic", dir / "IMG_0001.HEIC");

    ScanOptions o;
    o.directory = dir.string();
    o.db_path = (dir / "phig.db").string();
    o.recursive = false;
    o.parallel = 2;
    o.log_enabled = false;

    Database db(o.db_path);
    db.init_schema();
    ScanEngine engine(o, db);
    CancelToken token;
    engine.run(token, nullptr);

    EXPECT_EQ(db.get_all_images().size(), 2u)
        << "same basename is not pairing evidence; both must be indexed";
    fs::remove_all(dir);
}
