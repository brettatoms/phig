#pragma once

#include <stdexcept>
#include <string>

// Thrown by WriterLock when no_wait is set and the lock is already held.
struct LockBusy : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Process-wide advisory writer lock (flock on a sidecar file). Held for the
// lifetime of the object; released on destruction or process death. Blocks
// waiting for the lock unless no_wait is set. Reads must never construct one.
class WriterLock {
   public:
    explicit WriterLock(const std::string& path, bool no_wait = false);
    ~WriterLock();
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    int holder_pid() const { return holder_pid_; }

   private:
    int fd_ = -1;
    int holder_pid_ = -1;
};
