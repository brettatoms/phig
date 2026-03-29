#include <gtest/gtest.h>
#include "database.h"
#include "faces.h"
#include "types.h"
#include <filesystem>
#include <array>

namespace fs = std::filesystem;

// ---- Database face operations tests ----

class FaceDatabaseTest : public ::testing::Test {
protected:
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        db_path = (fs::temp_directory_path() / "phig-test-faces.db").string();
        fs::remove(db_path);
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove(db_path);
    }

    ImageInfo make_image(const std::string& path, const std::string& sha = "abc123") {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.file_size = 1000;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = sha;
        info.phash = 0;
        info.width = 640;
        info.height = 480;
        info.exif_json = "{}";
        return info;
    }

    FaceInfo make_face(float base_val = 0.0f, int x = 0, int y = 0, int w = 100, int h = 100) {
        FaceInfo fi;
        for (int i = 0; i < 128; i++) {
            fi.embedding[i] = base_val + (i * 0.01f);
        }
        fi.x = x; fi.y = y; fi.width = w; fi.height = h;
        return fi;
    }
};

TEST_F(FaceDatabaseTest, InsertAndHasFaces) {
    db->insert(make_image("/test/photo.jpg", "sha1"));
    int64_t image_id = db->get_image_id("/test/photo.jpg");
    ASSERT_GE(image_id, 0);

    EXPECT_FALSE(db->has_faces("/test/photo.jpg"));

    db->insert_face(image_id, make_face(0.0f));
    EXPECT_TRUE(db->has_faces("/test/photo.jpg"));
}

TEST_F(FaceDatabaseTest, HasFacesNonexistentImage) {
    EXPECT_FALSE(db->has_faces("/nonexistent/photo.jpg"));
}

TEST_F(FaceDatabaseTest, MultipleFacesPerImage) {
    db->insert(make_image("/test/group.jpg", "sha1"));
    int64_t image_id = db->get_image_id("/test/group.jpg");

    db->insert_face(image_id, make_face(0.0f, 10, 10, 50, 50));
    db->insert_face(image_id, make_face(1.0f, 200, 10, 50, 50));
    db->insert_face(image_id, make_face(2.0f, 100, 200, 50, 50));

    EXPECT_TRUE(db->has_faces("/test/group.jpg"));
}

TEST_F(FaceDatabaseTest, GetImageId) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));

    EXPECT_GE(db->get_image_id("/test/a.jpg"), 0);
    EXPECT_GE(db->get_image_id("/test/b.jpg"), 0);
    EXPECT_EQ(db->get_image_id("/test/nonexistent.jpg"), -1);
}

TEST_F(FaceDatabaseTest, GetOrCreatePerson) {
    int64_t id1 = db->get_or_create_person("Alice");
    int64_t id2 = db->get_or_create_person("Bob");
    int64_t id3 = db->get_or_create_person("Alice"); // same as id1

    EXPECT_GE(id1, 0);
    EXPECT_GE(id2, 0);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(id1, id3); // same person
}

TEST_F(FaceDatabaseTest, SetFacePerson) {
    db->insert(make_image("/test/photo.jpg", "sha1"));
    int64_t image_id = db->get_image_id("/test/photo.jpg");
    db->insert_face(image_id, make_face(0.0f));
    int64_t face_id = 1; // first face inserted

    int64_t person_id = db->get_or_create_person("Alice");
    db->set_face_person(face_id, person_id);

    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1);
    EXPECT_EQ(people[0].name, "Alice");
    EXPECT_EQ(people[0].face_count, 1);
}

TEST_F(FaceDatabaseTest, ListPeopleEmpty) {
    auto people = db->list_people();
    EXPECT_TRUE(people.empty());
}

TEST_F(FaceDatabaseTest, ListPeopleMultiple) {
    int64_t alice_id = db->get_or_create_person("Alice");
    int64_t bob_id = db->get_or_create_person("Bob");

    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));
    int64_t img1 = db->get_image_id("/test/a.jpg");
    int64_t img2 = db->get_image_id("/test/b.jpg");

    // Alice has 2 faces, Bob has 1
    db->insert_face(img1, make_face(0.0f));
    db->set_face_person(1, alice_id);
    db->insert_face(img1, make_face(1.0f));
    db->set_face_person(2, alice_id);
    db->insert_face(img2, make_face(2.0f));
    db->set_face_person(3, bob_id);

    auto people = db->list_people();
    ASSERT_EQ(people.size(), 2);
    // Sorted by name
    EXPECT_EQ(people[0].name, "Alice");
    EXPECT_EQ(people[0].face_count, 2);
    EXPECT_EQ(people[1].name, "Bob");
    EXPECT_EQ(people[1].face_count, 1);
}

TEST_F(FaceDatabaseTest, GetImagesForPerson) {
    int64_t alice_id = db->get_or_create_person("Alice");

    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));
    db->insert(make_image("/test/c.jpg", "sha3"));
    int64_t img1 = db->get_image_id("/test/a.jpg");
    int64_t img2 = db->get_image_id("/test/b.jpg");

    db->insert_face(img1, make_face(0.0f));
    db->set_face_person(1, alice_id);
    db->insert_face(img2, make_face(1.0f));
    db->set_face_person(2, alice_id);

    auto paths = db->get_images_for_person("Alice");
    EXPECT_EQ(paths.size(), 2);

    auto paths_bob = db->get_images_for_person("Bob");
    EXPECT_TRUE(paths_bob.empty());
}

TEST_F(FaceDatabaseTest, AutoLabelFaceMatch) {
    // Create a known person
    int64_t alice_id = db->get_or_create_person("Alice");
    db->insert(make_image("/test/ref.jpg", "sha1"));
    int64_t ref_img = db->get_image_id("/test/ref.jpg");

    FaceInfo ref_face = make_face(0.0f);
    db->insert_face(ref_img, ref_face);
    db->set_face_person(1, alice_id);

    // Insert a similar face (small distance)
    db->insert(make_image("/test/new.jpg", "sha2"));
    int64_t new_img = db->get_image_id("/test/new.jpg");
    FaceInfo similar_face = make_face(0.001f); // very close embedding
    db->insert_face(new_img, similar_face);

    std::string result = db->auto_label_face(2, similar_face.embedding);
    EXPECT_EQ(result, "Alice");
}

TEST_F(FaceDatabaseTest, AutoLabelFaceNoMatch) {
    // Create a known person
    int64_t alice_id = db->get_or_create_person("Alice");
    db->insert(make_image("/test/ref.jpg", "sha1"));
    int64_t ref_img = db->get_image_id("/test/ref.jpg");

    FaceInfo ref_face = make_face(0.0f);
    db->insert_face(ref_img, ref_face);
    db->set_face_person(1, alice_id);

    // Insert a very different face
    db->insert(make_image("/test/new.jpg", "sha2"));
    int64_t new_img = db->get_image_id("/test/new.jpg");
    FaceInfo different_face = make_face(100.0f); // very different embedding
    db->insert_face(new_img, different_face);

    std::string result = db->auto_label_face(2, different_face.embedding);
    EXPECT_EQ(result, ""); // no match
}

TEST_F(FaceDatabaseTest, AutoLabelNoPeople) {
    db->insert(make_image("/test/photo.jpg", "sha1"));
    int64_t img = db->get_image_id("/test/photo.jpg");
    FaceInfo face = make_face(0.0f);
    db->insert_face(img, face);

    std::string result = db->auto_label_face(1, face.embedding);
    EXPECT_EQ(result, ""); // no people defined
}

// ---- FaceInfo / embedding_distance tests ----

TEST(EmbeddingTest, DistanceZero) {
    std::array<float, 128> a{}, b{};
    for (int i = 0; i < 128; i++) { a[i] = b[i] = i * 0.1f; }
    EXPECT_FLOAT_EQ(embedding_distance(a, b), 0.0f);
}

TEST(EmbeddingTest, DistanceNonZero) {
    std::array<float, 128> a{}, b{};
    for (int i = 0; i < 128; i++) { a[i] = 0.0f; b[i] = 1.0f; }
    float dist = embedding_distance(a, b);
    // sqrt(128 * 1^2) = sqrt(128) ≈ 11.31
    EXPECT_NEAR(dist, std::sqrt(128.0f), 0.01f);
}

TEST(EmbeddingTest, DistanceSymmetric) {
    std::array<float, 128> a{}, b{};
    for (int i = 0; i < 128; i++) { a[i] = i * 0.1f; b[i] = i * 0.2f; }
    EXPECT_FLOAT_EQ(embedding_distance(a, b), embedding_distance(b, a));
}
