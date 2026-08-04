#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "database.h"
#include "types.h"

// Which query to run. Exactly one mode is active per call.
enum class SearchMode { Filter, Person, Face, Similar };

struct SearchQuery {
    SearchMode mode = SearchMode::Filter;

    // Reused rather than restated: Database::SearchCriteria already declares
    // every filter field the CLI populates. A parallel copy here would be a
    // second struct to keep in sync.
    //
    // `filter.limit` is the SQL-level limit and is only set when no path
    // selection is pending — see `limit` below.
    Database::SearchCriteria filter;

    // Path selection and the final limit apply to Filter and Similar only, so
    // they sit outside `filter`. `limit` is applied after path selection;
    // pushing it into SQL while a selection is pending would truncate before
    // filtering and return fewer rows than asked for. Person mode applies
    // neither. Face mode has its own per-face limit and no selection.
    std::vector<std::string> matches, excludes;
    int limit = -1;

    // Person mode.
    std::string person;

    // Face mode: embeddings the CALLER extracted from the query image. The
    // engine neither decodes nor detects — it holds no OpenCV dependency at all,
    // so a malformed query file cannot crash it and a GUI caller chooses its own
    // decode isolation.
    std::vector<std::array<float, 128>> face_embeddings;
    float face_threshold = 0.6f;

    // Similar mode: a phash the caller computed from a file or parsed from a hex
    // argument.
    uint64_t phash = 0;
    int phash_threshold = 5;
};

// Which vector is populated depends on the mode. One result type carries all
// three rather than three near-identical types.
//
// Person mode yields bare paths because Database::get_images_for_person returns
// std::vector<std::string>; widening it to ImageInfo would change what
// --format json emits for --person, which is a user-visible change unrelated to
// moving this code.
struct SearchResult {
    std::vector<ImageInfo> images;                    // Filter and Similar
    std::vector<Database::FaceSearchResult> matches;  // Face
    std::vector<std::string> paths;                   // Person
};

// Collapses face matches to one entry per path, keeping the SMALLEST distance,
// then sorts ascending by distance.
//
// Matches arrive as one sorted run per query face, so a path can appear in
// several runs at different distances. Deduplicating on first-seen would rank an
// image by whichever query face happened to be detected first rather than by its
// best match, and under a limit could drop it entirely.
//
// Free function, not a private method, so the rule is testable without a
// database or face models.
std::vector<Database::FaceSearchResult> dedupe_face_matches(
    std::vector<Database::FaceSearchResult> matches);

// Runs one query against a database. Reads only: no writes, no transaction, no
// mutex. Single-threaded and single-shot, so it takes no CancelToken and
// publishes no progress.
class SearchEngine {
   public:
    SearchEngine(const SearchQuery& query, Database& db);

    SearchResult run();

   private:
    // By value: constructing from a temporary would otherwise dangle.
    SearchQuery query_;
    Database& db_;
};
