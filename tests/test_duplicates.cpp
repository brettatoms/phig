#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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

namespace {
// Normalizes groups to a comparable form: each group as a sorted set of paths,
// and the collection of groups itself sorted, so ordering differences between
// implementations do not register as behavioral differences.
std::set<std::vector<std::string>> normalize(const std::vector<DuplicateGroup>& gs) {
    std::set<std::vector<std::string>> out;
    for (const auto& g : gs) {
        std::vector<std::string> paths;
        for (const auto& im : g.images) paths.push_back(im.path);
        std::sort(paths.begin(), paths.end());
        out.insert(paths);
    }
    return out;
}
}  // namespace

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

// normalize() above sorts group order away, so it cannot see a reordering.
// `phig duplicates` prints groups in the order they are returned, so ordering
// is user-visible and needs its own comparison.
namespace {
::testing::AssertionResult GroupsIdentical(const std::vector<DuplicateGroup>& expected,
                                           const std::vector<DuplicateGroup>& actual) {
    if (expected.size() != actual.size()) {
        return ::testing::AssertionFailure()
               << "group count " << actual.size() << " != " << expected.size();
    }
    for (size_t g = 0; g < expected.size(); ++g) {
        if (expected[g].type != actual[g].type || expected[g].distance != actual[g].distance) {
            return ::testing::AssertionFailure() << "group " << g << " type/distance differs";
        }
        if (expected[g].images.size() != actual[g].images.size()) {
            return ::testing::AssertionFailure() << "group " << g << " size differs";
        }
        for (size_t k = 0; k < expected[g].images.size(); ++k) {
            if (expected[g].images[k].path != actual[g].images[k].path) {
                return ::testing::AssertionFailure()
                       << "group " << g << " member " << k << ": " << actual[g].images[k].path
                       << " != " << expected[g].images[k].path;
            }
        }
    }
    return ::testing::AssertionSuccess();
}
}  // namespace

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

TEST(Duplicates, FortyThousandImagesCompletesQuickly) {
    std::mt19937_64 rng(4242);
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 40000; ++i) hashes.push_back(rng());
    auto imgs = make_images(hashes);

    auto start = std::chrono::steady_clock::now();
    auto groups = find_near_duplicates(imgs, 5, {});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();

    // The brute-force implementation took ~5200 ms for this input. A generous
    // ceiling still fails loudly if the banded index degrades to a linear scan.
    EXPECT_LT(ms, 2000) << "40k images took " << ms << " ms";
    (void)groups;
}

// A high threshold must not be slower than the all-pairs scan it replaced.
// Without the cutoff the banded index degrades badly here — bands become a bit
// or two wide, so nearly the whole corpus matches every band. Measured at
// n=5000: ~16000 ms at threshold 64 against ~110 ms for brute force. The
// dispatch keeps this on the brute-force path, and the ceiling is set well
// above brute force's time but far below the index's.
TEST(Duplicates, HighThresholdDoesNotRegressAgainstBruteForce) {
    std::mt19937_64 rng(4242);
    std::vector<uint64_t> hashes;
    for (int i = 0; i < 5000; ++i) hashes.push_back(rng());
    auto imgs = make_images(hashes);

    for (int threshold : {32, 64}) {
        auto start = std::chrono::steady_clock::now();
        auto groups = find_near_duplicates(imgs, threshold, {});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        EXPECT_LT(ms, 1500) << "threshold " << threshold << " on 5k images took " << ms << " ms";
        (void)groups;
    }
}

// The regressions that matter most happen at the DEFAULT threshold, not at high
// ones: a collection of few distinct scenes makes most of the corpus share a
// band, so candidate lists explode while the threshold stays small. The
// threshold cutoff cannot see this; only the candidate-volume guard catches it.
//
// Measured: the tight corpus runs ~310 ms and the clustered one ~530 ms as
// shipped, against ~1620 ms and ~990 ms with the guards disabled. The clustered
// case also separates the candidate-volume divisor from its previous, too-lax
// value — at n/2 it does not trigger and takes ~990 ms. Ceilings sit ~1.5-3x
// above the shipped times and well below the regressed ones, so a real
// regression fails while timing noise (a few ms across runs) does not.
TEST(Duplicates, ClusteredCorporaDoNotRegressAtTheDefaultThreshold) {
    // Few centers, small variations: the shape that defeats the banded index.
    {
        std::mt19937_64 rng(4242);
        std::vector<uint64_t> centers;
        for (int c = 0; c < 3; ++c) centers.push_back(rng());
        std::vector<uint64_t> hashes;
        for (int i = 0; i < 8000; ++i) {
            uint64_t h = centers[rng() % centers.size()];
            for (int f = 0; f < static_cast<int>(rng() % 4); ++f) h ^= (1ULL << (rng() % 64));
            hashes.push_back(h);
        }
        auto imgs = make_images(hashes);
        auto start = std::chrono::steady_clock::now();
        auto groups = find_near_duplicates(imgs, 5, {});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        EXPECT_LT(ms, 1000) << "tight corpus (3 centers, 8k images) took " << ms << " ms";
        (void)groups;
    }

    // Eight scenes with wider variation — an ordinary photo collection, and the
    // case that regressed 3.13x before the candidate-volume divisor was lowered.
    {
        std::mt19937_64 rng(4242);
        std::vector<uint64_t> centers;
        for (int c = 0; c < 8; ++c) centers.push_back(rng());
        std::vector<uint64_t> hashes;
        for (int i = 0; i < 12000; ++i) {
            uint64_t h = centers[rng() % centers.size()];
            for (int f = 0; f < static_cast<int>(rng() % 9); ++f) h ^= (1ULL << (rng() % 64));
            hashes.push_back(h);
        }
        auto imgs = make_images(hashes);
        auto start = std::chrono::steady_clock::now();
        auto groups = find_near_duplicates(imgs, 5, {});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        EXPECT_LT(ms, 800) << "clustered corpus (8 centers, 12k images) took " << ms << " ms";
        (void)groups;
    }
}

// A corpus whose first images are unrepresentative of the rest must still be
// classified correctly. Newest-first ordering puts recent varied shots ahead of
// an older near-identical burst, and a front-loaded sample would miss the burst
// entirely: measured, a prefix sample saw 69 candidates per image against a true
// 14495 and ran 4.4x slower than brute force. The strided sample sees both.
TEST(Duplicates, SamplingIsNotFooledByCorpusOrdering) {
    std::mt19937_64 rng(4242);
    std::vector<uint64_t> centers;
    for (int c = 0; c < 3; ++c) centers.push_back(rng());

    std::vector<uint64_t> hashes;
    for (int i = 0; i < 8000; ++i) {
        if (i < 512) {
            hashes.push_back(rng());  // varied head
        } else {
            uint64_t h = centers[rng() % centers.size()];  // near-identical tail
            for (int f = 0; f < static_cast<int>(rng() % 4); ++f) h ^= (1ULL << (rng() % 64));
            hashes.push_back(h);
        }
    }
    auto imgs = make_images(hashes);

    auto start = std::chrono::steady_clock::now();
    auto groups = find_near_duplicates(imgs, 5, {});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    // Ceiling is generous: this runs ~300 ms here and ~620 ms on slower machines,
    // against ~1300 ms if the sampler misreads the corpus. Perf assertions that
    // sit close to the measured time are worse than none.
    EXPECT_LT(ms, 2000) << "varied head over tight tail took " << ms << " ms";

    // Correctness must hold whichever path the sampler picks.
    EXPECT_TRUE(GroupsIdentical(find_near_duplicates_bruteforce(imgs, 5, {}), groups));
}

// The candidate-volume sample walks fixed positions, so a corpus can in
// principle be built that is tight everywhere the sampler does not look. This
// reconstructs exactly that: near-identical throughout, except the positions the
// first pass probes, which get distinct hashes. Measured with a single pass, the
// sample read ~220 candidates per image against a true ~87000 and the scan ran
// 5.9x slower than brute force.
//
// The second pass exists to catch this. It walks a step coprime to n from a
// phash-derived offset, so its positions are neither the first pass's nor
// predictable from n alone. Hiding from both at once would need two disjoint
// decoy sets, and covering the second pass's positions exposes the corpus to the
// first.
TEST(Duplicates, SamplingIsNotFooledByAlignmentToItsOwnStride) {
    const size_t n = 11000;
    const size_t sample = 256;

    // The positions the first pass probes, computed the same way it does:
    // `sample` steps of n/sample from zero.
    const size_t stride = std::max<size_t>(n / sample, 1);
    std::set<size_t> probed;
    for (size_t k = 0; k < sample; ++k) probed.insert((k * stride) % n);

    std::mt19937_64 rng(24680);
    std::vector<uint64_t> centers;
    for (int c = 0; c < 2; ++c) centers.push_back(rng());

    std::vector<uint64_t> hashes(n);
    for (size_t i = 0; i < n; ++i) {
        if (probed.count(i)) {
            hashes[i] = rng();  // distinct exactly where the first pass looks
        } else {
            uint64_t h = centers[rng() % centers.size()];
            for (int f = 0; f < static_cast<int>(rng() % 3); ++f) h ^= (1ULL << (rng() % 64));
            hashes[i] = h;
        }
    }
    auto imgs = make_images(hashes);

    auto start = std::chrono::steady_clock::now();
    auto groups = find_near_duplicates(imgs, 5, {});
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    // Runs ~1400 ms here (the guard sends this corpus to brute force, which is
    // the right choice for it) against ~6240 ms with only one sampling pass.
    // The ceiling sits ~2x above the former and well below the latter.
    EXPECT_LT(ms, 3000) << "stride-aligned corpus took " << ms << " ms";

    EXPECT_TRUE(GroupsIdentical(find_near_duplicates_bruteforce(imgs, 5, {}), groups));
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
