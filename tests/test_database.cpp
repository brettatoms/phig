#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

class DatabaseTest : public ::testing::Test {
   protected:
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        db_path = (fs::temp_directory_path() / "image-cleanup-test.db").string();
        fs::remove(db_path);
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }

    ImageInfo make_image(const std::string& path, const std::string& sha = "abc123",
                         uint64_t phash = 0) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.file_size = 1000;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = sha;
        info.phash = phash;
        info.width = 640;
        info.height = 480;
        info.metadata_json = "{}";
        return info;
    }
};

TEST_F(DatabaseTest, EmptyDatabaseCount) { EXPECT_EQ(db->count(), 0); }

TEST_F(DatabaseTest, InsertAndCount) {
    db->insert(make_image("/test/a.jpg"));
    EXPECT_EQ(db->count(), 1);
}

TEST_F(DatabaseTest, InsertMultiple) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));
    db->insert(make_image("/test/c.jpg", "sha3"));
    EXPECT_EQ(db->count(), 3);
}

TEST_F(DatabaseTest, InsertBatch) {
    std::vector<ImageInfo> batch = {
        make_image("/test/a.jpg", "sha1"),
        make_image("/test/b.jpg", "sha2"),
        make_image("/test/c.jpg", "sha3"),
    };
    db->insert_batch(batch);
    EXPECT_EQ(db->count(), 3);
}

TEST_F(DatabaseTest, InsertReplaceOnDuplicatePath) {
    auto img = make_image("/test/a.jpg", "sha_old");
    db->insert(img);
    EXPECT_EQ(db->count(), 1);

    img.sha256 = "sha_new";
    db->insert(img);
    EXPECT_EQ(db->count(), 1);  // replaced, not duplicated
}

TEST_F(DatabaseTest, GetModifiedAtExists) {
    auto img = make_image("/test/a.jpg");
    img.modified_at = "2024-06-15T12:00:00Z";
    db->insert(img);

    auto mtime = db->get_modified_at("/test/a.jpg");
    ASSERT_TRUE(mtime.has_value());
    EXPECT_EQ(*mtime, "2024-06-15T12:00:00Z");
}

TEST_F(DatabaseTest, GetModifiedAtNotExists) {
    auto mtime = db->get_modified_at("/nonexistent/path.jpg");
    EXPECT_FALSE(mtime.has_value());
}

TEST_F(DatabaseTest, UpdatePath) {
    db->insert(make_image("/old/path/photo.jpg"));
    db->update_path("/old/path/photo.jpg", "/new/path/photo.jpg");

    EXPECT_FALSE(db->get_modified_at("/old/path/photo.jpg").has_value());
    EXPECT_TRUE(db->get_modified_at("/new/path/photo.jpg").has_value());
    EXPECT_EQ(db->count(), 1);
}

TEST_F(DatabaseTest, UpdatePathWhenDestAlreadyExists) {
    // A row already tracks the destination (e.g. it was scanned in a prior run).
    // Moving another file onto that path must not hit UNIQUE(path); the stale
    // dest row is dropped and the source row is repointed.
    db->insert(make_image("/src/photo.jpg", "sha_source"));
    db->insert(make_image("/dst/photo.jpg", "sha_stale_dest"));
    EXPECT_EQ(db->count(), 2);

    EXPECT_NO_THROW(db->update_path("/src/photo.jpg", "/dst/photo.jpg"));

    // One row survives, at the dest, describing the file just moved in.
    EXPECT_EQ(db->count(), 1);
    EXPECT_FALSE(db->get_modified_at("/src/photo.jpg").has_value());
    EXPECT_TRUE(db->get_modified_at("/dst/photo.jpg").has_value());
    EXPECT_EQ(*db->get_sha256("/dst/photo.jpg"), "sha_source");
}

TEST_F(DatabaseTest, SetTagCreatesAndResolvesTag) {
    // Guards the checked tag-insert write: creating a new tag and applying it
    // must still succeed (the write result is now verified, not ignored).
    db->insert(make_image("/lib/x.jpg", "sx"));
    int affected = db->set_tag({"/lib/x.jpg"}, "sunset", /*add=*/true);
    EXPECT_EQ(affected, 1);
    auto tags = db->get_tags_for_image("/lib/x.jpg");
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "sunset");
}

TEST_F(DatabaseTest, UpdatePathSourceEqualsDestKeepsRow) {
    // A no-op move (old == new) must not delete the row. The DELETE-then-UPDATE
    // path would otherwise drop the row as a "stale dest" and repoint nothing.
    db->insert(make_image("/lib/photo.jpg", "sha_x"));
    EXPECT_EQ(db->count(), 1);

    EXPECT_NO_THROW(db->update_path("/lib/photo.jpg", "/lib/photo.jpg"));

    EXPECT_EQ(db->count(), 1);
    EXPECT_TRUE(db->get_modified_at("/lib/photo.jpg").has_value());
    EXPECT_EQ(*db->get_sha256("/lib/photo.jpg"), "sha_x");
}

TEST_F(DatabaseTest, DeletePathsCascadesTagsAndFaces) {
    // delete_paths must remove the image row, its cascaded tags (FK ON DELETE
    // CASCADE, dead unless PRAGMA foreign_keys=ON), and its face vectors in
    // vec_faces (a vec0 virtual table that FK cascade can never reach).
    db->insert(make_image("/lib/portrait.jpg", "sha_face"));
    db->insert(make_image("/lib/keep.jpg", "sha_keep"));

    // Tag both, and add a face + embedding to the one we'll delete.
    db->set_tag({"/lib/portrait.jpg"}, "vacation", /*add=*/true);
    db->set_tag({"/lib/keep.jpg"}, "vacation", /*add=*/true);

    FaceInfo face;
    face.embedding.fill(0.5f);
    face.x = 1;
    face.y = 2;
    face.width = 3;
    face.height = 4;
    int64_t img_id = db->get_image_id("/lib/portrait.jpg");
    ASSERT_GE(img_id, 0);
    db->insert_face(img_id, face);

    // Sanity: the face is searchable before deletion.
    EXPECT_FALSE(db->search_faces(face.embedding, /*threshold=*/1.0f, /*limit=*/5).empty());

    db->delete_paths({"/lib/portrait.jpg"});

    // Image row gone; the other image and its tag untouched.
    EXPECT_FALSE(db->get_modified_at("/lib/portrait.jpg").has_value());
    EXPECT_TRUE(db->get_modified_at("/lib/keep.jpg").has_value());
    EXPECT_TRUE(db->get_tags_for_image("/lib/portrait.jpg").empty());  // cascaded away
    EXPECT_FALSE(db->get_tags_for_image("/lib/keep.jpg").empty());     // untouched

    // The orphaned face vector must be gone — otherwise search returns a hit
    // that JOINs to a nonexistent face row.
    EXPECT_TRUE(db->search_faces(face.embedding, /*threshold=*/1.0f, /*limit=*/5).empty());
}

TEST_F(DatabaseTest, GetPathsWithPrefix) {
    db->insert(make_image("/photos/2024/a.jpg", "sha1"));
    db->insert(make_image("/photos/2024/b.jpg", "sha2"));
    db->insert(make_image("/photos/2023/c.jpg", "sha3"));
    db->insert(make_image("/backup/d.jpg", "sha4"));

    auto paths = db->get_paths_with_prefix("/photos/2024/");
    EXPECT_EQ(paths.size(), 2);

    paths = db->get_paths_with_prefix("/photos/");
    EXPECT_EQ(paths.size(), 3);

    paths = db->get_paths_with_prefix("/backup/");
    EXPECT_EQ(paths.size(), 1);

    paths = db->get_paths_with_prefix("/nonexistent/");
    EXPECT_EQ(paths.size(), 0);
}

TEST_F(DatabaseTest, GetPathsWithPrefixTreatsUnderscoreLiterally) {
    // '_' is a LIKE single-char wildcard. Under the old LIKE query, prefix
    // "/a_b/" would also match "/aXb/". Range comparison treats it literally.
    db->insert(make_image("/a_b/photo.jpg", "s1"));
    db->insert(make_image("/aXb/photo.jpg", "s2"));  // sibling that '_' would wrongly match
    db->insert(make_image("/a_b/more.jpg", "s3"));

    auto paths = db->get_paths_with_prefix("/a_b/");
    EXPECT_EQ(paths.size(), 2);  // only the two /a_b/ rows, not /aXb/
    for (const auto& p : paths) EXPECT_NE(p, "/aXb/photo.jpg");
}

TEST_F(DatabaseTest, GetPathsWithPrefixTreatsPercentLiterally) {
    // '%' is a LIKE any-run wildcard. Prefix "/50%/" must match only that dir.
    db->insert(make_image("/50%/a.jpg", "s1"));
    db->insert(make_image("/50-off/a.jpg", "s2"));

    auto paths = db->get_paths_with_prefix("/50%/");
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths[0], "/50%/a.jpg");
}

TEST_F(DatabaseTest, GetPathsWithPrefixBoundaryDoesNotMatchSiblingSharingPrefix) {
    // "/photos" prefix must not swallow "/photos-backup/"; the range upper bound
    // stops exactly at the prefix boundary.
    db->insert(make_image("/photos/a.jpg", "s1"));
    db->insert(make_image("/photos-backup/a.jpg", "s2"));

    auto paths = db->get_paths_with_prefix("/photos/");
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths[0], "/photos/a.jpg");
}

TEST_F(DatabaseTest, GetPathsWithPrefixEmptyReturnsAll) {
    db->insert(make_image("/x/a.jpg", "s1"));
    db->insert(make_image("/y/b.jpg", "s2"));
    EXPECT_EQ(db->get_paths_with_prefix("").size(), 2);
}

TEST_F(DatabaseTest, DeletePaths) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));
    db->insert(make_image("/test/c.jpg", "sha3"));

    db->delete_paths({"/test/a.jpg", "/test/c.jpg"});
    EXPECT_EQ(db->count(), 1);

    auto mtime = db->get_modified_at("/test/b.jpg");
    EXPECT_TRUE(mtime.has_value());
}

TEST_F(DatabaseTest, DeletePathsEmpty) {
    db->insert(make_image("/test/a.jpg"));
    db->delete_paths({});
    EXPECT_EQ(db->count(), 1);
}

TEST_F(DatabaseTest, GetExactDuplicates) {
    db->insert(make_image("/test/a.jpg", "sha_dup"));
    db->insert(make_image("/test/b.jpg", "sha_dup"));
    db->insert(make_image("/test/c.jpg", "sha_unique"));

    auto groups = db->get_exact_duplicates();
    ASSERT_EQ(groups.size(), 1);
    EXPECT_EQ(groups[0].size(), 2);
    EXPECT_EQ(groups[0][0].sha256, "sha_dup");
}

TEST_F(DatabaseTest, GetExactDuplicatesNone) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));

    auto groups = db->get_exact_duplicates();
    EXPECT_TRUE(groups.empty());
}

TEST_F(DatabaseTest, GetExactDuplicatesWithPrefix) {
    db->insert(make_image("/folderA/a.jpg", "sha_dup"));
    db->insert(make_image("/folderA/b.jpg", "sha_dup"));
    db->insert(make_image("/folderB/c.jpg", "sha_dup"));

    auto groups = db->get_exact_duplicates("/folderA/");
    ASSERT_EQ(groups.size(), 1);
    EXPECT_EQ(groups[0].size(), 2);  // only folderA entries
}

TEST_F(DatabaseTest, GetAllImages) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));

    auto all = db->get_all_images();
    EXPECT_EQ(all.size(), 2);
}

TEST_F(DatabaseTest, GetAllImagesWithPrefix) {
    db->insert(make_image("/photos/a.jpg", "sha1"));
    db->insert(make_image("/photos/b.jpg", "sha2"));
    db->insert(make_image("/backup/c.jpg", "sha3"));

    auto filtered = db->get_all_images("/photos/");
    EXPECT_EQ(filtered.size(), 2);
}

TEST_F(DatabaseTest, PurgeByGlob) {
    db->insert(make_image("/test/vacation1.jpg", "sha1"));
    db->insert(make_image("/test/vacation2.jpg", "sha2"));
    db->insert(make_image("/test/winter.jpg", "sha3"));

    auto matched = db->purge("", "vacation*");
    EXPECT_EQ(matched.size(), 2);

    db->delete_paths(matched);
    EXPECT_EQ(db->count(), 1);
}

TEST_F(DatabaseTest, PurgeByPath) {
    db->insert(make_image("/folderA/a.jpg", "sha1"));
    db->insert(make_image("/folderA/b.jpg", "sha2"));
    db->insert(make_image("/folderB/c.jpg", "sha3"));

    auto matched = db->purge("/folderA/", "");
    EXPECT_EQ(matched.size(), 2);
}

TEST_F(DatabaseTest, PurgeByPathAndGlob) {
    db->insert(make_image("/folderA/vacation1.jpg", "sha1"));
    db->insert(make_image("/folderA/winter.jpg", "sha2"));
    db->insert(make_image("/folderB/vacation2.jpg", "sha3"));

    auto matched = db->purge("/folderA/", "vacation*");
    EXPECT_EQ(matched.size(), 1);
}

TEST_F(DatabaseTest, PurgeNoMatch) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    auto matched = db->purge("", "nonexistent*");
    EXPECT_TRUE(matched.empty());
}

TEST_F(DatabaseTest, CreatesDatabaseDirectory) {
    auto nested_path =
        (fs::temp_directory_path() / "image-cleanup-test-nested" / "sub" / "test.db").string();
    {
        Database nested_db(nested_path);
        nested_db.init_schema();
        EXPECT_EQ(nested_db.count(), 0);
    }
    fs::remove_all(fs::temp_directory_path() / "image-cleanup-test-nested");
}

// ---- Tags & favorites (plan 011) ----

static bool has_tag(const std::vector<std::string>& tags, const std::string& t) {
    return std::find(tags.begin(), tags.end(), t) != tags.end();
}

static int64_t tag_count(const std::vector<std::pair<std::string, int64_t>>& counts,
                         const std::string& name) {
    for (const auto& [n, c] : counts) {
        if (n == name) return c;
    }
    return -1;
}

TEST_F(DatabaseTest, FavoriteTagSeeded) {
    // The built-in favorite tag exists on a fresh DB, with zero associations.
    EXPECT_EQ(tag_count(db->get_tag_counts(), "favorite"), 0);
}

TEST_F(DatabaseTest, SetTagAddsAndQueries) {
    db->insert(make_image("/t/a.jpg"));
    EXPECT_EQ(db->set_tag({"/t/a.jpg"}, "vacation", true), 1);
    auto tags = db->get_tags_for_image("/t/a.jpg");
    EXPECT_EQ(tags.size(), 1u);
    EXPECT_TRUE(has_tag(tags, "vacation"));
}

TEST_F(DatabaseTest, SetTagRemoves) {
    db->insert(make_image("/t/a.jpg"));
    db->set_tag({"/t/a.jpg"}, "vacation", true);
    EXPECT_EQ(db->set_tag({"/t/a.jpg"}, "vacation", false), 1);
    EXPECT_TRUE(db->get_tags_for_image("/t/a.jpg").empty());
}

TEST_F(DatabaseTest, SetTagCountsOnlyExistingPaths) {
    db->insert(make_image("/t/a.jpg", "s1"));
    db->insert(make_image("/t/b.jpg", "s2"));
    EXPECT_EQ(db->set_tag({"/t/a.jpg", "/t/b.jpg", "/t/missing.jpg"}, "x", true), 2);
}

TEST_F(DatabaseTest, SetTagAddIsIdempotent) {
    db->insert(make_image("/t/a.jpg"));
    db->set_tag({"/t/a.jpg"}, "x", true);
    db->set_tag({"/t/a.jpg"}, "x", true);
    EXPECT_EQ(db->get_tags_for_image("/t/a.jpg").size(), 1u);
}

TEST_F(DatabaseTest, TagCountsReflectAssociations) {
    db->insert(make_image("/t/a.jpg", "s1"));
    db->insert(make_image("/t/b.jpg", "s2"));
    db->set_tag({"/t/a.jpg", "/t/b.jpg"}, "vacation", true);
    db->set_tag({"/t/a.jpg"}, "family", true);
    auto counts = db->get_tag_counts();
    EXPECT_EQ(tag_count(counts, "vacation"), 2);
    EXPECT_EQ(tag_count(counts, "family"), 1);
}

TEST_F(DatabaseTest, FavoriteBehavesLikeATag) {
    db->insert(make_image("/t/a.jpg"));
    EXPECT_EQ(db->set_tag({"/t/a.jpg"}, "favorite", true), 1);
    EXPECT_TRUE(has_tag(db->get_tags_for_image("/t/a.jpg"), "favorite"));
    EXPECT_EQ(tag_count(db->get_tag_counts(), "favorite"), 1);
}

// ---- Galleries & tag/gallery search (plan 011, stage 2) ----

TEST_F(DatabaseTest, SetGalleryAndSearch) {
    db->insert(make_image("/g/a.jpg", "s1"));
    db->insert(make_image("/g/b.jpg", "s2"));
    EXPECT_EQ(db->set_gallery({"/g/a.jpg"}, "Italy 2024", false), 1);
    Database::SearchCriteria c;
    c.gallery = "Italy 2024";
    auto r = db->search(c);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].path, "/g/a.jpg");
}

TEST_F(DatabaseTest, GalleryNoOverwriteSkips) {
    db->insert(make_image("/g/a.jpg"));
    db->set_gallery({"/g/a.jpg"}, "Italy", false);
    EXPECT_EQ(db->set_gallery({"/g/a.jpg"}, "France", true), 0);  // skipped
    Database::SearchCriteria c;
    c.gallery = "France";
    EXPECT_TRUE(db->search(c).empty());
    c.gallery = "Italy";
    EXPECT_EQ(db->search(c).size(), 1u);
}

TEST_F(DatabaseTest, GalleryOverwritesByDefault) {
    db->insert(make_image("/g/a.jpg"));
    db->set_gallery({"/g/a.jpg"}, "Italy", false);
    EXPECT_EQ(db->set_gallery({"/g/a.jpg"}, "France", false), 1);
    Database::SearchCriteria c;
    c.gallery = "France";
    EXPECT_EQ(db->search(c).size(), 1u);
}

TEST_F(DatabaseTest, ClearGallery) {
    db->insert(make_image("/g/a.jpg"));
    db->set_gallery({"/g/a.jpg"}, "Italy", false);
    EXPECT_EQ(db->clear_gallery({"/g/a.jpg"}), 1);
    Database::SearchCriteria c;
    c.gallery = "Italy";
    EXPECT_TRUE(db->search(c).empty());
}

TEST_F(DatabaseTest, GalleryCounts) {
    db->insert(make_image("/g/a.jpg", "s1"));
    db->insert(make_image("/g/b.jpg", "s2"));
    db->insert(make_image("/g/c.jpg", "s3"));
    db->set_gallery({"/g/a.jpg", "/g/b.jpg"}, "Italy", false);
    db->set_gallery({"/g/c.jpg"}, "France", false);
    auto counts = db->get_gallery_counts();
    EXPECT_EQ(tag_count(counts, "Italy"), 2);
    EXPECT_EQ(tag_count(counts, "France"), 1);
}

TEST_F(DatabaseTest, SearchByTag) {
    db->insert(make_image("/s/a.jpg", "s1"));
    db->insert(make_image("/s/b.jpg", "s2"));
    db->set_tag({"/s/a.jpg"}, "vacation", true);
    Database::SearchCriteria c;
    c.tags = {"vacation"};
    auto r = db->search(c);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].path, "/s/a.jpg");
}

TEST_F(DatabaseTest, SearchByMultipleTagsAnd) {
    db->insert(make_image("/s/a.jpg", "s1"));
    db->insert(make_image("/s/b.jpg", "s2"));
    db->set_tag({"/s/a.jpg", "/s/b.jpg"}, "vacation", true);
    db->set_tag({"/s/a.jpg"}, "family", true);
    Database::SearchCriteria c;
    c.tags = {"vacation", "family"};
    auto r = db->search(c);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].path, "/s/a.jpg");
}

TEST_F(DatabaseTest, SearchByFavoriteTag) {
    db->insert(make_image("/s/a.jpg", "s1"));
    db->insert(make_image("/s/b.jpg", "s2"));
    db->set_tag({"/s/b.jpg"}, "favorite", true);
    Database::SearchCriteria c;
    c.tags = {"favorite"};
    auto r = db->search(c);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].path, "/s/b.jpg");
}

TEST_F(DatabaseTest, BlurScoreRoundTrips) {
    auto img = make_image("/b/x.jpg");
    img.blur_score = 123.5;
    db->insert(img);
    auto all = db->get_all_images();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_DOUBLE_EQ(all[0].blur_score, 123.5);
}

TEST_F(DatabaseTest, UnscoredImageHasNegativeBlurScore) {
    db->insert(make_image("/b/x.jpg"));  // make_image leaves blur_score = -1
    auto all = db->get_all_images();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_LT(all[0].blur_score, 0.0);  // stored NULL -> read back as -1
}

TEST_F(DatabaseTest, SearchBySharpness) {
    auto sharp = make_image("/b/sharp.jpg", "s1");
    sharp.blur_score = 800;
    auto blurry = make_image("/b/blurry.jpg", "s2");
    blurry.blur_score = 50;
    db->insert(sharp);
    db->insert(blurry);

    Database::SearchCriteria hi;
    hi.min_sharpness = 500;
    auto r = db->search(hi);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].path, "/b/sharp.jpg");

    Database::SearchCriteria lo;
    lo.max_sharpness = 100;
    auto r2 = db->search(lo);
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_EQ(r2[0].path, "/b/blurry.jpg");
}

TEST_F(DatabaseTest, GetAllImagesPopulatesGalleryAndFavorite) {
    db->insert(make_image("/o/a.jpg", "s1"));
    db->insert(make_image("/o/b.jpg", "s2"));
    db->set_gallery({"/o/a.jpg"}, "Trip", false);
    db->set_tag({"/o/a.jpg"}, "favorite", true);
    for (const auto& img : db->get_all_images()) {
        if (img.path == "/o/a.jpg") {
            EXPECT_EQ(img.gallery, "Trip");
            EXPECT_TRUE(img.favorite);
        } else {
            EXPECT_TRUE(img.gallery.empty());
            EXPECT_FALSE(img.favorite);
        }
    }
}
