#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>

#include "database.h"
#include "faces.h"
#include "models.h"
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

const std::string kModelA(64, 'a'), kModelB(64, 'b');

FaceProvenance prov(const std::string& model, const std::string& frame_sha, int w, int h) {
    return {"file", w, h, frame_sha, model};
}

// Stored under model A in frame "f"; kCurrent is model B in the same frame, so
// the overlap tests below exercise the model-change path.
LabelledFace labelled(int x, int y, int w, int h, int64_t person_id) {
    return LabelledFace{box(x, y, w, h), person_id, prov(kModelA, "f", 100, 100)};
}
const FaceProvenance kCurrent = prov(kModelB, "f", 100, 100);

FaceInfo face_at(int x, int y, int w, int h, float seed) {
    FaceInfo f{};
    for (int i = 0; i < 128; ++i) f.embedding[i] = std::sin(seed * (i + 1));
    f.x = x;
    f.y = y;
    f.width = w;
    f.height = h;
    return f;
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
    LabelCarry c = carry_labels({labelled(10, 10, 50, 50, 7)}, {box(10, 10, 50, 50)}, kCurrent);
    ASSERT_EQ(c.person_ids.size(), 1u);
    EXPECT_EQ(c.person_ids[0], 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, AShiftedBoxAboveTheThresholdCarriesItsLabel) {
    // IoU 0.68.
    LabelCarry c = carry_labels({labelled(0, 0, 100, 100, 7)}, {box(10, 10, 100, 100)}, kCurrent);
    EXPECT_EQ(c.person_ids[0], 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, ABoxBelowTheThresholdDropsTheLabel) {
    // IoU 2500 / 17500 = 0.14.
    LabelCarry c = carry_labels({labelled(0, 0, 100, 100, 7)}, {box(50, 50, 100, 100)}, kCurrent);
    EXPECT_EQ(c.person_ids[0], -1);
    EXPECT_EQ(c.dropped, 1u);
}

TEST(CarryLabels, TheLabelGoesToTheBestOverlappingFace) {
    LabelCarry c = carry_labels({labelled(0, 0, 100, 100, 7)},
                                {box(10, 10, 100, 100), box(0, 0, 100, 100)}, kCurrent);
    EXPECT_EQ(c.person_ids[0], -1);
    EXPECT_EQ(c.person_ids[1], 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, OneDetectedFaceTakesAtMostOneLabel) {
    // Both labelled faces overlap the one detection; the closer one wins and
    // the other is dropped rather than overwriting it.
    LabelCarry c = carry_labels({labelled(10, 10, 100, 100, 7), labelled(0, 0, 100, 100, 8)},
                                {box(0, 0, 100, 100)}, kCurrent);
    EXPECT_EQ(c.person_ids[0], 8);
    EXPECT_EQ(c.dropped, 1u);
}

TEST(CarryLabels, NoDetectionsDropsEveryLabel) {
    LabelCarry c =
        carry_labels({labelled(0, 0, 10, 10, 7), labelled(50, 50, 10, 10, 8)}, {}, kCurrent);
    EXPECT_TRUE(c.person_ids.empty());
    EXPECT_EQ(c.dropped, 2u);
}

TEST(CarryLabels, NoLabelledFacesCarriesNothing) {
    LabelCarry c = carry_labels({}, {box(0, 0, 10, 10), box(50, 50, 10, 10)}, kCurrent);
    EXPECT_EQ(c.person_ids, (std::vector<int64_t>{-1, -1}));
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, SameModelMatchesByEmbeddingWhateverTheBoxes) {
    FaceProvenance cur = prov(kModelA, "f2", 320, 240);
    LabelledFace old{face_at(0, 0, 50, 50, 1.0f), 7, prov(kModelA, "f1", 640, 480)};
    FaceInfo moved = face_at(200, 100, 25, 25, 1.0f);  // no overlap, same embedding
    LabelCarry c = carry_labels({old}, {moved}, cur);
    EXPECT_EQ(c.person_ids.at(0), 7);
    EXPECT_EQ(c.dropped, 0u);
}

TEST(CarryLabels, SameModelNeverMovesALabelToAnotherPersonsFace) {
    // A frame rotated 180 degrees: the old box of person 7 now sits exactly on
    // person 8's new box. Embeddings decide, so each label stays with its person.
    FaceProvenance cur = prov(kModelA, "rot", 100, 100);
    LabelledFace p7{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "orig", 100, 100)};
    LabelledFace p8{face_at(80, 80, 20, 20, 2.0f), 8, prov(kModelA, "orig", 100, 100)};
    FaceInfo new7 = face_at(80, 80, 20, 20, 1.0f);
    FaceInfo new8 = face_at(0, 0, 20, 20, 2.0f);
    LabelCarry c = carry_labels({p7, p8}, {new7, new8}, cur);
    EXPECT_EQ(c.person_ids.at(0), 7);
    EXPECT_EQ(c.person_ids.at(1), 8);
}

TEST(CarryLabels, AModelChangeInTheSameFrameMatchesByOverlap) {
    FaceProvenance cur = prov(kModelB, "f", 100, 100);
    LabelledFace old{face_at(10, 10, 30, 30, 1.0f), 7, prov(kModelA, "f", 100, 100)};
    FaceInfo again = face_at(10, 10, 30, 30, 9.0f);  // new model, unrelated embedding
    EXPECT_EQ(carry_labels({old}, {again}, cur).person_ids.at(0), 7);
}

TEST(CarryLabels, AModelChangeInASameSizedRotatedFrameDropsRatherThanMoves) {
    // Square frame rotated 90 degrees: same size, different bytes. Under a model
    // change nothing can match safely, so both labels drop.
    FaceProvenance cur = prov(kModelB, "rot90", 100, 100);
    LabelledFace p7{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "orig", 100, 100)};
    LabelledFace p8{face_at(80, 0, 20, 20, 2.0f), 8, prov(kModelA, "orig", 100, 100)};
    FaceInfo a = face_at(80, 0, 20, 20, 5.0f);  // where p7's old box was rotated onto p8's
    FaceInfo b = face_at(80, 80, 20, 20, 6.0f);
    LabelCarry c = carry_labels({p7, p8}, {a, b}, cur);
    EXPECT_EQ(c.person_ids.at(0), -1);
    EXPECT_EQ(c.person_ids.at(1), -1);
    EXPECT_EQ(c.dropped, 2u);
}

TEST(CarryLabels, ARowFromBeforeV10IsTakenAsTheSfaceModelWithAnUnknownFrame) {
    FaceProvenance legacy{};  // what a pre-v10 row reads back as
    LabelledFace old{face_at(0, 0, 20, 20, 1.0f), 7, legacy};
    FaceInfo same = face_at(50, 50, 20, 20, 1.0f);
    EXPECT_EQ(carry_labels({old}, {same}, prov(kSfaceModelSha256, "f", 100, 100)).person_ids.at(0),
              7)
        << "same model as the only one phig shipped: matched by embedding";
    FaceInfo overlapping = face_at(0, 0, 20, 20, 9.0f);
    EXPECT_EQ(carry_labels({old}, {overlapping}, prov(kModelB, "f", 100, 100)).dropped, 1u)
        << "a different model with an unknown frame cannot use overlap";
}

TEST(CarryLabels, AnEmbeddingBeyondTheThresholdDoesNotMatch) {
    FaceProvenance cur = prov(kModelA, "f", 100, 100);
    LabelledFace old{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "f", 100, 100)};
    FaceInfo stranger = face_at(0, 0, 20, 20, 1.0f);
    for (int i = 0; i < 128; ++i) stranger.embedding[i] = -stranger.embedding[i];  // distance 2
    EXPECT_EQ(carry_labels({old}, {stranger}, cur).dropped, 1u);
}

TEST(CarryLabels, AnEmbeddingMatchAndAnOverlapMatchShareOneDetection) {
    // p7 (same model as current) matches d0 by embedding; p8 (other model, same
    // frame) would match d0 by overlap. The embedding pass runs first and its
    // claim on d0 stands, so p8 drops.
    FaceProvenance cur = prov(kModelA, "f", 100, 100);
    LabelledFace p7{face_at(60, 60, 20, 20, 1.0f), 7, prov(kModelA, "g", 100, 100)};
    LabelledFace p8{face_at(0, 0, 20, 20, 2.0f), 8, prov(kModelB, "f", 100, 100)};
    FaceInfo d0 = face_at(0, 0, 20, 20, 1.0f);
    LabelCarry c = carry_labels({p8, p7}, {d0}, cur);
    EXPECT_EQ(c.person_ids.at(0), 7);
    EXPECT_EQ(c.dropped, 1u);
}

TEST(CarryLabels, SameModelOneDetectionTakesAtMostOneLabel) {
    FaceProvenance cur = prov(kModelA, "f", 100, 100);
    FaceInfo d0 = face_at(0, 0, 20, 20, 1.0f);
    LabelledFace far7{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "f", 100, 100)};
    far7.face.embedding = d0.embedding;
    LabelledFace near8 = far7;
    near8.person_id = 8;
    near8.face.embedding[0] += 0.05f;  // close to d0, but not identical
    LabelledFace far9 = far7;
    far9.person_id = 9;
    far9.face.embedding[0] += 0.3f;
    LabelCarry c = carry_labels({far9, near8, far7}, {d0}, cur);
    EXPECT_EQ(c.person_ids.at(0), 7);
    EXPECT_EQ(c.dropped, 2u);
}

TEST(CarryLabels, AModelChangeInA180RotatedFrameDropsRatherThanMoves) {
    FaceProvenance cur = prov(kModelB, "rot180", 100, 100);
    LabelledFace p7{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "orig", 100, 100)};
    LabelledFace p8{face_at(80, 80, 20, 20, 2.0f), 8, prov(kModelA, "orig", 100, 100)};
    FaceInfo a = face_at(80, 80, 20, 20, 5.0f);  // p7's old box rotated onto p8's
    FaceInfo b = face_at(0, 0, 20, 20, 6.0f);
    LabelCarry c = carry_labels({p7, p8}, {a, b}, cur);
    EXPECT_EQ(c.person_ids, (std::vector<int64_t>{-1, -1}));
    EXPECT_EQ(c.dropped, 2u);
}

TEST(CarryLabels, TheEmbeddingThresholdIsAppliedToNormalizedDistance) {
    // b = cos(t) * a + sin(t) * u with u a unit vector orthogonal to a, so the
    // normalized distance is 2 * sin(t / 2). Scaling b must not change the verdict.
    FaceProvenance cur = prov(kModelA, "f", 100, 100);
    auto at_distance = [](const FaceInfo& a, float dist) {
        std::array<double, 128> u{};
        double na = 0, dot = 0, nu = 0;
        for (int i = 0; i < 128; ++i) na += double{a.embedding[i]} * a.embedding[i];
        for (int i = 0; i < 128; ++i) u[i] = (i % 2 ? 1.0 : -1.0) * (i + 1);
        for (int i = 0; i < 128; ++i) dot += u[i] * a.embedding[i];
        for (int i = 0; i < 128; ++i) {
            u[i] -= dot / na * a.embedding[i];
            nu += u[i] * u[i];
        }
        double t = 2 * std::asin(dist / 2.0);
        FaceInfo b = a;
        for (int i = 0; i < 128; ++i)
            b.embedding[i] =
                static_cast<float>(3.0 * (std::cos(t) * a.embedding[i] / std::sqrt(na) +
                                          std::sin(t) * u[i] / std::sqrt(nu)));
        return b;
    };
    FaceInfo a = face_at(0, 0, 20, 20, 1.0f);
    LabelledFace old{a, 7, prov(kModelA, "f", 100, 100)};
    EXPECT_NEAR(normalized_embedding_distance(a.embedding, at_distance(a, 1.10f).embedding), 1.10f,
                1e-3);
    EXPECT_EQ(carry_labels({old}, {at_distance(a, 1.10f)}, cur).person_ids.at(0), 7);
    EXPECT_EQ(carry_labels({old}, {at_distance(a, 1.16f)}, cur).dropped, 1u);
}

TEST(SameFrame, NeedsAKnownHashAndTheSameSize) {
    EXPECT_TRUE(same_frame(prov("", "h", 10, 20), prov("", "h", 10, 20)));
    EXPECT_FALSE(same_frame(prov("", "h", 10, 20), prov("", "h", 20, 10)));
    EXPECT_FALSE(same_frame(prov("", "h", 10, 20), prov("", "g", 10, 20)));
    EXPECT_FALSE(same_frame(prov("", "", 10, 20), prov("", "", 10, 20)));
}

TEST(NormalizedEmbeddingDistance, IsZeroForItselfTwoForItsNegationAndScaleFree) {
    FaceInfo a = face_at(0, 0, 1, 1, 1.0f);
    std::array<float, 128> neg = a.embedding, scaled = a.embedding;
    for (int i = 0; i < 128; ++i) {
        neg[i] = -neg[i];
        scaled[i] *= 5.0f;
    }
    EXPECT_NEAR(normalized_embedding_distance(a.embedding, a.embedding), 0.0f, 1e-6);
    EXPECT_NEAR(normalized_embedding_distance(a.embedding, neg), 2.0f, 1e-5);
    EXPECT_NEAR(normalized_embedding_distance(a.embedding, scaled), 0.0f, 1e-5);
    std::array<float, 128> zero{};
    EXPECT_FLOAT_EQ(normalized_embedding_distance(a.embedding, zero), 2.0f);
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
