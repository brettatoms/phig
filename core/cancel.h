#pragma once

#include <atomic>

// Cooperative cancellation for long-running core operations.
//
// Replaces the CLI's file-scope g_interrupted flag: a global cannot work when
// two operations run concurrently, and a library caller cannot set one. The
// caller owns the token and signals it (from a signal handler, a GUI button, a
// test); the operation polls stop() and returns early.
//
// Only the flag itself is synchronized (relaxed atomics — no ordering
// guarantee beyond the flag's own value). Data written before cancel() is NOT
// guaranteed visible to a thread that observes stop() == true; a caller
// needing to publish state alongside cancellation must synchronize it
// separately (e.g. a mutex, or an atomic with acquire/release ordering).
struct CancelToken {
    std::atomic<bool> cancelled{false};

    void cancel() { cancelled.store(true, std::memory_order_relaxed); }
    bool stop() const { return cancelled.load(std::memory_order_relaxed); }
};
