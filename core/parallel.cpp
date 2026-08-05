#include "parallel.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

bool parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t, int)>& fn) {
    if (count == 0) return true;
    if (num_threads < 1) num_threads = 1;
    if (static_cast<size_t>(num_threads) > count) {
        num_threads = static_cast<int>(count);
    }

    std::atomic<size_t> next{0};
    std::atomic<size_t> completed{0};
    std::atomic<bool> failed{false};
    std::exception_ptr first_error;
    std::mutex error_mutex;

    auto worker = [&](int worker_id) {
        for (;;) {
            if (cancel.stop() || failed.load(std::memory_order_relaxed)) return;
            size_t i = next.fetch_add(1);
            if (i >= count) return;
            try {
                fn(i, worker_id);
                completed.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                // Keep only the first failure; the rest are consequences.
                std::lock_guard<std::mutex> g(error_mutex);
                if (!first_error) {
                    first_error = std::current_exception();
                    failed.store(true, std::memory_order_relaxed);
                }
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(num_threads));
    for (int t = 0; t < num_threads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    // Rethrow on the caller's thread, after every worker has been joined.
    // The return value is moot on this path — the exception is what the
    // caller observes — so completion isn't checked here.
    if (first_error) std::rethrow_exception(first_error);

    // Count completed items rather than re-reading the token: cancellation
    // can flip after the last item finishes, which would misreport a full
    // run as cancelled.
    return completed.load(std::memory_order_relaxed) == count;
}
