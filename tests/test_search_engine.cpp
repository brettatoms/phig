#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "faces.h"
#include "search_engine.h"

namespace fs = std::filesystem;

namespace {
Database::FaceSearchResult match(const std::string& path, float distance) {
    Database::FaceSearchResult r;
    r.path = path;
    r.distance = distance;
    r.face_x = r.face_y = r.face_w = r.face_h = 0;
    return r;
}
}  // namespace

TEST(SearchEngineDedupe, KeepsTheClosestMatchPerPathNotTheFirstSeen) {
    // The defect this fixes: matches arrive as one sorted run per query face, so
    // dedupe spans separate runs. A photo matching query face A at 0.8 and query
    // face B at 0.3 must be kept at 0.3, not at whichever face was detected first.
    std::vector<Database::FaceSearchResult> in{
        match("/p/both.jpg", 0.8f), match("/p/other.jpg", 0.5f), match("/p/both.jpg", 0.3f)};

    auto out = dedupe_face_matches(in);

    ASSERT_EQ(out.size(), 2u);
    // Sorted ascending by distance, so the deduped 0.3 entry comes first.
    EXPECT_EQ(out[0].path, "/p/both.jpg");
    EXPECT_FLOAT_EQ(out[0].distance, 0.3f);
    EXPECT_EQ(out[1].path, "/p/other.jpg");
    EXPECT_FLOAT_EQ(out[1].distance, 0.5f);
}

TEST(SearchEngineDedupe, SortsAscendingByDistance) {
    std::vector<Database::FaceSearchResult> in{match("/p/c.jpg", 0.9f), match("/p/a.jpg", 0.1f),
                                               match("/p/b.jpg", 0.5f)};

    auto out = dedupe_face_matches(in);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_FLOAT_EQ(out[0].distance, 0.1f);
    EXPECT_FLOAT_EQ(out[1].distance, 0.5f);
    EXPECT_FLOAT_EQ(out[2].distance, 0.9f);
}

TEST(SearchEngineDedupe, PreservesTheFaceBoxOfTheClosestMatch) {
    // Keeping the minimum distance must carry that match's box, not the box of
    // the entry it replaced — a caller drawing the rectangle needs the right one.
    Database::FaceSearchResult far_match = match("/p/x.jpg", 0.8f);
    far_match.face_x = 11;
    Database::FaceSearchResult near_match = match("/p/x.jpg", 0.2f);
    near_match.face_x = 77;

    auto out = dedupe_face_matches({far_match, near_match});

    ASSERT_EQ(out.size(), 1u);
    EXPECT_FLOAT_EQ(out[0].distance, 0.2f);
    EXPECT_EQ(out[0].face_x, 77);
}

TEST(SearchEngineDedupe, EmptyInputYieldsEmptyOutput) {
    EXPECT_TRUE(dedupe_face_matches({}).empty());
}

TEST(SearchEngineDedupe, PreservesInsertionOrderForTiedDistances) {
    // Equal distances are common with duplicate images and identical face
    // embeddings. A stable sort ensures consistent output across runs, and
    // callers may diff this output byte-for-byte (e.g., Task 5's baseline
    // comparison). This uses 20 tied entries to exceed the insertion-sort
    // threshold where an unstable sort would reorder them.
    std::vector<Database::FaceSearchResult> in;
    in.push_back(match("/p/closer.jpg", 0.1f));
    for (int i = 0; i < 20; ++i) {
        // Zero-padded paths so string order matches numeric order.
        char path[32];
        snprintf(path, sizeof(path), "/p/t%02d.jpg", i);
        in.push_back(match(path, 0.5f));
    }

    auto out = dedupe_face_matches(in);

    ASSERT_EQ(out.size(), 21u);
    EXPECT_FLOAT_EQ(out[0].distance, 0.1f);
    EXPECT_EQ(out[0].path, "/p/closer.jpg");
    // All 20 tied entries at 0.5 must appear in insertion order.
    for (int i = 0; i < 20; ++i) {
        char expected_path[32];
        snprintf(expected_path, sizeof(expected_path), "/p/t%02d.jpg", i);
        EXPECT_FLOAT_EQ(out[1 + i].distance, 0.5f);
        EXPECT_EQ(out[1 + i].path, expected_path);
    }
}

namespace {
// A scratch database with rows inserted directly. The engine reads only, so no
// real files and no decoding are needed.
struct ScratchSearch {
    fs::path dir;
    std::string db_path;
    std::unique_ptr<Database> db;

    ScratchSearch() {
        dir = fs::temp_directory_path() / ("phig_search_engine_" + std::to_string(::getpid()) +
                                           "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(dir);
        fs::create_directories(dir);
        db_path = (dir / "phig.db").string();
        db = std::make_unique<Database>(db_path);
        db->init_schema();
    }
    ~ScratchSearch() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Distinct sha per row so nothing collides.
    std::string add(const std::string& rel, const std::string& ext, uint64_t phash,
                    const std::string& media = "image", int64_t size = 1000) {
        ImageInfo info;
        info.path = (dir / rel).string();
        info.filename = fs::path(rel).filename().string();
        info.extension = ext;
        info.file_size = size;
        info.media_type = media;
        info.created_at = "2024-01-01T00:00:00Z";
        info.modified_at = "2024-01-01T00:00:00Z";
        info.sha256 = std::string(63, '0') + std::to_string(db->count() % 10);
        info.phash = phash;
        info.width = 100;
        info.height = 100;
        info.metadata_json = "{}";
        db->insert(info);
        return info.path;
    }

    // Labels the image at `path` as containing `person`, so it shows up in
    // get_images_for_person results.
    void label(const std::string& path, const std::string& person) {
        int64_t image_id = db->get_image_id(path);
        FaceInfo face{};
        int64_t face_id = db->insert_face(image_id, face);
        db->set_face_person(face_id, db->get_or_create_person(person));
    }
};
}  // namespace

TEST(SearchEngineFilter, ReturnsEveryRowWhenNoCriteriaAreSet) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0);
    s.add("b.jpg", "jpg", 0);
    SearchQuery q;
    EXPECT_EQ(SearchEngine(q, *s.db).run().images.size(), 2u);
}

TEST(SearchEngineFilter, MediaCriterionExcludesVideos) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0, "image");
    s.add("clip.mov", "mov", 0, "video");
    SearchQuery q;
    q.filter.media = "image";
    auto r = SearchEngine(q, *s.db).run();
    ASSERT_EQ(r.images.size(), 1u);
    EXPECT_EQ(r.images[0].extension, "jpg");
}

TEST(SearchEngineFilter, MatchPatternSelectsBySubpath) {
    ScratchSearch s;
    s.add("keep_1.jpg", "jpg", 0);
    s.add("drop_1.jpg", "jpg", 0);
    SearchQuery q;
    q.matches.push_back("*keep*");
    auto r = SearchEngine(q, *s.db).run();
    ASSERT_EQ(r.images.size(), 1u);
    EXPECT_NE(r.images[0].path.find("keep_1"), std::string::npos);
}

TEST(SearchEngineFilter, LimitAppliesAfterPathSelectionNotBeforeIt) {
    // The interlock: pushing `limit` into SQL while a selection is pending would
    // truncate the wrong rows and return fewer than asked for. Four rows, two of
    // which match; a limit of 2 must yield exactly the 2 matching rows.
    ScratchSearch s;
    s.add("drop_1.jpg", "jpg", 0);
    s.add("drop_2.jpg", "jpg", 0);
    s.add("keep_1.jpg", "jpg", 0);
    s.add("keep_2.jpg", "jpg", 0);
    SearchQuery q;
    q.matches.push_back("*keep*");
    q.limit = 2;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.images.size(), 2u);
    for (const auto& img : r.images) {
        EXPECT_NE(img.path.find("keep_"), std::string::npos) << img.path;
    }
}

TEST(SearchEngineFilter, LimitTruncatesWhenNoSelectionIsPending) {
    ScratchSearch s;
    for (int i = 0; i < 5; ++i) s.add("f" + std::to_string(i) + ".jpg", "jpg", 0);
    SearchQuery q;
    q.limit = 3;
    EXPECT_EQ(SearchEngine(q, *s.db).run().images.size(), 3u);
}

TEST(SearchEngineSimilar, ReturnsRowsWithinThresholdSortedByDistance) {
    ScratchSearch s;
    s.add("exact.jpg", "jpg", 0xFFFFFFFFFFFFFFFFull);  // distance 0
    s.add("near.jpg", "jpg", 0xFFFFFFFFFFFFFFFEull);   // distance 1
    s.add("far.jpg", "jpg", 0x0000000000000000ull);    // distance 64
    SearchQuery q;
    q.mode = SearchMode::Similar;
    q.phash = 0xFFFFFFFFFFFFFFFFull;
    q.phash_threshold = 5;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.images.size(), 2u);
    EXPECT_NE(r.images[0].path.find("exact"), std::string::npos) << "closest must sort first";
    EXPECT_NE(r.images[1].path.find("near"), std::string::npos);
}

TEST(SearchEngineSimilar, ThresholdIsInclusive) {
    ScratchSearch s;
    s.add("at_threshold.jpg", "jpg", 0xFFFFFFFFFFFFFFF0ull);  // distance 4
    SearchQuery q;
    q.mode = SearchMode::Similar;
    q.phash = 0xFFFFFFFFFFFFFFFFull;
    q.phash_threshold = 4;
    EXPECT_EQ(SearchEngine(q, *s.db).run().images.size(), 1u);
}

TEST(SearchEngineSimilar, LimitTruncatesAfterDistanceSort) {
    // Similar mode has no SQL-level limit, so this can only pass if the final
    // post-selection resize runs — unlike Filter mode, where the SQL limit alone
    // can satisfy a same-shaped assertion.
    ScratchSearch s;
    s.add("exact.jpg", "jpg", 0xFFFFFFFFFFFFFFFFull);  // distance 0
    s.add("near.jpg", "jpg", 0xFFFFFFFFFFFFFFFEull);   // distance 1
    s.add("far.jpg", "jpg", 0xFFFFFFFFFFFFFFF0ull);    // distance 4
    SearchQuery q;
    q.mode = SearchMode::Similar;
    q.phash = 0xFFFFFFFFFFFFFFFFull;
    q.phash_threshold = 5;
    q.limit = 2;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.images.size(), 2u);
    EXPECT_NE(r.images[0].path.find("exact"), std::string::npos);
    EXPECT_NE(r.images[1].path.find("near"), std::string::npos);
}

TEST(SearchEnginePerson, UnknownPersonYieldsNoPaths) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0);
    SearchQuery q;
    q.mode = SearchMode::Person;
    q.person = "nobody-by-this-name";
    auto r = SearchEngine(q, *s.db).run();
    EXPECT_TRUE(r.paths.empty());
    EXPECT_TRUE(r.images.empty()) << "person mode fills `paths`, not `images`";
}

TEST(SearchEnginePerson, IgnoresPathSelectionAndLimit) {
    // The CLI's person branch returns before the shared match/filter/limit
    // block (main.cpp:953-984 returns; the block is at 1130-1143), so neither
    // applies here. `matches` is set to exclude "drop" and `limit` is set below
    // the labeled count; both must be ignored.
    ScratchSearch s;
    std::string keep1 = s.add("keep_1.jpg", "jpg", 0);
    std::string keep2 = s.add("keep_2.jpg", "jpg", 0);
    std::string drop = s.add("drop_1.jpg", "jpg", 0);
    s.label(keep1, "Alice");
    s.label(keep2, "Alice");
    s.label(drop, "Alice");

    SearchQuery q;
    q.mode = SearchMode::Person;
    q.person = "Alice";
    q.matches.push_back("*keep*");
    q.limit = 1;

    auto r = SearchEngine(q, *s.db).run();

    EXPECT_EQ(r.paths.size(), 3u) << "selection and limit must not apply to person mode";
    EXPECT_NE(std::find(r.paths.begin(), r.paths.end(), drop), r.paths.end())
        << "drop_1.jpg would be excluded by the pending match pattern if selection ran";
}

namespace {
// A 128-float embedding that is all `v`. Two such embeddings are at a
// predictable distance from each other, which is what makes threshold and
// ordering assertions checkable without real face data.
std::array<float, 128> flat_embedding(float v) {
    std::array<float, 128> e{};
    e.fill(v);
    return e;
}
}  // namespace

TEST(SearchEngineFace, ReturnsMatchesInMatchesNotImages) {
    ScratchSearch s;
    std::string p = s.add("a.jpg", "jpg", 0);
    int64_t id = s.db->get_image_id(p);
    ASSERT_GE(id, 0);
    FaceInfo f;
    f.embedding = flat_embedding(0.1f);
    s.db->insert_face(id, f);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.1f));
    q.face_threshold = 10.0f;  // generous: this test is about plumbing, not ranking

    auto r = SearchEngine(q, *s.db).run();

    EXPECT_FALSE(r.matches.empty()) << "face mode fills `matches`";
    EXPECT_TRUE(r.images.empty()) << "face mode must not fill `images`";
}

TEST(SearchEngineFace, CrossBatchConflictKeepsTheClosestMatch) {
    // The bug this fixes, end to end. A single-image, two-face fixture is NOT
    // enough to catch this: `search_faces` sorts within a call, so with only
    // one path in the database, each batch already contains that path's own
    // minimum and first-seen-wins accidentally agrees with closest-wins. To
    // force a genuine cross-batch conflict, `decoy.jpg` gives query face 1 an
    // exact match, pushing `target.jpg`'s poor-but-present face out to the
    // SECOND slot of batch 1 — and `q.limit = 2` (which becomes per_face_limit)
    // trims batch 1 to exactly those two rows, so target's true (exact) match
    // in batch 2 never gets a chance to be "first seen". Do not simplify this
    // fixture by dropping the decoy or the limit; doing so re-hides the bug, as
    // happened once already while designing this test.
    ScratchSearch s;
    std::string target_path = s.add("target.jpg", "jpg", 0);
    int64_t target_id = s.db->get_image_id(target_path);
    ASSERT_GE(target_id, 0);
    FaceInfo target_poor;
    target_poor.embedding = flat_embedding(0.50f);  // poor match for query 1
    target_poor.x = target_poor.y = target_poor.width = target_poor.height = 1;
    s.db->insert_face(target_id, target_poor);
    FaceInfo target_exact;
    target_exact.embedding = flat_embedding(0.10f);  // exact match for query 2
    target_exact.x = target_exact.y = target_exact.width = target_exact.height = 2;
    s.db->insert_face(target_id, target_exact);

    std::string decoy_path = s.add("decoy.jpg", "jpg", 0);
    int64_t decoy_id = s.db->get_image_id(decoy_path);
    ASSERT_GE(decoy_id, 0);
    FaceInfo decoy_face;
    decoy_face.embedding = flat_embedding(0.90f);  // exact match for query 1
    decoy_face.x = decoy_face.y = decoy_face.width = decoy_face.height = 3;
    s.db->insert_face(decoy_id, decoy_face);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.90f));  // exact on decoy, poor on target
    q.face_embeddings.push_back(flat_embedding(0.10f));  // exact on target
    q.face_threshold = 100.0f;
    q.limit = 2;  // load-bearing: caps per_face_limit at 2, see comment above

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.matches.size(), 2u);
    auto it = std::find_if(r.matches.begin(), r.matches.end(), [](const auto& m) {
        return m.path.find("target") != std::string::npos;
    });
    ASSERT_NE(it, r.matches.end());
    EXPECT_NEAR(it->distance, 0.0f, 1e-3)
        << "kept distance must be the closest match, not whichever query face's batch saw it "
           "first";
}

TEST(SearchEngineFace, RespectsTheDistanceThreshold) {
    // Checks that the threshold is plumbed through to `Database::search_faces`,
    // whose internal `if (dist > threshold) continue;` does the actual filtering.
    ScratchSearch s;
    std::string p = s.add("a.jpg", "jpg", 0);
    int64_t id = s.db->get_image_id(p);
    FaceInfo f;
    f.embedding = flat_embedding(0.9f);
    s.db->insert_face(id, f);

    SearchQuery q;
    q.mode = SearchMode::Face;
    q.face_embeddings.push_back(flat_embedding(0.1f));
    q.face_threshold = 0.001f;  // far too tight for this pair

    EXPECT_TRUE(SearchEngine(q, *s.db).run().matches.empty());
}

TEST(SearchEngineFace, AppliesTheLimitAfterDeduplication) {
    // Limiting before dedupe could return fewer distinct paths than asked for.
    // `per_face_limit` equals `q.limit` (2) and caps EACH batch at the database,
    // so a single query embedding can never make `all` exceed the limit — dedup
    // then has nothing left to do. Two query embeddings are required: each is
    // capped at 2 rows, but together they can produce up to 4 rows, more than
    // the limit, which is what makes the post-dedup resize meaningfully
    // different from a pre-dedup one. `both.jpg` has two faces that each win
    // their own query (so it appears once per batch, twice total); `other.jpg`
    // has one face that also matches both queries (same: twice total). Limiting
    // the raw 4 rows to 2 before dedup can keep two rows of the SAME path and
    // drop the other path entirely; limiting after dedup (2 distinct paths)
    // keeps both. Do not shrink this to one query embedding — that shape passes
    // even if the limit is applied before dedup.
    ScratchSearch s;
    std::string both = s.add("both.jpg", "jpg", 0);
    int64_t both_id = s.db->get_image_id(both);
    ASSERT_GE(both_id, 0);
    FaceInfo f1;
    f1.embedding = flat_embedding(0.10f);
    f1.x = f1.y = f1.width = f1.height = 1;
    s.db->insert_face(both_id, f1);
    FaceInfo f2;
    f2.embedding = flat_embedding(0.20f);
    f2.x = f2.y = f2.width = f2.height = 2;
    s.db->insert_face(both_id, f2);

    std::string other = s.add("other.jpg", "jpg", 0);
    int64_t other_id = s.db->get_image_id(other);
    ASSERT_GE(other_id, 0);
    FaceInfo f3;
    f3.embedding = flat_embedding(0.80f);
    f3.x = f3.y = f3.width = f3.height = 3;
    s.db->insert_face(other_id, f3);

    SearchQuery q;
    q.mode = SearchMode::Face;
    // Query 1 (0.10) ranks both.jpg's two faces (f1, f2) as its closest two,
    // pushing other.jpg's face (f3) out of that batch's top-2 entirely. Query 2
    // (0.80) ranks other.jpg's face (f3) first and both.jpg's f2 second,
    // pushing f1 out. So `all` (batch1 ++ batch2) is [f1, f2, f3, f2] — its
    // first two entries are BOTH `both.jpg`. Resizing `all` to 2 before dedup
    // keeps only those two identical-path rows and loses `other.jpg` entirely;
    // resizing the deduped (2-distinct-path) result instead keeps both.
    q.face_embeddings.push_back(flat_embedding(0.10f));
    q.face_embeddings.push_back(flat_embedding(0.80f));
    q.face_threshold = 100.0f;
    q.limit = 2;

    auto r = SearchEngine(q, *s.db).run();

    ASSERT_EQ(r.matches.size(), 2u);
    EXPECT_NE(r.matches[0].path, r.matches[1].path)
        << "must be 2 distinct paths, not 2 rows of one";
}

TEST(SearchEngineFace, NoQueryEmbeddingsYieldsNoMatches) {
    ScratchSearch s;
    s.add("a.jpg", "jpg", 0);
    SearchQuery q;
    q.mode = SearchMode::Face;
    EXPECT_TRUE(SearchEngine(q, *s.db).run().matches.empty());
}
