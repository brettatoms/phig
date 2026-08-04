#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "search_engine.h"

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
    // comparison). The two tied entries must appear in insertion order.
    std::vector<Database::FaceSearchResult> in{
        match("/p/closer.jpg", 0.1f),
        match("/p/tied_first.jpg", 0.5f),
        match("/p/tied_second.jpg", 0.5f),
    };

    auto out = dedupe_face_matches(in);

    ASSERT_EQ(out.size(), 3u);
    EXPECT_FLOAT_EQ(out[0].distance, 0.1f);
    EXPECT_EQ(out[0].path, "/p/closer.jpg");
    // Tied at 0.5: must appear in insertion order.
    EXPECT_FLOAT_EQ(out[1].distance, 0.5f);
    EXPECT_EQ(out[1].path, "/p/tied_first.jpg");
    EXPECT_FLOAT_EQ(out[2].distance, 0.5f);
    EXPECT_EQ(out[2].path, "/p/tied_second.jpg");
}
