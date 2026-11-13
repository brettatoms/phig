#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The gtk client's only route into phig. src/collection.cpp implements it over
// core/; the tests link tests/fake-collection.cpp instead. Every function may be
// called from any thread, and none throws: a failure comes back as a value, with
// phig's reason where the call can fail.
//
// Reads are paged: no call returns the whole collection's rows. A full read
// through core/ carries every row's EXIF blob, about 3.8 KB a row that the
// client never reads. ids returns every match, but as ids alone.
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

struct OpenError {
    bool missing = false;  // the catalog has no database yet; only a scan creates one
    std::string message;
};

// Opens the catalog in catalog_dir for reading and metadata writes, migrating
// an older schema, and installs it as the process's catalog, so thumbnail
// lookups resolve inside it. Fails, creating nothing, if the catalog has no
// database or its schema is newer than this build reads.
std::expected<CollectionPtr, OpenError> open(const std::string& catalog_dir) noexcept;

// The catalog phig uses when none is named: PHIG_CATALOG, then the config
// file's [paths] catalog, then $XDG_DATA_HOME/phig. Fails if the config file
// cannot be read or names an empty catalog.
std::expected<std::string, std::string> default_catalog() noexcept;

enum class Sort { DateNewest, DateOldest, Filename, Size };

// What the gallery shows. Each field that is set narrows it; they AND together.
struct Filter {
    std::vector<std::string> tags;  // has every one of these
    std::string gallery;            // is in exactly this gallery
    bool favorite = false;          // is a favorite
    std::string after;              // YYYY-MM-DD: dated on or after this day
    std::string before;             // YYYY-MM-DD: dated before this day
    std::string camera;             // EXIF Model contains this
    std::string make;               // EXIF Make contains this
    std::string media;              // "image", "video", or "" for both
    std::string extension;          // without the dot, any case

    bool operator==(const Filter&) const = default;
};

struct Query {
    Filter filter;
    // Filename or path text. Text without * or ? matches anywhere in the full
    // path; text with either is a glob over the full path, as --match takes.
    // Case-sensitive.
    std::string search;
    Sort sort = Sort::DateNewest;

    bool operator==(const Query&) const = default;
};

// Every id the query selects, in its order. Ids break ties, so the order is
// the same on every call over the same rows.
std::expected<std::vector<int64_t>, std::string> ids(Collection& coll, const Query& query) noexcept;

// The rows for ids, in the order given. An id with no row is left out.
std::expected<std::vector<Row>, std::string> rows(Collection& coll,
                                                  std::span<const int64_t> ids) noexcept;

struct NamedCount {
    std::string name;
    uint32_t count = 0;
};

// What the sidebar lists. Tags leave out the built-in favorite tag, which is
// what favorites counts, and any tag no image carries.
struct Counts {
    uint32_t all = 0;
    uint32_t favorites = 0;
    std::vector<NamedCount> galleries;  // by name
    std::vector<NamedCount> tags;       // by name
};

std::expected<Counts, std::string> counts(Collection& coll) noexcept;

// One image's metadata, for the viewer.
struct Details {
    int64_t id = 0;
    std::string path;
    std::string filename;
    std::string sha256;
    std::string media_type;  // "image" or "video"
    std::string date;        // "YYYY-MM-DD HH:MM:SS": the date the gallery sorts by
    std::string make;        // EXIF Make, as phig stored it
    std::string model;       // EXIF Model
    std::string exposure_time;
    std::string f_number;
    std::string iso;
    std::string focal_length;
    int width = 0;
    int height = 0;
    int64_t file_size = 0;
    double blur_score = -1.0;       // -1 when not measured
    std::vector<std::string> tags;  // by name; favorite left out
    std::string gallery;            // empty when none
    bool favorite = false;
};

std::expected<Details, std::string> details(Collection& coll, int64_t id) noexcept;

struct WriteError {
    bool busy = false;  // another phig process holds the writer lock
    std::string message;
};

// A metadata write changes the images named in ids and no others. It takes the
// catalog's writer lock without waiting: while another phig process holds it,
// the write fails with busy set and changes nothing. An empty ids changes
// nothing and succeeds. An id with no row is skipped.
using WriteResult = std::expected<void, WriteError>;

WriteResult tag_add(Collection& coll, const std::vector<int64_t>& ids,
                    const std::string& tag) noexcept;
WriteResult tag_remove(Collection& coll, const std::vector<int64_t>& ids,
                       const std::string& tag) noexcept;
// Replaces whatever gallery each image had.
WriteResult gallery_set(Collection& coll, const std::vector<int64_t>& ids,
                        const std::string& gallery) noexcept;
WriteResult gallery_clear(Collection& coll, const std::vector<int64_t>& ids) noexcept;
WriteResult favorite_set(Collection& coll, const std::vector<int64_t>& ids, bool favorite) noexcept;

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
