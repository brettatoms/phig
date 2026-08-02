#pragma once

#include <cstddef>
#include <functional>

#include "cancel.h"

// Runs fn(i) for every i in [0, count), across num_threads workers pulling
// from a shared atomic index.
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
// num_threads <= 0 is treated as 1.
void parallel_for_each(size_t count, int num_threads, const CancelToken& cancel,
                       const std::function<void(size_t)>& fn);
