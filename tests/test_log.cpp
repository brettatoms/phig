#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "log.h"

namespace fs = std::filesystem;

class LogTest : public ::testing::Test {
   protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("phig-log-test-" + std::to_string(::getpid()));
        fs::create_directories(dir_);
        path_ = (dir_ / "phig.log").string();
    }
    void TearDown() override {
        log_close();
        fs::remove_all(dir_);
    }
    std::string read_log() {
        std::ifstream f(path_);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // The stderr sink writes to fd 2, which gtest's CaptureStderr also
    // redirects. Redirecting the real fd 2 to a file keeps the capture
    // independent of that.
    void RedirectStderrToFile() {
        stderr_capture_path_ = (dir_ / "stderr_capture.txt").string();
        fflush(stderr);
        saved_stderr_fd_ = dup(STDERR_FILENO);
        int fd = open(stderr_capture_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    std::string RestoreStderrAndRead() {
        fflush(stderr);
        dup2(saved_stderr_fd_, STDERR_FILENO);
        close(saved_stderr_fd_);
        std::ifstream f(stderr_capture_path_);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    fs::path dir_;
    std::string path_;
    std::string stderr_capture_path_;
    int saved_stderr_fd_ = -1;
};

TEST_F(LogTest, UninitializedIsNoOp) {
    log_warn("nothing should happen");
    EXPECT_FALSE(fs::exists(path_));
}

TEST_F(LogTest, WarnReachesStderrAndFile) {
    RedirectStderrToFile();
    log_init(path_, true, LogLevel::Info);
    log_warn("scan: a.jpg -- Cannot decode image");
    std::string err = RestoreStderrAndRead();
    EXPECT_EQ(err, "[warning] scan: a.jpg -- Cannot decode image\n");
    // file line: UTC ISO-8601 timestamp + level + message
    std::regex line(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z \[warning\] )"
                    R"(scan: a\.jpg -- Cannot decode image\n$)");
    EXPECT_TRUE(std::regex_search(read_log(), line));
}

TEST_F(LogTest, InfoReachesFileOnly) {
    RedirectStderrToFile();
    log_init(path_, true, LogLevel::Info);
    log_info("phig scan /pics");
    EXPECT_EQ(RestoreStderrAndRead(), "");
    EXPECT_NE(read_log().find("[info] phig scan /pics"), std::string::npos);
}

TEST_F(LogTest, MultilineEscapedInFileRawOnStderr) {
    RedirectStderrToFile();
    log_init(path_, true, LogLevel::Info);
    log_warn("first\nsecond");
    EXPECT_EQ(RestoreStderrAndRead(), "[warning] first\nsecond\n");
    std::string body = read_log();
    EXPECT_EQ(std::count(body.begin(), body.end(), '\n'), 1);  // one entry, one line
    EXPECT_NE(body.find("[warning] first\\nsecond\n"), std::string::npos);
}

TEST_F(LogTest, FileLevelErrorKeepsStderrWarnings) {
    RedirectStderrToFile();
    log_init(path_, true, LogLevel::Error);
    log_warn("dropped from file");
    log_error("kept everywhere");
    std::string err = RestoreStderrAndRead();
    EXPECT_NE(err.find("[warning] dropped from file"), std::string::npos);
    EXPECT_NE(err.find("[error] kept everywhere"), std::string::npos);
    std::string body = read_log();
    EXPECT_EQ(body.find("dropped from file"), std::string::npos);
    EXPECT_NE(body.find("[error] kept everywhere"), std::string::npos);
}

TEST_F(LogTest, FileDisabledStillWarnsOnStderr) {
    RedirectStderrToFile();
    log_init(path_, false, LogLevel::Info);
    log_warn("terminal only");
    EXPECT_NE(RestoreStderrAndRead().find("[warning] terminal only"), std::string::npos);
    EXPECT_FALSE(fs::exists(path_));
}

TEST_F(LogTest, BracesAreLoggedVerbatim) {
    log_init(path_, true, LogLevel::Info);
    log_warn("weird {file} {} name.jpg");  // must not be treated as a fmt string
    EXPECT_NE(read_log().find("weird {file} {} name.jpg"), std::string::npos);
}

TEST_F(LogTest, RotatesAtMaxBytes) {
    log_init(path_, true, LogLevel::Info, 512);
    for (int i = 0; i < 20; i++) {
        log_info("padding entry to force rotation " + std::to_string(i));
    }
    EXPECT_TRUE(fs::exists((dir_ / "phig.1.log").string()));  // spdlog's rotated name
}

TEST_F(LogTest, LevelFromStringParsesAndThrows) {
    EXPECT_EQ(log_level_from_string("info"), LogLevel::Info);
    EXPECT_EQ(log_level_from_string("warn"), LogLevel::Warn);
    EXPECT_EQ(log_level_from_string("error"), LogLevel::Error);
    EXPECT_THROW(log_level_from_string("debug"), std::runtime_error);
}
