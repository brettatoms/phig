#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "database.h"
#include "lockfile.h"
#include "phig/phig.h"

namespace {
struct TempDbPath {
    std::string path;
    TempDbPath() {
        path = (std::filesystem::temp_directory_path() /
                ("phig_mut_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                   .string();
    }
    ~TempDbPath() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }
};

// Seeds three images and returns their ids in insertion order.
std::vector<int64_t> seed_three(const std::string& db_path) {
    Database db(db_path);
    db.init_schema();
    for (int i = 0; i < 3; ++i) {
        ImageInfo info;
        info.path = "/photos/m" + std::to_string(i) + ".jpg";
        info.filename = "m" + std::to_string(i) + ".jpg";
        info.extension = "jpg";
        info.modified_at = "2021-01-01 00:00:00";
        info.sha256 = std::string(63, '1') + std::to_string(i);
        db.insert(info);
    }
    // NOTE: do NOT use get_all_images() to collect ids — it does not SELECT the
    // id column, so every ImageInfo::id comes back 0 and the tests would assert
    // against nothing. get_image_id(path) is the correct lookup.
    std::vector<int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(db.get_image_id("/photos/m" + std::to_string(i) + ".jpg"));
    }
    return ids;
}

phig_ctx* open_rw(const std::string& path) {
    phig_config cfg{};
    cfg.db_path = path.c_str();
    cfg.mode = PHIG_READ_WRITE;
    phig_ctx* ctx = nullptr;
    EXPECT_EQ(phig_init(&cfg, &ctx), PHIG_OK);
    return ctx;
}
}  // namespace

TEST(AbiMutate, TagAddThenRemove) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    ASSERT_EQ(ids.size(), 3u);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_tag_add(ctx, ids.data(), ids.size(), "landscape"), PHIG_OK);
    {
        Database db(tmp.path);
        EXPECT_EQ(db.get_tags_for_image("/photos/m0.jpg").size(), 1u);
    }
    ASSERT_EQ(phig_tag_remove(ctx, ids.data(), ids.size(), "landscape"), PHIG_OK);
    {
        Database db(tmp.path);
        EXPECT_TRUE(db.get_tags_for_image("/photos/m0.jpg").empty());
    }
    phig_shutdown(ctx);
}

TEST(AbiMutate, GallerySetThenClear) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_gallery_set(ctx, ids.data(), 1, "Trip 2024"), PHIG_OK);
    {
        Database db(tmp.path);
        auto rows = db.get_all_images();
        int matched = 0;
        for (const auto& r : rows) {
            if (r.path == "/photos/m0.jpg") {
                EXPECT_EQ(r.gallery, "Trip 2024");
                ++matched;
            }
        }
        EXPECT_EQ(matched, 1);
    }
    ASSERT_EQ(phig_gallery_clear(ctx, ids.data(), 1), PHIG_OK);
    {
        Database db(tmp.path);
        for (const auto& r : db.get_all_images()) {
            if (r.path == "/photos/m0.jpg") EXPECT_TRUE(r.gallery.empty());
        }
    }
    phig_shutdown(ctx);
}

TEST(AbiMutate, FavoriteToggleIsVisibleInQuery) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_favorite_set(ctx, ids.data(), 2, true), PHIG_OK);
    phig_query* q = nullptr;
    ASSERT_EQ(phig_query_new(ctx, &q), PHIG_OK);
    phig_image_list* list = nullptr;
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    int favs = 0;
    for (int64_t i = 0; i < list->count; ++i) {
        if (list->items[i].favorite) ++favs;
    }
    EXPECT_EQ(favs, 2);
    phig_image_list_free(list);

    ASSERT_EQ(phig_favorite_set(ctx, ids.data(), 2, false), PHIG_OK);
    ASSERT_EQ(phig_query_page(ctx, q, 0, 10, &list), PHIG_OK);
    favs = 0;
    for (int64_t i = 0; i < list->count; ++i) {
        if (list->items[i].favorite) ++favs;
    }
    EXPECT_EQ(favs, 0);
    phig_image_list_free(list);
    phig_query_free(q);
    phig_shutdown(ctx);
}

TEST(AbiMutate, ReadOnlyContextRefusesWrites) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_config cfg{};
    cfg.db_path = tmp.path.c_str();
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx = nullptr;
    ASSERT_EQ(phig_init(&cfg, &ctx), PHIG_OK);

    EXPECT_EQ(phig_tag_add(ctx, ids.data(), ids.size(), "x"), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_tag_remove(ctx, ids.data(), ids.size(), "x"), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_gallery_set(ctx, ids.data(), 1, "g"), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_gallery_clear(ctx, ids.data(), 1), PHIG_ERR_READONLY);
    EXPECT_EQ(phig_favorite_set(ctx, ids.data(), 1, true), PHIG_ERR_READONLY);

    // And nothing was written.
    Database db(tmp.path);
    EXPECT_TRUE(db.get_tags_for_image("/photos/m0.jpg").empty());
    phig_shutdown(ctx);
}

TEST(AbiMutate, RejectsBadArguments) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    EXPECT_EQ(phig_tag_add(ctx, nullptr, 3, "t"), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_tag_add(ctx, ids.data(), 3, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_tag_add(ctx, ids.data(), 3, ""), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_gallery_set(ctx, ids.data(), 1, nullptr), PHIG_ERR_INVALID_ARG);
    EXPECT_EQ(phig_tag_add(nullptr, ids.data(), 3, "t"), PHIG_ERR_INVALID_ARG);
    phig_shutdown(ctx);
}

TEST(AbiMutate, GallerySetRejectsEmptyString) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    ASSERT_EQ(phig_gallery_set(ctx, ids.data(), 1, "Trip 2024"), PHIG_OK);
    EXPECT_EQ(phig_gallery_set(ctx, ids.data(), 1, ""), PHIG_ERR_INVALID_ARG);
    {
        Database db(tmp.path);
        for (const auto& r : db.get_all_images()) {
            if (r.path == "/photos/m0.jpg") EXPECT_EQ(r.gallery, "Trip 2024");
        }
    }
    phig_shutdown(ctx);
}

TEST(AbiMutate, EmptyIdListIsNoOp) {
    TempDbPath tmp;
    seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);
    int64_t none = 0;
    EXPECT_EQ(phig_tag_add(ctx, &none, 0, "t"), PHIG_OK);
    EXPECT_EQ(phig_favorite_set(ctx, &none, 0, true), PHIG_OK);
    phig_shutdown(ctx);
}

TEST(AbiMutate, UnknownIdsAreSkippedNotAnError) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);
    std::vector<int64_t> mixed = {ids[0], 999999};
    EXPECT_EQ(phig_tag_add(ctx, mixed.data(), mixed.size(), "t"), PHIG_OK);
    Database db(tmp.path);
    EXPECT_EQ(db.get_tags_for_image("/photos/m0.jpg").size(), 1u);
    phig_shutdown(ctx);
}

TEST(AbiMutate, WriteFailsWhileAnotherProcessHoldsTheWriterLock) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    // Hold the lock the way a running CLI job would, then attempt an ABI write.
    // Construction succeeds or throws LockBusy; there is no acquired() method.
    WriterLock held(tmp.path + ".lock", /*no_wait=*/true);

    phig_status st = phig_tag_add(ctx, ids.data(), ids.size(), "contended");
    EXPECT_EQ(st, PHIG_ERR_DB);
    EXPECT_STRNE(phig_last_error(ctx), "");

    Database db(tmp.path);
    EXPECT_TRUE(db.get_tags_for_image("/photos/m0.jpg").empty());
    phig_shutdown(ctx);
}

TEST(AbiMutate, WriteSucceedsAfterTheLockIsReleased) {
    TempDbPath tmp;
    auto ids = seed_three(tmp.path);
    phig_ctx* ctx = open_rw(tmp.path);

    {
        WriterLock held(tmp.path + ".lock", /*no_wait=*/true);
        EXPECT_EQ(phig_tag_add(ctx, ids.data(), 1, "blocked"), PHIG_ERR_DB);
    }  // released here

    // The ABI must recover, not stay wedged after a contended attempt.
    EXPECT_EQ(phig_tag_add(ctx, ids.data(), 1, "allowed"), PHIG_OK);
    Database db(tmp.path);
    EXPECT_EQ(db.get_tags_for_image("/photos/m0.jpg").size(), 1u);
    phig_shutdown(ctx);
}
