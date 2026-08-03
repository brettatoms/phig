#include "duplicates.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

int hamming_distance(uint64_t a, uint64_t b) { return std::popcount(a ^ b); }

namespace {

// A banded (multi-index) hash over the 64-bit phashes.
//
// Pigeonhole principle: if two hashes differ in at most `threshold` bits, then
// splitting the 64 bits into `threshold + 1` disjoint bands forces at least one
// band to be bit-identical — there are more bands than differing bits. So every
// true near-duplicate pair is found by looking up exact band matches, and the
// filter is complete: it never misses a pair. Candidates are then confirmed
// with a real Hamming distance check, so it never reports a false one either.
//
// A BK-tree was measured here first and rejected. Hamming distance is a metric,
// so its pruning is valid, but on random 64-bit phashes at the CLI's default
// threshold of 5, the [d-5, d+5] band still admits most of each node's
// children. Nodes visited per query grew with n — ~5.4k at 40k images and
// ~16.6k at 160k — so the scan stayed quadratic even with flat arrays and no
// std::map: 9 s at 40k and 150 s at 160k. The banded index does 160k in 1.7 s.
class BandedIndex {
   public:
    // `threshold` is clamped to [0, 64]: the CLI parses it with std::stoi and
    // does not range-check, and a threshold of 64 already matches everything.
    BandedIndex(int threshold, size_t capacity) : bands_(std::clamp(threshold, 0, 64) + 1) {
        shifts_.resize(bands_);
        masks_.resize(bands_);
        for (int k = 0; k < bands_; ++k) {
            // Split 64 bits as evenly as possible; widths differ by at most 1.
            const int lo = static_cast<int>(64LL * k / bands_);
            const int hi = static_cast<int>(64LL * (k + 1) / bands_);
            const int width = hi - lo;
            shifts_[k] = lo;
            masks_[k] = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
        }
        buckets_.resize(bands_);
        for (auto& b : buckets_) b.reserve(capacity * 2);
    }

    // Appends every already-inserted image index sharing a band with `hash`.
    // May report an index more than once; callers must tolerate duplicates.
    void collect_candidates(uint64_t hash, std::vector<uint32_t>& out) const {
        for (int k = 0; k < bands_; ++k) {
            auto it = buckets_[k].find(key(hash, k));
            if (it == buckets_[k].end()) continue;
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
    }

    void insert(uint32_t image_index, uint64_t hash) {
        for (int k = 0; k < bands_; ++k) {
            buckets_[k][key(hash, k)].push_back(image_index);
        }
    }

   private:
    uint64_t key(uint64_t hash, int k) const { return (hash >> shifts_[k]) & masks_[k]; }

    int bands_;
    std::vector<int> shifts_;
    std::vector<uint64_t> masks_;
    std::vector<std::unordered_map<uint64_t, std::vector<uint32_t>>> buckets_;
};

}  // namespace

// Above this threshold the banded index is slower than the all-pairs scan, so
// find_near_duplicates hands off to find_near_duplicates_bruteforce.
//
// Do not remove this cutoff. The index splits 64 bits into threshold + 1 bands,
// so a rising threshold narrows every band and each one matches more of the
// corpus. Past the crossover the candidate lists approach the whole collection
// and the per-image sort/unique is pure overhead on top of the work the nested
// loop was already doing. Measured at n=5000, n=20000 and n=40000: on random
// hashes threshold 8 runs at 0.42x of brute force, threshold 10 at 1.3x,
// threshold 32 at 32x and threshold 64 at 143x. Clustered corpora cross over
// sooner — threshold 7 is 0.90x but threshold 8 is already 1.3x — so 7 is the
// last value that wins on both shapes. The cutoff also caps index memory, which
// grows with threshold + 1 entries per image.
constexpr int kBandedIndexMaxThreshold = 7;

// Also fall back when the index yields more than n/6 candidates per image.
//
// The threshold cutoff alone is not enough: a collection of few distinct scenes
// produces huge candidate lists even at the default threshold of 5, because most
// of the corpus shares a band. Each candidate costs a distance check plus a
// share of the per-image sort/unique, so past some rate the index does more work
// than the nested loop it replaced.
//
// The rate, not the threshold, is what decides it. Measured over cluster counts
// 4/8/12/20/40/200 at n=20000 and n=40000, banded/brute ratio against candidates
// per image: everything at n/candidates >= 7.0 wins (0.83x-0.99x), everything at
// <= 4.4 loses (1.33x-2.27x), and the break-even sits near n/6.3. n/6 clears
// every measured regression while giving up none of the measured wins; n/2 (the
// previous value) missed nine of them, the worst at 2.27x.
constexpr size_t kMaxCandidatesPerImageDivisor = 6;

std::vector<DuplicateGroup> find_near_duplicates(const std::vector<ImageInfo>& all_images,
                                                 int threshold,
                                                 const std::set<std::string>& exact_sha256s) {
    if (threshold > kBandedIndexMaxThreshold) {
        return find_near_duplicates_bruteforce(all_images, threshold, exact_sha256s);
    }

    std::vector<DuplicateGroup> groups;

    // Union-find to group near duplicates
    std::vector<int> parent(all_images.size());
    for (size_t i = 0; i < parent.size(); i++) parent[i] = i;

    std::function<int(int)> find = [&](int x) -> int {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[a] = b;
    };

    // Candidate generation is the only thing that changes: the banded index
    // replaces the all-pairs scan.
    //
    // The iteration deliberately mirrors the nested loop exactly — outer i,
    // inner j > i, ascending, calling unite(i, j) on every matching pair
    // including pairs already in the same set. That matters: union-find roots
    // depend on the order and multiplicity of unite() calls, group_map below is
    // keyed by root, and that key picks the order groups are printed in. Doing
    // fewer or differently-ordered unites still yields the same partition but
    // can reorder `phig duplicates` output.
    BandedIndex index(threshold, all_images.size());
    for (size_t i = 0; i < all_images.size(); i++) {
        index.insert(static_cast<uint32_t>(i), all_images[i].phash);
    }

    // Sample before committing to the index. If candidate lists are already too
    // long the index cannot pay for itself, so bail out before doing the work
    // rather than after.
    //
    // The sample is strided across the whole corpus, not taken from the front.
    // Collections are commonly ordered — a newest-first import puts recent
    // varied shots ahead of an older tight burst — and a prefix of such an input
    // is not representative: measured on a corpus whose first 64 images are
    // random and whose remainder is near-identical, a prefix sample reported 69
    // candidates per image against a true 14495, missed the fallback entirely
    // and ran 4.4x slower than brute force. A stride sees both regions and is
    // accurate to within ~1.5% on that shape.
    {
        const size_t n = all_images.size();
        const size_t sample = std::min<size_t>(n, 256);
        const size_t stride = (sample > 0) ? std::max<size_t>(n / sample, 1) : 1;
        std::vector<uint32_t> probe;

        size_t total = 0;
        for (size_t k = 0; k < sample; k++) {
            const size_t i = (n <= sample) ? k : (k * stride) % n;
            probe.clear();
            index.collect_candidates(all_images[i].phash, probe);
            total += probe.size();
        }

        if (sample > 0 && total > sample * (n / kMaxCandidatesPerImageDivisor)) {
            return find_near_duplicates_bruteforce(all_images, threshold, exact_sha256s);
        }
    }

    std::vector<uint32_t> candidates;
    for (size_t i = 0; i < all_images.size(); i++) {
        const uint64_t hash = all_images[i].phash;
        candidates.clear();
        index.collect_candidates(hash, candidates);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        for (uint32_t j : candidates) {
            if (static_cast<size_t>(j) <= i) continue;
            // The band match is only a filter; confirm the real distance.
            if (hamming_distance(hash, all_images[j].phash) > threshold) continue;
            // Skip pairs that are exact duplicates
            if (all_images[i].sha256 == all_images[j].sha256 &&
                exact_sha256s.count(all_images[i].sha256)) {
                continue;
            }
            unite(static_cast<int>(i), static_cast<int>(j));
        }
    }

    // Collect groups
    std::map<int, std::vector<size_t>> group_map;
    for (size_t i = 0; i < all_images.size(); i++) {
        group_map[find(i)].push_back(i);
    }

    for (const auto& [root, indices] : group_map) {
        if (indices.size() < 2) continue;
        DuplicateGroup g;
        g.type = "near";
        g.distance = threshold;
        for (size_t idx : indices) {
            g.images.push_back(all_images[idx]);
        }
        groups.push_back(std::move(g));
    }

    return groups;
}

// Not dead code: this is the oracle the equivalence tests in
// tests/test_duplicates.cpp check `find_near_duplicates` against, and the only
// independent check that the banded index groups images identically. Keep it.
std::vector<DuplicateGroup> find_near_duplicates_bruteforce(
    const std::vector<ImageInfo>& all_images, int threshold,
    const std::set<std::string>& exact_sha256s) {
    std::vector<DuplicateGroup> groups;

    // Union-find to group near duplicates
    std::vector<int> parent(all_images.size());
    std::vector<int> min_dist(all_images.size(), 0);
    for (size_t i = 0; i < parent.size(); i++) parent[i] = i;

    std::function<int(int)> find = [&](int x) -> int {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[a] = b;
    };

    for (size_t i = 0; i < all_images.size(); i++) {
        for (size_t j = i + 1; j < all_images.size(); j++) {
            // Skip pairs that are exact duplicates
            if (all_images[i].sha256 == all_images[j].sha256 &&
                exact_sha256s.count(all_images[i].sha256)) {
                continue;
            }
            int dist = hamming_distance(all_images[i].phash, all_images[j].phash);
            if (dist <= threshold) {
                unite(i, j);
            }
        }
    }

    // Collect groups
    std::map<int, std::vector<size_t>> group_map;
    for (size_t i = 0; i < all_images.size(); i++) {
        group_map[find(i)].push_back(i);
    }

    for (const auto& [root, indices] : group_map) {
        if (indices.size() < 2) continue;
        DuplicateGroup g;
        g.type = "near";
        g.distance = threshold;
        for (size_t idx : indices) {
            g.images.push_back(all_images[idx]);
        }
        groups.push_back(std::move(g));
    }

    return groups;
}
