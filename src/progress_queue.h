#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

// A bounded queue for publishing progress from a worker to a consumer.
//
// The producer never blocks: when the queue is full the OLDEST item is dropped,
// because a progress update nobody has read yet is worthless once a newer one
// exists. `dropped()` reports how many were discarded so a caller can tell the
// difference between "nothing happened" and "I could not keep up".
//
// Why a queue rather than a callback: a callback runs on the producer's thread,
// so an engine holding a database lock would invoke arbitrary caller code while
// holding it. Here the consumer pulls on its own thread and no engine code ever
// runs in caller context.
template <typename T>
class ProgressQueue {
   public:
    explicit ProgressQueue(size_t capacity = 256) : capacity_(capacity ? capacity : 1) {}

    ProgressQueue(const ProgressQueue&) = delete;
    ProgressQueue& operator=(const ProgressQueue&) = delete;

    // Never blocks. Dropped silently (and counted) if the queue is full, and
    // ignored entirely once closed.
    void push(T v) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (closed_) return;
            if (items_.size() >= capacity_) {
                items_.pop_front();
                ++dropped_;
            }
            items_.push_back(std::move(v));
        }
        cv_.notify_one();
    }

    // Blocks until an item is available or the queue is closed and drained.
    // Returns false only in the latter case.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) return false;  // closed and drained
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        return true;
    }

    // Wakes every waiter. Items already queued stay readable, so a consumer
    // draining after close() still sees them.
    void close() {
        {
            std::lock_guard<std::mutex> lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(m_);
        return closed_;
    }

    size_t dropped() const {
        std::lock_guard<std::mutex> lock(m_);
        return dropped_;
    }

   private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<T> items_;
    size_t capacity_;
    bool closed_ = false;
    size_t dropped_ = 0;
};
