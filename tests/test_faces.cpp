#include <gtest/gtest.h>

#include <array>
#include <filesystem>

#include "database.h"
#include "faces.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

// ---- Database face operations tests ----

class FaceDatabaseTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = make_temp_dir("faces");
        db_path = (dir / "test.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        fs::remove_all(dir);
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
        info.metadata_json = "{}";
        return info;
    }

    FaceInfo make_face(float base_val = 0.0f, int x = 0, int y = 0, int w = 100, int h = 100) {
        FaceInfo fi;
        for (int i = 0; i < 128; i++) {
            fi.embedding[i] = base_val + (i * 0.01f);
        }
        fi.x = x;
        fi.y = y;
        fi.width = w;
        fi.height = h;
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
    int64_t id3 = db->get_or_create_person("Alice");  // same as id1

    EXPECT_GE(id1, 0);
    EXPECT_GE(id2, 0);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(id1, id3);  // same person
}

TEST_F(FaceDatabaseTest, SetFacePerson) {
    db->insert(make_image("/test/photo.jpg", "sha1"));
    int64_t image_id = db->get_image_id("/test/photo.jpg");
    db->insert_face(image_id, make_face(0.0f));
    int64_t face_id = 1;  // first face inserted

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
    FaceInfo similar_face = make_face(0.001f);  // very close embedding
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
    FaceInfo different_face = make_face(100.0f);  // very different embedding
    db->insert_face(new_img, different_face);

    std::string result = db->auto_label_face(2, different_face.embedding);
    EXPECT_EQ(result, "");  // no match
}

TEST_F(FaceDatabaseTest, AutoLabelNoPeople) {
    db->insert(make_image("/test/photo.jpg", "sha1"));
    int64_t img = db->get_image_id("/test/photo.jpg");
    FaceInfo face = make_face(0.0f);
    db->insert_face(img, face);

    std::string result = db->auto_label_face(1, face.embedding);
    EXPECT_EQ(result, "");  // no people defined
}

// ---- FaceInfo / embedding_distance tests ----

TEST(EmbeddingTest, DistanceZero) {
    std::array<float, 128> a{}, b{};
    for (int i = 0; i < 128; i++) {
        a[i] = b[i] = i * 0.1f;
    }
    EXPECT_FLOAT_EQ(embedding_distance(a, b), 0.0f);
}

TEST(EmbeddingTest, DistanceNonZero) {
    std::array<float, 128> a{}, b{};
    for (int i = 0; i < 128; i++) {
        a[i] = 0.0f;
        b[i] = 1.0f;
    }
    float dist = embedding_distance(a, b);
    // sqrt(128 * 1^2) = sqrt(128) ≈ 11.31
    EXPECT_NEAR(dist, std::sqrt(128.0f), 0.01f);
}

TEST(EmbeddingTest, DistanceSymmetric) {
    std::array<float, 128> a{}, b{};
    for (int i = 0; i < 128; i++) {
        a[i] = i * 0.1f;
        b[i] = i * 0.2f;
    }
    EXPECT_FLOAT_EQ(embedding_distance(a, b), embedding_distance(b, a));
}

// ---- Label carry across a rebuild ----

namespace {
FaceInfo box(int x, int y, int w, int h) {
    FaceInfo f{};
    f.x = x;
    f.y = y;
    f.width = w;
    f.height = h;
    return f;
}

LabelledFace labelled(int x, int y, int w, int h, int64_t person_id) {
    return LabelledFace{box(x, y, w, h), person_id};
}
}  // namespace

TEST(BoxOverlap, IdenticalBoxesOverlapCompletely) {
    EXPECT_FLOAT_EQ(box_overlap(box(10, 20, 100, 100), box(10, 20, 100, 100)), 1.0f);
}

TEST(BoxOverlap, DisjointBoxesDoNotOverlap) {
    EXPECT_FLOAT_EQ(box_overlap(box(0, 0, 10, 10), box(20, 20, 10, 10)), 0.0f);
}

TEST(BoxOverlap, IsIntersectionOverUnion) {
    // Intersection 90x90 = 8100; union 2 * 10000 - 8100 = 11900.
    EXPECT_NEAR(box_overlap(box(0, 0, 100, 100), box(10, 10, 100, 100)), 8100.0f / 11900.0f, 1e-6);
}

TEST(BoxOverlap, AZeroAreaBoxOverlapsNothing) {
    EXPECT_FLOAT_EQ(box_overlap(box(0, 0, 0, 0), box(0, 0, 10, 10)), 0.0f);
}

TEST(CarryLabels, AnIdenticalBoxCarriesItsLabel) {
    LabelCarry c = carry_labels({labelled(10, 10, 50, 50, 7)}, {box(10, 10, 50, 50)});
    ASSERT_EQ(c.person_ids.size(), 1u);
    EXPECT_EQ(c.person_ids[0], 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, AShiftedBoxAboveTheThresholdCarriesItsLabel) {
    // IoU 0.68.
    LabelCarry c = carry_labels({labelled(0, 0, 100, 100, 7)}, {box(10, 10, 100, 100)});
    EXPECT_EQ(c.person_ids[0], 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, ABoxBelowTheThresholdDropsTheLabel) {
    // IoU 2500 / 17500 = 0.14.
    LabelCarry c = carry_labels({labelled(0, 0, 100, 100, 7)}, {box(50, 50, 100, 100)});
    EXPECT_EQ(c.person_ids[0], -1);
    EXPECT_EQ(c.dropped, 1u);
}

TEST(CarryLabels, TheLabelGoesToTheBestOverlappingFace) {
    LabelCarry c =
        carry_labels({labelled(0, 0, 100, 100, 7)}, {box(10, 10, 100, 100), box(0, 0, 100, 100)});
    EXPECT_EQ(c.person_ids[0], -1);
    EXPECT_EQ(c.person_ids[1], 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, OneDetectedFaceTakesAtMostOneLabel) {
    // Both labelled faces overlap the one detection; the closer one wins and
    // the other is dropped rather than overwriting it.
    LabelCarry c = carry_labels({labelled(10, 10, 100, 100, 7), labelled(0, 0, 100, 100, 8)},
                                {box(0, 0, 100, 100)});
    EXPECT_EQ(c.person_ids[0], 8);
    EXPECT_EQ(c.dropped, 1u);
}

TEST(CarryLabels, NoDetectionsDropsEveryLabel) {
    LabelCarry c = carry_labels({labelled(0, 0, 10, 10, 7), labelled(50, 50, 10, 10, 8)}, {});
    EXPECT_TRUE(c.person_ids.empty());
    EXPECT_EQ(c.dropped, 2u);
}

TEST(CarryLabels, NoLabelledFacesCarriesNothing) {
    LabelCarry c = carry_labels({}, {box(0, 0, 10, 10), box(50, 50, 10, 10)});
    EXPECT_EQ(c.person_ids, (std::vector<int64_t>{-1, -1}));
    EXPECT_EQ(c.dropped, 0u);
}

TEST_F(FaceDatabaseTest, LabelledFacesForImageReturnsOnlyLabelledFacesOfThatImage) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    db->insert(make_image("/test/b.jpg", "sha2"));
    int64_t a = db->get_image_id("/test/a.jpg");
    int64_t b = db->get_image_id("/test/b.jpg");
    int64_t ada = db->get_or_create_person("Ada");

    int64_t named = db->insert_face(a, make_face(0.0f, 10, 20, 30, 40));
    ASSERT_TRUE(db->set_face_person(named, ada));
    db->insert_face(a, make_face(1.0f, 100, 100, 30, 40));  // unlabelled
    int64_t other = db->insert_face(b, make_face(2.0f, 10, 20, 30, 40));
    ASSERT_TRUE(db->set_face_person(other, ada));  // labelled, other image

    std::vector<LabelledFace> got = db->get_labelled_faces_for_image(a);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].person_id, ada);
    EXPECT_EQ(got[0].face.x, 10);
    EXPECT_EQ(got[0].face.y, 20);
    EXPECT_EQ(got[0].face.width, 30);
    EXPECT_EQ(got[0].face.height, 40);
    EXPECT_EQ(got[0].face.embedding, make_face(0.0f).embedding);
}

TEST_F(FaceDatabaseTest, LabelledFacesForAnImageWithNoneIsEmpty) {
    db->insert(make_image("/test/a.jpg", "sha1"));
    int64_t a = db->get_image_id("/test/a.jpg");
    db->insert_face(a, make_face(0.0f));
    EXPECT_TRUE(db->get_labelled_faces_for_image(a).empty());
}
