#pragma once

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <functional>
#include <string>

// Captures what this process writes to fd 2 while `fn` runs.
inline std::string capture_fd2(const std::function<void()>& fn) {
    int pipefd[2];
    EXPECT_EQ(pipe(pipefd), 0);
    int saved = dup(2);
    dup2(pipefd[1], 2);
    close(pipefd[1]);
    fn();
    dup2(saved, 2);
    close(saved);
    std::string out;
    char buf[4096];
    ssize_t n;
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);
    return out;
}
