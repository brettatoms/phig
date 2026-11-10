#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <thread>

// Captures what this process writes to fd 2 while `fn` runs. A reader thread drains the
// pipe as it fills, so neither `fn` nor another thread writing to fd 2 ever blocks on it.
inline std::string capture_fd2(const std::function<void()>& fn) {
    int pipefd[2];
    EXPECT_EQ(pipe(pipefd), 0);
    std::string out;
    std::thread reader([&out, fd = pipefd[0]] {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    });
    int saved = dup(2);
    dup2(pipefd[1], 2);
    close(pipefd[1]);
    fn();
    // Closing fd 2's copy of the write end is what lets the reader see end-of-file.
    dup2(saved, 2);
    close(saved);
    reader.join();
    close(pipefd[0]);
    return out;
}
