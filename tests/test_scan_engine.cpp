#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"
#include "scan_engine.h"
#include "scanner.h"

namespace fs = std::filesystem;

namespace {
// A scratch directory with N tiny copies of a real fixture, removed on scope exit.
struct ScratchCollection {
    fs::path dir;
    std::string db_path;

    explicit ScratchCollection(int n) {
        dir = fs::temp_directory_path() / ("phig_scan_engine_" + std::to_string(::getpid()) + "_" +
                                           std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
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
