#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "face_path.h"
#include "paths.h"
#include "phig/phig.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A scratch collection plus a redirected thumbnail cache, so a thumbs operation
// writes into the fixture instead of the user's real cache.
struct OpFixture {
    fs::path dir;
    std::string db_path;
    std::string orig_cache;
    bool had_cache = false;

    explicit OpFixture(int images = 0) {
        dir = make_temp_dir("libphig-ops");
        db_path = (dir / "phig.db").string();

        const char* xdg = std::getenv("XDG_CACHE_HOME");
        had_cache = xdg != nullptr;
        if (xdg) orig_cache = xdg;
        setenv("XDG_CACHE_HOME", dir.c_str(), 1);

        Database db(db_path);
        db.init_schema();
        for (int i = 0; i < images; ++i) {
            fs::path p = dir / ("img" + std::to_string(i) + ".jpg");
            fs::copy_file("tests/data/faces_small.jpg", p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            info.sha256 = std::string(62, '0') + (i < 10 ? "0" : "") + std::to_string(i);
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            db.insert(info);
        }
    }

    ~OpFixture() {
        if (had_cache) {
            setenv("XDG_CACHE_HOME", orig_cache.c_str(), 1);
        } else {
            unsetenv("XDG_CACHE_HOME");
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Adds a loose file on disk that no row references, for index to find.
    void add_unindexed(const std::string& name) {
        fs::copy_file("tests/data/faces_small.jpg", dir / name);
    }

    // Adds an indexed row whose file will not decode, so an operation counts it
    // as a failure and publishes the reason.
    void add_broken_row(const std::string& name) {
        fs::path junk = dir / name;
        {
            std::ofstream(junk) << "not an image";
        }
        Database db(db_path);
        ImageInfo info;
        info.path = junk.string();
        info.filename = name;
        info.extension = "jpg";
        info.file_size = fs::file_size(junk);
        info.media_type = "image";
        // Distinct per row: thumbs keys the cache on sha256, so a shared one
        // would make the second file look already-generated.
        info.sha256 = "f" + std::string(63 - std::to_string(broken_count).size(), 'f') +
                      std::to_string(broken_count);
        ++broken_count;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        db.insert(info);
    }
    int broken_count = 0;

    phig_ctx* open(phig_access_mode mode = PHIG_READ_WRITE) {
        phig_config cfg{};
        cfg.db_path = db_path.c_str();
        cfg.mode = mode;
        phig_ctx* ctx = nullptr;
        EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
        return ctx;
    }
};

// Every id in the collection. These tests predate id selection and meant "the
// whole collection" by setting no filter at all; NULL/0 now selects nothing, so
// what they meant has to be said. Resolving through phig_query_ids is also how
// a real client would do it.
struct AllIds {
    phig_id_list* list = nullptr;
    explicit AllIds(phig_ctx* ctx) {
        phig_query* q = nullptr;
        EXPECT_EQ(phig_query_new(ctx, &q), PHIG_OK);
        EXPECT_EQ(phig_query_ids(ctx, q, &list), PHIG_OK);
        phig_query_free(q);
    }
    ~AllIds() { phig_id_list_free(list); }
    AllIds(const AllIds&) = delete;
    AllIds& operator=(const AllIds&) = delete;
};

// `ids` may be null, which selects nothing — the right default for the tests
// that assert an operation did no work.
phig_thumbs_options default_thumbs(const phig_id_list* ids = nullptr) {
    phig_thumbs_options o{};
    if (ids != nullptr) {
        o.ids = ids->items;
        o.id_count = static_cast<size_t>(ids->count);
    }
    o.size = 0;
    o.force = false;
    o.parallel = 1;
    return o;
}

// `dir` must outlive the returned struct: it holds a pointer into it, which is
// exactly the lifetime the ABI documents for the _start call. Callers keep a
// named std::string rather than passing a temporary.
phig_index_options default_index(const std::string& dir) {
    phig_index_options o{};
    o.directory = dir.c_str();
    o.recursive = false;
    o.include_hidden = false;
    o.force = false;
    o.media = PHIG_MEDIA_ANY;
    o.parallel = 1;
    return o;
}

}  // namespace

TEST(AbiOperations, ThumbsRunsToCompletionAndReportsCounts) {
    OpFixture fx(3);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    ASSERT_NE(op, nullptr);
    phig_op_wait(op);

    EXPECT_EQ(phig_op_status(op), PHIG_OP_DONE);
    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    EXPECT_EQ(r.total_selected, 3u);
    EXPECT_EQ(r.generated, 3u);
    EXPECT_EQ(r.errors, 0u);
    EXPECT_TRUE(r.completed);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, IndexAddsFilesFoundOnDisk) {
    OpFixture fx;
    fx.add_unindexed("a.jpg");
    fx.add_unindexed("b.jpg");
    phig_ctx* ctx = fx.open();
    const std::string scan_dir = fx.dir.string();
    phig_index_options opts = default_index(scan_dir);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    EXPECT_EQ(phig_op_status(op), PHIG_OP_DONE);
    phig_index_result r{};
    ASSERT_EQ(phig_op_index_result(op, &r), PHIG_OK);
    EXPECT_EQ(r.total_files, 2u);
    EXPECT_EQ(r.processed, 2u);
    EXPECT_TRUE(r.completed);
    phig_op_free(op);

    // The rows are really there, through a second connection.
    Database db(fx.db_path);
    EXPECT_EQ(db.get_all_images().size(), 2u);
    phig_shutdown(ctx);
}

TEST(AbiOperations, PollDrainsProgressAndCountersAreAbsolute) {
    OpFixture fx(4);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    size_t file_done = 0;
    bool saw_selected = false;
    uint64_t last_done = 0;
    phig_op_progress p{};
    while (phig_op_poll(op, &p)) {
        if (p.stage == PHIG_STAGE_SELECTED) {
            saw_selected = true;
            EXPECT_EQ(p.total, 4u);
        } else if (p.stage == PHIG_STAGE_FILE_DONE) {
            ++file_done;
            // Absolute, so they never go backwards even if updates are dropped.
            EXPECT_GE(p.done, last_done);
            last_done = p.done;
            EXPECT_NE(p.path, nullptr);
            EXPECT_NE(p.error, nullptr);
        }
    }
    EXPECT_TRUE(saw_selected);
    EXPECT_EQ(file_done, 4u);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, PollReturnsFalseWhenNothingIsQueued) {
    OpFixture fx(1);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_op_progress p{};
    while (phig_op_poll(op, &p)) {
    }
    // Drained: false means "nothing new", not "finished".
    EXPECT_FALSE(phig_op_poll(op, &p));
    EXPECT_EQ(phig_op_status(op), PHIG_OP_DONE);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, CancelBeforeAnyWorkReportsCancelledAndIncomplete) {
    OpFixture fx(8);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_cancel(op);
    phig_op_wait(op);

    EXPECT_EQ(phig_op_status(op), PHIG_OP_CANCELLED);
    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    EXPECT_FALSE(r.completed);
    // Measured: the cancel reliably lands before the parallel loop pulls its
    // first item, so this covers cancel-before-work only. Partial work is
    // CancelMidRunKeepsTheWorkAlreadyDone below.
    EXPECT_LT(r.generated, r.total_selected);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, CancelMidRunKeepsTheWorkAlreadyDone) {
    // The header's claim: "whatever completed before the cancellation is
    // committed, not rolled back". Cancelling immediately never exercises it,
    // because no item has started. Wait for real progress first, then cancel.
    OpFixture fx(24);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);

    uint64_t seen = 0;
    phig_op_progress p{};
    while (phig_op_status(op) == PHIG_OP_RUNNING) {
        if (phig_op_poll(op, &p) && p.stage == PHIG_STAGE_FILE_DONE && p.processed > 0) {
            seen = p.processed;
            break;
        }
    }
    phig_op_cancel(op);
    phig_op_wait(op);

    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    if (seen > 0) {
        // Thumbnails written before the cancel are still written, and the count
        // never goes backwards from what progress already reported.
        EXPECT_GE(r.generated, seen);
        EXPECT_GT(r.generated, 0u);
    }
    // Either it was cancelled partway, or it finished first on a fast machine.
    // What must never happen is completed = true alongside CANCELLED.
    if (phig_op_status(op) == PHIG_OP_CANCELLED) {
        EXPECT_FALSE(r.completed);
    } else {
        EXPECT_EQ(phig_op_status(op), PHIG_OP_DONE);
        EXPECT_TRUE(r.completed);
    }

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, CancelIsIdempotentAndSafeAfterCompletion) {
    OpFixture fx(1);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    // Already finished: these must be no-ops, not crashes.
    phig_op_cancel(op);
    phig_op_cancel(op);
    phig_op_wait(op);
    EXPECT_EQ(phig_op_status(op), PHIG_OP_DONE);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, FreeWhileRunningWaitsForTheWorker) {
    // The worker holds a raw pointer to the handle and its Database, so free
    // must join rather than detach. Under ASan/TSan this is where a
    // use-after-free would show up.
    OpFixture fx(8);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_free(op);  // no cancel, no wait — must still return cleanly

    phig_shutdown(ctx);
}

TEST(AbiOperations, ResultBeforeCompletionIsRejected) {
    OpFixture fx(8);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);

    // Which branch runs is racy — the operation may finish first on a fast
    // machine — so assert the pairing rather than the code. Either way the
    // caller must never be handed a half-written result.
    phig_thumbs_result r{};
    phig_status st = phig_op_thumbs_result(op, &r);
    if (st == PHIG_ERR_INVALID_ARG) {
        EXPECT_EQ(r.total_selected, 0u) << "a rejected getter must not write to out";
        EXPECT_EQ(r.generated, 0u);
        EXPECT_FALSE(r.completed);
    } else {
        EXPECT_EQ(st, PHIG_OK);
        EXPECT_NE(phig_op_status(op), PHIG_OP_RUNNING) << "a result was handed out mid-run";
    }

    phig_op_cancel(op);
    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, WrongResultGetterForTheOperationKindIsRejected) {
    OpFixture fx(1);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_index_result ir{};
    phig_faces_result fr{};
    EXPECT_EQ(phig_op_index_result(op, &ir), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_faces_result(op, &fr), PHIG_ERR_INVALID_ARG);
    phig_thumbs_result tr{};
    EXPECT_EQ(phig_op_thumbs_result(op, &tr), PHIG_OK);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, ReadOnlyContextRefusesEveryOperation) {
    OpFixture fx(1);
    {
        phig_ctx* rw = fx.open();  // migrate first; a read-only ctx cannot
        phig_shutdown(rw);
    }
    phig_ctx* ctx = fx.open(PHIG_READ_ONLY);

    phig_op* op = nullptr;
    phig_thumbs_options topts = default_thumbs();
    EXPECT_EQ(phig_op_thumbs_start(ctx, &topts, &op), PHIG_ERR_READONLY);
    EXPECT_EQ(op, nullptr);

    const std::string scan_dir = fx.dir.string();
    phig_index_options iopts = default_index(scan_dir);
    EXPECT_EQ(phig_op_index_start(ctx, &iopts, &op), PHIG_ERR_READONLY);
    EXPECT_EQ(op, nullptr);

    phig_faces_options fopts{};
    fopts.parallel = 1;
    EXPECT_EQ(phig_op_faces_start(ctx, &fopts, &op), PHIG_ERR_READONLY);
    EXPECT_EQ(op, nullptr);

    phig_shutdown(ctx);
}

TEST(AbiOperations, RejectsBadArguments) {
    OpFixture fx(1);
    phig_ctx* ctx = fx.open();
    phig_op* op = nullptr;

    phig_thumbs_options topts = default_thumbs();
    EXPECT_EQ(phig_op_thumbs_start(nullptr, &topts, &op), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_thumbs_start(ctx, nullptr, &op), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_op_thumbs_start(ctx, &topts, nullptr), PHIG_ERR_INVALID_ARG);

    const std::string scan_dir = fx.dir.string();
    phig_index_options iopts = default_index(scan_dir);
    iopts.directory = nullptr;
    EXPECT_EQ(phig_op_index_start(ctx, &iopts, &op), PHIG_ERR_INVALID_ARG);
    iopts.directory = "";
    EXPECT_EQ(phig_op_index_start(ctx, &iopts, &op), PHIG_ERR_INVALID_ARG);

    // Every accessor tolerates NULL rather than crashing.
    EXPECT_EQ(phig_op_status(nullptr), PHIG_OP_FAILED);
    phig_op_progress p{};
    EXPECT_FALSE(phig_op_poll(nullptr, &p));
    EXPECT_EQ(phig_op_error_count(nullptr), 0u);
    EXPECT_STREQ(phig_op_error_at(nullptr, 0), "");
    EXPECT_STREQ(phig_op_error(nullptr), "");
    phig_op_cancel(nullptr);
    phig_op_wait(nullptr);
    phig_op_free(nullptr);

    phig_shutdown(ctx);
}

TEST(AbiOperations, FailedStartClearsTheCallersHandle) {
    // "On failure *out is NULL" has to hold for the argument checks too, not
    // just the ones past them. A client that reuses one variable across calls
    // would otherwise be left holding the handle it already freed.
    OpFixture fx(1);
    phig_ctx* ctx = fx.open();

    phig_op* op = nullptr;
    phig_thumbs_options topts = default_thumbs();
    ASSERT_EQ(phig_op_thumbs_start(ctx, &topts, &op), PHIG_OK);
    phig_op_wait(op);
    phig_op_free(op);
    // `op` is now a stale pointer, exactly as a real client's variable would be.

    EXPECT_EQ(phig_op_thumbs_start(ctx, nullptr, &op), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(op, nullptr) << "a rejected start left the caller's stale handle in place";

    op = reinterpret_cast<phig_op*>(0x1);
    EXPECT_EQ(phig_op_thumbs_start(nullptr, &topts, &op), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(op, nullptr);

    const std::string scan_dir = fx.dir.string();
    phig_index_options iopts = default_index(scan_dir);
    iopts.directory = "";
    op = reinterpret_cast<phig_op*>(0x1);
    EXPECT_EQ(phig_op_index_start(ctx, &iopts, &op), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(op, nullptr);

    phig_faces_options fopts{};
    op = reinterpret_cast<phig_op*>(0x1);
    EXPECT_EQ(phig_op_faces_start(nullptr, &fopts, &op), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(op, nullptr);

    phig_shutdown(ctx);
}

TEST(AbiOperations, ErrorIsEmptyWhileRunningAndSetAfterAFailure) {
    // phig_op_error reads a string the worker writes, so it reports "" until the
    // terminal state publishes it. Calling it mid-run must be safe and honest,
    // not a race — this is the case ThreadSanitizer flags if the guard is gone.
    OpFixture fx;
    phig_ctx* ctx = fx.open();
    std::string missing = (fx.dir / "no-such-dir").string();
    phig_index_options opts = default_index(missing);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &opts, &op), PHIG_OK);
    // Spin on it from this thread while the worker fails on its own. Asserting
    // "still running implies empty" would be its own race — the operation may
    // finish between the two calls — so assert the direction that cannot race:
    // a message is only ever published together with the terminal state, and
    // the state never returns to RUNNING.
    for (;;) {
        bool has_message = phig_op_error(op)[0] != '\0';
        phig_op_state st = phig_op_status(op);
        if (has_message) {
            EXPECT_NE(st, PHIG_OP_RUNNING) << "a message was readable before the terminal state";
            break;
        }
        if (st != PHIG_OP_RUNNING) break;
    }
    phig_op_wait(op);
    EXPECT_EQ(phig_op_status(op), PHIG_OP_FAILED);
    EXPECT_STRNE(phig_op_error(op), "");

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, FailedOperationReportsItsMessageAndRefusesAResult) {
    OpFixture fx;
    phig_ctx* ctx = fx.open();
    // A directory that does not exist: the engine throws out of the walk.
    std::string missing = (fx.dir / "no-such-dir").string();
    phig_index_options opts = default_index(missing);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    EXPECT_EQ(phig_op_status(op), PHIG_OP_FAILED);
    EXPECT_STRNE(phig_op_error(op), "");
    phig_index_result r{};
    EXPECT_EQ(phig_op_index_result(op, &r), PHIG_ERR_INTERNAL);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, PerFileErrorsAreRetainedForInspection) {
    OpFixture fx(1);
    fx.add_broken_row("broken.jpg");

    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_op_progress p{};
    while (phig_op_poll(op, &p)) {
    }

    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    EXPECT_EQ(r.errors, 1u);
    ASSERT_EQ(phig_op_error_count(op), 1u);
    EXPECT_NE(std::string(phig_op_error_at(op, 0)).find("broken.jpg"), std::string::npos);
    // Out of range is empty, not a crash.
    EXPECT_STREQ(phig_op_error_at(op, 99), "");

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, ErrorsAreRetainedForACallerThatNeverPolled) {
    // The header's promise is for a caller that "was not polling" — a client
    // that starts an operation, waits, and reads the list. Retaining only as a
    // side effect of polling would leave that caller with an errors count and no
    // way to learn which files failed.
    OpFixture fx(1);
    fx.add_broken_row("broken.jpg");

    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);  // no poll, ever

    phig_thumbs_result r{};
    ASSERT_EQ(phig_op_thumbs_result(op, &r), PHIG_OK);
    EXPECT_EQ(r.errors, 1u);
    ASSERT_EQ(phig_op_error_count(op), 1u);
    EXPECT_NE(std::string(phig_op_error_at(op, 0)).find("broken.jpg"), std::string::npos);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, AFailureSeenByBothPollAndTheSweepIsRetainedOnce) {
    // Two producers fill the list: the caller's poll, and the worker sweeping
    // what was never polled. A file both of them see must appear once.
    OpFixture fx(1);
    fx.add_broken_row("broken.jpg");

    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    phig_op_progress p{};
    while (phig_op_poll(op, &p)) {
    }

    EXPECT_EQ(phig_op_error_count(op), 1u);

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, EveryRetainedErrorPointerStaysReadable) {
    // Each entry names its own file, so a pointer that had been invalidated by
    // the list growing would read as another entry's text or as garbage.
    OpFixture fx(1);
    for (int i = 0; i < 12; ++i) fx.add_broken_row("broken" + std::to_string(i) + ".jpg");

    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);
    phig_op_wait(op);

    std::vector<const char*> held;
    for (size_t i = 0; i < phig_op_error_count(op); ++i) held.push_back(phig_op_error_at(op, i));
    ASSERT_EQ(held.size(), 12u);

    std::vector<std::string> seen;
    for (const char* s : held) {
        ASSERT_NE(s, nullptr);
        seen.emplace_back(s);
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::unique(seen.begin(), seen.end()), seen.end())
        << "two retained pointers read the same text";

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, TwoDatabaseWritingOperationsRunConcurrentlyWithoutDeadlock) {
    // Two index operations, not index + thumbs: thumbs writes only the
    // filesystem cache, so pairing with it would never put two writers on the
    // database at once and the claim being tested — that SQLite serializes them
    // rather than one blocking forever — would go unexercised.
    OpFixture fx;
    for (int i = 0; i < 12; ++i) fx.add_unindexed("extra" + std::to_string(i) + ".jpg");
    phig_ctx* ctx = fx.open();

    const std::string scan_dir = fx.dir.string();
    phig_index_options a_opts = default_index(scan_dir);
    phig_index_options b_opts = default_index(scan_dir);
    b_opts.force = true;  // so the second one writes rather than skipping

    phig_op* a = nullptr;
    phig_op* b = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &a_opts, &a), PHIG_OK);
    ASSERT_EQ(phig_op_index_start(ctx, &b_opts, &b), PHIG_OK);

    phig_op_wait(a);
    phig_op_wait(b);

    EXPECT_NE(phig_op_status(a), PHIG_OP_FAILED) << "a: " << phig_op_error(a);
    EXPECT_NE(phig_op_status(b), PHIG_OP_FAILED) << "b: " << phig_op_error(b);

    // Both wrote the same files; the collection has each one once, not twice.
    Database after(fx.db_path);
    EXPECT_EQ(after.get_all_images().size(), 12u);

    phig_op_free(a);
    phig_op_free(b);
    phig_shutdown(ctx);
}

TEST(AbiOperations, MetadataWritesStayServicedWhileAnOperationRuns) {
    // The reason none of these take the writer lock: a client must be able to
    // keep tagging while a long operation runs. A lock-holding operation would
    // make this return PHIG_ERR_BUSY for the whole run. Driven by index, which
    // writes rows — thumbs would leave the database free and prove nothing about
    // a write landing beside one.
    OpFixture fx(8);
    for (int i = 0; i < 12; ++i) fx.add_unindexed("extra" + std::to_string(i) + ".jpg");
    phig_ctx* ctx = fx.open();
    Database probe(fx.db_path);
    std::vector<int64_t> ids;
    for (const auto& img : probe.get_all_images()) ids.push_back(probe.get_image_id(img.path));
    ASSERT_FALSE(ids.empty());

    const std::string scan_dir = fx.dir.string();
    phig_index_options opts = default_index(scan_dir);
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_index_start(ctx, &opts, &op), PHIG_OK);

    EXPECT_EQ(phig_tag_add(ctx, ids.data(), 1, "during-operation"), PHIG_OK);

    phig_op_wait(op);
    EXPECT_NE(phig_op_status(op), PHIG_OP_FAILED) << phig_op_error(op);
    phig_op_free(op);

    Database after(fx.db_path);
    EXPECT_FALSE(after.get_tags_for_image(probe.get_all_images()[0].path).empty());
    phig_shutdown(ctx);
}

TEST(AbiOperations, PolledStringsStayValidUntilTheNextPoll) {
    // The contract is that the bytes survive until the caller polls AGAIN, so
    // the pointer has to be taken while the worker is still producing updates
    // and read back after it has produced many more.
    OpFixture fx(16);
    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_thumbs_options opts = default_thumbs(all.list);

    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_thumbs_start(ctx, &opts, &op), PHIG_OK);

    phig_op_progress p{};
    bool got = false;
    for (;;) {
        if (phig_op_poll(op, &p)) {
            got = true;
            break;
        }
        if (phig_op_status(op) != PHIG_OP_RUNNING) {
            got = phig_op_poll(op, &p);  // whatever was queued as it finished
            break;
        }
    }
    ASSERT_TRUE(got) << "the operation published no progress at all";
    const char* held = p.path;
    const std::string copied = held;

    // The worker runs to completion, pushing many more updates, while the
    // caller holds the pointer and does not poll. Nothing may rewrite it.
    phig_op_wait(op);
    EXPECT_EQ(std::string(held), copied) << "a held pointer changed without a poll";

    phig_op_free(op);
    phig_shutdown(ctx);
}

TEST(AbiOperations, FacesResultReportsLabelsItCouldNotCarry) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    // OpFixture points XDG_CACHE_HOME into its scratch directory, which moves
    // models_dir() away from the installed models. Resolve them first, then
    // link them into the scratch cache. libphig.so keeps its own copy of the
    // path overrides, so set_path_overrides() here would not reach it.
    const std::string models = models_dir();
    OpFixture fx(1);
    fs::create_directories(fx.dir / "phig");
    fs::create_directory_symlink(models, fx.dir / "phig" / "models");

    {
        Database db(fx.db_path);
        int64_t image_id = db.get_image_id((fx.dir / "img0.jpg").string());
        ASSERT_GT(image_id, 0);
        FaceInfo phantom{};
        phantom.embedding.fill(0.5f);
        phantom.width = 4;
        phantom.height = 4;
        int64_t face_id = db.insert_face(image_id, phantom);
        ASSERT_TRUE(db.set_face_person(face_id, db.get_or_create_person("Ada")));
    }

    phig_ctx* ctx = fx.open();
    AllIds all(ctx);
    phig_faces_options fopts{};
    fopts.ids = all.list->items;
    fopts.id_count = static_cast<size_t>(all.list->count);
    fopts.parallel = 1;
    phig_op* op = nullptr;
    ASSERT_EQ(phig_op_faces_start(ctx, &fopts, &op), PHIG_OK);
    phig_op_wait(op);

    ASSERT_EQ(phig_op_status(op), PHIG_OP_DONE);
    phig_faces_result r{};
    ASSERT_EQ(phig_op_faces_result(op, &r), PHIG_OK);
    EXPECT_GT(r.faces_detected, 0u);
    EXPECT_EQ(r.labels_dropped, 1u);

    phig_op_free(op);
    phig_shutdown(ctx);
}
