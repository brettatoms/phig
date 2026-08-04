#include "search_engine.h"

#include <algorithm>
#include <unordered_map>

std::vector<Database::FaceSearchResult> dedupe_face_matches(
    std::vector<Database::FaceSearchResult> matches) {
    std::unordered_map<std::string, size_t> best;  // path -> index into `out`
    std::vector<Database::FaceSearchResult> out;
    out.reserve(matches.size());

    for (auto& m : matches) {
        auto it = best.find(m.path);
        if (it == best.end()) {
            best.emplace(m.path, out.size());
            out.push_back(std::move(m));
        } else if (m.distance < out[it->second].distance) {
            out[it->second] = std::move(m);
        }
    }

    // Use stable_sort to preserve insertion order for equal distances. Duplicate
    // images and ties from the face embedding model produce multiple matches at
    // identical distances; a stable order ensures consistent output across runs.
    std::stable_sort(out.begin(), out.end(),
                     [](const Database::FaceSearchResult& a, const Database::FaceSearchResult& b) {
                         return a.distance < b.distance;
                     });
    return out;
}
