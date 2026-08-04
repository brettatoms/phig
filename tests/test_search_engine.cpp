#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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
