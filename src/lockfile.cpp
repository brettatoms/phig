#include "lockfile.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <system_error>

namespace {
int read_pid(int fd) {
    char buf[32] = {0};
    if (::pread(fd, buf, sizeof(buf) - 1, 0) > 0) {
        try {
            return std::stoi(std::string(buf));
        } catch (...) {
        }
    }
    return -1;
}

void write_pid(int fd) {
    std::string s = std::to_string(::getpid());
    if (::ftruncate(fd, 0) == 0) {
        (void)::pwrite(fd, s.data(), s.size(), 0);  // best-effort
    }
}
}  // namespace

WriterLock::WriterLock(const std::string& path, bool no_wait) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot open lock file " + path + ": " + std::strerror(errno));
    }

    if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
        holder_pid_ = ::getpid();
        write_pid(fd_);
        return;
    }
    if (errno != EWOULDBLOCK) {
        int e = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot lock " + path + ": " + std::strerror(e));
    }

    // Lock is held by another process.
    int other_pid = read_pid(fd_);
    if (no_wait) {
        ::close(fd_);
        fd_ = -1;
        throw LockBusy("Another phig operation is running (pid " + std::to_string(other_pid) + ")");
    }

    std::cerr << "Another phig operation is running (pid " << other_pid << "); waiting..."
              << std::endl;
    if (::flock(fd_, LOCK_EX) != 0) {  // blocking
        int e = errno;
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("Cannot lock " + path + ": " + std::strerror(e));
    }
    holder_pid_ = ::getpid();
    write_pid(fd_);
}

WriterLock::~WriterLock() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);  // explicit; close() would also release
        ::close(fd_);
    }
}
