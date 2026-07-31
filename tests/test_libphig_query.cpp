#include <gtest/gtest.h>

#include "database.h"

TEST(EffectiveDate, PrefersDateTimeOriginal) {
    std::string meta = R"({"DateTimeOriginal":"2021:05:04 10:00:00",
                           "DateTimeDigitized":"2022:01:01 00:00:00"})";
    EXPECT_EQ(compute_effective_date(meta, "2023-09-09 09:09:09"), "2021-05-04 10:00:00");
}

TEST(EffectiveDate, FallsBackToDigitized) {
    std::string meta = R"({"DateTimeDigitized":"2022:01:01 00:00:00"})";
    EXPECT_EQ(compute_effective_date(meta, "2023-09-09 09:09:09"), "2022-01-01 00:00:00");
}

TEST(EffectiveDate, FallsBackToModifiedAt) {
    EXPECT_EQ(compute_effective_date("", "2023-09-09 09:09:09"), "2023-09-09 09:09:09");
}

TEST(EffectiveDate, EmptyWhenNothingAvailable) { EXPECT_EQ(compute_effective_date("", ""), ""); }

TEST(EffectiveDate, NormalizesExifSeparators) {
    std::string meta = R"({"DateTimeOriginal":"2021:05:04 10:00:00"})";
    EXPECT_EQ(compute_effective_date(meta, ""), "2021-05-04 10:00:00");
}

TEST(EffectiveDate, SortsAcrossExifAndMtimeForms) {
    std::string exif = R"({"DateTimeOriginal":"2021:05:04 10:00:00"})";
    std::string a = compute_effective_date(exif, "");
    std::string b = compute_effective_date("", "2022-01-01 00:00:00");
    EXPECT_LT(a, b);
}

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
// Unique temp DB path per test; removed by TempDb's destructor.
struct TempDb {
    std::string path;
    TempDb() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_test_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                   .string();
    }
    ~TempDb() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
};
}  // namespace

TEST(Migration, BackfillsEffectiveDateForExistingRows) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        ImageInfo info;
        info.path = "/photos/a.jpg";
        info.filename = "a.jpg";
        info.extension = "jpg";
        info.file_size = 100;
        info.modified_at = "2020-03-03 03:03:03";
        info.sha256 = std::string(64, 'a');
        info.metadata_json = R"({"DateTimeOriginal":"2019:07:07 07:07:07"})";
        db.insert(info);
    }
    {
        Database db(tmp.path);
        db.init_schema();  // idempotent; must not lose data
        auto rows = db.get_all_images();
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].effective_date, "2019-07-07 07:07:07");
    }
}
