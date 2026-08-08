#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "live_photo.h"
#include "live_photo_internal.h"

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

// ---- containment ----
//
// The pure half is tested with synthetic buffers because a real Live Photo
// cannot be committed as a fixture; the file-backed half runs only where the
// Photos probe has exported samples, and skips otherwise.

TEST(LivePhotoContainment, FindsAnIdentifierPresentInTheBlob) {
    const std::string id = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    std::vector<unsigned char> blob = {0x00, 0x11, 0x22};
    blob.insert(blob.end(), id.begin(), id.end());
    blob.push_back(0x00);
    EXPECT_TRUE(exif_blob_contains_identifier(blob, id));
}

TEST(LivePhotoContainment, RejectsAnIdentifierThatIsAbsent) {
    std::vector<unsigned char> blob = {'n', 'o', 't', ' ', 'h', 'e', 'r', 'e'};
    EXPECT_FALSE(exif_blob_contains_identifier(blob, "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
}

// A truncated blob must not match: otherwise a still could claim a video whose
// identifier merely shares a prefix with something it carries.
TEST(LivePhotoContainment, DoesNotMatchOnAPrefix) {
    const std::string full = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    std::vector<unsigned char> blob(full.begin(), full.end() - 4);
    EXPECT_FALSE(exif_blob_contains_identifier(blob, full));
}

// An empty needle would otherwise match every still, pairing one video to all
// of them. Anything that is not a 36-character UUID is refused outright.
TEST(LivePhotoContainment, RefusesInputsThatAreNotUuids) {
    std::vector<unsigned char> blob = {'x', 'y', 'z'};
    EXPECT_FALSE(exif_blob_contains_identifier(blob, ""));
    EXPECT_FALSE(exif_blob_contains_identifier(blob, "short"));
    EXPECT_FALSE(exif_blob_contains_identifier({}, "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
}

TEST(LivePhotoContainment, StillWithoutAMakerNoteNeverMatches) {
    EXPECT_FALSE(
        still_has_identifier(fixture("orient_1.heic"), "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
}

namespace {
std::string samples_dir() { return std::string(getenv("HOME") ?: "") + "/phig-live-samples"; }
}  // namespace

TEST(LivePhotoRealFiles, StillClaimsItsOwnVideoAndNoOther) {
    const std::string dir = samples_dir();
    const std::string mov = dir + "/IMG_1112.MOV";
    const std::string heic = dir + "/IMG_1112.HEIC";
    const std::string other = dir + "/IMG_2147.HEIC";
    if (!fs::exists(mov) || !fs::exists(heic) || !fs::exists(other)) {
        GTEST_SKIP() << "no exported Live Photo samples at " << dir;
    }

    const std::string id = live_photo_content_identifier(mov);
    ASSERT_FALSE(id.empty()) << "the sample MOV carries no identifier";
    EXPECT_TRUE(still_has_identifier(heic, id)) << "the still did not claim its own video";
    EXPECT_FALSE(still_has_identifier(other, id)) << "an unrelated still claimed this video";
}

// Two different shots must not cross-match, which is the property that makes
// containment safe to pair on at all.
TEST(LivePhotoRealFiles, DifferentShotsDoNotCrossMatch) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1112.MOV") || !fs::exists(dir + "/IMG_1111.HEIC")) {
        GTEST_SKIP() << "no exported Live Photo samples at " << dir;
    }
    const std::string id = live_photo_content_identifier(dir + "/IMG_1112.MOV");
    ASSERT_FALSE(id.empty());
    EXPECT_FALSE(still_has_identifier(dir + "/IMG_1111.HEIC", id));
}
