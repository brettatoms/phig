#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "duplicates.h"

// Shared helpers for tests/test_duplicates.cpp (correctness) and
// tests/perf_duplicates.cpp (performance). Kept in one place so the two
// files can't drift into two different notions of "identical groups" —
// exactly the kind of fork Task 2 of this plan removed elsewhere.

// Builds n images with controlled phashes so grouping is predictable.
inline std::vector<ImageInfo> make_images(const std::vector<uint64_t>& hashes) {
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

// Normalizes groups to a comparable form: each group as a sorted set of paths,
// and the collection of groups itself sorted, so ordering differences between
// implementations do not register as behavioral differences.
inline std::set<std::vector<std::string>> normalize(const std::vector<DuplicateGroup>& gs) {
    std::set<std::vector<std::string>> out;
    for (const auto& g : gs) {
        std::vector<std::string> paths;
        for (const auto& im : g.images) paths.push_back(im.path);
        std::sort(paths.begin(), paths.end());
        out.insert(paths);
    }
    return out;
}

// normalize() above sorts group order away, so it cannot see a reordering.
// `phig duplicates` prints groups in the order they are returned, so ordering
// is user-visible and needs its own comparison.
inline ::testing::AssertionResult GroupsIdentical(const std::vector<DuplicateGroup>& expected,
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
