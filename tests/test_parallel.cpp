#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include "cancel.h"
#include "parallel.h"

TEST(Parallel, RunsEveryIndexExactlyOnce) {
    const size_t n = 1000;
    std::vector<std::atomic<int>> seen(n);
    for (auto& s : seen) s.store(0);
    CancelToken tok;

    parallel_for_each(n, 4, tok, [&](size_t i) { seen[i].fetch_add(1); });

    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(seen[i].load(), 1) << "index " << i;
    }
}

TEST(Parallel, SingleThreadedMatchesMultiThreaded) {
    const size_t n = 500;
    std::set<size_t> one, many;
    std::mutex m;
    CancelToken tok;

    parallel_for_each(n, 1, tok, [&](size_t i) {
        std::lock_guard<std::mutex> g(m);
        one.insert(i);
    });
    parallel_for_each(n, 8, tok, [&](size_t i) {
        std::lock_guard<std::mutex> g(m);
        many.insert(i);
    });
    EXPECT_EQ(one, many);
    EXPECT_EQ(one.size(), n);
}

TEST(Parallel, ZeroCountIsANoOp) {
    CancelToken tok;
    int calls = 0;
    parallel_for_each(0, 4, tok, [&](size_t) { ++calls; });
    EXPECT_EQ(calls, 0);
}

TEST(Parallel, CancellationStopsEarly) {
    const size_t n = 10000;
    std::atomic<size_t> done{0};
    CancelToken tok;

    parallel_for_each(n, 4, tok, [&](size_t) {
        size_t d = done.fetch_add(1) + 1;
        if (d == 100) tok.cancel();
    });

    // Workers in flight when the token flips still finish their current item,
    // so allow slack — the point is that it stopped well short of n.
    EXPECT_LT(done.load(), n);
    EXPECT_GE(done.load(), 100u);
}

TEST(Parallel, WorkerExceptionPropagatesToCaller) {
    CancelToken tok;
    EXPECT_THROW(parallel_for_each(100, 4, tok,
                                   [&](size_t i) {
                                       if (i == 50) throw std::runtime_error("boom");
                                   }),
                 std::runtime_error);
}

TEST(Parallel, WorkerExceptionCarriesItsMessage) {
    CancelToken tok;
    try {
        parallel_for_each(100, 4, tok, [&](size_t i) {
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
                                   [&](size_t) {
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
    parallel_for_each(100, 4, tok, [&](size_t) { ++calls; });
    EXPECT_EQ(calls, 0);
}
