#pragma once

#include <cstddef>
#include <functional>

#include "cancel.h"

// Runs fn(i, worker_id) for every i in [0, count), across num_threads workers
// pulling from a shared atomic index. worker_id is in [0, num_threads) and is
// stable for the lifetime of the worker thread that calls fn — callers can use
// it to index a per-worker resource (e.g. one FaceDetector per worker) without
// a mutex or thread_local.
//
// Cancellation: workers stop pulling new items once `cancel` is signalled. An
// item already in flight runs to completion.
//
// Exceptions: if fn throws, the FIRST exception is captured, further work is
// abandoned, every worker is joined, and the exception is rethrown on the
// calling thread. This is deliberate — an exception escaping a std::thread
// lambda calls std::terminate, which is how `phig scan --fail-on-error`
// currently aborts the process mid-write.
//
// Returns false if cancellation stopped the run before every item was
// processed, true if all `count` items ran. Re-polling the token after the
// call is ambiguous when cancellation lands as the final item completes.
//
// num_threads <= 0 is treated as 1.
bool parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t, int)>& fn);

// The most workers an engine accepts for `parallel`. Above any real core count, so it
// refuses a mistake rather than a tuning choice; scan does I/O, where more threads than
// cores can still help.
constexpr int kMaxParallel = 256;

// The most face workers: one per core. Each loads its own ~150MB detector and is
// CPU-bound, so a worker past the core count costs memory and buys nothing.
int max_face_workers();
