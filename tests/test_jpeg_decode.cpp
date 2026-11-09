#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <thread>
#include <vector>

#include "jpeg_decode.h"

namespace fs = std::filesystem;

namespace {
fs::path fixture(const std::string& name) {
    return fs::path(__FILE__).parent_path() / "data" / "jpeg" / name;
}
std::vector<unsigned char> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void expect_identical(const cv::Mat& a, const cv::Mat& b) {
    ASSERT_EQ(a.type(), b.type());
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(cv::norm(a, b, cv::NORM_INF), 0.0);
}
// The exif_* fixtures pin which EXIF layouts OpenCV orients; make_fixtures.py
// says what each one exercises.
const char* kDecodable[] = {
    "baseline.jpg",
    "progressive.jpg",
    "grayscale.jpg",
    "cmyk_adobe.jpg",
    "sub422.jpg",
    "sub444.jpg",
    "orient_1.jpg",
    "orient_2.jpg",
    "orient_3.jpg",
    "orient_4.jpg",
    "orient_5.jpg",
    "orient_6.jpg",
    "orient_7.jpg",
    "orient_8.jpg",
    "exif_after_xmp.jpg",
    "exif_intel.jpg",
    "exif_tags_before.jpg",
    "exif_make_out_of_range.jpg",
    "exif_xres_out_of_range.jpg",
    "exif_white_point_out_of_range.jpg",
    "exif_chromaticities_out_of_range.jpg",
    "exif_count_overrun.jpg",
    "exif_two_markers.jpg",
    "exif_second_marker_orients.jpg",
};
}  // namespace

TEST(JpegDecode, FileMatchesImreadOnEveryFixture) {
    for (const char* name : kDecodable) {
        SCOPED_TRACE(name);
        DecodeResult r = decode_jpeg_file(fixture(name).string());
        expect_identical(r.img, cv::imread(fixture(name).string(), cv::IMREAD_COLOR));
        EXPECT_EQ(r.frame.width, r.img.cols);
        EXPECT_EQ(r.frame.height, r.img.rows);
    }
}

TEST(JpegDecode, MemoryMatchesImdecodeOnEveryFixture) {
    for (const char* name : kDecodable) {
        SCOPED_TRACE(name);
        std::vector<unsigned char> bytes = read_all(fixture(name));
        DecodeResult r = decode_jpeg_memory(bytes);
        expect_identical(r.img, cv::imdecode(bytes, cv::IMREAD_COLOR));
        EXPECT_EQ(r.frame.width, r.img.cols);
        EXPECT_EQ(r.frame.height, r.img.rows);
    }
}

TEST(JpegDecode, TruncatedFileGivesImreadsPartialImageAndTheMessage) {
    std::string p = fixture("truncated.jpg").string();
    DecodeResult r = decode_jpeg_file(p);
    expect_identical(r.img, cv::imread(p, cv::IMREAD_COLOR));
    EXPECT_NE(r.warning.find("Premature end of JPEG file"), std::string::npos) << r.warning;
}

TEST(JpegDecode, TruncatedBytesFailWithAnEmptyImageAndTheMessage) {
    DecodeResult r = decode_jpeg_memory(read_all(fixture("truncated.jpg")));
    EXPECT_TRUE(r.img.empty());
    EXPECT_FALSE(r.warning.empty());
}

TEST(JpegDecode, ANonJpegIsNotClaimed) {
    std::vector<unsigned char> head = read_all(fixture("png_named.jpg"));
    EXPECT_FALSE(is_jpeg_signature(head));
    EXPECT_TRUE(is_jpeg_signature(read_all(fixture("baseline.jpg"))));
}

TEST(JpegDecode, GarbageFailsWithAnEmptyImageAndAMessage) {
    std::vector<unsigned char> junk = {0xFF, 0xD8, 0xFF, 0x00, 0x01, 0x02};
    DecodeResult r = decode_jpeg_memory(junk);
    EXPECT_TRUE(r.img.empty());
    EXPECT_FALSE(r.warning.empty());
}

TEST(JpegDecode, ConcurrentDecodesKeepTheirOwnMessages) {
    std::string bad = fixture("truncated.jpg").string();
    std::string good = fixture("baseline.jpg").string();
    std::vector<std::string> bad_warnings(200), good_warnings(200);
    std::thread a([&] {
        for (auto& w : bad_warnings) w = decode_jpeg_file(bad).warning;
    });
    std::thread b([&] {
        for (auto& w : good_warnings) w = decode_jpeg_file(good).warning;
    });
    a.join();
    b.join();
    for (const auto& w : bad_warnings) EXPECT_FALSE(w.empty());
    for (const auto& w : good_warnings) EXPECT_TRUE(w.empty()) << w;
}
