#pragma once

#include <cstdint>
#include <string>

#include "collection.h"

// A Collection with synthetic rows and no database, for the gtk tests.
namespace phig::gtk::fake {

// n_rows rows. Row i (0-based) has id i + 1, path "/fake/<i>.jpg", filename
// "<i>.jpg" and the sha256 sha(i) gives, and starts with no tags, no gallery
// and no favorite. Thumbnails live in a private temp directory that closing
// the collection removes. Generating one writes a 64x64 PNG; generation
// reports NoSuchRow for an id outside [1, n_rows].
//
// ids honours the filter's tags, gallery and favorite, and search as a
// substring of the filename; Sort::DateOldest reverses id order, and every
// other sort is id order. The other filter fields are ignored.
CollectionPtr make(uint32_t n_rows);

// Row index's sha256: the index in hex, zero-padded to 64 digits.
std::string sha(uint32_t index);

// The reason rows gives while fail_pages is set.
inline constexpr const char* kPageError = "fake: page fetch failed";

// While fail is set, rows fails with kPageError.
void fail_pages(Collection& coll, bool fail);

// How many thumbnail files thumb_generate has written.
uint32_t generate_count(Collection& coll);

// While hold is set, thumb_generate waits before generating.
void hold_generation(Collection& coll, bool hold);

// While hold is set, rows waits before returning.
void hold_pages(Collection& coll, bool hold);

// How many rows calls are inside rows right now.
uint32_t pages_in_progress(Collection& coll);

// open fails for this directory with "fake: no catalog at /fake/missing" and
// missing set, and succeeds with a 3-row collection for any other.
inline constexpr const char* kMissingCatalog = "/fake/missing";

// While hold is set, open waits before it reads its argument.
void hold_open(bool hold);

// The catalog_dir that open last read.
std::string last_opened();

}  // namespace phig::gtk::fake
