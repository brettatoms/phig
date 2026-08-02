#pragma once

#include <atomic>

// Cooperative cancellation for long-running core operations.
//
// Replaces the CLI's file-scope g_interrupted flag: a global cannot work when
// two operations run concurrently, and a library caller cannot set one. The
// caller owns the token and signals it (from a signal handler, a GUI button, a
// test); the operation polls stop() and returns early.
struct CancelToken {
    std::atomic<bool> cancelled{false};

    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool stop() const { return cancelled.load(std::memory_order_relaxed); }
};
