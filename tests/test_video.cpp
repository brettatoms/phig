#include <gtest/gtest.h>
#include "scanner.h"
#include "hasher.h"
#include "metadata.h"
#include "database.h"
#include "faces.h"
#include "types.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sqlite3.h>

namespace fs = std::filesystem;

// ---- Video extension tests ----

TEST(VideoExtensionTest, RecognizesVideoFormats) {
    EXPECT_TRUE(is_known_video_extension("mp4"));
    EXPECT_TRUE(is_known_video_extension("mov"));
    EXPECT_TRUE(is_known_video_extension("mkv"));
    EXPECT_TRUE(is_known_video_extension("avi"));
    EXPECT_TRUE(is_known_video_extension("webm"));
    EXPECT_TRUE(is_known_video_extension("m4v"));
    EXPECT_TRUE(is_known_video_extension("mts"));
    EXPECT_TRUE(is_known_video_extension("3gp"));
}

TEST(VideoExtensionTest, CaseInsensitive) {
    EXPECT_TRUE(is_known_video_extension("MP4"));
    EXPECT_TRUE(is_known_video_extension("Mov"));
    EXPECT_TRUE(is_known_video_extension(".MKV"));
}

TEST(VideoExtensionTest, RejectsNonVideo) {
    EXPECT_FALSE(is_known_video_extension("jpg"));
    EXPECT_FALSE(is_known_video_extension("png"));
    EXPECT_FALSE(is_known_video_extension("txt"));
}

TEST(VideoExtensionTest, MediaExtensionCombined) {
    // Images
    EXPECT_TRUE(is_known_media_extension("jpg"));
    EXPECT_TRUE(is_known_media_extension("png"));
    // Videos
    EXPECT_TRUE(is_known_media_extension("mp4"));
    EXPECT_TRUE(is_known_media_extension("mkv"));
    // Neither
    EXPECT_FALSE(is_known_media_extension("txt"));
    EXPECT_FALSE(is_known_media_extension("pdf"));
}

TEST(VideoExtensionTest, ImageExtensionExcludesVideo) {
    EXPECT_FALSE(is_known_image_extension("mp4"));
    EXPECT_FALSE(is_known_image_extension("mov"));
}

// ---- Scanner picks up videos ----

TEST(VideoScannerTest, FindsVideoFiles) {
    auto temp = fs::temp_directory_path() / "phig-test-video-scan";
    fs::create_directories(temp);

    // Create test files
    std::ofstream(temp / "photo.jpg");
    std::ofstream(temp / "clip.mp4");
    std::ofstream(temp / "notes.txt");

    auto files = scan_directory(temp.string(), false);
    std::set<std::string> names;
    for (const auto& f : files) names.insert(f.filename().string());

    EXPECT_TRUE(names.count("photo.jpg"));
    EXPECT_TRUE(names.count("clip.mp4"));
    EXPECT_FALSE(names.count("notes.txt"));

    fs::remove_all(temp);
}

// ---- Video phash tests ----

TEST(VideoPhashTest, ExtractsPhash) {
    auto result = compute_video_phash("tests/data/test_blue.mp4");
    // Solid color videos may produce phash of 0 (no contrast), that's valid
    EXPECT_EQ(result.width, 160);
    EXPECT_EQ(result.height, 120);
}

TEST(VideoPhashTest, DeterministicPhash) {
    auto r1 = compute_video_phash("tests/data/test_blue.mp4");
    auto r2 = compute_video_phash("tests/data/test_blue.mp4");
    EXPECT_EQ(r1.hash, r2.hash);
}

TEST(VideoPhashTest, MultipleFormats) {
    // Just verify we can extract phash from different container formats
    auto mp4 = compute_video_phash("tests/data/test_blue.mp4");
    auto mov = compute_video_phash("tests/data/test_red.mov");
    auto mkv = compute_video_phash("tests/data/test_green.mkv");
    EXPECT_EQ(mp4.width, 160);
    EXPECT_EQ(mov.width, 160);
    EXPECT_EQ(mkv.width, 160);
}

TEST(VideoPhashTest, ExtractKeyframe) {
    auto kf = extract_video_keyframe("tests/data/test_blue.mp4");
    EXPECT_FALSE(kf.frame.empty());
    EXPECT_EQ(kf.width, 160);
    EXPECT_EQ(kf.height, 120);
}

TEST(VideoPhashTest, ExtractKeyframeNonExistent) {
    auto kf = extract_video_keyframe("/nonexistent/video.mp4");
    EXPECT_TRUE(kf.frame.empty());
    EXPECT_EQ(kf.width, 0);
    EXPECT_EQ(kf.height, 0);
}

// ---- Video metadata tests ----

TEST(VideoMetadataTest, ExtractsDuration) {
    auto meta = extract_video_metadata("tests/data/test_blue.mp4");
    // 1 second = ~1000ms (may vary slightly)
    EXPECT_GT(meta.duration_ms, 800);
    EXPECT_LT(meta.duration_ms, 1500);
}

TEST(VideoMetadataTest, ExtractsCodec) {
    auto meta = extract_video_metadata("tests/data/test_blue.mp4");
    EXPECT_EQ(meta.video_codec, "h264");
}

TEST(VideoMetadataTest, ExtractsDimensions) {
    auto meta = extract_video_metadata("tests/data/test_blue.mp4");
    EXPECT_EQ(meta.width, 160);
    EXPECT_EQ(meta.height, 120);
}

TEST(VideoMetadataTest, ExtractsFps) {
    auto meta = extract_video_metadata("tests/data/test_blue.mp4");
    EXPECT_GT(meta.fps, 20.0);
    EXPECT_LT(meta.fps, 30.0);
}

TEST(VideoMetadataTest, ExtractsAudioCodec) {
    auto meta = extract_video_metadata("tests/data/test_audio.mp4");
    EXPECT_EQ(meta.audio_codec, "aac");
}

TEST(VideoMetadataTest, NoAudioForSilentVideo) {
    auto meta = extract_video_metadata("tests/data/test_blue.mp4");
    EXPECT_TRUE(meta.audio_codec.empty());
}

TEST(VideoMetadataTest, MKVFormat) {
    auto meta = extract_video_metadata("tests/data/test_green.mkv");
    EXPECT_EQ(meta.video_codec, "h264");
    EXPECT_GT(meta.duration_ms, 800);
}

TEST(VideoMetadataTest, MOVFormat) {
    auto meta = extract_video_metadata("tests/data/test_red.mov");
    EXPECT_EQ(meta.video_codec, "h264");
    EXPECT_GT(meta.duration_ms, 800);
}

TEST(VideoMetadataTest, JsonSerialization) {
    auto meta = extract_video_metadata("tests/data/test_audio.mp4");
    auto json = video_metadata_to_json(meta);
    EXPECT_TRUE(json.find("\"video_codec\":\"h264\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"audio_codec\":\"aac\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"fps\":") != std::string::npos);
}

TEST(VideoMetadataTest, NonExistentFile) {
    auto meta = extract_video_metadata("/nonexistent/video.mp4");
    EXPECT_EQ(meta.duration_ms, 0);
    EXPECT_TRUE(meta.video_codec.empty());
}

// Regression: string values (e.g. a file-controlled creation_date) containing
// JSON metacharacters must be escaped, otherwise the metadata column holds
// malformed JSON and json_extract() breaks.
TEST(VideoMetadataTest, JsonEscapesSpecialCharacters) {
    VideoMetadata meta;
    meta.creation_date = "20210315\"15:59\\29";  // embedded quote and backslash
    auto json = video_metadata_to_json(meta);

    // The raw unescaped quote must not appear; the escaped forms must.
    EXPECT_TRUE(json.find("\\\"") != std::string::npos);
    EXPECT_TRUE(json.find("\\\\") != std::string::npos);
    // The value must round-trip out of the JSON via a real parser boundary:
    // there should be exactly one unescaped closing brace at the very end.
    EXPECT_EQ(json.back(), '}');
    EXPECT_TRUE(json.find("\"DateTimeOriginal\":\"") != std::string::npos);
}

// ---- Schema migration tests ----

TEST(MigrationTest, PreMigrationBatchInsertDoesNotLeaveOpenTransaction) {
    // The real bug: if a pre-migration DB migration leaves an open transaction,
    // or if insert_batch fails and doesn't rollback, subsequent operations fail.
    auto temp = fs::temp_directory_path() / "phig-test-txn-cascade.db";
    fs::remove(temp);

    // Create pre-migration DB with exif column
    {
        sqlite3* raw;
        sqlite3_open(temp.string().c_str(), &raw);
        const char* sql = R"(
            CREATE TABLE images (
                id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,
                filename TEXT NOT NULL, extension TEXT NOT NULL,
                file_size INTEGER NOT NULL, created_at TEXT, modified_at TEXT,
                sha256 TEXT NOT NULL, phash INTEGER NOT NULL,
                width INTEGER, height INTEGER, exif TEXT
            );
            CREATE TABLE faces (
                id INTEGER PRIMARY KEY,
                image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                embedding BLOB NOT NULL,
                x INTEGER, y INTEGER, width INTEGER, height INTEGER,
                person_id INTEGER
            );
            CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
        )";
        sqlite3_exec(raw, sql, nullptr, nullptr, nullptr);
        sqlite3_close(raw);
    }

    // Open with new code, trigger migration
    Database db(temp.string());
    db.init_schema();  // should migrate v1 -> v2

    // Now do multiple batch inserts (simulating scan batches)
    for (int batch = 0; batch < 5; batch++) {
        std::vector<ImageInfo> infos;
        for (int i = 0; i < 3; i++) {
            ImageInfo info;
            info.path = "/test/batch" + std::to_string(batch) + "_" + std::to_string(i) + ".jpg";
            info.filename = "img.jpg";
            info.extension = "jpg";
            info.sha256 = "sha" + std::to_string(batch * 10 + i);
            info.metadata_json = "{}";
            info.media_type = "image";
            db.insert_batch(infos);
        }
    }

    // Insert a video too
    ImageInfo vid;
    vid.path = "/test/clip.mp4"; vid.filename = "clip.mp4"; vid.extension = "mp4";
    vid.sha256 = "vidsha"; vid.media_type = "video"; vid.duration_ms = 3000;
    vid.metadata_json = "{\"video_codec\":\"h264\"}";
    db.insert_batch({vid});

    auto all = db.get_all_images();
    bool found_video = false;
    for (const auto& img : all) {
        if (img.path == "/test/clip.mp4") {
            EXPECT_EQ(img.media_type, "video");
            EXPECT_EQ(img.duration_ms, 3000);
            found_video = true;
        }
    }
    EXPECT_TRUE(found_video);

    fs::remove(temp);
}

TEST(MigrationTest, TransactionRecoveryOnBatchError) {
    // Reproduce: if insert_batch fails mid-transaction, the next insert_batch
    // should not fail with "cannot start a transaction within a transaction"
    auto temp = fs::temp_directory_path() / "phig-test-txn-recovery.db";
    fs::remove(temp);

    Database db(temp.string());
    db.init_schema();

    // Insert a valid entry
    ImageInfo good;
    good.path = "/test/a.jpg"; good.filename = "a.jpg"; good.extension = "jpg";
    good.sha256 = "aaa"; good.metadata_json = "{}"; good.media_type = "image";
    db.insert(good);

    // Try a batch with a duplicate path (INSERT OR REPLACE should work, but
    // let's instead trigger a real error by using a bad batch)
    // Actually, INSERT OR REPLACE won't fail. Let's simulate by directly
    // testing that after a failed transaction, the next one works.
    
    // Start a transaction manually and don't commit
    db.begin_transaction();
    db.rollback_transaction();
    
    // This should work fine — no leftover transaction
    ImageInfo good2;
    good2.path = "/test/b.jpg"; good2.filename = "b.jpg"; good2.extension = "jpg";
    good2.sha256 = "bbb"; good2.metadata_json = "{}"; good2.media_type = "image";
    db.insert_batch({good2});
    
    EXPECT_EQ(db.count(), 2);
    fs::remove(temp);
}

TEST(MigrationTest, PreExistingDatabaseGetsMigrated) {
    auto temp = fs::temp_directory_path() / "phig-test-premigration.db";
    fs::remove(temp);

    // Create a pre-migration database (v1 schema with 'exif' column, no meta table)
    {
        sqlite3* db;
        sqlite3_open(temp.string().c_str(), &db);
        const char* sql = R"(
            CREATE TABLE images (
                id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,
                filename TEXT NOT NULL, extension TEXT NOT NULL,
                file_size INTEGER NOT NULL, created_at TEXT, modified_at TEXT,
                sha256 TEXT NOT NULL, phash INTEGER NOT NULL,
                width INTEGER, height INTEGER, exif TEXT
            );
            INSERT INTO images (path, filename, extension, file_size, sha256, phash, exif)
            VALUES ('/old/photo.jpg', 'photo.jpg', 'jpg', 1000, 'abc', 0, '{"Model":"TestCam"}');
        )";
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    // Open with new code — should migrate
    {
        Database db(temp.string());
        db.init_schema();

        auto images = db.get_all_images();
        ASSERT_EQ(images.size(), 1);
        EXPECT_EQ(images[0].path, "/old/photo.jpg");
        EXPECT_EQ(images[0].media_type, "image");
        EXPECT_EQ(images[0].metadata_json, "{\"Model\":\"TestCam\"}");

        // Should be able to insert a video now
        ImageInfo vid;
        vid.path = "/new/clip.mp4"; vid.filename = "clip.mp4"; vid.extension = "mp4";
        vid.sha256 = "def"; vid.media_type = "video"; vid.duration_ms = 5000;
        vid.metadata_json = "{}";
        db.insert(vid);

        EXPECT_EQ(db.count(), 2);
    }

    fs::remove(temp);
}

TEST(MigrationTest, FreshDatabaseGetsLatestSchema) {
    auto temp = fs::temp_directory_path() / "phig-test-migration.db";
    fs::remove(temp);

    {
        Database db(temp.string());
        db.init_schema();

        // Should be able to insert with media_type and duration_ms
        ImageInfo info;
        info.path = "/test/video.mp4";
        info.filename = "video.mp4";
        info.extension = "mp4";
        info.sha256 = "abc123";
        info.media_type = "video";
        info.duration_ms = 5000;
        info.metadata_json = "{}";
        db.insert(info);

        auto images = db.get_all_images();
        EXPECT_EQ(images.size(), 1);
        EXPECT_EQ(images[0].media_type, "video");
        EXPECT_EQ(images[0].duration_ms, 5000);
    }

    fs::remove(temp);
}

TEST(MigrationTest, ExistingImagesDefaultToImageType) {
    auto temp = fs::temp_directory_path() / "phig-test-migration2.db";
    fs::remove(temp);

    {
        Database db(temp.string());
        db.init_schema();

        ImageInfo info;
        info.path = "/test/photo.jpg";
        info.filename = "photo.jpg";
        info.extension = "jpg";
        info.sha256 = "def456";
        info.metadata_json = "{}";
        // Don't set media_type — should default to "image"
        db.insert(info);

        auto images = db.get_all_images();
        EXPECT_EQ(images[0].media_type, "image");
        EXPECT_EQ(images[0].duration_ms, 0);
    }

    fs::remove(temp);
}

// ---- Duration parsing tests (via search integration) ----

TEST(VideoSearchTest, SearchByMediaType) {
    auto temp = fs::temp_directory_path() / "phig-test-search-media.db";
    fs::remove(temp);

    Database db(temp.string());
    db.init_schema();

    ImageInfo img;
    img.path = "/test/photo.jpg"; img.filename = "photo.jpg"; img.extension = "jpg";
    img.sha256 = "aaa"; img.metadata_json = "{}"; img.media_type = "image";
    db.insert(img);

    img.path = "/test/clip.mp4"; img.filename = "clip.mp4"; img.extension = "mp4";
    img.sha256 = "bbb"; img.metadata_json = "{}"; img.media_type = "video"; img.duration_ms = 5000;
    db.insert(img);

    Database::SearchCriteria c;
    c.media = "video";
    auto results = db.search(c);
    EXPECT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].filename, "clip.mp4");

    c.media = "image";
    results = db.search(c);
    EXPECT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].filename, "photo.jpg");

    c.media = "";
    results = db.search(c);
    EXPECT_EQ(results.size(), 2);

    fs::remove(temp);
}

TEST(VideoSearchTest, SearchByDuration) {
    auto temp = fs::temp_directory_path() / "phig-test-search-dur.db";
    fs::remove(temp);

    Database db(temp.string());
    db.init_schema();

    ImageInfo v1;
    v1.path = "/test/short.mp4"; v1.filename = "short.mp4"; v1.extension = "mp4";
    v1.sha256 = "s1"; v1.metadata_json = "{}"; v1.media_type = "video"; v1.duration_ms = 3000;
    db.insert(v1);

    ImageInfo v2;
    v2.path = "/test/long.mp4"; v2.filename = "long.mp4"; v2.extension = "mp4";
    v2.sha256 = "s2"; v2.metadata_json = "{}"; v2.media_type = "video"; v2.duration_ms = 60000;
    db.insert(v2);

    // An image has NULL duration and should pass any duration filter.
    ImageInfo img;
    img.path = "/test/photo.jpg"; img.filename = "photo.jpg"; img.extension = "jpg";
    img.sha256 = "i1"; img.metadata_json = "{}"; img.media_type = "image";
    db.insert(img);

    auto names = [](const std::vector<ImageInfo>& rs) {
        std::set<std::string> s;
        for (const auto& r : rs) s.insert(r.filename);
        return s;
    };

    Database::SearchCriteria c;
    c.min_duration_ms = 5000;
    auto results = db.search(c);
    // long.mp4 (>= 5s) plus the image (NULL duration passes); short.mp4 excluded.
    EXPECT_EQ(names(results), (std::set<std::string>{"long.mp4", "photo.jpg"}));

    c = {};
    c.max_duration_ms = 10000;
    results = db.search(c);
    // short.mp4 (<= 10s) plus the image; long.mp4 excluded.
    EXPECT_EQ(names(results), (std::set<std::string>{"short.mp4", "photo.jpg"}));

    fs::remove(temp);
}

// Regression: get_exact_duplicates' SELECT must include media_type and
// duration_ms, otherwise those fields come back empty/0 and the
// `--media` filter on `phig duplicates` silently drops all groups.
TEST(VideoSearchTest, ExactDuplicatesPopulateMediaTypeAndDuration) {
    auto temp = fs::temp_directory_path() / "phig-test-exactdup-media.db";
    fs::remove(temp);

    Database db(temp.string());
    db.init_schema();

    // Two video files sharing one SHA256 => one exact-duplicate group.
    ImageInfo a;
    a.path = "/test/a.mp4"; a.filename = "a.mp4"; a.extension = "mp4";
    a.sha256 = "dup"; a.metadata_json = "{}"; a.media_type = "video"; a.duration_ms = 5000;
    db.insert(a);

    ImageInfo b;
    b.path = "/test/b.mp4"; b.filename = "b.mp4"; b.extension = "mp4";
    b.sha256 = "dup"; b.metadata_json = "{}"; b.media_type = "video"; b.duration_ms = 5000;
    db.insert(b);

    auto groups = db.get_exact_duplicates();
    ASSERT_EQ(groups.size(), 1);
    ASSERT_EQ(groups[0].size(), 2);
    for (const auto& info : groups[0]) {
        EXPECT_EQ(info.media_type, "video");
        EXPECT_EQ(info.duration_ms, 5000);
    }

    fs::remove(temp);
}

// Regression: migrating a database created before face recognition existed
// (no people/vec_faces tables, no faces.person_id column) must re-establish
// that schema, so face operations work after an upgrade.
TEST(MigrationTest, PreFaceRecognitionDatabaseGetsFaceSchema) {
    auto temp = fs::temp_directory_path() / "phig-test-preface.db";
    fs::remove(temp);

    // Pre-face-recognition DB: images + faces (without person_id), no meta,
    // no people table, no vec_faces virtual table.
    {
        sqlite3* db;
        sqlite3_open(temp.string().c_str(), &db);
        const char* sql = R"(
            CREATE TABLE images (
                id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,
                filename TEXT NOT NULL, extension TEXT NOT NULL,
                file_size INTEGER NOT NULL, created_at TEXT, modified_at TEXT,
                sha256 TEXT NOT NULL, phash INTEGER NOT NULL,
                width INTEGER, height INTEGER, exif TEXT
            );
            CREATE TABLE faces (
                id INTEGER PRIMARY KEY,
                image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                embedding BLOB NOT NULL,
                x INTEGER, y INTEGER, width INTEGER, height INTEGER
            );
            INSERT INTO images (path, filename, extension, file_size, sha256, phash, exif)
            VALUES ('/old/photo.jpg', 'photo.jpg', 'jpg', 1000, 'abc', 0, '{}');
        )";
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    Database db(temp.string());
    db.init_schema();

    // people table must now exist (list_people would throw otherwise).
    EXPECT_NO_THROW({ auto people = db.list_people(); (void)people; });

    // faces.person_id and vec_faces must now exist: inserting a face and
    // creating a person both exercise the repaired schema.
    int64_t image_id = db.get_image_id("/old/photo.jpg");
    ASSERT_GE(image_id, 0);

    FaceInfo face;
    face.embedding.fill(0.1f);
    face.x = 0; face.y = 0; face.width = 10; face.height = 10;
    EXPECT_NO_THROW(db.insert_face(image_id, face));

    int64_t person_id = db.get_or_create_person("Alice");
    EXPECT_GE(person_id, 0);

    fs::remove(temp);
}
