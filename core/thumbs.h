#pragma once

#include <filesystem>
#include <opencv2/core.hpp>
#include <string>

// The thumbnail size phig writes unless a caller asks for another. Sizes are
// independent: generating one never invalidates or replaces another.
inline constexpr int PHIG_DEFAULT_THUMB_SIZE = 512;

// Derive the thumbnail cache path from a SHA256 hash and a size (longest edge,
// in pixels). The size is part of the path — e.g.
// ~/.cache/phig/thumbs/512/ab/ab3f7c9e...d4e2.jpg — so several sizes coexist
// rather than one overwriting another. Returns "" for a too-short sha or a
// non-positive size.
std::string get_thumb_path(const std::string& sha256, int size);

// Check if a thumbnail of this size exists in the cache. A thumbnail at some
// other size does not count.
bool thumb_exists(const std::string& sha256, int size);

// Generate a thumbnail at `size` (longest edge) from an already-decoded image.
// Never upscales: a source smaller than `size` is cached at its own dimensions.
// Returns true on success
bool generate_thumb(const cv::Mat& img, const std::string& sha256, int size);

// Generate a thumbnail at `size` by reading an image from disk
// Returns true on success
bool generate_thumb(const std::string& image_path, const std::string& sha256, int size);

// Move any pre-size-partition thumbnails (<thumbs>/ab/<sha>.jpg) into
// <thumbs>/512/ab/<sha>.jpg, returning how many were moved. They were all 512px,
// so this is a rename and never a re-encode. Idempotent and safe to call on
// every startup: once moved, there is nothing left at the old layout to find.
int migrate_flat_thumb_cache();

// True if `filename` matches phig's thumbnail naming contract: a 64-char
// lowercase hex SHA256 stem with a ".jpg" extension (e.g. "ab3f...d4e2.jpg").
// Used to confine `thumbs clean` deletion so it can never remove a non-thumbnail
// file if the cache directory is misconfigured to hold user data.
bool is_thumbnail_filename(const std::string& filename);

// The Cache Directory Tagging marker (https://bford.info/cachedir/). Backup
// tools that honour it, such as borg, restic and GNU tar with --exclude-caches,
// skip the directory holding it. Thumbnails can be regenerated, so a catalog
// backup need not carry them.
inline constexpr const char* PHIG_CACHEDIR_TAG = "CACHEDIR.TAG";

// Writes PHIG_CACHEDIR_TAG into dir if it is not already there.
void ensure_cachedir_tag(const std::string& dir);
