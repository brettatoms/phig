#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include "progress_queue.h"

TEST(ProgressQueue, PopReturnsItemsInPushOrder) {
    ProgressQueue<int> q(16);
    q.push(1);
    q.push(2);
    q.push(3);
    int v = 0;
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 2);
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 3);
}

TEST(ProgressQueue, TryPopIsFalseWhenEmpty) {
    ProgressQueue<int> q(4);
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    q.push(7);
    ASSERT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 7);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(ProgressQueue, PushNeverBlocksAndDropsOldestWhenFull) {
    ProgressQueue<int> q(3);
    for (int i = 0; i < 10; ++i) q.push(i);  // must not block
    EXPECT_EQ(q.dropped(), 7u);

    // The three most recent survive, in order.
    int v = 0;
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 7);
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 8);
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 9);
}

TEST(ProgressQueue, CloseDrainsRemainingItemsThenReturnsFalse) {
    ProgressQueue<int> q(8);
    q.push(1);
    q.push(2);
    q.close();
    int v = 0;
    // Closing must not discard what was already queued.
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 1);
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.pop(v));
    EXPECT_TRUE(q.closed());
}

TEST(ProgressQueue, PopOnAnEmptyClosedQueueReturnsFalseImmediately) {
    ProgressQueue<int> q(4);
    q.close();
    int v = 0;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(q.pop(v));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    EXPECT_LT(ms, 500) << "pop() on a closed queue must not block";
}

TEST(ProgressQueue, CloseWakesABlockedConsumer) {
    ProgressQueue<int> q(4);
    std::atomic<bool> returned{false};
    std::thread consumer([&] {
        int v = 0;
        q.pop(v);  // blocks: queue is empty and open
        returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(returned.load()) << "pop() should still be blocked";
    q.close();
    consumer.join();
    EXPECT_TRUE(returned.load());
}

TEST(ProgressQueue, PushAfterCloseIsANoOp) {
    ProgressQueue<int> q(4);
    q.close();
    q.push(42);  // must not throw, must not enqueue
    int v = 0;
    EXPECT_FALSE(q.pop(v));
}

TEST(ProgressQueue, ManyProducersOneConsumerLosesNothingUnderCapacity) {
    // 4 producers x 50 items = 200, capacity 1024, so nothing is dropped and
    // the consumer must observe every value exactly once.
    ProgressQueue<int> q(1024);
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < 50; ++i) q.push(p * 100 + i);
        });
    }
    std::set<int> seen;
    std::thread consumer([&] {
        int v = 0;
        while (q.pop(v)) seen.insert(v);
    });
    for (auto& t : producers) t.join();
    q.close();
    consumer.join();

    EXPECT_EQ(q.dropped(), 0u);
    EXPECT_EQ(seen.size(), 200u);
}

TEST(ProgressQueue, SurvivesAProducerFasterThanItsConsumer) {
    // The real shape: a fast producer and a slow consumer with a small queue.
    // Nothing may block or crash; some items are dropped and that is correct.
    ProgressQueue<int> q(8);
    std::atomic<size_t> consumed{0};
    std::thread consumer([&] {
        int v = 0;
        while (q.pop(v)) {
            consumed.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    for (int i = 0; i < 5000; ++i) q.push(i);
    q.close();
    consumer.join();

    EXPECT_GT(consumed.load(), 0u);
    EXPECT_LE(consumed.load(), 5000u);
    EXPECT_EQ(consumed.load() + q.dropped(), 5000u)
        << "every pushed item was either consumed or counted as dropped";
}
