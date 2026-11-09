#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "dates.h"
#include "face_path.h"
#include "faces_engine.h"
#include "hasher.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {
// Alternating face fixtures at two sizes, as in test_faces_engine.cpp: a
// same-size set would let the detector skip reconfiguring between images.
struct PerfFaces {
    fs::path dir;
    std::unique_ptr<Database> db;
    std::vector<fs::path> files;

    explicit PerfFaces(int n) {
        dir = fs::temp_directory_path() / ("phig_faces_perf_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        db = std::make_unique<Database>((dir / "phig.db").string());
        db->init_schema();
        for (int i = 0; i < n; ++i) {
            const char* src =
                (i % 2 == 0) ? "tests/data/faces_small.jpg" : "tests/data/faces_large.jpg";
            fs::path p = dir / ("f" + std::to_string(i) + ".jpg");
            fs::copy_file(src, p);
            files.push_back(p);
            ImageInfo info;
            info.path = p.string();
            info.filename = p.filename().string();
            info.extension = "jpg";
            info.file_size = fs::file_size(p);
            info.media_type = "image";
            info.sha256 = compute_sha256(p.string());
            info.created_at = "2024-01-01T00:00:00Z";
            info.modified_at = format_time(fs::last_write_time(p));
            db->insert(info);
        }
    }
    ~PerfFaces() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::vector<int64_t> all_ids() const {
        Database::PageCriteria pc;
        pc.limit = -1;
        return db->ids_matching(pc);
    }
    // Names every face in the table as one person.
    size_t name_every_face() {
        int64_t person = db->get_or_create_person("perf person");
        std::vector<int64_t> ids;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(db->db_ptr(), "SELECT id FROM faces", -1, &s, nullptr);
        while (sqlite3_step(s) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(s, 0));
        sqlite3_finalize(s);
        for (int64_t id : ids) db->set_face_person(id, person);
        return ids.size();
    }
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
}  // namespace

// Compares a rebuild whose files match their recorded rows with one whose files
// were re-encoded since, so the frame differs. Re-encoding keeps the dimensions
// and alternates the JPEG quality, so every changed run decodes the same pixel
// count and sees new bytes. Within one recognition model labels carry by
// embedding either way; the ratio bounds the cost of recording and comparing
// provenance plus re-decoding.
TEST(FacesPerf, AFrameChangeDoesNotSlowARebuild) {
    if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;
    PerfFaces coll(40);
    FacesRebuildOptions opts;
    opts.ids = coll.all_ids();
    CancelToken tok;

    FacesResult scan = FacesEngine(opts, *coll.db).run(tok, nullptr);
    ASSERT_GT(scan.faces_detected, 0u);
    size_t named = coll.name_every_face();
    ASSERT_GT(named, 0u);

    auto time_rebuild = [&]() {
        auto t0 = std::chrono::steady_clock::now();
        FacesResult r = FacesEngine(opts, *coll.db).run(tok, nullptr);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
        EXPECT_EQ(r.labels_dropped, 0u);
        EXPECT_EQ(r.errors, 0u);
        return ms;
    };

    std::vector<double> unchanged, changed;
    for (int i = 0; i < 3; ++i) unchanged.push_back(time_rebuild());

    std::vector<cv::Mat> decoded;
    for (const auto& p : coll.files) {
        decoded.push_back(cv::imread(p.string()));
        ASSERT_FALSE(decoded.back().empty());
    }
    for (int i = 0; i < 3; ++i) {
        int quality = (i % 2 == 0) ? 85 : 95;
        for (size_t f = 0; f < coll.files.size(); ++f) {
            ASSERT_TRUE(cv::imwrite(coll.files[f].string(), decoded[f],
                                    {cv::IMWRITE_JPEG_QUALITY, quality}));
        }
        changed.push_back(time_rebuild());
    }

    double u = median(unchanged), c = median(changed);
    std::cout << "unchanged median " << u << " ms, changed median " << c << " ms\n";
    EXPECT_LE(c, 1.15 * u);
}
