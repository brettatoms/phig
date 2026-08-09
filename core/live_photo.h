#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Apple Live Photo pairing.
//
// A Live Photo is a still plus a short MOV written together by the camera, and
// Apple links the two with a Content Identifier UUID carried in both files. One
// shot, two files — so phig indexes the still and carries the MOV as an
// attachment, exactly as plan 026 treats a RAW and its sidecar JPEG.
//
// The two halves are not equally reachable. The MOV's copy is a plain
// QuickTime metadata key and reads cleanly. The still's copy sits inside an
// Apple MakerNote that libexif cannot parse at all — measured 2026-08-08:
// exif_data_get_mnote_data() returns NULL on real Live Photos and libexif
// 0.6.26 exports zero Apple symbols, shipping Canon, Olympus and Pentax
// parsers only. So the still is never asked for its identifier; it is tested
// for containment of the MOV's. See plans/038-live-photos.md.

// The Content Identifier from a video container, or empty when the file has no
// such key, is not a video, or cannot be opened. Thread-safe. Never throws.
std::string live_photo_content_identifier(const std::string& video_path);

// True when `still_path`'s MakerNote contains `id`, i.e. the still is the
// master of the video that identifier came from.
//
// Containment rather than extraction: Apple's identifier sits in a MakerNote
// libexif cannot parse, and its position among the other UUIDs there is not a
// contract Apple owes us — a Live still carries three UUID-shaped strings and
// an ordinary still two. Searching for a UUID already known from the video
// avoids both problems, and a false positive would need a 36-character
// collision. Never throws.
bool still_has_identifier(const std::string& still_path, const std::string& id);

struct LivePhotoPair {
    std::string still;  // the master: holds the row, EXIF, phash, thumbnail, faces
    std::string video;  // the attachment: carried, never indexed on its own
};

// The Live Photo pairs among `paths`. A video is absent from the result unless
// it carries a Content Identifier and exactly one still in `paths` contains it:
// no identifier, no match, or more than one match all leave it independent.
//
// Refusing ambiguity is deliberate. A wrong pairing silently removes a video row
// and makes organize drag the file somewhere it does not belong, so the failure
// mode is "not paired" — today's behaviour — and never "paired wrongly".
//
// Reads each still's MakerNote at most once, and does no still work at all when
// no video in the set carries an identifier. Plan 044 measured what the naive
// shape costs: has_sibling_raw re-listed a directory per file and made scan
// quadratic in directory size.
std::vector<LivePhotoPair> find_live_photo_pairs(const std::vector<std::string>& paths);

// The Live Photo video belonging to `still_path`, or empty when there is none.
//
// Searches the still's own directory, and exits immediately when the still has
// no MakerNote at all — which is every image not produced by an Apple device,
// so an ordinary collection never pays for the directory listing. Returns empty
// if more than one video matches, on the same refuse-ambiguity rule as
// find_live_photo_pairs. Never throws.
std::filesystem::path live_photo_video_for(const std::filesystem::path& still_path);

// As above, but reads the still's identifier from one path and searches another
// directory for its video.
//
// Organize needs this split: its carry pass runs after the parent has already
// been moved, so the still's bytes are at the destination while its video is
// still in the source directory. Reading and searching the same path there
// would find nothing at all.
std::filesystem::path live_photo_video_in(const std::filesystem::path& still_for_metadata,
                                          const std::filesystem::path& search_dir);

// The still that claims `video_path`'s Content Identifier, searched in the
// video's own directory, or empty when there is none or more than one.
//
// The inverse of live_photo_video_in, needed by scan's reconcile phase: a
// collection indexed before pairing existed has independent video rows whose
// files are still on disk, so they never appear as "removed" and must be
// demoted by looking up the master they belong to. Never throws.
std::filesystem::path live_photo_still_for(const std::filesystem::path& video_path);
