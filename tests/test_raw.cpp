#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <filesystem>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "hasher.h"
#include "raw.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

class RawDecodeTest : public ::testing::Test {
   protected:
    std::string test_dir;

    void SetUp() override { test_dir = make_temp_dir("raw"); }

    void TearDown() override { fs::remove_all(test_dir); }

    std::string create_image(const std::string& name, int width, int height, cv::Scalar color) {
        auto path = (fs::path(test_dir) / name).string();
        cv::Mat img(height, width, CV_8UC3, color);
        cv::imwrite(path, img);
        return path;
    }
};

// Tier 1: a real sidecar JPEG beside the RAW is used directly (no LibRaw
// decode of the CR2 itself — the CR2 here is a truncated fixture that LibRaw
// cannot decode, proving tier 1 never touches it).
TEST_F(RawDecodeTest, UsesSidecarJpegWhenPresent) {
    create_image("IMG_1.JPG", 64, 48, cv::Scalar(0, 128, 255));

    fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
    fs::path raw_path = fs::path(test_dir) / "IMG_1.CR2";
    fs::copy_file(fixture, raw_path);

    cv::Mat result = raw_decode(raw_path.string());

    ASSERT_FALSE(result.empty());
    EXPECT_EQ(result.cols, 64);
    EXPECT_EQ(result.rows, 48);
}

// No sidecar, undecodable RAW -> empty Mat, no crash (graceful failure path).
// The truncated fixture rejects LibRaw's own unpack/decode (verified with
// dcraw_emu/identify), so this only exercises tiers 2+3 failing gracefully;
// the LibRaw-success path (valid CR2 -> preview or full decode) has no
// committable fixture and is covered by real use and the Task 8 e2e instead.
TEST_F(RawDecodeTest, TruncatedRawWithNoSidecarReturnsEmpty) {
    fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
    fs::path raw_path = fs::path(test_dir) / "truncated.cr2";
    fs::copy_file(fixture, raw_path);

    cv::Mat result;
    EXPECT_NO_THROW(result = raw_decode(raw_path.string()));
    EXPECT_TRUE(result.empty());
}

TEST_F(RawDecodeTest, NonExistentFileReturnsEmpty) {
    cv::Mat result;
    EXPECT_NO_THROW(result = raw_decode((fs::path(test_dir) / "missing.cr2").string()));
    EXPECT_TRUE(result.empty());
}

// raw_decode must not put LibRaw on the stack: sizeof(LibRaw) is ~750 KB and
// macOS gives secondary threads a 512 KB stack by default, so a stack-local
// instance overflows before decoding anything. This crashed
// HasherTest.DecodeWarningNotMisattributedAcrossThreads with a bus error on
// macOS CI while passing everywhere on Linux, whose threads get 8 MB.
//
// Asserting on the object's size rather than trying to overflow a stack: a
// thread with a deliberately small stack would crash the whole test binary
// rather than fail, which is exactly the failure mode being prevented.
TEST(RawStack, LibRawIsTooLargeToLiveOnAThreadStack) {
    // If this ever shrinks below a thread stack, the heap allocation in
    // raw_decode is merely unnecessary rather than load-bearing — but it is
    // currently ~750 KB against macOS's 512 KB, so it is load-bearing.
    EXPECT_GT(sizeof(LibRaw), 512u * 1024u)
        << "sizeof(LibRaw) = " << sizeof(LibRaw)
        << "; if this is now small, revisit the comment in raw_decode";
}

TEST_F(RawDecodeTest, ReportsTheSidecarFrameAndHashesItOnlyWhenAsked) {
    std::string jpg = create_image("IMG_2.JPG", 64, 48, cv::Scalar(10, 20, 30));
    fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
    fs::path raw_path = fs::path(test_dir) / "IMG_2.CR2";
    fs::copy_file(fixture, raw_path);

    DecodeResult plain = raw_decode_result(raw_path.string());
    EXPECT_EQ(plain.frame.source, DecodeSource::SidecarJpeg);
    EXPECT_EQ(plain.frame.width, 64);
    EXPECT_EQ(plain.frame.height, 48);
    EXPECT_TRUE(plain.frame.source_sha256.empty());

    DecodeResult hashed = raw_decode_result(raw_path.string(), /*hash_source=*/true);
    EXPECT_EQ(hashed.frame.source_sha256, compute_sha256(jpg));
    EXPECT_EQ(cv::norm(hashed.img, cv::imread(jpg, cv::IMREAD_COLOR), cv::NORM_INF), 0.0);
}

// A sidecar decodes to what cv::imread gave for the file: a truncated one to
// its partial image, an oriented one with the orientation applied.
TEST_F(RawDecodeTest, SidecarPixelsMatchImreadForTruncatedAndOrientedFiles) {
    fs::path data = fs::path(__FILE__).parent_path() / "data";
    for (const char* name : {"truncated.jpg", "orient_6.jpg"}) {
        for (bool hash_source : {false, true}) {
            SCOPED_TRACE(std::string(name) + (hash_source ? " hashed" : " plain"));
            fs::path dir = fs::path(test_dir) / (std::string(name) + (hash_source ? "h" : "p"));
            fs::create_directories(dir);
            fs::path sidecar = dir / "IMG_3.JPG";
            fs::copy_file(data / "jpeg" / name, sidecar);
            fs::path raw_path = dir / "IMG_3.CR2";
            fs::copy_file(data / "truncated_ojpeg.cr2", raw_path);

            cv::Mat expected = cv::imread(sidecar.string(), cv::IMREAD_COLOR);
            ASSERT_FALSE(expected.empty());
            DecodeResult r = raw_decode_result(raw_path.string(), hash_source);
            ASSERT_FALSE(r.img.empty());
            EXPECT_EQ(r.frame.source, DecodeSource::SidecarJpeg);
            ASSERT_EQ(r.img.size(), expected.size());
            EXPECT_EQ(cv::norm(r.img, expected, cv::NORM_INF), 0.0);
            if (hash_source) EXPECT_EQ(r.frame.source_sha256, compute_sha256(sidecar.string()));
        }
    }
}
