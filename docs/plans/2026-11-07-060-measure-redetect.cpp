// Measures how closely a fresh face detection reproduces the faces a collection
// already stores, to confirm kLabelCarryMinOverlap from data rather than choose
// it. Treats every stored face as labelled, so "carried" is the share of faces
// a rebuild would keep a label on.
//
// Read-only: opens the database with SQLITE_OPEN_READONLY and never constructs
// phig's Database, which opens read-write and migrates.
//
// Build by adding to the root CMakeLists.txt, temporarily:
//   add_executable(measure-redetect docs/plans/2026-11-07-060-measure-redetect.cpp)
//   target_link_libraries(measure-redetect PRIVATE phig-core)
// Run: ./build/measure-redetect <db> <stride>
#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "faces.h"
#include "hasher.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <db> <stride>\n", argv[0]);
        return 2;
    }
    const size_t stride = static_cast<size_t>(std::max(1, std::atoi(argv[2])));

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    std::vector<std::pair<int64_t, std::string>> images;
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT DISTINCT i.id, i.path FROM images i "
                       "JOIN faces f ON f.image_id = i.id ORDER BY i.id",
                       -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        images.emplace_back(sqlite3_column_int64(s, 0),
                            reinterpret_cast<const char*>(sqlite3_column_text(s, 1)));
    }
    sqlite3_finalize(s);

    FaceDetector detector;
    size_t measured = 0, unreadable = 0, count_changed = 0, stored_total = 0, carried = 0;
    std::vector<float> best;

    for (size_t k = 0; k < images.size(); k += stride) {
        const auto& [id, path] = images[k];

        std::vector<LabelledFace> stored;
        sqlite3_prepare_v2(db,
                           "SELECT x, y, width, height FROM faces WHERE image_id = ? ORDER BY id",
                           -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, id);
        while (sqlite3_step(s) == SQLITE_ROW) {
            LabelledFace lf{};
            lf.face.x = sqlite3_column_int(s, 0);
            lf.face.y = sqlite3_column_int(s, 1);
            lf.face.width = sqlite3_column_int(s, 2);
            lf.face.height = sqlite3_column_int(s, 3);
            lf.person_id = 1;
            stored.push_back(lf);
        }
        sqlite3_finalize(s);

        DecodeResult decoded = decode_image_with_warning(path);
        if (decoded.img.empty()) {
            ++unreadable;
            continue;
        }
        std::vector<FaceInfo> fresh = detector.detect(decoded.img);
        ++measured;
        if (fresh.size() != stored.size()) ++count_changed;
        stored_total += stored.size();

        for (const auto& sf : stored) {
            float b = 0.0f;
            for (const auto& f : fresh) b = std::max(b, box_overlap(sf.face, f));
            best.push_back(b);
        }
        carried += stored.size() - carry_labels(stored, fresh).dropped;
    }
    sqlite3_close(db);

    std::sort(best.begin(), best.end());
    auto pct = [&](double p) {
        if (best.empty()) return 0.0f;
        size_t i = std::min(best.size() - 1, static_cast<size_t>(p * best.size()));
        return best[i];
    };
    auto at_least = [&](float t) {
        return static_cast<size_t>(best.end() - std::lower_bound(best.begin(), best.end(), t));
    };

    std::printf("images with faces: %zu, measured: %zu, unreadable: %zu\n", images.size(),
                measured, unreadable);
    std::printf("images whose face count changed: %zu\n", count_changed);
    std::printf("stored faces: %zu, carried at %.2f: %zu (%.2f%%)\n", stored_total,
                kLabelCarryMinOverlap, carried,
                stored_total ? 100.0 * carried / stored_total : 0.0);
    std::printf("best overlap per stored face: p0.1 %.3f  p1 %.3f  p5 %.3f  p50 %.3f\n",
                pct(0.001), pct(0.01), pct(0.05), pct(0.5));
    std::printf("  == 1.0: %zu\n", at_least(1.0f));
    for (float t : {0.9f, 0.7f, 0.5f, 0.3f, 0.0f}) {
        std::printf("  >= %.1f: %zu\n", t, at_least(t));
    }
    return 0;
}
