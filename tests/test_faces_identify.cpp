#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
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

TEST_F(IdentifyTest, RowidsAreNotReusedAfterADelete) {
    // faces.id is INTEGER PRIMARY KEY AUTOINCREMENT, so SQLite tracks the high
    // water mark in sqlite_sequence and a deleted id never comes back. Without
    // that, a `WHERE id = ?` written from a stale read lands on a face nobody
    // compared. Deleting the highest id is the case that matters: plain rowid
    // assignment is max(rowid)+1, so deleting below the maximum frees nothing.
    int64_t doomed = add_face("/p/a.jpg", "sha_a", 5.0f);
    db->delete_faces_for_image(db->get_image_id("/p/a.jpg"));

    db->insert(make_image("/p/b.jpg", "sha_b"));
    int64_t next = db->insert_face(db->get_image_id("/p/b.jpg"), make_face(99.0f));

    EXPECT_GT(next, doomed) << "a deleted face id was handed out again";
}

TEST_F(IdentifyTest, AFaceWhoseEmbeddingChangedIsNotLabeled) {
    // The guard's contract: phase 2 writes against the embedding it compared, so
    // the write refers to the same face rather than the same row number. Schema
    // v8 made rowid reuse unreachable, so the changed embedding is written
    // directly here — the property under test is the UPDATE's predicate, not the
    // route by which a row can come to hold different pixels.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t face = add_face("/p/a.jpg", "sha_a", 0.001f);
    std::array<float, 128> compared = make_face(0.001f).embedding;

    std::array<float, 128> replaced = make_face(99.0f).embedding;
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db->db_ptr(), "UPDATE faces SET embedding = ? WHERE id = ?", -1,
                                 &s, nullptr),
              SQLITE_OK);
    sqlite3_bind_blob(s, 1, replaced.data(), 128 * sizeof(float), SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, face);
    ASSERT_EQ(sqlite3_step(s), SQLITE_DONE);
    sqlite3_finalize(s);

    // What phase 2 does with the id and embedding it remembers from phase 1.
    EXPECT_FALSE(db->set_face_person_if_unchanged(face, alice, compared));

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
    // writer deletes faces identify has already read as candidates and inserts
    // replacements far from Alice, after phase 1 and before phase 2. Nothing
    // serializes that window, so the invariant has to hold on its own: every
    // face labeled Alice is near Alice.
    //
    // What holds it is id retirement. faces.id is AUTOINCREMENT, so the
    // replacements get fresh ids and the ids phase 1 read never come back;
    // phase 2's `WHERE id = ?` matches nothing, and a row that does not exist
    // cannot match whatever else the predicate asks. So this case pins id
    // retirement, not the whole guard — it would pass with the embedding clause
    // removed. That clause's only cover is
    // `AFaceWhoseEmbeddingChangedIsNotLabeled`, which writes the changed
    // embedding by hand because no concurrent path produces one.
    //
    // Two things keep the window wide enough to be reachable rather than
    // theoretical: the comparison is big, and the replacement happens once and
    // stays, so a wrong label could not be erased before it is checked. That
    // the replaced faces hold the highest ids is incidental — retirement does
    // not depend on where in the table they sat.
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

// Two labelers racing for one candidate set, which is the case `person_id IS
// NULL` exists for and the one nothing covered: the deletion clause is pinned
// by ConcurrentRebuildNeverGetsAFaceLabeledItDidNotMatch, and the compare-and-set
// against another labeler only by AnUnchangedFaceIsStillLabeled, which makes two
// calls in one thread with no gap and no second writer.
//
// The window is forced rather than raced. A third connection holds the write
// lock, so both runs finish phase 1 — reading every candidate as unlabeled —
// before either can open phase 2's transaction. Both then block on the busy
// timeout until the lock is released, and whichever commits second finds the
// contested faces already named.
//
// Both runs reading the full candidate set is the witness that the interleave
// happened: a second run that started after the first committed would have seen
// fewer candidates, and the sum below would be arithmetic about nothing.
TEST_F(IdentifyTest, TwoLabelersRacingForOneCandidateSetLabelEachFaceOnce) {
    // Alice at 0.00 and Bob at 0.08 are 0.905 apart — sqrt(128) * 0.08 — so each
    // one's own candidates are out of the other's reach at a 0.6 threshold,
    // while the contested group at 0.04 is 0.452 from both.
    int64_t alice = db->get_or_create_person("Alice");
    int64_t bob = db->get_or_create_person("Bob");
    for (int i = 0; i < 2; i++) {
        db->set_face_person(
            add_face("/p/ra" + std::to_string(i) + ".jpg", "sha_ra" + std::to_string(i), 0.00f),
            alice);
        db->set_face_person(
            add_face("/p/rb" + std::to_string(i) + ".jpg", "sha_rb" + std::to_string(i), 0.08f),
            bob);
    }
    const size_t kAliceOnly = 3, kShared = 4, kBobOnly = 3;
    for (size_t i = 0; i < kAliceOnly; i++)
        add_face("/p/a" + std::to_string(i) + ".jpg", "sha_a" + std::to_string(i), 0.01f);
    for (size_t i = 0; i < kShared; i++)
        add_face("/p/s" + std::to_string(i) + ".jpg", "sha_s" + std::to_string(i), 0.04f);
    for (size_t i = 0; i < kBobOnly; i++)
        add_face("/p/b" + std::to_string(i) + ".jpg", "sha_b" + std::to_string(i), 0.07f);
    const size_t kCandidates = kAliceOnly + kShared + kBobOnly;

    // Holds the write lock for the whole of both phase 1s. Reads do not take
    // it, so neither run is delayed until it reaches its write.
    Database blocker(db_path);
    blocker.begin_transaction();

    Database ca(db_path), cb(db_path);
    IdentifyResult ra, rb;
    std::atomic<int> started{0};
    auto run = [&](Database& c, const char* who, IdentifyResult& out) {
        started.fetch_add(1);
        out = identify_person(c, who, 0.6f);
    };
    std::thread ta(run, std::ref(ca), "Alice", std::ref(ra));
    std::thread tb(run, std::ref(cb), "Bob", std::ref(rb));
    while (started.load() < 2) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    blocker.rollback_transaction();
    ta.join();
    tb.join();

    ASSERT_TRUE(ra.person_found);
    ASSERT_TRUE(rb.person_found);
    EXPECT_EQ(ra.candidates, kCandidates)
        << "Alice's run did not read the whole candidate set unlabeled, so the two runs did not "
           "overlap and nothing below is about a race";
    EXPECT_EQ(rb.candidates, kCandidates)
        << "Bob's run did not read the whole candidate set unlabeled, so the two runs did not "
           "overlap and nothing below is about a race";

    EXPECT_EQ(ra.labeled + rb.labeled, kCandidates)
        << "each face is labeled once: without the compare-and-set the loser relabels the "
           "contested faces and the sum runs to "
        << kCandidates + kShared;

    // And nobody was given a face they never matched.
    size_t wrong = 0;
    for (const auto& row : db->get_face_embeddings_for_person(alice))
        if (embedding_distance(make_face(0.00f).embedding, row.embedding) > 0.6f) ++wrong;
    for (const auto& row : db->get_face_embeddings_for_person(bob))
        if (embedding_distance(make_face(0.08f).embedding, row.embedding) > 0.6f) ++wrong;
    EXPECT_EQ(wrong, 0u) << wrong << " face(s) were named by someone they are not near";
    EXPECT_EQ(db->get_unlabeled_face_embeddings().size(), 0u) << "every candidate matched someone";
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

// Schema v8 migrates faces.id from a plain rowid to AUTOINCREMENT. The copy has
// to carry ids across unchanged, because vec_faces is keyed on them and is a
// vec0 virtual table the copy cannot touch: a shifted id would silently point
// every embedding at the wrong face.
TEST(FacesMigrationV8, PreservesIdsLabelsAndVectorsAndStopsReuse) {
    auto dir = make_temp_dir("faces-v8");
    auto db_path = (dir / "test.db").string();

    std::vector<int64_t> ids;
    int64_t alice = -1;
    {
        Database db(db_path);
        db.init_schema();

        // Rewind to a genuine v7 faces table: plain INTEGER PRIMARY KEY. Built
        // by hand rather than by rolling the version back alone, because a
        // fresh database already has AUTOINCREMENT and would make the migration
        // a no-op that proves nothing.
        const char* rewind = R"(
            DROP TABLE faces;
            CREATE TABLE faces (
                id          INTEGER PRIMARY KEY,
                image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
                embedding   BLOB NOT NULL,
                x INTEGER, y INTEGER, width INTEGER, height INTEGER,
                person_id   INTEGER REFERENCES people(id)
            );
            CREATE INDEX idx_faces_image ON faces(image_id);
            UPDATE meta SET value = '7' WHERE key = 'schema_version';
        )";
        ASSERT_EQ(sqlite3_exec(db.db_ptr(), rewind, nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(db.db_ptr());

        for (int i = 0; i < 3; i++) {
            ImageInfo info;
            info.path = "/p/" + std::to_string(i) + ".jpg";
            info.filename = std::to_string(i) + ".jpg";
            info.extension = "jpg";
            info.file_size = 1000;
            info.sha256 = "sha" + std::to_string(i);
            info.metadata_json = "{}";
            db.insert(info);
            FaceInfo fi;
            fi.embedding.fill(0.1f * static_cast<float>(i + 1));
            fi.x = fi.y = fi.width = fi.height = 1;
            ids.push_back(db.insert_face(db.get_image_id(info.path), fi));
        }
        alice = db.get_or_create_person("Alice");
        ASSERT_TRUE(db.set_face_person(ids[1], alice));

        // The rewind is real: on this table a deleted id comes back.
        int64_t top = ids.back();
        db.delete_faces_for_image(db.get_image_id("/p/2.jpg"));
        ImageInfo again;
        again.path = "/p/again.jpg";
        again.filename = "again.jpg";
        again.extension = "jpg";
        again.file_size = 1000;
        again.sha256 = "sha_again";
        again.metadata_json = "{}";
        db.insert(again);
        FaceInfo fi;
        fi.embedding.fill(0.9f);
        fi.x = fi.y = fi.width = fi.height = 1;
        ids.back() = db.insert_face(db.get_image_id("/p/again.jpg"), fi);
        ASSERT_EQ(ids.back(), top) << "the v7 rewind did not produce a reusing table";
    }

    // Reopening runs the v8 migration.
    Database db(db_path);
    db.init_schema();
    EXPECT_EQ(db.schema_version(), 8);

    std::vector<int64_t> after;
    sqlite3_stmt* s = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.db_ptr(), "SELECT id FROM faces ORDER BY id", -1, &s, nullptr),
              SQLITE_OK);
    while (sqlite3_step(s) == SQLITE_ROW) after.push_back(sqlite3_column_int64(s, 0));
    sqlite3_finalize(s);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(after, ids) << "face ids changed, so every vec_faces row now points at another face";

    auto people = db.list_people();
    ASSERT_EQ(people.size(), 1u);
    EXPECT_EQ(people[0].face_count, 1) << "the person label did not survive the copy";

    int64_t vec_rows = -1;
    ASSERT_EQ(sqlite3_prepare_v2(db.db_ptr(), "SELECT COUNT(*) FROM vec_faces", -1, &s, nullptr),
              SQLITE_OK);
    if (sqlite3_step(s) == SQLITE_ROW) vec_rows = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    EXPECT_EQ(vec_rows, static_cast<int64_t>(ids.size()));

    // KNN still resolves to a face that exists, and reuse is gone.
    std::array<float, 128> probe;
    probe.fill(0.2f);
    EXPECT_EQ(db.find_closest_face(probe, 0.6f), ids[1]);

    int64_t top = ids.back();
    db.delete_faces_for_image(db.get_image_id("/p/again.jpg"));
    ImageInfo last;
    last.path = "/p/last.jpg";
    last.filename = "last.jpg";
    last.extension = "jpg";
    last.file_size = 1000;
    last.sha256 = "sha_last";
    last.metadata_json = "{}";
    db.insert(last);
    FaceInfo fi;
    fi.embedding.fill(0.5f);
    fi.x = fi.y = fi.width = fi.height = 1;
    EXPECT_GT(db.insert_face(db.get_image_id("/p/last.jpg"), fi), top);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Mirrors cmd_face's `name` path in main.cpp: find_closest_face, then create the
// person, then write the label. That is a read-then-write with two statements in
// between, and it runs lock-free beside a rebuild. Unlike identify it has no
// stored embedding to re-check against, so nothing but a stable face id keeps
// the label on the face that was actually matched.
TEST_F(IdentifyTest, NameDoesNotLabelAFaceItDidNotMatchWhenARebuildRecyclesIds) {
    // Fillers first so the matched face holds the highest id: ids are handed out
    // above the maximum, so deleting anything lower frees nothing to reuse.
    for (int i = 0; i < 5; i++) {
        add_face("/p/filler" + std::to_string(i) + ".jpg", "sha_f" + std::to_string(i), 5.0f);
    }
    int64_t matched = add_face("/p/target.jpg", "sha_target", 0.0f);
    std::array<float, 128> probe = make_face(0.0f).embedding;

    // The rebuild holds the write lock across its delete-and-reinsert. That is
    // what widens the window: get_or_create_person below is a write, so it waits
    // on the busy timeout while this runs, and the id it is about to write to is
    // recycled onto a different face in the meantime.
    std::atomic<bool> holding{false};
    Database other(db_path);
    std::thread rebuilder([&] {
        other.begin_transaction();
        holding.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        other.delete_faces_for_image(other.get_image_id("/p/target.jpg"));
        other.insert_face(other.get_image_id("/p/filler0.jpg"), make_face(99.0f));
        other.commit_transaction();
    });
    while (!holding.load()) std::this_thread::yield();

    int64_t face_id = db->find_closest_face(probe, 0.6f);
    ASSERT_EQ(face_id, matched) << "the read did not find the face this test is about";
    int64_t mom = db->get_or_create_person("Mom");  // blocks until the rebuild commits
    db->set_face_person(face_id, mom);
    rebuilder.join();

    size_t wrong = 0;
    for (const auto& row : db->get_face_embeddings_for_person(mom)) {
        if (embedding_distance(probe, row.embedding) > 0.6f) ++wrong;
    }
    EXPECT_EQ(wrong, 0u) << wrong << " face(s) far from the reference were named Mom";
}
