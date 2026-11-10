#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The gtk client's only route into phig. src/collection.cpp implements it over
// core/; the tests link tests/fake-collection.cpp instead. Every function may be
// called from any thread, and none throws: a failure comes back as a value, with
// phig's reason where the call can fail.
//
// Reads are paged: no call returns the whole collection. A full-collection read
// through core/ carries every row's EXIF blob, about 3.8 KB a row that the
// client never reads.
namespace phig::gtk {

struct Row {
    int64_t id = 0;
    std::string path;
    std::string sha256;  // empty when phig has not hashed the row
    std::string filename;
};

struct Collection;  // defined by collection.cpp, or by the fake
struct CollectionClose {
    void operator()(Collection* coll) const noexcept;
};
using CollectionPtr = std::unique_ptr<Collection, CollectionClose>;

// Opens the catalog in catalog_dir read-only and installs it as the process's
// catalog, so thumbnail lookups resolve inside it. Fails if the catalog has no
// database or its schema is not the version this build reads. Never creates a
// file.
std::expected<CollectionPtr, std::string> open(const std::string& catalog_dir) noexcept;

// The catalog phig uses when none is named: PHIG_CATALOG, then the config
// file's [paths] catalog, then $XDG_DATA_HOME/phig. Fails if the config file
// cannot be read or names an empty catalog.
std::expected<std::string, std::string> default_catalog() noexcept;

// Rows are in gallery order: newest effective date first, id breaking ties.
std::expected<uint32_t, std::string> count(Collection& coll) noexcept;

// Rows [offset, offset + limit) in gallery order. A page past the end is empty.
std::expected<std::vector<Row>, std::string> page(Collection& coll, uint32_t offset,
                                                  uint32_t limit) noexcept;

enum class ThumbLookup {
    Present,  // the file at path exists
    Absent,   // path is where it would be; generate to create it
    Invalid   // sha256 or size is not valid; path is empty
};

struct ThumbLocation {
    ThumbLookup state = ThumbLookup::Invalid;
    std::string path;
};

// Where the cached thumbnail for (sha256, size) is.
ThumbLocation thumb_lookup(Collection& coll, std::string_view sha256, int size) noexcept;

// Makes sure row image_id has a cached thumbnail at size, generating it if
// absent. Mirrors core/'s EnsureThumb: NoSuchRow covers an unknown id and an
// unhashed row, SourceMissing a row whose file is gone.
enum class ThumbGenerate { Ok, NoSuchRow, SourceMissing, Failed };
ThumbGenerate thumb_generate(Collection& coll, int64_t image_id, int size) noexcept;

}  // namespace phig::gtk
