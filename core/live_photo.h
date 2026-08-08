#pragma once

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
