#include <gtest/gtest.h>
#include "database.h"
#include "types.h"
#include <filesystem>

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
        info.exif_json = "{}";
        return info;
    }
};

TEST_F(DatabaseTest, EmptyDatabaseCount) {
    EXPECT_EQ(db->count(), 0);
}

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
    EXPECT_EQ(db->count(), 1); // replaced, not duplicated
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
    EXPECT_EQ(groups[0].size(), 2); // only folderA entries
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
    auto nested_path = (fs::temp_directory_path() /
        "image-cleanup-test-nested" / "sub" / "test.db").string();
    {
        Database nested_db(nested_path);
        nested_db.init_schema();
        EXPECT_EQ(nested_db.count(), 0);
    }
    fs::remove_all(fs::temp_directory_path() / "image-cleanup-test-nested");
}
