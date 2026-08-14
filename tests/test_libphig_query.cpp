#include <gtest/gtest.h>

#include "database.h"
#include "phig/phig.h"

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

// format_time() (src/main.cpp) writes mtimes as "YYYY-MM-DDTHH:MM:SSZ". That
// form must be folded into the canonical space-separated one, or every
// mtime-dated row outranks every EXIF-dated row from the same day.
TEST(EffectiveDate, NormalizesMtimeIsoZForm) {
    EXPECT_EQ(compute_effective_date("", "2026-08-01T15:00:19Z"), "2026-08-01 15:00:19");
}

TEST(EffectiveDate, MtimeIsoZSortsAgainstExifSameDay) {
    std::string exif = R"({"DateTimeOriginal":"2021:06:15 23:00:00"})";
    std::string later_exif = compute_effective_date(exif, "");
    std::string earlier_mtime = compute_effective_date("", "2021-06-15T22:00:00Z");
    EXPECT_LT(earlier_mtime, later_exif);
}

TEST(EffectiveDate, LeavesNonMatchingShapesAlone) {
    // Already canonical.
    EXPECT_EQ(compute_effective_date("", "2021-06-15 23:00:00"), "2021-06-15 23:00:00");
    // Nothing at all.
    EXPECT_EQ(compute_effective_date("", ""), "");
    // Too short to be either shape; must not read out of bounds or mangle.
    EXPECT_EQ(compute_effective_date("", "2021"), "2021");
    EXPECT_EQ(compute_effective_date("", "x"), "x");
    EXPECT_EQ(compute_effective_date("", "garbage"), "garbage");
    // Right length, wrong shape: a 'T' that is not the date/time separator.
    EXPECT_EQ(compute_effective_date("", "a string with a T!!"), "a string with a T!!");
    // 'T' in place but no trailing 'Z' — not format_time()'s output.
    EXPECT_EQ(compute_effective_date("", "2021-06-15T23:00:00"), "2021-06-15T23:00:00");
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

// Insert one image whose date comes from EXIF, in the real EXIF ':' form.
void insert_exif_dated(Database& db, const std::string& path, const std::string& exif_date,
                       char sha_fill) {
    ImageInfo info;
    info.path = path;
    info.filename = path.substr(path.rfind('/') + 1);
    info.extension = "jpg";
    info.sha256 = std::string(64, sha_fill);
    // A real scan always has an mtime; make it a distant one so a regression
    // that ignores EXIF and falls back to modified_at is visible.
    info.modified_at = "2099-12-31T00:00:00Z";
    info.metadata_json = R"({"DateTimeOriginal":")" + exif_date + R"("})";
    db.insert(info);
}

// Insert one image dated only by its file mtime, in format_time()'s real
// "YYYY-MM-DDTHH:MM:SSZ" form (src/main.cpp).
void insert_mtime_dated(Database& db, const std::string& path, const std::string& mtime,
                        char sha_fill) {
    ImageInfo info;
    info.path = path;
    info.filename = path.substr(path.rfind('/') + 1);
    info.extension = "jpg";
    info.sha256 = std::string(64, sha_fill);
    info.modified_at = mtime;
    db.insert(info);
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
// Reads one column back as a quoted SQL literal so NULL and '' stay
// distinguishable in an assertion message.
std::string quoted_effective_date(sqlite3* db, const std::string& path) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT quote(effective_date) FROM images WHERE path = ?", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    std::string out = "<no row>";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return out;
}

// Every effective_date value shape the v7 migration has to survive, as
// {path, raw stored value as a SQL literal, expected value after migration}.
struct DateShape {
    const char* path;
    const char* stored_sql;
    const char* expected_sql;
};
const std::vector<DateShape>& v7_shapes() {
    static const std::vector<DateShape> shapes = {
        // Already canonical — must be left byte-for-byte alone.
        {"/s/canonical.jpg", "'2021-06-15 23:00:00'", "'2021-06-15 23:00:00'"},
        // format_time()'s mtime form — the whole point of the migration.
        {"/s/isoz.jpg", "'2026-08-01T15:00:19Z'", "'2026-08-01 15:00:19'"},
        // EXIF ':' form, in case an old insert() stored one un-normalized.
        {"/s/exif.jpg", "'2021:06:15 23:00:00'", "'2021-06-15 23:00:00'"},
        // Absent values, both spellings.
        {"/s/empty.jpg", "''", "''"},
        {"/s/null.jpg", "NULL", "NULL"},
        // Malformed / short — must pass through, not crash or truncate.
        {"/s/garbage.jpg", "'garbage'", "'garbage'"},
        {"/s/short.jpg", "'2021'", "'2021'"},
        // Exactly 19 chars with a 'T': one short of the guarded length.
        {"/s/tnoz.jpg", "'2021-06-15T23:00:00'", "'2021-06-15T23:00:00'"},
        // 20 chars, 'T' at 10, but no trailing 'Z'.
        {"/s/tnozlong.jpg", "'2021-06-15T23:00:00x'", "'2021-06-15T23:00:00x'"},
        // 20 chars ending in 'Z' but no 'T' separator.
        {"/s/noz.jpg", "'2021-06-15 23:00:00Z'", "'2021-06-15 23:00:00Z'"},
        // A 'T' elsewhere in a long value must not be treated as a separator.
        {"/s/othert.jpg", "'a string with a T!!!!'", "'a string with a T!!!!'"},
    };
    return shapes;
}

// Writes the v7_shapes() rows directly, then rolls the recorded schema version
// back to 6 so init_schema() runs the v7 migration on the next open.
void seed_v6_with_all_shapes(const std::string& db_path) {
    Database db(db_path);
    db.init_schema();
    int i = 0;
    for (const auto& s : v7_shapes()) {
        std::string sql =
            "INSERT INTO images (path, filename, extension, file_size, modified_at, sha256, "
            "phash, metadata, effective_date) VALUES ('" +
            std::string(s.path) + "', 'f.jpg', 'jpg', 100, NULL, '" +
            std::string(64, static_cast<char>('a' + (i % 26))) + "', 0, NULL, " + s.stored_sql +
            ")";
        raw_exec(db.db_ptr(), sql.c_str());
        ++i;
    }
    raw_exec(db.db_ptr(), "UPDATE meta SET value='6' WHERE key='schema_version'");
}
}  // namespace

TEST(MigrationV7, CanonicalizesEveryValueShape) {
    TempDb tmp;
    seed_v6_with_all_shapes(tmp.path);
    Database db(tmp.path);
    db.init_schema();  // runs the v7 migration
    for (const auto& s : v7_shapes()) {
        EXPECT_EQ(quoted_effective_date(db.db_ptr(), s.path), s.expected_sql)
            << "shape " << s.path << " stored as " << s.stored_sql;
    }
}

// The migration SQL and compute_effective_date() are two implementations of
// one rule. They must not drift: run the same inputs through both.
TEST(MigrationV7, SqlAgreesWithComputeEffectiveDate) {
    TempDb tmp;
    seed_v6_with_all_shapes(tmp.path);
    Database db(tmp.path);
    db.init_schema();
    for (const auto& s : v7_shapes()) {
        std::string stored = s.stored_sql;
        if (stored == "NULL") continue;  // C++ has no NULL input; SQL leaves it alone
        std::string raw = stored.substr(1, stored.size() - 2);  // strip the quotes
        std::string via_cpp = "'" + compute_effective_date("", raw) + "'";
        EXPECT_EQ(quoted_effective_date(db.db_ptr(), s.path), via_cpp)
            << "SQL/C++ divergence for input " << stored;
    }
}

TEST(MigrationV7, IsIdempotent) {
    TempDb tmp;
    seed_v6_with_all_shapes(tmp.path);
    {
        Database db(tmp.path);
        db.init_schema();
    }
    // A second init_schema() is a no-op (version already current); force the
    // migration to run a second time over its own output to prove the SQL is
    // idempotent even if it were re-applied.
    {
        Database db(tmp.path);
        raw_exec(db.db_ptr(), "UPDATE meta SET value='6' WHERE key='schema_version'");
        db.init_schema();
    }
    {
        Database db(tmp.path);
        db.init_schema();  // third open: nothing left to do
        for (const auto& s : v7_shapes()) {
            EXPECT_EQ(quoted_effective_date(db.db_ptr(), s.path), s.expected_sql)
                << "shape " << s.path << " drifted on re-run";
        }
    }
}

// A v5 database has no effective_date column at all. The v6+v7 chain must
// backfill it in the canonical form straight from a T...Z modified_at.
TEST(MigrationV7, V5DatabaseMigratesStraightToCanonical) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        insert_mtime_dated(db, "/photos/v5.jpg", "2026-08-01T15:00:19Z", 'a');
        raw_exec(db.db_ptr(),
                 "UPDATE meta SET value='5' WHERE key='schema_version';"
                 "UPDATE images SET effective_date = NULL");
    }
    Database db(tmp.path);
    db.init_schema();
    EXPECT_EQ(quoted_effective_date(db.db_ptr(), "/photos/v5.jpg"), "'2026-08-01 15:00:19'");
}

// A GUI pointed at a not-yet-migrated collection must be told so at init,
// not handed a context whose every page fails with "no such column".
TEST(AbiSchema, ReadOnlyInitRejectsOlderDatabase) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        insert_mtime_dated(db, "/photos/old.jpg", "2021-06-15T22:00:00Z", 'a');
        // A real v5 database: no effective_date column at all.
        raw_exec(db.db_ptr(),
                 "DROP INDEX idx_effective_date;"
                 "ALTER TABLE images DROP COLUMN effective_date;"
                 "UPDATE meta SET value='5' WHERE key='schema_version'");
    }
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_SCHEMA);
    EXPECT_EQ(ctx, nullptr);
}

TEST(AbiSchema, ReadOnlyInitRejectsNewerDatabase) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        raw_exec(db.db_ptr(), "UPDATE meta SET value='999' WHERE key='schema_version'");
    }
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_SCHEMA);
    EXPECT_EQ(ctx, nullptr);

    // Read-write cannot migrate downward either, so it must refuse the same way
    // rather than reporting a generic PHIG_ERR_DB.
    cfg.mode = PHIG_READ_WRITE;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_ERR_SCHEMA);
    EXPECT_EQ(ctx, nullptr);
}

// The read-write path is allowed to migrate, so the same older database that
// read-only rejects must open cleanly and serve queries here.
TEST(AbiSchema, ReadWriteInitMigratesOlderDatabase) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        insert_mtime_dated(db, "/photos/old.jpg", "2021-06-15T22:00:00Z", 'a');
        raw_exec(db.db_ptr(),
                 "DROP INDEX idx_effective_date;"
                 "ALTER TABLE images DROP COLUMN effective_date;"
                 "UPDATE meta SET value='5' WHERE key='schema_version'");
    }
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_WRITE;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_EQ(list->count, 1);
    EXPECT_STREQ(list->items[0].effective_date, "2021-06-15 22:00:00");
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);

    // And afterwards a read-only context opens fine, because it is now current.
    cfg.mode = PHIG_READ_ONLY;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    phig_shutdown(ctx);
}

namespace {
// Insert n images with predictable, distinct effective dates (ascending).
//
// modified_at is written in format_time()'s real "YYYY-MM-DDTHH:MM:SSZ" shape
// and effective_date is left empty so insert() derives it through
// compute_effective_date(), exactly as a scan does. Seeding a hand-written
// canonical literal here is what let the two-date-family bug survive nine
// reviews — do not reintroduce it.
void seed(Database& db, int n) {
    for (int i = 0; i < n; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "2020-01-%02dT00:00:00Z", i + 1);
        ImageInfo info;
        info.path = "/photos/img" + std::to_string(i) + ".jpg";
        info.filename = "img" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.file_size = 1000 + i;
        info.modified_at = buf;
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
        info.modified_at = "2020-06-06T06:06:06Z";  // all identical
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

namespace {
// One collection holding both date families on the SAME day, interleaved so
// any single-family sort gets it wrong. Returns the expected DESC order of
// effective_date values.
//
// This is the shape that Blocker 1 got wrong: ' ' (0x20) < 'T' (0x54), so
// before canonicalization every mtime-dated row outranked every EXIF-dated
// row from the same day regardless of the actual time.
std::vector<std::string> seed_mixed_families(Database& db) {
    insert_exif_dated(db, "/photos/exif_2300.jpg", "2021:06:15 23:00:00", 'a');
    insert_mtime_dated(db, "/photos/mtime_2200.jpg", "2021-06-15T22:00:00Z", 'b');
    insert_exif_dated(db, "/photos/exif_0200.jpg", "2021:06:15 02:00:00", 'c');
    insert_mtime_dated(db, "/photos/mtime_0100.jpg", "2021-06-15T01:00:00Z", 'd');
    return {"2021-06-15 23:00:00", "2021-06-15 22:00:00", "2021-06-15 02:00:00",
            "2021-06-15 01:00:00"};
}
}  // namespace

TEST(MixedDateFamilies, StoredCanonicallyRegardlessOfSource) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed_mixed_families(db);
    for (const auto& r : db.get_all_images()) {
        EXPECT_EQ(r.effective_date.size(), 19u) << r.path << " -> " << r.effective_date;
        EXPECT_EQ(r.effective_date.find('T'), std::string::npos) << r.path;
        EXPECT_EQ(r.effective_date.find('Z'), std::string::npos) << r.path;
        EXPECT_EQ(r.effective_date[10], ' ') << r.path;
    }
}

TEST(MixedDateFamilies, SortsCorrectlyAcrossTheBoundary) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    auto expected_desc = seed_mixed_families(db);

    Database::PageCriteria c;
    c.sort = Database::SortKey::Date;
    c.descending = true;
    std::vector<std::string> got;
    for (const auto& r : db.search_page(c)) got.push_back(r.effective_date);
    EXPECT_EQ(got, expected_desc);

    c.descending = false;
    std::vector<std::string> expected_asc(expected_desc.rbegin(), expected_desc.rend());
    got.clear();
    for (const auto& r : db.search_page(c)) got.push_back(r.effective_date);
    EXPECT_EQ(got, expected_asc);
}

TEST(MixedDateFamilies, AbiSortMatchesAcrossTheBoundary) {
    TempDb tmp;
    std::vector<std::string> expected_desc;
    {
        Database db(tmp.path);
        db.init_schema();
        expected_desc = seed_mixed_families(db);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_set_sort(q, PHIG_SORT_DATE, true);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    ASSERT_EQ(list->count, 4);
    for (int64_t i = 0; i < list->count; ++i) {
        EXPECT_STREQ(list->items[i].effective_date, expected_desc[static_cast<size_t>(i)].c_str());
    }
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(MixedDateFamilies, DateFilterKeepsBothFamilies) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_mixed_families(db);  // all four are dated 2021-06-15
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);

    // "after 2021-06-15" must keep all four, not just the EXIF-derived ones.
    phig_query_set_date_range(q, "2021-06-15", nullptr);
    int64_t n = -1;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 4);

    // A bound after the whole day excludes all four, from both families.
    phig_query_set_date_range(q, "2021-06-16", nullptr);
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 0);

    // A mid-day bound splits the collection by real time, not by family.
    phig_query_set_date_range(q, "2021-06-15 12:00:00", nullptr);
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 2);  // 23:00 (exif) and 22:00 (mtime)

    phig_query_set_date_range(q, nullptr, "2021-06-15 12:00:00");
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 2);  // 02:00 (exif) and 01:00 (mtime)

    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(MixedDateFamilies, CliSearchDateFilterKeepsBothFamilies) {
    TempDb tmp;
    Database db(tmp.path);
    db.init_schema();
    seed_mixed_families(db);

    Database::SearchCriteria c;
    c.after = "2021-06-15";
    EXPECT_EQ(db.search(c).size(), 4u);

    c.after = "2021-06-15 12:00:00";
    EXPECT_EQ(db.search(c).size(), 2u);

    // A caller passing the EXIF ':' form is normalized the same way.
    c.after = "2021:06:15 12:00:00";
    EXPECT_EQ(db.search(c).size(), 2u);
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
    // A NULL query with a valid, pre-poisoned out-param must still null it —
    // a caller that only checks the return code and then unconditionally
    // frees *out (a common FFI pattern) would otherwise free garbage.
    list = reinterpret_cast<phig_image_list*>(0x1);
    EXPECT_EQ(phig_query_page(ctx, nullptr, 0, 10, &list), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(list, nullptr);
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

// ---- Glob predicates and id resolution (plan 053 decision 2) ----

namespace {
// Inserts one row per path, with the path's own extension rather than a fixed
// one, so an extension filter and a glob can be combined in a test.
void seed_paths(Database& db, const std::vector<std::string>& paths) {
    int i = 0;
    for (const auto& p : paths) {
        ImageInfo info;
        info.path = p;
        auto slash = p.find_last_of('/');
        info.filename = (slash == std::string::npos) ? p : p.substr(slash + 1);
        auto dot = info.filename.find_last_of('.');
        info.extension = (dot == std::string::npos) ? "" : info.filename.substr(dot + 1);
        info.file_size = 1000 + i;
        info.modified_at = "2020-01-01T00:00:00Z";
        info.sha256 = std::string(63, '0') + std::to_string(i % 10);
        db.insert(info);
        i++;
    }
}
}  // namespace

TEST(AbiQuery, AddMatchFiltersByGlob) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_paths(db, {"/a/one.jpg", "/a/two.cr2", "/b/three.jpg"});
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/a/*");

    int64_t n = 0;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 2) << "the ABI had no filename search before this";
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, QueryIdsReturnsEveryMatchingId) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_paths(db, {"/a/one.jpg", "/a/two.cr2", "/b/three.jpg"});
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/a/*");

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    ASSERT_NE(ids, nullptr);
    ASSERT_EQ(ids->count, 2);
    ASSERT_NE(ids->items, nullptr);
    EXPECT_GT(ids->items[0], 0);
    EXPECT_NE(ids->items[0], ids->items[1]);
    phig_id_list_free(ids);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, QueryIdsIgnoresPaginationAndReturnsTheWholeSelection) {
    // PageCriteria defaults limit to 200. Resolving a selection through the
    // paging path would stop there and say nothing, which is the difference
    // between naming a set and reading a page of one.
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed(db, 250);
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    ASSERT_NE(ids, nullptr);
    EXPECT_EQ(ids->count, 250) << "the selection was truncated at the default page size";
    phig_id_list_free(ids);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, QueryIdsOnAnEmptyResultIsAnEmptyListNotAnError) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_paths(db, {"/a/one.jpg"});
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/nothing/*");

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    ASSERT_NE(ids, nullptr);
    EXPECT_EQ(ids->count, 0);
    phig_id_list_free(ids);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, ExcludeWinsOverMatch) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_paths(db, {"/a/one.jpg", "/a/two.cr2", "/b/three.jpg"});
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(q, "/a/*");
    phig_query_add_exclude(q, "*.cr2");

    phig_id_list* ids = nullptr;
    ASSERT_EQ(phig_query_ids(ctx, q, &ids), PHIG_OK);
    EXPECT_EQ(ids->count, 1);
    phig_id_list_free(ids);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, GlobSettersIgnoreNullsLikeTheirNeighbours) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_paths(db, {"/a/one.jpg", "/a/two.cr2", "/b/three.jpg"});
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_query_add_match(nullptr, "/a/*");  // must not crash
    phig_query_add_match(q, nullptr);       // must not narrow
    phig_query_add_match(q, "");            // must not narrow
    phig_query_add_exclude(q, nullptr);
    phig_query_add_exclude(q, "");

    int64_t n = 0;
    ASSERT_EQ(phig_query_count(ctx, q, &n), PHIG_OK);
    EXPECT_EQ(n, 3) << "a NULL or empty pattern must not narrow the query";
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiQuery, QueryIdsRejectsNullArgumentsAndFreeToleratesNull) {
    TempDb tmp;
    {
        Database db(tmp.path);
        db.init_schema();
        seed_paths(db, {"/a/one.jpg"});
    }
    phig_ctx* ctx = open_ro(tmp.path);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);

    phig_id_list* ids = nullptr;
    EXPECT_EQ(phig_query_ids(ctx, nullptr, &ids), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(ids, nullptr);
    EXPECT_EQ(phig_query_ids(ctx, q, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_query_ids(nullptr, q, &ids), PHIG_ERR_INVALID_ARG);
    phig_id_list_free(nullptr);  // must not crash

    phig_query_free(q);
    phig_shutdown(ctx);
}
