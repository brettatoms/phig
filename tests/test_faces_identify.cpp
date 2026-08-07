#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "faces.h"
#include "faces_engine.h"
#include "temp_dir.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// No face models required: these exercise the label-matching and transaction
// behavior over embeddings written directly, not detection.
class IdentifyTest : public ::testing::Test {
   protected:
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    void SetUp() override {
        dir = make_temp_dir("faces-identify");
        db_path = (dir / "test.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }

    void TearDown() override {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    ImageInfo make_image(const std::string& path, const std::string& sha) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.file_size = 1000;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = sha;
        info.metadata_json = "{}";
        return info;
    }

    // Embeddings a fixed distance apart: `base` shifts every component, so two
    // faces built with bases b1 and b2 sit sqrt(128)*|b1-b2| apart.
    FaceInfo make_face(float base) {
        FaceInfo fi;
        for (int i = 0; i < 128; i++) fi.embedding[i] = base;
        fi.x = fi.y = 0;
        fi.width = fi.height = 100;
        return fi;
    }

    int64_t add_face(const std::string& path, const std::string& sha, float base) {
        db->insert(make_image(path, sha));
        int64_t image_id = db->get_image_id(path);
        return db->insert_face(image_id, make_face(base));
    }
};

}  // namespace

TEST_F(IdentifyTest, LabelsFacesWithinThresholdAndLeavesOthers) {
    int64_t alice = db->get_or_create_person("Alice");
    int64_t ref = add_face("/p/ref.jpg", "sha_ref", 0.0f);
    db->set_face_person(ref, alice);

    // Near: distance sqrt(128)*0.001 ~= 0.011, inside the 0.6 threshold.
    int64_t near = add_face("/p/near.jpg", "sha_near", 0.001f);
    // Far: distance sqrt(128)*1.0 ~= 11.3, well outside.
    int64_t far = add_face("/p/far.jpg", "sha_far", 1.0f);

    IdentifyResult r = identify_person(*db, "Alice", 0.6f);

    EXPECT_EQ(r.labeled, 1u);
    // The near face joined Alice; the far one did not. face_count is the public
    // surface for this — 2 means the reference plus `near` only.
    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].name, "Alice");
    EXPECT_EQ(people[0].face_count, 2);
    (void)near;
    (void)far;
}

TEST_F(IdentifyTest, UnknownPersonReportsNotFoundAndLabelsNothing) {
    add_face("/p/a.jpg", "sha_a", 0.0f);

    IdentifyResult r = identify_person(*db, "Nobody", 0.6f);

    EXPECT_FALSE(r.person_found);
    EXPECT_EQ(r.labeled, 0u);
}

TEST_F(IdentifyTest, PersonWithNoReferenceFacesLabelsNothing) {
    db->get_or_create_person("Alice");  // exists, but has no faces
    add_face("/p/a.jpg", "sha_a", 0.0f);

    IdentifyResult r = identify_person(*db, "Alice", 0.6f);

    EXPECT_TRUE(r.person_found);
    EXPECT_EQ(r.references, 0u);
    EXPECT_EQ(r.labeled, 0u);
}

TEST_F(IdentifyTest, LabelWritesLandInOneTransaction) {
    // The whole point of the fix: the writes commit together. Previously each
    // set_face_person was its own implicit transaction, so a concurrent reader
    // could observe a half-labeled collection and a failure partway through
    // left some faces labeled and others not.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t ref = add_face("/p/ref.jpg", "sha_ref", 0.0f);
    db->set_face_person(ref, alice);
    for (int i = 0; i < 20; i++) {
        add_face("/p/n" + std::to_string(i) + ".jpg", "sha_n" + std::to_string(i), 0.001f);
    }

    IdentifyResult r = identify_person(*db, "Alice", 0.6f);
    EXPECT_EQ(r.labeled, 20u);

    // Every one of them, not a prefix.
    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].face_count, 21);  // 20 labeled + the reference
}

TEST_F(IdentifyTest, FaceDeletedBeforeIdentifyIsNeverConsidered) {
    // A face gone before phase 1 reads it is simply not a candidate. This does
    // NOT cover the phase-1-to-phase-2 window; see the two tests below.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t ref = add_face("/p/ref.jpg", "sha_ref", 0.0f);
    db->set_face_person(ref, alice);
    add_face("/p/gone.jpg", "sha_gone", 0.001f);

    int64_t image_id = db->get_image_id("/p/gone.jpg");
    db->delete_faces_for_image(image_id);

    IdentifyResult r = identify_person(*db, "Alice", 0.6f);
    EXPECT_EQ(r.labeled, 0u);

    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].face_count, 1);  // the reference only
}

TEST_F(IdentifyTest, StaleFaceIdUpdatesNoRowsAndIsNotCounted) {
    // The benign half of the phase-1-to-phase-2 gap: an id deleted and NOT
    // reused updates zero rows. `labeled` must report changed rows, not
    // statements issued, or the CLI over-reports what it wrote.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t ref = add_face("/p/ref.jpg", "sha_ref", 0.0f);
    db->set_face_person(ref, alice);

    EXPECT_FALSE(db->set_face_person(999999, alice));

    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].face_count, 1);
}

TEST_F(IdentifyTest, RowidsAreReusedAfterADelete) {
    // The premise the guard below defends against. faces.id is INTEGER PRIMARY
    // KEY without AUTOINCREMENT, so a deleted rowid comes back and a bare
    // `WHERE id = ?` written from a stale read lands on a face nobody compared.
    // When plan 045 migrates the id to AUTOINCREMENT this stops being true and
    // the test should be inverted.
    int64_t doomed = add_face("/p/a.jpg", "sha_a", 5.0f);
    db->delete_faces_for_image(db->get_image_id("/p/a.jpg"));

    db->insert(make_image("/p/b.jpg", "sha_b"));
    int64_t reused = db->insert_face(db->get_image_id("/p/b.jpg"), make_face(99.0f));

    EXPECT_EQ(reused, doomed) << "rowid was not reused — see plan 045";
}

TEST_F(IdentifyTest, AReusedRowidIsNotLabeledBecauseItsEmbeddingChanged) {
    // The guard itself: phase 2 writes against the embedding it compared, so a
    // rowid carrying a different face than the one phase 1 read updates zero
    // rows instead of taking a label it never earned.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t doomed = add_face("/p/a.jpg", "sha_a", 0.001f);
    std::array<float, 128> compared = make_face(0.001f).embedding;

    db->delete_faces_for_image(db->get_image_id("/p/a.jpg"));
    db->insert(make_image("/p/b.jpg", "sha_b"));
    int64_t reused = db->insert_face(db->get_image_id("/p/b.jpg"), make_face(99.0f));
    ASSERT_EQ(reused, doomed) << "rowid was not reused — see plan 045";

    // What phase 2 does with the id and embedding it remembers from phase 1.
    EXPECT_FALSE(db->set_face_person_if_unchanged(doomed, alice, compared));

    auto people = db->list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].face_count, 0) << "a face nobody compared was labeled";
}

TEST_F(IdentifyTest, AnUnchangedFaceIsStillLabeled) {
    // The guard must not refuse the ordinary case it exists to protect.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t face = add_face("/p/a.jpg", "sha_a", 0.001f);

    EXPECT_TRUE(db->set_face_person_if_unchanged(face, alice, make_face(0.001f).embedding));
    EXPECT_EQ(db->list_people()[0].face_count, 1);

    // And it is a compare-and-set against another labeler, not just against
    // reuse: a face somebody else already named is left alone.
    int64_t bob = db->get_or_create_person("Bob");
    EXPECT_FALSE(db->set_face_person_if_unchanged(face, bob, make_face(0.001f).embedding));
    EXPECT_EQ(db->list_people()[0].face_count, 1) << "an already-labeled face was relabeled";
}

TEST_F(IdentifyTest, ConcurrentRebuildNeverGetsAFaceLabeledItDidNotMatch) {
    // End to end, with the interleave that matters actually happening: another
    // writer replaces faces — recycling their rowids — after identify has read
    // them as candidates and before it writes. Nothing serializes that window,
    // so the invariant has to hold on its own: every face labeled Alice is near
    // Alice. Phase 2 keyed on ids alone would label whatever landed on them.
    //
    // Three things make the interleave reachable rather than theoretical:
    // the replaced faces hold the HIGHEST rowids (SQLite hands out
    // max(rowid)+1, so deleting below the maximum frees nothing); the
    // comparison is big enough to be a wide window; and the replacement happens
    // once and stays, so a wrong label cannot be erased before it is checked.
    int64_t alice = db->get_or_create_person("Alice");
    for (int i = 0; i < 40; i++) {
        int64_t ref =
            add_face("/p/ref" + std::to_string(i) + ".jpg", "sha_ref" + std::to_string(i), 0.0f);
        db->set_face_person(ref, alice);
    }
    std::vector<std::string> top;
    for (int i = 0; i < 3000; i++) {
        std::string p = "/p/c" + std::to_string(i) + ".jpg";
        add_face(p, "sha_c" + std::to_string(i), 0.001f);
        if (i >= 3000 - 20) top.push_back(p);
    }

    Database other(db_path);
    std::thread rebuilder([&] {
        // Long enough for identify to have read its candidates and be comparing
        // them. Phase 2 opens a transaction, so this has to land before that.
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        for (auto it = top.rbegin(); it != top.rend(); ++it) {
            other.delete_faces_for_image(other.get_image_id(*it));
        }
        for (const auto& p : top) other.insert_face(other.get_image_id(p), make_face(99.0f));
    });

    IdentifyResult r = identify_person(*db, "Alice", 0.6f);
    rebuilder.join();

    size_t wrong = 0;
    for (const auto& row : db->get_face_embeddings_for_person(alice)) {
        if (embedding_distance(make_face(0.0f).embedding, row.embedding) > 0.6f) ++wrong;
    }
    EXPECT_EQ(wrong, 0u) << wrong << " face(s) far from Alice were labeled Alice";
    EXPECT_GT(r.labeled, 0u) << "the run labeled nothing, so it proved nothing";
}

TEST_F(IdentifyTest, ConcurrentFaceWritesDoNotBlockOrCorruptLabeling) {
    // identify must not hold a write transaction across its comparison phase:
    // another writer has to be able to commit while identify is comparing. If
    // the comparison ran inside BEGIN IMMEDIATE this would contend for the full
    // duration rather than only across the short label write.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t ref = add_face("/p/ref.jpg", "sha_ref", 0.0f);
    db->set_face_person(ref, alice);
    for (int i = 0; i < 50; i++) {
        add_face("/p/n" + std::to_string(i) + ".jpg", "sha_n" + std::to_string(i), 0.001f);
    }

    Database other(db_path);
    std::thread writer([&] {
        for (int i = 0; i < 20; i++) {
            other.insert(
                make_image("/p/w" + std::to_string(i) + ".jpg", "sha_w" + std::to_string(i)));
        }
    });

    IdentifyResult r = identify_person(*db, "Alice", 0.6f);
    writer.join();

    EXPECT_EQ(r.labeled, 50u);
}
