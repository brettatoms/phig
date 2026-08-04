#include "search_engine.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>

#include "duplicates.h"
#include "selection.h"

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

SearchEngine::SearchEngine(const SearchQuery& query, Database& db) : query_(query), db_(db) {}

SearchResult SearchEngine::run() {
    SearchResult result;
    const bool selecting = !query_.matches.empty() || !query_.excludes.empty();

    switch (query_.mode) {
        case SearchMode::Person:
            // Neither path selection nor the limit applies here, mirroring the
            // CLI: the person branch returns directly without reaching the
            // shared match/filter/limit block.
            result.paths = db_.get_images_for_person(query_.person);
            return result;

        case SearchMode::Similar: {
            struct Scored {
                ImageInfo info;
                int distance;
            };
            std::vector<Scored> scored;
            for (auto& img : db_.get_all_images()) {
                int dist = hamming_distance(query_.phash, img.phash);
                if (dist <= query_.phash_threshold) scored.push_back({std::move(img), dist});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const Scored& a, const Scored& b) { return a.distance < b.distance; });
            for (auto& s : scored) result.images.push_back(std::move(s.info));
            break;
        }

        case SearchMode::Face: {
            // One KNN query per detected query face, then collapse to one entry
            // per path keeping the closest distance. `search_faces` sorts within
            // a single call, so the runs must be merged rather than concatenated.
            const int per_face_limit = query_.limit > 0 ? query_.limit : 100;
            std::vector<Database::FaceSearchResult> all;
            for (const auto& embedding : query_.face_embeddings) {
                auto batch = db_.search_faces(embedding, query_.face_threshold, per_face_limit);
                all.insert(all.end(), std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            }
            result.matches = dedupe_face_matches(std::move(all));
            if (query_.limit > 0 && static_cast<int>(result.matches.size()) > query_.limit) {
                result.matches.resize(query_.limit);
            }
            return result;
        }

        case SearchMode::Filter: {
            Database::SearchCriteria criteria = query_.filter;
            // Only push the limit into SQL when no path selection is pending:
            // truncating before selection would return fewer rows than asked for.
            criteria.limit = selecting ? -1 : query_.limit;
            result.images = db_.search(criteria);
            break;
        }
    }

    if (selecting) {
        result.images.erase(std::remove_if(result.images.begin(), result.images.end(),
                                           [&](const ImageInfo& img) {
                                               return !passes_selection(img.path, query_.matches,
                                                                        query_.excludes);
                                           }),
                            result.images.end());
    }
    if (query_.limit > 0 && static_cast<int>(result.images.size()) > query_.limit) {
        result.images.resize(query_.limit);
    }
    return result;
}
