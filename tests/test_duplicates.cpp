#include <gtest/gtest.h>

#include <random>
#include <set>
#include <string>
#include <vector>

#include "duplicates.h"
#include "duplicates_test_util.h"

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

TEST(Duplicates, NewImplementationMatchesBruteForceOverRandomCorpora) {
    std::mt19937_64 rng(12345);  // fixed seed: reproducible failures
    for (int trial = 0; trial < 200; ++trial) {
        size_t n = 2 + (rng() % 60);
        std::vector<uint64_t> hashes;
        // Cluster around a few centers so near-duplicates actually occur.
        std::vector<uint64_t> centers;
        for (int c = 0; c < 4; ++c) centers.push_back(rng());
        for (size_t i = 0; i < n; ++i) {
            uint64_t h = centers[rng() % centers.size()];
            int flips = static_cast<int>(rng() % 10);
            for (int f = 0; f < flips; ++f) h ^= (1ULL << (rng() % 64));
            hashes.push_back(h);
        }
        auto imgs = make_images(hashes);
        int threshold = static_cast<int>(rng() % 12);

        auto expected = normalize(find_near_duplicates_bruteforce(imgs, threshold, {}));
        auto actual = normalize(find_near_duplicates(imgs, threshold, {}));
        ASSERT_EQ(expected, actual)
            << "trial " << trial << " n=" << n << " threshold=" << threshold;
    }
}

TEST(Duplicates, NewImplementationMatchesBruteForceWithExactSkips) {
    std::mt19937_64 rng(999);
    for (int trial = 0; trial < 50; ++trial) {
        size_t n = 5 + (rng() % 30);
        std::vector<ImageInfo> imgs;
        std::set<std::string> exact;
        uint64_t center = rng();
        for (size_t i = 0; i < n; ++i) {
            ImageInfo im;
            im.path = "/photos/i" + std::to_string(i) + ".jpg";
            // Deliberately give some images a shared sha256 and mark it exact.
            im.sha256 =
                (i % 5 == 0) ? std::string(64, 'x') : std::string(63, '0') + std::to_string(i % 10);
            uint64_t h = center;
            for (int f = 0; f < static_cast<int>(rng() % 8); ++f) h ^= (1ULL << (rng() % 64));
            im.phash = h;
            imgs.push_back(im);
        }
        exact.insert(std::string(64, 'x'));
        int threshold = static_cast<int>(rng() % 10);

        auto expected = normalize(find_near_duplicates_bruteforce(imgs, threshold, exact));
        auto actual = normalize(find_near_duplicates(imgs, threshold, exact));
        ASSERT_EQ(expected, actual) << "trial " << trial;
    }
}

TEST(Duplicates, GroupAndMemberOrderMatchBruteForceExactly) {
    std::mt19937_64 rng(31337);
    for (int trial = 0; trial < 500; ++trial) {
        size_t n = 1 + (rng() % 120);
        size_t ncenters = 1 + (rng() % 6);
        std::vector<uint64_t> centers;
        for (size_t c = 0; c < ncenters; ++c) centers.push_back(rng());

        std::vector<ImageInfo> imgs;
        for (size_t i = 0; i < n; ++i) {
            ImageInfo im;
            im.path = "/photos/i" + std::to_string(i) + ".jpg";
            im.sha256 = (rng() % 4 == 0) ? std::string(64, 'x')
                                         : std::string(63, '0') + std::to_string(i % 10);
            uint64_t h = centers[rng() % centers.size()];
            for (int f = 0; f < static_cast<int>(rng() % 12); ++f) h ^= (1ULL << (rng() % 64));
            im.phash = h;
            imgs.push_back(im);
        }
        std::set<std::string> exact;
        if (rng() % 2) exact.insert(std::string(64, 'x'));
        int threshold = static_cast<int>(rng() % 14);

        ASSERT_TRUE(GroupsIdentical(find_near_duplicates_bruteforce(imgs, threshold, exact),
                                    find_near_duplicates(imgs, threshold, exact)))
            << "trial " << trial << " n=" << n << " threshold=" << threshold;
    }
}

// find_near_duplicates dispatches to the brute-force path above a threshold
// cutoff, and again when candidate lists get too large. Both hand-offs must be
// invisible: the groups, their order and their members have to match whichever
// path runs. These walk across the cutoff so a change to it cannot silently
// alter results.
TEST(Duplicates, DispatchMatchesBruteForceAcrossTheThresholdCutoff) {
    std::mt19937_64 rng(20260802);
    // 0..12 spans both sides of the cutoff; 63 and 64 pin the top of the range,
    // which the CLI accepts and which the index handles worst.
    for (int threshold : {0, 1, 5, 6, 7, 8, 9, 10, 12, 63, 64}) {
        for (int trial = 0; trial < 12; ++trial) {
            size_t n = 2 + (rng() % 80);
            std::vector<uint64_t> centers;
            for (int c = 0; c < 3; ++c) centers.push_back(rng());
            std::vector<uint64_t> hashes;
            for (size_t i = 0; i < n; ++i) {
                uint64_t h = centers[rng() % centers.size()];
                for (int f = 0; f < static_cast<int>(rng() % 10); ++f) h ^= (1ULL << (rng() % 64));
                hashes.push_back(h);
            }
            auto imgs = make_images(hashes);
            ASSERT_TRUE(GroupsIdentical(find_near_duplicates_bruteforce(imgs, threshold, {}),
                                        find_near_duplicates(imgs, threshold, {})))
                << "threshold=" << threshold << " trial=" << trial << " n=" << n;
        }
    }
}

// A corpus of near-identical images makes every band match most of the
// collection, which trips the candidate-volume fallback even at low thresholds.
TEST(Duplicates, DispatchMatchesBruteForceWhenCandidateListsExplode) {
    std::mt19937_64 rng(7654321);
    for (int threshold : {1, 5, 7}) {
        // Two centers only, at most 3 flipped bits: nearly everything collides.
        std::vector<uint64_t> centers{rng(), rng()};
        std::vector<ImageInfo> imgs;
        for (size_t i = 0; i < 600; ++i) {
            ImageInfo im;
            im.path = "/photos/t" + std::to_string(i) + ".jpg";
            im.sha256 =
                (i % 7 == 0) ? std::string(64, 'x') : std::string(63, '0') + std::to_string(i % 10);
            uint64_t h = centers[i % centers.size()];
            for (int f = 0; f < static_cast<int>(rng() % 4); ++f) h ^= (1ULL << (rng() % 64));
            im.phash = h;
            imgs.push_back(im);
        }
        std::set<std::string> exact{std::string(64, 'x')};
        ASSERT_TRUE(GroupsIdentical(find_near_duplicates_bruteforce(imgs, threshold, exact),
                                    find_near_duplicates(imgs, threshold, exact)))
            << "threshold=" << threshold;
    }
}
