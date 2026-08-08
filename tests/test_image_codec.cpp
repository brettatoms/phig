#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "image_codec.h"
#include "image_codec_internal.h"
#include "scanner.h"

namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name) {
    return (fs::path(__FILE__).parent_path() / "data" / name).string();
}

// Writes a file that is named like a container image but is not one, for the
// decoders' failure paths.
std::string write_garbage(const std::string& name) {
    std::string path = (fs::path(testing::TempDir()) / name).string();
    std::ofstream f(path, std::ios::binary);
    f << "not a container image at all";
    return path;
}

}  // namespace

TEST(ImageCodecExtension, MatchesModernContainerFormats) {
    EXPECT_TRUE(is_codec_extension("heic"));
    EXPECT_TRUE(is_codec_extension("heif"));
    EXPECT_TRUE(is_codec_extension("avif"));
}

TEST(ImageCodecExtension, IsCaseInsensitiveAndDotTolerant) {
    EXPECT_TRUE(is_codec_extension("HEIC"));
    EXPECT_TRUE(is_codec_extension(".heic"));
    EXPECT_TRUE(is_codec_extension(".HEIC"));
}

TEST(ImageCodecExtension, RejectsFormatsOpenCVAlreadyHandles) {
    EXPECT_FALSE(is_codec_extension("jpg"));
    EXPECT_FALSE(is_codec_extension("png"));
    EXPECT_FALSE(is_codec_extension("cr2"));
    EXPECT_FALSE(is_codec_extension(""));
}

// Formats phig advertised but links no codec for. Scan now skips them the way
// it already skips .tga, instead of reporting one decode error per file.
TEST(KnownExtensions, DropsFormatsNothingCanDecode) {
    EXPECT_FALSE(is_known_image_extension("psd"));
    EXPECT_FALSE(is_known_image_extension("svg"));
    EXPECT_FALSE(is_known_image_extension("xcf"));
    EXPECT_FALSE(is_known_image_extension("ico"));
}

TEST(KnownExtensions, StillAcceptsWhatOpenCVDecodes) {
    EXPECT_TRUE(is_known_image_extension("jpg"));
    EXPECT_TRUE(is_known_image_extension("png"));
    EXPECT_TRUE(is_known_image_extension("tiff"));
    EXPECT_TRUE(is_known_image_extension("jp2"));
    EXPECT_TRUE(is_known_image_extension("heic"));
}

TEST(LibheifDecode, DecodesHeicToThreeChannelBgr) {
    cv::Mat img = codec_decode_libheif(fixture("orient_1.heic"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols, 200);
    EXPECT_EQ(img.rows, 100);
    EXPECT_EQ(img.channels(), 3);
    EXPECT_EQ(img.type(), CV_8UC3);
}

TEST(LibheifDecode, DecodesAvif) {
    cv::Mat img = codec_decode_libheif(fixture("sample.avif"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols, 200);
    EXPECT_EQ(img.rows, 100);
}

// The fixture is a red field with a blue 20x20 block at top-left. Checking the
// marker's position proves channel order and row order, which dimensions alone
// would not.
TEST(LibheifDecode, ProducesBgrChannelOrderAndTopLeftOrigin) {
    cv::Mat img = codec_decode_libheif(fixture("orient_1.heic"));
    ASSERT_FALSE(img.empty());
    cv::Vec3b marker = img.at<cv::Vec3b>(5, 5);
    cv::Vec3b field = img.at<cv::Vec3b>(50, 150);
    EXPECT_GT(marker[0], 200) << "blue channel high inside the marker";
    EXPECT_LT(marker[2], 60) << "red channel low inside the marker";
    EXPECT_LT(field[0], 60) << "blue channel low on the red field";
    EXPECT_GT(field[2], 200) << "red channel high on the red field";
}

// libheif applies the container's rotation itself, which is what makes it match
// cv::imread's behaviour for JPEG without any orientation code of our own.
TEST(LibheifDecode, AppliesContainerRotation) {
    cv::Mat upright = codec_decode_libheif(fixture("orient_1.heic"));
    cv::Mat rotated = codec_decode_libheif(fixture("orient_6.heic"));
    ASSERT_FALSE(upright.empty());
    ASSERT_FALSE(rotated.empty());
    EXPECT_EQ(upright.cols, 200);
    EXPECT_EQ(upright.rows, 100);
    EXPECT_EQ(rotated.cols, 100);
    EXPECT_EQ(rotated.rows, 200);
}

TEST(LibheifDecode, ReturnsEmptyMatOnGarbageAndDoesNotThrow) {
    std::string path = write_garbage("garbage_heif.heic");
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_libheif(path); });
    EXPECT_TRUE(img.empty());
    fs::remove(path);
}

TEST(LibheifDecode, ReturnsEmptyMatForMissingFile) {
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_libheif("/nonexistent/file.heic"); });
    EXPECT_TRUE(img.empty());
}
