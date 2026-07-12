#pragma once

#include <filesystem>
#include <string>
#include <vector>

// True if `ext` (with or without a leading dot, any case) is a known metadata
// sidecar extension: xmp, pp3, dop, on1, aae, json, thm, lrv. Media/RAW
// extensions (jpg, cr2, ...) are NOT sidecars here — the RAW↔JPEG same-shot
// relationship is plan 026's concern, not this module's.
bool is_known_sidecar_extension(const std::string& ext);

// Existing sidecar files belonging to `media_file`, matched against the
// known-sidecar allowlist by BOTH naming conventions:
//   stem style:   IMG_1234.xmp       (media stem + sidecar ext)
//   append style: IMG_1234.CR2.xmp   (full media filename + sidecar ext)
// Returns absolute/normalized paths that exist on disk. Empty if none.
std::vector<std::filesystem::path> find_sidecars(const std::filesystem::path& media_file);

// Organizing date read from a sidecar, in EXIF shape "YYYY:MM:DD HH:MM:SS",
// or "" if none. Parses XMP (xmp:CreateDate / exif:DateTimeOriginal, whose
// values may be "M/D/YYYY h:mm:ss AM" or "YYYY-MM-DDThh:mm:ss") and Google
// Takeout JSON (photoTakenTime.formatted / .timestamp). Returns "" on any
// unparseable or dateless file.
std::string sidecar_date(const std::filesystem::path& sidecar_file);
