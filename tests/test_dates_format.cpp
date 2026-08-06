#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include "dates.h"

namespace {
// Creates a file and returns its path; removed by the caller.
std::string make_temp_file() {
    auto p = std::filesystem::temp_directory_path() /
             ("phig_fmttime_" + std::to_string(::getpid()) + ".txt");
    std::ofstream(p) << "x";
    return p.string();
}
}  // namespace

TEST(FormatTime, ProducesIso8601UtcWithZSuffix) {
    auto path = make_temp_file();
    auto ftime = std::filesystem::last_write_time(path);
    std::string s = format_time(ftime);

    // "YYYY-MM-DDTHH:MM:SSZ" — exactly 20 characters.
    EXPECT_EQ(s.size(), 20u) << s;
    EXPECT_TRUE(std::regex_match(s, std::regex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"))) << s;

    std::filesystem::remove(path);
}

TEST(FormatTime, IsStableAcrossRepeatedCalls) {
    auto path = make_temp_file();
    auto ftime = std::filesystem::last_write_time(path);
    // The mtime-skip check in scan compares a fresh format against a stored
    // one, so the same input must always produce the same string.
    EXPECT_EQ(format_time(ftime), format_time(ftime));
    std::filesystem::remove(path);
}

TEST(FormatTime, LaterFileSortsAfterEarlierFile) {
    auto a = make_temp_file();
    auto ta = std::filesystem::last_write_time(a);
    // Move one file's mtime a day forward; the formatted strings must order
    // the same way, since modified_at is compared lexicographically.
    auto tb = ta + std::chrono::hours(24);
    EXPECT_LT(format_time(ta), format_time(tb));
    std::filesystem::remove(a);
}

TEST(FormatTime, EpochZeroFormatsAsUnixEpochInUtc) {
    auto path = make_temp_file();
    // Pin the exact output, not just its shape: a UTC->local-time regression
    // still produces a valid-looking 20-char ISO-8601 string, and that is the
    // change that would make every tracked file look modified. The file clock's
    // own default-constructed epoch is not the Unix epoch on every platform, so
    // convert a known system_clock instant into file_time_type rather than
    // relying on file_time_type{}.
    //
    // The conversion is the offset between the two clocks, sampled at the same
    // moment — the inverse of what format_time itself does. std::chrono::
    // clock_cast would express this directly but is absent from libc++, so it
    // cannot be used here.
    auto sys_epoch = std::chrono::system_clock::from_time_t(0);
    auto file_now = std::filesystem::file_time_type::clock::now();
    auto sys_now = std::chrono::system_clock::now();
    auto file_epoch = file_now + (sys_epoch - sys_now);
    std::filesystem::last_write_time(path, file_epoch);

    EXPECT_EQ(format_time(std::filesystem::last_write_time(path)), "1970-01-01T00:00:00Z");

    std::filesystem::remove(path);
}
