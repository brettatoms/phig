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

// Database::exec() is private; drive raw SQL through the public db_ptr(), as
// tests/test_database.cpp's raw_exec() does (duplicated here since that one
// is file-local static).
void raw_exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "sqlite error";
        sqlite3_free(err);
        throw std::runtime_error(msg);
    }
}
}  // namespace

// Creates an initialized, empty phig database at `path`. Used by
// test_libphig_abi.cpp.
void make_db(const std::string& path) {
    Database db(path);
    db.init_schema();
}

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

        // Empty metadata ("" is ImageInfo::metadata_json's default and what
        // insert() binds unconditionally) must not abort the backfill with
        // "malformed JSON" — it should fall back to modified_at.
        ImageInfo empty_meta;
        empty_meta.path = "/photos/b.jpg";
        empty_meta.filename = "b.jpg";
        empty_meta.extension = "jpg";
        empty_meta.file_size = 100;
        empty_meta.modified_at = "2020-04-04 04:04:04";
        empty_meta.sha256 = std::string(64, 'b');
        empty_meta.metadata_json = "";
        db.insert(empty_meta);

        // NULL metadata (e.g. a hand-crafted row, or one from a source that
        // never set the column) must also fall back to modified_at.
        std::string insert_c =
            "INSERT INTO images (path, filename, extension, file_size, modified_at, "
            "sha256, phash, metadata) VALUES ('/photos/c.jpg', 'c.jpg', 'jpg', 100, "
            "'2020-05-05 05:05:05', '" +
            std::string(64, 'c') + "', 0, NULL)";
        raw_exec(db.db_ptr(), insert_c.c_str());

        // Roll back to pre-v6 and clear effective_date so the v6 migration's
        // SQL backfill actually runs on reopen, instead of insert() having
        // already populated it (which would pass even with a broken migration).
        raw_exec(db.db_ptr(),
                 "UPDATE meta SET value='5' WHERE key='schema_version';"
                 "UPDATE images SET effective_date = NULL");
    }
    {
        Database db(tmp.path);
        db.init_schema();  // runs the v6 migration
        auto rows = db.get_all_images();
        ASSERT_EQ(rows.size(), 3u);

        auto find = [&](const std::string& path) {
            for (auto& r : rows)
                if (r.path == path) return r;
            ADD_FAILURE() << "missing row " << path;
            return rows[0];
        };
        EXPECT_EQ(find("/photos/a.jpg").effective_date, "2019-07-07 07:07:07");
        EXPECT_EQ(find("/photos/b.jpg").effective_date, "2020-04-04 04:04:04");
        EXPECT_EQ(find("/photos/c.jpg").effective_date, "2020-05-05 05:05:05");
    }
}

namespace {
// Insert n images with predictable, distinct effective dates (ascending).
void seed(Database& db, int n) {
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "2020-01-%02d 00:00:00", i + 1);
        ImageInfo info;
        info.path = "/photos/img" + std::to_string(i) + ".jpg";
        info.filename = "img" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.file_size = 1000 + i;
        info.modified_at = buf;
        info.effective_date = buf;
        info.sha256 = std::string(63, '0') + std::to_string(i % 10);
        db.insert(info);
    }
}
}  // namespace

TEST(QueryPage, ReturnsRequestedPageSize) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 25);
    Database::PageCriteria c;
    c.limit = 10;
    EXPECT_EQ(db.search_page(c).size(), 10u);
}

TEST(QueryPage, OffsetWalksWithoutOverlapOrGaps) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 25);
    Database::PageCriteria c;
    c.limit = 10;
    std::vector<std::string> seen;
    for (int64_t off = 0; off < 30; off += 10) {
        c.offset = off;
        for (const auto& r : db.search_page(c)) seen.push_back(r.path);
    }
    EXPECT_EQ(seen.size(), 25u);
    std::set<std::string> uniq(seen.begin(), seen.end());
    EXPECT_EQ(uniq.size(), 25u);  // no duplicates across pages
}

TEST(QueryPage, DateDescendingIsNewestFirst) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 5);
    Database::PageCriteria c;
    c.sort = Database::SortKey::Date;
    c.descending = true;
    auto rows = db.search_page(c);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows.front().effective_date, "2020-01-05 00:00:00");
    EXPECT_EQ(rows.back().effective_date, "2020-01-01 00:00:00");
}

TEST(QueryPage, StableUnderEqualDates) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    for (int i = 0; i < 10; ++i) {
        ImageInfo info;
        info.path = "/photos/same" + std::to_string(i) + ".jpg";
        info.filename = "same" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.modified_at = "2020-06-06 06:06:06";
        info.effective_date = "2020-06-06 06:06:06";  // all identical
        info.sha256 = std::string(64, 'b');
        db.insert(info);
    }
    Database::PageCriteria c;
    c.limit = 5;
    auto p1 = db.search_page(c);
    c.offset = 5;
    auto p2 = db.search_page(c);
    std::set<std::string> uniq;
    for (const auto& r : p1) uniq.insert(r.path);
    for (const auto& r : p2) uniq.insert(r.path);
    EXPECT_EQ(uniq.size(), 10u);  // id tiebreaker prevents page shuffling
}

TEST(QueryPage, OffsetPastEndReturnsEmpty) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 5);
    Database::PageCriteria c;
    c.offset = 100;
    EXPECT_TRUE(db.search_page(c).empty());
}

TEST(QueryPage, RowsWithNoDateSortLastDescending) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 3);
    ImageInfo undated;
    undated.path = "/photos/undated.jpg";
    undated.filename = "undated.jpg";
    undated.extension = "jpg";
    undated.sha256 = std::string(64, 'c');
    db.insert(undated);  // no modified_at, no metadata
    Database::PageCriteria c;
    c.descending = true;
    auto rows = db.search_page(c);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows.back().path, "/photos/undated.jpg");
}

TEST(CountMatching, CountsWithoutFetching) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 25);
    EXPECT_EQ(db.count_matching(Database::SearchCriteria{}), 25);
}

TEST(CountMatching, RespectsFilter) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed(db, 25);
    Database::SearchCriteria f;
    f.extension = "png";
    EXPECT_EQ(db.count_matching(f), 0);
    f.extension = "jpg";
    EXPECT_EQ(db.count_matching(f), 25);
}

#include "phig/phig.h"

namespace {
// Opens a read-only ABI context on `path`. Caller shuts it down.
phig_ctx* open_ro(const std::string& path) {
    phig_config cfg{};
    cfg.db_path = path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    return ctx;
}
}  // namespace

TEST(AbiQuery, CountMatchesSeededRows) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 25);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    int64_t n = 0;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 25);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, PageReturnsLimitAndPopulatesFields) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 25);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->count, 10);
    EXPECT_GT(list->items[0].id, 0);
    EXPECT_NE(list->items[0].path[0], '\0');
    EXPECT_NE(list->items[0].effective_date[0], '\0');
    EXPECT_EQ(list->items[0].media_type, PHIG_MEDIA_IMAGE);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, NoStringFieldIsNull) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        ImageInfo info;  // minimal row: most text columns empty/NULL
        info.path = "/photos/bare.jpg";
        info.filename = "bare.jpg";
        info.extension = "jpg";
        info.sha256 = std::string(64, 'd');
        db.insert(info);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_EQ(list->count, 1);
    const phig_image& im = list->items[0];
    EXPECT_NE(im.path, nullptr);
    EXPECT_NE(im.filename, nullptr);
    EXPECT_NE(im.extension, nullptr);
    EXPECT_NE(im.sha256, nullptr);
    EXPECT_NE(im.created_at, nullptr);
    EXPECT_NE(im.modified_at, nullptr);
    EXPECT_NE(im.effective_date, nullptr);
    EXPECT_NE(im.gallery, nullptr);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, StringsStayValidUntilListFreed) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 3);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 3, &list), PHIG_OK);
    // Copy the pointer, then run another query; the first list must be intact.
    const char* first_path = list->items[0].path;
    std::string before(first_path);
    phig_image_list* other = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 3, &other), PHIG_OK);
    EXPECT_EQ(before, std::string(first_path));
    phig_image_list_free(other);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, PagePastEndIsEmptyNotError) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 3);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 500, 10, &list), PHIG_OK);
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count, 0);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, RejectsNonPositiveLimit) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_image_list* list = nullptr;
    EXPECT_EQ(phig_query_page(ctx, q, 0, 0, &list), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_page(ctx, q, 0, -5, &list), PHIG_ERR_INVALID_ARG);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, ExtensionFilterApplies) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 5);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_query_set_extension(q, "png");
    int64_t n = -1;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 0);
    phig_query_set_extension(q, "jpg");
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 5);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, SortDateDescendingIsNewestFirst) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 5);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    phig_query_set_sort(q, PHIG_SORT_DATE, true);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 5, &list), PHIG_OK);
    ASSERT_EQ(list->count, 5);
    EXPECT_STREQ(list->items[0].effective_date, "2020-01-05 00:00:00");
    EXPECT_STREQ(list->items[4].effective_date, "2020-01-01 00:00:00");
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, NullArgumentsAreRejected) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    phig_query_new(ctx, &q);
    int64_t n = 0;
    phig_image_list* list = nullptr;
    EXPECT_EQ(phig_query_count(nullptr, q, &n), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_count(ctx, nullptr, &n), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_count(ctx, q, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_page(ctx, q, 0, 10, nullptr), PHIG_ERR_INVALID_ARG);
    phig_query_set_extension(nullptr, "jpg");  // must not crash
    phig_query_free(nullptr);                  // must not crash
    phig_image_list_free(nullptr);             // must not crash
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, MetadataFetchedOnDemand) {
    TempDb tmp;
    int64_t id = 0;
    {
        Database db(tmp.path);
        db.init_schema();
        ImageInfo info;
        info.path = "/photos/meta.jpg";
        info.filename = "meta.jpg";
        info.extension = "jpg";
        info.sha256 = std::string(64, 'e');
        info.metadata_json = R"({"Model":"Canon EOS 5D"})";
        db.insert(info);
        id = db.get_image_id(info.path);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    char* json = nullptr;
    ASSERT_EQ(phig_image_metadata(ctx, id, &json), PHIG_OK);
    ASSERT_NE(json, nullptr);
    EXPECT_NE(std::string(json).find("Canon EOS 5D"), std::string::npos);
    phig_string_free(json);
    phig_string_free(nullptr);  // must not crash
    phig_shutdown(ctx);
}

TEST(AbiQuery, MetadataMissingIdIsNotFound) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
    }
    phig_ctx* ctx = open_ro(tmp.path);
    char* json = nullptr;
    EXPECT_EQ(phig_image_metadata(ctx, 999999, &json), PHIG_ERR_NOT_FOUND);
    phig_shutdown(ctx);
}

TEST(AbiQuery, FavoriteIsPopulated) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        ImageInfo info;
        info.path = "/photos/fav.jpg";
        info.filename = "fav.jpg";
        info.extension = "jpg";
        info.sha256 = std::string(64, 'f');
        db.insert(info);
        db.set_tag({"/photos/fav.jpg"}, "favorite", true);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_EQ(list->count, 1);
    EXPECT_TRUE(list->items[0].favorite);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}
