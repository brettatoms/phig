#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "duplicates.h"
#include "exif.h"
#include "hasher.h"
#include "image_codec.h"
#include "image_codec_internal.h"
#include "scanner.h"

namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name) {
    return (fs::path(__FILE__).parent_path() / "data" / name).string();
}

// Removes one "Key":"Value" pair, and its adjoining comma, from a flat JSON
// object string. Adequate for extract_exif_json's output, which never nests
// and never emits a quote inside a value.
std::string erase_json_key(std::string json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    size_t start = json.find(needle);
    if (start == std::string::npos) return json;
    size_t end = json.find('"', start + needle.size());
    if (end == std::string::npos) return json;
    ++end;  // past the value's closing quote
    if (end < json.size() && json[end] == ',') {
        ++end;
    } else if (start > 0 && json[start - 1] == ',') {
        --start;
    }
    json.erase(start, end - start);
    return json;
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

TEST(ImageCodecExtension, MatchesJpegXl) { EXPECT_TRUE(is_codec_extension("jxl")); }

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

// libjxl is the JXL decoder on Linux, and is compiled on macOS too so this
// path is testable here rather than only in CI -- the same arrangement as
// libheif. libheif does not handle JXL at all, so it gets its own backend.
TEST(LibjxlDecode, DecodesJxlToThreeChannelBgr) {
    cv::Mat img = codec_decode_libjxl(fixture("sample.jxl"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols, 200);
    EXPECT_EQ(img.rows, 100);
    EXPECT_EQ(img.type(), CV_8UC3);
}

TEST(LibjxlDecode, ProducesBgrChannelOrderAndTopLeftOrigin) {
    cv::Mat img = codec_decode_libjxl(fixture("sample.jxl"));
    ASSERT_FALSE(img.empty());
    cv::Vec3b marker = img.at<cv::Vec3b>(5, 5);
    cv::Vec3b field = img.at<cv::Vec3b>(50, 150);
    EXPECT_GT(marker[0], 200) << "blue channel high inside the marker";
    EXPECT_LT(marker[2], 60) << "red channel low inside the marker";
    EXPECT_LT(field[0], 60) << "blue channel low on the red field";
    EXPECT_GT(field[2], 200) << "red channel high on the red field";
}

TEST(LibjxlDecode, ReturnsEmptyMatOnGarbageAndDoesNotThrow) {
    std::string path = write_garbage("garbage_jxl.jxl");
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_libjxl(path); });
    EXPECT_TRUE(img.empty());
    fs::remove(path);
}

// Whichever backend the platform picks, the scan path must handle JXL.
TEST(CodecDecodeIntegration, DecodeImageHandlesJxl) {
    DecodeResult result = decode_image_with_warning(fixture("sample.jxl"));
    EXPECT_FALSE(result.img.empty());
    EXPECT_TRUE(result.warning.empty()) << result.warning;
    EXPECT_EQ(result.img.cols, 200);
}

// Same principle as HEIC: pixels without metadata would leave the format worse
// than unsupported, since the photo would file under its copy date.
TEST(CodecExif, JxlCarriesTheOriginalCaptureDate) {
    std::string json = extract_exif_json(fixture("sample.jxl"));
    EXPECT_NE(json.find("2026:08:08 14:32:11"), std::string::npos) << json;
}

// exif_sample.heic and exif_sample.jpg were written from one source with the
// same tag values, so a difference between the two extractions is a defect in
// the HEIC path rather than a difference in the files. This is the test that
// catches a future platform-specific metadata shortcut producing different
// JSON keys on macOS and Linux -- which would silently change json_extract()
// results for a database moved between them.
//
// The two dimension tags are excluded, and only those: HEIF records dimensions
// in the container's `ispe` box rather than duplicating them into EXIF, so
// ImageIO writes PixelXDimension/PixelYDimension for the JPEG and not for the
// HEIC. phig takes dimensions from the decoded image, never from EXIF.
TEST(CodecExif, HeicMetadataMatchesJpegControl) {
    const std::string from_heic = extract_exif_json(fixture("exif_sample.heic"));
    std::string from_jpeg = extract_exif_json(fixture("exif_sample.jpg"));
    ASSERT_NE(from_heic, "{}") << "no EXIF extracted from the HEIC at all";

    from_jpeg = erase_json_key(from_jpeg, "PixelXDimension");
    from_jpeg = erase_json_key(from_jpeg, "PixelYDimension");
    EXPECT_EQ(from_heic, from_jpeg);
}

// The date is the reason this matters: without it a HEIC files under its copy
// date rather than when it was taken, breaking organize and date sort.
TEST(CodecExif, HeicCarriesTheOriginalCaptureDate) {
    std::string json = extract_exif_json(fixture("exif_sample.heic"));
    EXPECT_NE(json.find("2026:08:08 14:32:11"), std::string::npos) << json;
}

TEST(CodecExif, ExtractsBlobFromHeicContainer) {
    std::vector<unsigned char> blob = codec_exif_blob(fixture("exif_sample.heic"));
    EXPECT_FALSE(blob.empty());
}

// The blob must start at the TIFF header for exif_data_load_data to parse it.
TEST(CodecExif, BlobStartsAtTheTiffHeader) {
    std::vector<unsigned char> blob = codec_exif_blob(fixture("exif_sample.heic"));
    ASSERT_GE(blob.size(), 4u);
    const bool little = blob[0] == 'I' && blob[1] == 'I' && blob[2] == 42 && blob[3] == 0;
    const bool big = blob[0] == 'M' && blob[1] == 'M' && blob[2] == 0 && blob[3] == 42;
    EXPECT_TRUE(little || big) << "blob does not begin with a TIFF byte-order marker";
}

TEST(CodecExif, ReturnsEmptyBlobForGarbageAndDoesNotThrow) {
    std::string path = write_garbage("garbage_exif.heic");
    std::vector<unsigned char> blob;
    EXPECT_NO_THROW({ blob = codec_exif_blob(path); });
    EXPECT_TRUE(blob.empty());
    fs::remove(path);
}

TEST(CodecExif, ReturnsEmptyBlobForMissingFile) {
    std::vector<unsigned char> blob;
    EXPECT_NO_THROW({ blob = codec_exif_blob("/nonexistent/file.heic"); });
    EXPECT_TRUE(blob.empty());
}

// The dispatch in decode_image_with_warning: whichever backend the platform
// selects, a HEIC must come out of the scan path decoded and upright.
TEST(CodecDecodeIntegration, DecodeImageHandlesHeic) {
    DecodeResult result = decode_image_with_warning(fixture("orient_1.heic"));
    EXPECT_FALSE(result.img.empty());
    EXPECT_TRUE(result.warning.empty()) << result.warning;
    EXPECT_EQ(result.img.cols, 200);
    EXPECT_EQ(result.img.rows, 100);
}

TEST(CodecDecodeIntegration, DecodeImageHandlesAvif) {
    DecodeResult result = decode_image_with_warning(fixture("sample.avif"));
    EXPECT_FALSE(result.img.empty());
    EXPECT_TRUE(result.warning.empty()) << result.warning;
}

TEST(CodecDecodeIntegration, HeicOrientationIsAppliedOnTheScanPath) {
    DecodeResult result = decode_image_with_warning(fixture("orient_6.heic"));
    ASSERT_FALSE(result.img.empty());
    EXPECT_EQ(result.img.cols, 100) << "orientation was not applied on the scan path";
    EXPECT_EQ(result.img.rows, 200);
}

// An undecodable HEIC has to say why. Before plan 031 the thumbs path collapsed
// every failure into a bare bool; a silent skip here would be the same defect.
TEST(CodecDecodeIntegration, ReportsAReasonWhenAHeicCannotBeDecoded) {
    std::string path = write_garbage("bad_integration.heic");
    DecodeResult result = decode_image_with_warning(path);
    EXPECT_TRUE(result.img.empty());
    EXPECT_FALSE(result.warning.empty()) << "an undecodable HEIC must report a reason";
    fs::remove(path);
}

#ifdef __APPLE__

// ImageIO is the production decoder on macOS, so its output has to be
// indistinguishable from libheif's: a phash computed here is compared against
// phashes computed on Linux.
TEST(ImageIODecode, MatchesLibheifDimensionsForEveryOrientation) {
    for (const char* name : {"orient_1.heic", "orient_6.heic", "orient_8.heic"}) {
        cv::Mat apple = codec_decode_imageio(fixture(name));
        cv::Mat heif = codec_decode_libheif(fixture(name));
        ASSERT_FALSE(apple.empty()) << name;
        ASSERT_FALSE(heif.empty()) << name;
        EXPECT_EQ(apple.cols, heif.cols) << name;
        EXPECT_EQ(apple.rows, heif.rows) << name;
    }
}

// Orientation 6 is a 90-degree rotation, so the 200x100 source must come back
// 100x200. This assertion fails if the reported orientation is ignored, and
// equally if it is applied twice.
TEST(ImageIODecode, AppliesOrientationSoResultIsUpright) {
    cv::Mat upright = codec_decode_imageio(fixture("orient_1.heic"));
    cv::Mat rotated = codec_decode_imageio(fixture("orient_6.heic"));
    ASSERT_FALSE(upright.empty());
    ASSERT_FALSE(rotated.empty());
    EXPECT_EQ(upright.cols, 200);
    EXPECT_EQ(upright.rows, 100);
    EXPECT_EQ(rotated.cols, 100);
    EXPECT_EQ(rotated.rows, 200);
}

TEST(ImageIODecode, ProducesBgrChannelOrder) {
    cv::Mat img = codec_decode_imageio(fixture("orient_1.heic"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.type(), CV_8UC3);
    cv::Vec3b marker = img.at<cv::Vec3b>(5, 5);
    cv::Vec3b field = img.at<cv::Vec3b>(50, 150);
    EXPECT_GT(marker[0], 200) << "blue channel high inside the marker";
    EXPECT_LT(marker[2], 60) << "red channel low inside the marker";
    EXPECT_LT(field[0], 60) << "blue channel low on the red field";
    EXPECT_GT(field[2], 200) << "red channel high on the red field";
}

TEST(ImageIODecode, DecodesAvif) {
    cv::Mat img = codec_decode_imageio(fixture("sample.avif"));
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(img.cols, 200);
    EXPECT_EQ(img.rows, 100);
}

TEST(ImageIODecode, ReturnsEmptyMatOnGarbageAndDoesNotThrow) {
    std::string path = write_garbage("garbage_io.heic");
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_imageio(path); });
    EXPECT_TRUE(img.empty());
    fs::remove(path);
}

TEST(ImageIODecode, ReturnsEmptyMatForMissingFile) {
    cv::Mat img;
    EXPECT_NO_THROW({ img = codec_decode_imageio("/nonexistent/file.heic"); });
    EXPECT_TRUE(img.empty());
}

// The invariant that actually matters across platforms: a phash computed on
// macOS is compared against one computed on Linux, so the two decoders must
// produce the same hash. Per-pixel equality is deliberately NOT the assertion
// here -- see the test below for why it cannot be.
//
// Caveat on strength: these fixtures are a flat field with one marker block,
// so their phash is nearly all zeros and agreement is easy. A photographic
// fixture would exercise this properly; one arrives with plan 049's real
// Photos-library files, and this test should be re-pointed at it then.
TEST(CrossDecoderAgreement, PerceptualHashesMatch) {
    for (const char* name : {"orient_1.heic", "orient_6.heic", "exif_sample.heic", "sample.avif"}) {
        cv::Mat apple = codec_decode_imageio(fixture(name));
        cv::Mat heif = codec_decode_libheif(fixture(name));
        ASSERT_FALSE(apple.empty()) << name;
        ASSERT_FALSE(heif.empty()) << name;

        PhashResult from_apple = compute_phash(apple);
        PhashResult from_heif = compute_phash(heif);
        EXPECT_EQ(hamming_distance(from_apple.hash, from_heif.hash), 0)
            << name << ": the two decoders would hash this file differently";
    }
}

// JXL carries the same exposure as HEIC: ImageIO decodes it on macOS,
// libjxl on Linux, and the resulting phashes are compared across machines.
TEST(CrossDecoderAgreement, JxlPerceptualHashesMatch) {
    cv::Mat apple = codec_decode_imageio(fixture("sample.jxl"));
    cv::Mat jxl = codec_decode_libjxl(fixture("sample.jxl"));
    ASSERT_FALSE(apple.empty());
    ASSERT_FALSE(jxl.empty());
    ASSERT_EQ(apple.size(), jxl.size());
    EXPECT_EQ(hamming_distance(compute_phash(apple).hash, compute_phash(jxl).hash), 0)
        << "ImageIO and libjxl would hash this file differently";
}

// The decoders do not agree pixel-for-pixel, and cannot: both fixtures are
// 4:2:0, so chroma is stored at half resolution and a sample straddling a
// sharp colour edge is upsampled differently by each implementation. Measured
// on these fixtures: agreement is exact across the flat interior, and the
// disagreements are confined to the marker block's boundary -- 117 of 60,000
// channel values (0.195%), peaking at 217/255 on the corner pixel itself.
//
// So the bound asserted is on how *widespread* the disagreement is, not how
// large. A backend that started colour-managing, or switched colour matrix,
// would shift the whole image and blow through this immediately.
TEST(CrossDecoderAgreement, DisagreementIsConfinedToChromaSubsampledEdges) {
    for (const char* name : {"orient_1.heic", "orient_6.heic", "exif_sample.heic", "sample.avif"}) {
        cv::Mat apple = codec_decode_imageio(fixture(name));
        cv::Mat heif = codec_decode_libheif(fixture(name));
        ASSERT_FALSE(apple.empty()) << name;
        ASSERT_EQ(apple.size(), heif.size()) << name;
        ASSERT_EQ(apple.type(), heif.type()) << name;

        cv::Mat diff;
        cv::absdiff(apple, heif, diff);
        cv::Mat flat = diff.reshape(1);
        const int total = flat.rows * flat.cols;
        const int disagreeing = cv::countNonZero(flat > 8);
        EXPECT_LT(100.0 * disagreeing / total, 1.0)
            << name << ": " << disagreeing << " of " << total
            << " channel values differ by more than 8 -- too widespread to be edge chroma";
    }
}

#endif  // __APPLE__
