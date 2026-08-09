#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "live_photo.h"
#include "live_photo_internal.h"
#include "temp_dir.h"

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

// ---- pairing over a set of paths ----

TEST(LivePhotoPairing, LeavesAnOrdinaryVideoAlone) {
    auto pairs = find_live_photo_pairs({fixture("test_red.mov"), fixture("orient_1.heic")});
    EXPECT_TRUE(pairs.empty()) << "a camera clip must never be demoted to an attachment";
}

TEST(LivePhotoPairing, IgnoresAVideoWhoseStillIsAbsent) {
    auto pairs = find_live_photo_pairs({fixture("live_photo.mov"), fixture("orient_1.heic")});
    EXPECT_TRUE(pairs.empty());
}

TEST(LivePhotoPairing, HandlesAnEmptyInput) { EXPECT_TRUE(find_live_photo_pairs({}).empty()); }

TEST(LivePhotoRealFiles, PairsAnExportedLivePhoto) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples at " << dir;

    auto pairs = find_live_photo_pairs(
        {dir + "/IMG_1112.MOV", dir + "/IMG_1112.HEIC", dir + "/IMG_2147.HEIC"});
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(fs::path(pairs[0].still).filename(), "IMG_1112.HEIC");
    EXPECT_EQ(fs::path(pairs[0].video).filename(), "IMG_1112.MOV");
}

TEST(LivePhotoRealFiles, PairsSeveralShotsIndependently) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1112.MOV") || !fs::exists(dir + "/IMG_1111.MOV")) {
        GTEST_SKIP() << "no exported samples at " << dir;
    }
    auto pairs = find_live_photo_pairs({dir + "/IMG_1112.MOV", dir + "/IMG_1111.MOV",
                                        dir + "/IMG_1112.HEIC", dir + "/IMG_1111.HEIC"});
    EXPECT_EQ(pairs.size(), 2u);
    for (const auto& p : pairs) {
        EXPECT_EQ(fs::path(p.still).stem(), fs::path(p.video).stem())
            << "a shot was paired with another shot's video";
    }
}

// Ambiguity is refused, never guessed. A wrong pairing silently removes a video
// row and makes `mv` drag the file somewhere it does not belong, so two stills
// claiming one video must leave that video independent.
TEST(LivePhotoRealFiles, RefusesToPairWhenTwoStillsClaimOneVideo) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples at " << dir;

    const std::string tmp = make_temp_dir("livepair");
    fs::copy_file(dir + "/IMG_1112.MOV", tmp + "/a.mov");
    fs::copy_file(dir + "/IMG_1112.HEIC", tmp + "/a.heic");
    fs::copy_file(dir + "/IMG_1112.HEIC", tmp + "/copy.heic");

    auto pairs = find_live_photo_pairs({tmp + "/a.mov", tmp + "/a.heic", tmp + "/copy.heic"});
    EXPECT_TRUE(pairs.empty()) << "an ambiguous match must pair nothing";
    fs::remove_all(tmp);
}

// Pairing is scoped to a directory. A still and its video are written together
// by the camera, and restricting the search is what keeps a scan from reading
// every still's MakerNote in a collection that has a Live Photo anywhere in it.
TEST(LivePhotoRealFiles, DoesNotPairAcrossDirectories) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples at " << dir;

    const std::string tmp = make_temp_dir("livedirs");
    fs::create_directories(tmp + "/a");
    fs::create_directories(tmp + "/b");
    fs::copy_file(dir + "/IMG_1112.MOV", tmp + "/a/shot.mov");
    fs::copy_file(dir + "/IMG_1112.HEIC", tmp + "/b/shot.heic");

    auto pairs = find_live_photo_pairs({tmp + "/a/shot.mov", tmp + "/b/shot.heic"});
    EXPECT_TRUE(pairs.empty()) << "a still in another directory must not be claimed";
    fs::remove_all(tmp);
}

// ---- finding a still's video, for organize's carry pass ----

TEST(LivePhotoVideoFor, ReturnsNothingForAStillWithNoMakerNote) {
    EXPECT_TRUE(live_photo_video_for(fixture("orient_1.heic")).empty());
}

TEST(LivePhotoVideoFor, ReturnsNothingForAMissingFile) {
    fs::path p;
    EXPECT_NO_THROW({ p = live_photo_video_for("/nonexistent/x.heic"); });
    EXPECT_TRUE(p.empty());
}

TEST(LivePhotoRealFiles, FindsTheVideoBelongingToAStill) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1112.MOV")) GTEST_SKIP() << "no exported samples at " << dir;

    fs::path tmp = make_temp_dir("livefind");
    fs::copy_file(dir + "/IMG_1112.HEIC", tmp / "shot.heic");
    fs::copy_file(dir + "/IMG_1112.MOV", tmp / "motion.mov");
    // An unrelated still and video in the same directory must not confuse it.
    fs::copy_file(dir + "/IMG_2147.HEIC", tmp / "other.heic");
    fs::copy_file("tests/data/test_red.mov", tmp / "clip.mov");

    EXPECT_EQ(live_photo_video_for(tmp / "shot.heic").filename(), "motion.mov")
        << "found by identifier, not by name";
    EXPECT_TRUE(live_photo_video_for(tmp / "other.heic").empty())
        << "an unrelated still must claim nothing";
    fs::remove_all(tmp);
}

TEST(LivePhotoStillFor, ReturnsNothingForAnOrdinaryVideo) {
    EXPECT_TRUE(live_photo_still_for(fixture("test_red.mov")).empty());
}

TEST(LivePhotoRealFiles, FindsTheStillBelongingToAVideo) {
    const std::string dir = samples_dir();
    if (!fs::exists(dir + "/IMG_1109.MOV")) GTEST_SKIP() << "no exported samples at " << dir;

    fs::path tmp = make_temp_dir("livestill");
    fs::copy_file(dir + "/IMG_1109.HEIC", tmp / "master.heic");
    fs::copy_file(dir + "/IMG_1109.MOV", tmp / "motion.mov");
    fs::copy_file(dir + "/IMG_2146.HEIC", tmp / "unrelated.heic");

    EXPECT_EQ(live_photo_still_for(tmp / "motion.mov").filename(), "master.heic");
    fs::remove_all(tmp);
}
