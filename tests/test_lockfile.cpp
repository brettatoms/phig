#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "lockfile.h"

namespace fs = std::filesystem;

class LockfileTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string lpath;
    void SetUp() override {
        dir = fs::temp_directory_path() / "phig-lockfile-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        lpath = (dir / "phig.db.lock").string();
    }
    void TearDown() override { fs::remove_all(dir); }
};

TEST_F(LockfileTest, AcquiresAndReleases) {
    {
        WriterLock lk(lpath);
        EXPECT_EQ(lk.holder_pid(), getpid());
    }
    // Released — a second lock in the same process succeeds without blocking.
    WriterLock lk2(lpath, /*no_wait=*/true);
    SUCCEED();
}

TEST_F(LockfileTest, NoWaitThrowsWhenHeldByAnotherFd) {
    // Simulate another process holding the lock by taking a raw flock on the
    // same file from a separate fd.
    int other = ::open(lpath.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(other, 0);
    ASSERT_EQ(::flock(other, LOCK_EX | LOCK_NB), 0);

    EXPECT_THROW({ WriterLock lk(lpath, /*no_wait=*/true); }, LockBusy);

    ::flock(other, LOCK_UN);
    ::close(other);
}

TEST_F(LockfileTest, WaitBlocksThenAcquiresAfterRelease) {
    int other = ::open(lpath.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(other, 0);
    ASSERT_EQ(::flock(other, LOCK_EX | LOCK_NB), 0);

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        WriterLock lk(lpath, /*no_wait=*/false);  // should block until release
        acquired.store(true);
    });

    // Give the waiter time to reach the blocking flock.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(acquired.load());  // still blocked

    ::flock(other, LOCK_UN);
    ::close(other);

    waiter.join();
    EXPECT_TRUE(acquired.load());
}
