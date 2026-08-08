#include <gtest/gtest.h>

#include <string>

#include "image_codec.h"
#include "scanner.h"

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
