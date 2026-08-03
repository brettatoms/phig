#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "types.h"

// Hamming distance between two perceptual hashes: the number of differing bits.
// This is a metric (it satisfies the triangle inequality).
int hamming_distance(uint64_t a, uint64_t b);

struct DuplicateGroup {
    std::string type;  // "exact" or "near"
    int distance;      // 0 for exact
    std::vector<ImageInfo> images;
};

// Groups images whose phashes are within `threshold` bits of each other.
//
// Grouping is transitive: A~B and B~C puts A, B and C in one group even when A
// and C are further apart than the threshold.
//
// Pairs whose sha256 matches and whose hash appears in `exact_sha256s` are
// skipped — they are already reported as exact duplicates.
//
// Groups are returned ordered by their union-find root index, and each group's
// images follow the order they appear in `all_images`.
std::vector<DuplicateGroup> find_near_duplicates(const std::vector<ImageInfo>& all_images,
                                                 int threshold,
                                                 const std::set<std::string>& exact_sha256s);

// All-pairs reference implementation, kept as the oracle the equivalence tests
// compare `find_near_duplicates` against. O(n^2); not used by the CLI.
std::vector<DuplicateGroup> find_near_duplicates_bruteforce(
    const std::vector<ImageInfo>& all_images, int threshold,
    const std::set<std::string>& exact_sha256s);
