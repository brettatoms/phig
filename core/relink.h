#pragma once

#include <string>
#include <vector>

#include "database.h"

// Scan re-link (plan 023): for each removed path whose stored sha256 matches
// EXACTLY ONE other DB row whose file exists on disk, transfer user-authored
// metadata (tags; gallery if the target has none; faces if the target has
// none) to that row, logging a 'relink' entry when op_id > 0. Zero or multiple
// on-disk candidates: do nothing (ambiguous). Rows are NOT deleted here — the
// caller deletes them afterwards. Returns the number of rows re-linked.
int relink_moved_rows(Database& db, const std::vector<std::string>& removed_paths, int64_t op_id);

// Scan cleanup (plan 026 task 5): for each indexed row under `scan_prefix`
// that is a JPEG with a same-basename RAW file on disk (has_sibling_raw), and
// whose RAW file is itself an indexed row, transfer the JPEG row's
// user-authored metadata to the RAW row (transfer_user_metadata), logging a
// 'relink' entry when op_id > 0. If the RAW isn't indexed yet, the JPEG is
// left alone (it will be demoted on a later scan once the RAW is indexed).
// Rows are NOT deleted here — the caller deletes the returned paths
// afterwards. Returns the demoted JPEG paths.
std::vector<std::string> demote_jpeg_rows(Database& db, const std::string& scan_prefix,
                                          int64_t op_id);

// Video rows that are really Live Photo attachments, demoted the way
// demote_jpeg_rows demotes a sidecar JPEG: user metadata moves to the still,
// then the caller deletes the returned paths.
//
// Needed because such a video is still on disk, so it never appears in
// removed_paths — a collection indexed before pairing existed keeps its stale
// video rows until this runs. Returns the paths demoted.
std::vector<std::string> demote_live_photo_rows(Database& db, const std::string& scan_prefix,
                                                int64_t op_id);
