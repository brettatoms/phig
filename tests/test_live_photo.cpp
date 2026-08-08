#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "live_photo.h"

namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name) {
    return (fs::path(__FILE__).parent_path() / "data" / name).string();
}

}  // namespace

TEST(LivePhotoIdentifier, ReadsTheContentIdentifierFromAMov) {
    EXPECT_EQ(live_photo_content_identifier(fixture("live_photo.mov")),
              "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
}

// An ordinary camera clip carries no such key. This is the assertion that keeps
// Canon MVI_*.MOV files from ever being mistaken for Live Photo motion — the
// mis-pairing that ruled out same-basename matching in the first place.
TEST(LivePhotoIdentifier, ReturnsEmptyForAnOrdinaryVideo) {
    EXPECT_EQ(live_photo_content_identifier(fixture("test_red.mov")), "");
}

TEST(LivePhotoIdentifier, ReturnsEmptyForAMissingFileAndDoesNotThrow) {
    std::string id;
    EXPECT_NO_THROW({ id = live_photo_content_identifier("/nonexistent/x.mov"); });
    EXPECT_EQ(id, "");
}

TEST(LivePhotoIdentifier, ReturnsEmptyForANonVideoAndDoesNotThrow) {
    std::string id;
    EXPECT_NO_THROW({ id = live_photo_content_identifier(fixture("orient_1.heic")); });
    EXPECT_EQ(id, "");
}
