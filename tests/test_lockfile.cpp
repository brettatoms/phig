#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "lockfile.h"
#include "paths.h"
#include "phig_binary.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

class LockfileTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string lpath;
    void SetUp() override {
        dir = make_temp_dir("lockfile");
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

// ---- Which commands take it (R49) ----

// The three tests above drive WriterLock directly, with a raw flock on a second
// descriptor standing in for the other process. That pins the class and says
// nothing about the rule, which is a statement about callers: a read takes no
// lock, and a writer that takes none is not excluded either. Every CLI call
// site is in cli/main.cpp, which is not linked into phig-tests, so this holds
// the lock the same way and drives the built binary against it.
//
// The scratch database sits at the default path inside run_phig's redirected
// XDG_DATA_HOME, so the lock file the child computes is the one this test
// holds. Passing --catalog with another directory would put them on different files,
// and the refusal below would prove nothing.
TEST_F(LockfileTest, CliRefusesALockTakingWriteWhileAllowingReadsAndLocklessWrites) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) {
        GTEST_SKIP() << "phig binary not found next to phig-tests or at ./build/phig; "
                        "build it with `cmake --build build` to run this test";
    }
    const fs::path db_path = phig_scratch_db_path(dir);
    const fs::path held = phig_scratch_lock_path(dir);
    fs::create_directories(db_path.parent_path());

    // Two images, and two faces with the same embedding, one of them named. That
    // is what gives `face identify` — a mutating command that takes no lock — a
    // row to write while the lock is held.
    {
        Database db(db_path.string());
        db.init_schema();
        std::vector<int64_t> image_ids;
        for (const char* name : {"one.jpg", "two.jpg"}) {
            ImageInfo info;
            info.path = (dir / name).string();
            info.filename = name;
            info.extension = "jpg";
            info.file_size = 1;
            info.media_type = "image";
            info.sha256 = std::string("sha-") + name;
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = "2024-01-01T00:00:00Z";
            info.metadata_json = "{}";
            db.insert(info);
            image_ids.push_back(db.get_image_id(info.path));
            ASSERT_GT(image_ids.back(), 0);
        }
        FaceInfo face{};
        face.embedding.fill(0.5f);
        face.width = 10;
        face.height = 10;
        int64_t reference = db.insert_face(image_ids[0], face);
        ASSERT_GT(reference, 0);
        ASSERT_GT(db.insert_face(image_ids[1], face), 0);
        int64_t person = db.get_or_create_person("Ada");
        ASSERT_GT(person, 0);
        ASSERT_TRUE(db.set_face_person(reference, person));
    }

    int other = ::open(held.c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(other, 0);
    ASSERT_EQ(::flock(other, LOCK_EX | LOCK_NB), 0);

    PhigRun refused = run_phig(phig_bin, dir, "--no-wait tag add holiday --match '*.jpg'");
    EXPECT_EQ(refused.exit_code, 1) << refused.output;
    EXPECT_NE(refused.output.find("Another phig operation is running"), std::string::npos)
        << refused.output;

    PhigRun read = run_phig(phig_bin, dir, "--no-wait search --count");
    EXPECT_EQ(read.exit_code, 0) << "a read took the writer lock: " << read.output;
    EXPECT_NE(read.output.find("2"), std::string::npos) << read.output;

    PhigRun lockless = run_phig(phig_bin, dir, "--no-wait face identify Ada");
    EXPECT_EQ(lockless.exit_code, 0) << "a lock-free writer was excluded: " << lockless.output;
    EXPECT_NE(lockless.output.find("Labeled 1"), std::string::npos)
        << "the lock-free writer ran but wrote nothing, so it proves nothing: " << lockless.output;

    ::flock(other, LOCK_UN);
    ::close(other);

    // The control: the same refused command succeeds once the lock is gone, so
    // the refusal above was the lock and not the argv, the database or the path.
    PhigRun after = run_phig(phig_bin, dir, "--no-wait tag add holiday --match '*.jpg'");
    EXPECT_EQ(after.exit_code, 0) << after.output;
    EXPECT_NE(after.output.find("Tagged 2 images"), std::string::npos) << after.output;
}

// Which lock file a command takes is the catalog's: holding catalog A's lock
// refuses a write to A, and leaves a write to B, or a write to A while only the
// default catalog's lock is held, free to run.
TEST_F(LockfileTest, CliLocksTheCatalogItWrites) {
    fs::path phig_bin = phig_binary_path();
    if (phig_bin.empty()) GTEST_SKIP() << "phig binary not built";

    auto make = [&](const std::string& name) {
        const fs::path c = dir / name;
        fs::create_directories(c);
        Database db((c / "phig.db").string());
        db.init_schema();
        ImageInfo info;
        info.path = (c / "a.jpg").string();
        info.filename = "a.jpg";
        info.extension = "jpg";
        info.file_size = 1;
        info.sha256 = "sha-" + name;
        info.metadata_json = "{}";
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        db.insert(info);
        return c;
    };
    const fs::path a = make("a");
    const fs::path b = make("b");
    auto tag = [&](const fs::path& c) {
        return run_phig(phig_bin, dir,
                        "--no-wait --catalog '" + c.string() + "' tag add t --match '*.jpg'");
    };
    auto hold = [](const fs::path& lock) {
        fs::create_directories(lock.parent_path());
        int fd = ::open(lock.c_str(), O_RDWR | O_CREAT, 0644);
        EXPECT_GE(fd, 0);
        EXPECT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
        return fd;
    };

    int fd = hold(catalog_paths(a.string()).lock);
    PhigRun refused = tag(a);
    EXPECT_EQ(refused.exit_code, 1) << refused.output;
    EXPECT_NE(refused.output.find("Another phig operation is running"), std::string::npos)
        << refused.output;
    PhigRun other = tag(b);
    EXPECT_EQ(other.exit_code, 0) << "B was excluded by A's lock: " << other.output;
    ::flock(fd, LOCK_UN);
    ::close(fd);

    fd = hold(phig_scratch_lock_path(dir));
    PhigRun free_a = tag(a);
    EXPECT_EQ(free_a.exit_code, 0)
        << "A was excluded by the default catalog's lock: " << free_a.output;
    ::flock(fd, LOCK_UN);
    ::close(fd);
}
