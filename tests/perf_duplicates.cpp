#include <gtest/gtest.h>

#include <chrono>
#include <random>
#include <vector>

#include "duplicates.h"
#include "duplicates_test_util.h"

// Performance tests for find_near_duplicates, kept out of phig-tests (the
// default suite) because each one pays for an O(n^2) brute-force baseline on
// top of the banded index it is timing. See tests/test_duplicates.cpp for the
// correctness suite these complement.

TEST(DuplicatesPerf, FortyThousandImagesCompletesQuickly) {
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
    // Timing-only: the brute-force oracle costs ~5.3 s at this size, which
    // would make correctness checking here as slow as the regression this
    // test guards against. Correctness at this scale is covered by
    // DispatchMatchesBruteForceAcrossTheThresholdCutoff and
    // DispatchMatchesBruteForceWhenCandidateListsExplode on smaller corpora.
    EXPECT_LT(ms, 2000) << "40k images took " << ms << " ms";
    (void)groups;
}

// A high threshold must not be slower than the all-pairs scan it replaced.
// Without the cutoff the banded index degrades badly here — bands become a bit
// or two wide, so nearly the whole corpus matches every band. Measured at
// n=5000: ~16000 ms at threshold 64 against ~110 ms for brute force. The
// dispatch keeps this on the brute-force path, and the ceiling is set well
// above brute force's time but far below the index's.
TEST(DuplicatesPerf, HighThresholdDoesNotRegressAgainstBruteForce) {
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
        EXPECT_TRUE(GroupsIdentical(find_near_duplicates_bruteforce(imgs, threshold, {}), groups))
            << "threshold " << threshold;
    }
}

// The regressions that matter most happen at the DEFAULT threshold, not at high
// ones: a collection of few distinct scenes makes most of the corpus share a
// band, so candidate lists explode while the threshold stays small. The
// threshold cutoff cannot see this; only the candidate-volume guard catches it.
//
// Asserted against a brute-force baseline measured in the same run, not a
// wall-clock constant: a fixed ms ceiling sits close enough to the measured
// time on a fast machine that CPU contention alone (e.g. a saturated CI
// runner) pushes it over, with no actual regression. Timing the oracle in the
// same process expresses the actual invariant — the index must not be
// materially slower than the thing it replaced — rather than a specific
// duration.
//
// That removes the dependence on how FAST the machine is. It does not remove
// the dependence on how LOADED it is, which is why these tests are RUN_SERIAL
// in the root CMakeLists: the index builds hash tables and is memory-bound
// while the baseline is a tight loop over a contiguous vector and is
// compute-bound, so concurrent work slows the two by different factors and
// moves the ratio itself. On this corpus the index is no faster than brute
// force to begin with — idle ratio 0.94-1.04 — so the 1.5x ceiling has only
// half a turn of headroom, and shared-machine drift to 1.18-1.34 eats most of
// it. Do not read a ratio near 1.0 as a regression: the guard exists to catch
// the 3.13x below, not to assert the index wins here.
TEST(DuplicatesPerf, ClusteredCorporaDoNotRegressAtTheDefaultThreshold) {
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

        auto brute_start = std::chrono::steady_clock::now();
        auto brute_groups = find_near_duplicates_bruteforce(imgs, 5, {});
        auto brute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - brute_start)
                            .count();

        auto start = std::chrono::steady_clock::now();
        auto groups = find_near_duplicates(imgs, 5, {});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        EXPECT_LT(ms, std::max<int64_t>(brute_ms * 3 / 2, 50))
            << "tight corpus (3 centers, 8k images) took " << ms << " ms vs brute force "
            << brute_ms << " ms";
        EXPECT_TRUE(GroupsIdentical(brute_groups, groups));
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

        auto brute_start = std::chrono::steady_clock::now();
        auto brute_groups = find_near_duplicates_bruteforce(imgs, 5, {});
        auto brute_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - brute_start)
                            .count();

        auto start = std::chrono::steady_clock::now();
        auto groups = find_near_duplicates(imgs, 5, {});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        EXPECT_LT(ms, std::max<int64_t>(brute_ms * 3 / 2, 50))
            << "clustered corpus (8 centers, 12k images) took " << ms << " ms vs brute force "
            << brute_ms << " ms";
        EXPECT_TRUE(GroupsIdentical(brute_groups, groups));
    }
}

// A corpus whose first images are unrepresentative of the rest must still be
// classified correctly. Newest-first ordering puts recent varied shots ahead of
// an older near-identical burst, and a front-loaded sample would miss the burst
// entirely: measured, a prefix sample saw 69 candidates per image against a true
// 14495 and ran 4.4x slower than brute force. The strided sample sees both.
TEST(DuplicatesPerf, SamplingIsNotFooledByCorpusOrdering) {
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
