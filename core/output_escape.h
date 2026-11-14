#pragma once

#include <string>
#include <string_view>

// Escaping for the CSV and JSON phig writes for other programs to read.
//
// Not for phig's own stored JSON (activity-log snapshots, metadata blobs): those
// keep a path's exact bytes so it can be restored, which json_string does not.

// One RFC 4180 field: always quoted, with each embedded `"` doubled. A newline
// inside the quotes is legal and kept.
std::string csv_field(std::string_view s);

// A JSON string literal, quotes included. Escapes `"`, `\` and every control
// character below 0x20. Invalid UTF-8 becomes U+FFFD, because JSON cannot carry
// arbitrary bytes and a Linux filename can hold them; --print0 and --porcelain
// are the formats that keep a path's exact bytes.
std::string json_string(std::string_view s);
