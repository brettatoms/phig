#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>
#include <vector>

#include "duplicates.h"

namespace {
// Builds n images with controlled phashes so grouping is predictable.
std::vector<ImageInfo> make_images(const std::vector<uint64_t>& hashes) {
    std::vector<ImageInfo> out;
    for (size_t i = 0; i < hashes.size(); ++i) {
        ImageInfo im;
        im.path = "/photos/img" + std::to_string(i) + ".jpg";
        im.filename = "img" + std::to_string(i) + ".jpg";
        im.sha256 = std::string(63, '0') + std::to_string(i % 10);
        im.phash = hashes[i];
        out.push_back(im);
    }
    return out;
}
}  // namespace

TEST(Duplicates, HammingDistanceCountsDifferingBits) {
    EXPECT_EQ(hamming_distance(0ULL, 0ULL), 0);
    EXPECT_EQ(hamming_distance(0ULL, 1ULL), 1);
    EXPECT_EQ(hamming_distance(0ULL, 0xFFULL), 8);
    EXPECT_EQ(hamming_distance(0xFFFFFFFFFFFFFFFFULL, 0ULL), 64);
}

TEST(Duplicates, IdenticalHashesGroupTogether) {
    auto imgs = make_images({0x1111ULL, 0x1111ULL, 0xFFFFULL});
    auto groups = find_near_duplicates(imgs, 5, {});
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].images.size(), 2u);
}

TEST(Duplicates, DistantHashesDoNotGroup) {
    auto imgs = make_images({0x0000ULL, 0xFFFFFFFFFFFFFFFFULL});
    auto groups = find_near_duplicates(imgs, 5, {});
    EXPECT_TRUE(groups.empty());
}

TEST(Duplicates, GroupingIsTransitiveAcrossTheThreshold) {
    // A~B (distance 1) and B~C (distance 1), but A~C is distance 2.
    // With threshold 1 all three still land in ONE group via union-find.
    auto imgs = make_images({0b000ULL, 0b001ULL, 0b011ULL});
    auto groups = find_near_duplicates(imgs, 1, {});
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].images.size(), 3u);
}

TEST(Duplicates, ExactDuplicatesAreSkipped) {
    std::vector<ImageInfo> imgs;
    for (int i = 0; i < 2; ++i) {
        ImageInfo im;
        im.path = "/photos/same" + std::to_string(i) + ".jpg";
        im.sha256 = std::string(64, 'a');  // identical content
        im.phash = 0x1234ULL;
        imgs.push_back(im);
    }
    std::set<std::string> exact{std::string(64, 'a')};
    auto groups = find_near_duplicates(imgs, 5, exact);
    EXPECT_TRUE(groups.empty()) << "pairs already reported as exact must not repeat as near";
}

TEST(Duplicates, EmptyInputYieldsNoGroups) { EXPECT_TRUE(find_near_duplicates({}, 5, {}).empty()); }

TEST(Duplicates, SingleImageYieldsNoGroups) {
    auto imgs = make_images({0x1ULL});
    EXPECT_TRUE(find_near_duplicates(imgs, 5, {}).empty());
}
