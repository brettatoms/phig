#pragma once

#include <string>

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
