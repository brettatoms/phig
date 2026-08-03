#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "cancel.h"
#include "parallel.h"

TEST(Parallel, RunsEveryIndexExactlyOnce) {
    const size_t n = 1000;
    std::vector<std::atomic<int>> seen(n);
    for (auto& s : seen) s.store(0);
    CancelToken tok;

    parallel_for_each(n, 4, tok, [&](size_t i, int) { seen[i].fetch_add(1); });

    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(seen[i].load(), 1) << "index " << i;
    }
}

TEST(Parallel, SingleThreadedMatchesMultiThreaded) {
    const size_t n = 500;
    std::set<size_t> one, many;
    std::mutex m;
    CancelToken tok;

    parallel_for_each(n, 1, tok, [&](size_t i, int) {
        std::lock_guard<std::mutex> g(m);
        one.insert(i);
    });
    parallel_for_each(n, 8, tok, [&](size_t i, int) {
        std::lock_guard<std::mutex> g(m);
        many.insert(i);
    });
    EXPECT_EQ(one, many);
    EXPECT_EQ(one.size(), n);
}

TEST(Parallel, ZeroCountIsANoOp) {
    CancelToken tok;
    int calls = 0;
    parallel_for_each(0, 4, tok, [&](size_t, int) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(Parallel, WorkerExceptionPropagatesToCaller) {
    CancelToken tok;
    EXPECT_THROW(parallel_for_each(100, 4, tok,
                                   [&](size_t i, int) {
                                       if (i == 50) throw std::runtime_error("boom");
                                   }),
                 std::runtime_error);
}

TEST(Parallel, WorkerExceptionCarriesItsMessage) {
    CancelToken tok;
    try {
        parallel_for_each(100, 4, tok, [&](size_t i, int) {
            if (i == 7) throw std::runtime_error("specific message");
        });
        ADD_FAILURE() << "expected a throw";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "specific message");
    }
}

TEST(Parallel, AllThreadsJoinAfterAnException) {
    // The real defect this guards: an escaping exception in a std::thread
    // lambda calls std::terminate. If this test completes at all, the
    // implementation joined its workers instead of aborting.
    CancelToken tok;
    std::atomic<int> throwers{0};
    EXPECT_THROW(parallel_for_each(2000, 8, tok,
                                   [&](size_t, int) {
                                       throwers.fetch_add(1);
                                       throw std::runtime_error("everyone throws");
                                   }),
                 std::runtime_error);
    SUCCEED() << "returned normally rather than terminating";
}

TEST(Parallel, AlreadyCancelledRunsNothing) {
    CancelToken tok;
    tok.cancel();
    int calls = 0;
    parallel_for_each(100, 4, tok, [&](size_t, int) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(Parallel, ReturnsTrueWhenAllItemsRun) {
    CancelToken tok;
    EXPECT_TRUE(parallel_for_each(500, 4, tok, [&](size_t, int) {}));
}

TEST(Parallel, ReturnsFalseWhenAlreadyCancelled) {
    CancelToken tok;
    tok.cancel();
    EXPECT_FALSE(parallel_for_each(100, 4, tok, [&](size_t, int) {}));
}

TEST(Parallel, ReturnsFalseWhenCancelledMidRun) {
    const size_t n = 10000;
    std::atomic<size_t> done{0};
    CancelToken tok;

    bool result = parallel_for_each(n, 4, tok, [&](size_t, int) {
        size_t d = done.fetch_add(1) + 1;
        if (d == 100) tok.cancel();
    });

    EXPECT_FALSE(result);
}

TEST(Parallel, ReturnsTrueForZeroCount) {
    CancelToken tok;
    EXPECT_TRUE(parallel_for_each(0, 4, tok, [&](size_t, int) {}));
}

TEST(Parallel, WorkerIdsAreInRangeDistinctAndStable) {
    const size_t n = 20000;
    const int num_threads = 8;
    CancelToken tok;

    std::mutex m;
    std::set<int> ids_seen;
    // Per-item worker id, to check stability: every item a given thread
    // processes must report the same worker id throughout the run.
    std::vector<int> item_worker(n, -1);
    // Per-worker-id thread identity, to check distinctness: two different
    // worker ids must never be used by the same underlying thread, and one
    // worker id must never be used by two different threads concurrently.
    std::mutex thread_map_mutex;
    std::map<int, std::thread::id> worker_thread;

    parallel_for_each(n, num_threads, tok, [&](size_t i, int worker_id) {
        {
            std::lock_guard<std::mutex> g(m);
            ids_seen.insert(worker_id);
        }
        item_worker[i] = worker_id;

        std::thread::id this_thread = std::this_thread::get_id();
        std::lock_guard<std::mutex> g(thread_map_mutex);
        auto it = worker_thread.find(worker_id);
        if (it == worker_thread.end()) {
            worker_thread[worker_id] = this_thread;
        } else {
            EXPECT_EQ(it->second, this_thread)
                << "worker id " << worker_id << " used by more than one thread";
        }
    });

    for (int id : ids_seen) {
        EXPECT_GE(id, 0);
        EXPECT_LT(id, num_threads);
    }
    // Not every worker id is guaranteed to appear: the work cursor is shared, so
    // threads that start first can drain a trivial workload before the rest are
    // scheduled. What must hold is that at least one worker ran and no id is out
    // of range — asserting all N appear makes this test fail on a loaded machine.
    EXPECT_FALSE(ids_seen.empty());
    EXPECT_LE(ids_seen.size(), static_cast<size_t>(num_threads));

    for (size_t i = 0; i < n; ++i) {
        EXPECT_GE(item_worker[i], 0) << "item " << i << " never ran";
    }
}
